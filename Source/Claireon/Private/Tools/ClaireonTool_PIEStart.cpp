// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEStart.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"

FString ClaireonTool_PIEStart::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEStart::GetOperation() const { return TEXT("start_async"); }

FString ClaireonTool_PIEStart::GetDescription() const
{
	// Kept under the 400-char budget Claireon.DescriptionLint enforces.
	return TEXT("Start a Play In Editor (PIE) session, optionally loading a map first. "
		"The start is deferred until the current script finishes, and waits for any "
		"in-progress asset-registry scan. "
		"If mapPath is supplied, a leaked-World guard runs before the pre-PIE map load and "
		"aborts with a structured error; "
		"use duplicate_and_open_map_async to PIE into a freshly-duplicated map.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEStart::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// mapPath - optional
	TSharedPtr<FJsonObject> MapPathProp = MakeShared<FJsonObject>();
	MapPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MapPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the map to load before starting PIE (optional, uses current map if not specified)"));
	Properties->SetObjectField(TEXT("mapPath"), MapPathProp);

	// wait_for_asset_compilation - optional, DEFAULT TRUE
	TSharedPtr<FJsonObject> WaitCompileProp = MakeShared<FJsonObject>();
	WaitCompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
	WaitCompileProp->SetStringField(TEXT("description"),
		TEXT("Wait for in-flight asset compilation (shaders, textures, post-process resources) to "
			 "drain before starting PIE. Default: true. Starting PIE while assets are still async-"
			 "compiling can hard-crash the editor on a render assert -- a cold-DDC ambient cubemap "
			 "hits FAmbientCubemapCompositePS with an unset AmbientCubemapSampler. Pass false only "
			 "when the DDC is known warm and the extra wait is unwanted."));
	Properties->SetObjectField(TEXT("wait_for_asset_compilation"), WaitCompileProp);

	// netMode - optional, filtered by settings
	TSharedPtr<FJsonObject> NetModeProp = MakeShared<FJsonObject>();
	NetModeProp->SetStringField(TEXT("type"), TEXT("string"));
	NetModeProp->SetStringField(TEXT("description"),
		TEXT("Network mode for PIE session (default: Client)"));
	{
		const TSet<FString>& DisabledModes = UClaireonSettings::Get()->DisabledPIENetModes;

		static const TArray<FString> AllModes = {
			TEXT("Standalone"), TEXT("ListenServer"), TEXT("DedicatedServer"), TEXT("Client")
		};

		TArray<TSharedPtr<FJsonValue>> EnumValues;
		for (const FString& Mode : AllModes)
		{
			if (!DisabledModes.Contains(Mode))
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Mode));
			}
		}

		if (EnumValues.Num() > 0)
		{
			NetModeProp->SetArrayField(TEXT("enum"), EnumValues);
		}
	}
	Properties->SetObjectField(TEXT("netMode"), NetModeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEStart::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	// Check if PIE is already running
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	if (PIEManager.GetActiveSession() != nullptr)
	{
		return MakeErrorResult(TEXT("A PIE session is already active. Stop it first with pie_stop_async."));
	}

	// Determine net mode (validate now, execute later)
	FString NetModeStr;
	Arguments->TryGetStringField(TEXT("netMode"), NetModeStr);

	const TSet<FString>& DisabledModes = UClaireonSettings::Get()->DisabledPIENetModes;

	if (!NetModeStr.IsEmpty() && DisabledModes.Contains(NetModeStr))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Net mode '%s' is disabled in MCP settings (Editor Preferences > Plugins > Claireon > PIE)."),
			*NetModeStr));
	}

	// Resolve default net mode if not specified
	if (NetModeStr.IsEmpty())
	{
		if (!DisabledModes.Contains(TEXT("Client"))) { NetModeStr = TEXT("Client"); }
		else if (!DisabledModes.Contains(TEXT("Standalone"))) { NetModeStr = TEXT("Standalone"); }
		else if (!DisabledModes.Contains(TEXT("ListenServer"))) { NetModeStr = TEXT("ListenServer"); }
		else { return MakeErrorResult(TEXT("All PIE net modes are disabled in MCP settings.")); }
	}

	FString MapPath;
	Arguments->TryGetStringField(TEXT("mapPath"), MapPath);

	// Defaults to true: a hard editor crash is a worse failure than a slow call, and the crash
	// only occurs on the cold path where the wait was unavoidable anyway.
	bool bWaitForAssetCompilation = true;
	Arguments->TryGetBoolField(TEXT("wait_for_asset_compilation"), bWaitForAssetCompilation);

	// Serialize validated args as JSON payload for deferred execution
	TSharedPtr<FJsonObject> PayloadObj = MakeShared<FJsonObject>();
	PayloadObj->SetStringField(TEXT("netMode"), NetModeStr);
	PayloadObj->SetBoolField(TEXT("waitForAssetCompilation"), bWaitForAssetCompilation);
	if (!MapPath.IsEmpty())
	{
		PayloadObj->SetStringField(TEXT("mapPath"), MapPath);
	}
	FString PayloadJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
	FJsonSerializer::Serialize(PayloadObj.ToSharedRef(), Writer);
	Writer->Close();

	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::PIEStart,
		PayloadJson
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("pie_start"));
	Data->SetStringField(TEXT("net_mode"), NetModeStr);

	// P0-7: pre-flight dirty-package census.
	//
	// This payload is built and returned BEFORE the deferred action runs, so the
	// post-save list of written packages does not exist yet. Report what WILL be
	// written instead -- deliberately labelled as prospective, because claiming a
	// write that has not happened is the same overclaiming disease in reverse.
	//
	// Reuses EnsureNoUnsavedWork rather than re-enumerating: it already builds
	// exactly this list with bIsMapPackage flags.
	TArray<FClaireonUnsavedPackage> DirtyPackages;
	FClaireonBridge::EnsureNoUnsavedWork(DirtyPackages);
	if (DirtyPackages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> PendingArray;
		PendingArray.Reserve(DirtyPackages.Num());
		for (const FClaireonUnsavedPackage& Package : DirtyPackages)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("package_name"), Package.PackageName);
			Entry->SetBoolField(TEXT("is_map_package"), Package.bIsMapPackage);
			PendingArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Data->SetArrayField(TEXT("packages_pending_auto_save"), PendingArray);
	}

	FString Summary = FString::Printf(TEXT("PIE start queued (net mode: %s) -- executes after script completes"), *NetModeStr);

	FToolResult Result = MakeSuccessResult(Data, Summary);
	if (DirtyPackages.Num() > 0)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("%d dirty package(s) will be written to disk by auto-save when this deferred PIE start runs. ")
			TEXT("Disable with ClaireonSettings.bAutoSaveBeforeDeferredActions if this is unwanted."),
			DirtyPackages.Num()));
	}
	return Result;
}

void ClaireonTool_PIEStart::ExecuteDeferredPIEStart(const FString& Payload)
{
	if (!IsValid(GEditor))
	{
		return;
	}

	// Gate on the asset-registry scan: starting PIE (or the pre-PIE map load
	// below) while the initial scan is still running is the same crash class
	// as loading assets from init_unreal during startup. Re-dispatch this
	// whole action once the scan drains.
	{
		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (AssetRegistry.IsLoadingAssets())
		{
			UE_LOG(LogClaireon, Log,
				TEXT("[pie_start_async] Asset registry scan in progress; PIE start waits for it to complete."));
			const FString CapturedPayload = Payload;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([CapturedPayload](float)
				{
					IAssetRegistry& InnerRegistry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					if (InnerRegistry.IsLoadingAssets())
					{
						return true; // keep waiting
					}
					ExecuteDeferredPIEStart(CapturedPayload);
					return false; // one-shot
				}), 0.25f);
			return;
		}
	}

	// Parse payload
	TSharedPtr<FJsonObject> PayloadObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, PayloadObj) || !PayloadObj.IsValid())
	{
		UE_LOG(LogClaireon, Error, TEXT("ExecuteDeferredPIEStart: Failed to parse payload"));
		return;
	}

	FString MapPath;
	PayloadObj->TryGetStringField(TEXT("mapPath"), MapPath);
	FString NetModeStr;
	PayloadObj->TryGetStringField(TEXT("netMode"), NetModeStr);

	// Run the leaked-World guard BEFORE any map load or PIE start so a
	// duplicate-asset leak crashes loud-and-clean instead of fatally
	// asserting in EditorDestroyWorld(). [RESOLVED] D1.
	// P2-9b: the barrier used to sit inside the MapPath branch, so PIE into
	// the ALREADY-OPEN map never purged leaked Python UObject refs and never
	// honored the guard -- PIE itself tears the editor world down either way,
	// so the no-map path needs both just as much.
	{
		// Match the deferred-tick barrier already run by
		// ClaireonTool_ExecutePython before this dispatch -- the second
		// pass catches anything orphaned between the post-execute
		// barrier and this site.
		FClaireonBridge::RunWorldTransitionBarrier();

		TArray<FClaireonLeakedWorld> Remaining;
		if (!FClaireonBridge::EnsureNoLeakedWorlds(Remaining))
		{
			const FString Msg = FClaireonBridge::FormatLeakedWorldError(Remaining);
			UE_LOG(LogClaireon, Error, TEXT("%s"), *Msg);
			FClaireonBridge::ReportDeferredActionAbort(Msg);
			return; // do NOT load map and do NOT start PIE
		}
	}

	// Optional: load a map first.
	if (!MapPath.IsEmpty())
	{
		FEditorFileUtils::LoadMap(MapPath);
	}

	// Drain async asset compilation before PIE. Must run AFTER any map load, since loading is
	// what queues the newly-referenced assets. Starting PIE mid-compile can fatally assert in
	// the renderer (a cold-DDC post-process ambient cubemap reaches
	// FAmbientCubemapCompositePS with AmbientCubemapSampler unset), which is unrecoverable --
	// so this defaults on and is only skipped when the caller opts out.
	bool bWaitForAssetCompilation = true;
	PayloadObj->TryGetBoolField(TEXT("waitForAssetCompilation"), bWaitForAssetCompilation);
	if (bWaitForAssetCompilation)
	{
		const int32 Remaining = FAssetCompilingManager::Get().GetNumRemainingAssets();
		if (Remaining > 0)
		{
			UE_LOG(LogClaireon, Log,
				TEXT("[pie_start_async] Draining %d compiling asset(s) before PIE to avoid a "
					 "mid-compile render assert."), Remaining);
			FAssetCompilingManager::Get().FinishAllCompilation();
		}
	}

	// Configure net mode
	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	if (NetModeStr == TEXT("Standalone"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
	}
	else if (NetModeStr == TEXT("ListenServer"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
	}
	else
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	}

	// Determine map path for session registration
	FString CurrentMapPath;
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (IsValid(EditorWorld))
	{
		CurrentMapPath = EditorWorld->GetPathName();
		if (!MapPath.IsEmpty())
		{
			CurrentMapPath = MapPath;
		}
	}

	// Generate session ID and register
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	FString SessionId = PIEManager.GenerateSessionId();
	PIEManager.OnPIEStarted(SessionId, CurrentMapPath, NetModeStr);
	PIEManager.DisableThrottleCPU();

	// Start PIE
	FRequestPlaySessionParams Params;
	GEditor->RequestPlaySession(Params);

	UE_LOG(LogClaireon, Log, TEXT("ExecuteDeferredPIEStart: Started PIE session %s on %s (%s)"),
		*SessionId, *CurrentMapPath, *NetModeStr);
}
