// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MapOpen.h"
#include "ClaireonPathResolver.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Containers/Ticker.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

FString ClaireonTool_MapOpen::GetCategory() const { return TEXT("map"); }
FString ClaireonTool_MapOpen::GetOperation() const { return TEXT("open_async"); }

FString ClaireonTool_MapOpen::GetDescription() const
{
	// Kept under the 400-char budget Claireon.DescriptionLint enforces. The
	// duplicate_and_open_map_async pointer was dropped rather than shortened further:
	// that tool is already reachable on its own name and this one's contract is not.
	return TEXT("Open a map (level) by asset path. Async: the load is deferred until the current script "
		"finishes, so nothing later in the same execute() call may assume it is loaded. Aborts with "
		"bIsError if a non-editor UWorld is in memory or the editor has unsaved packages. Waits for "
		"an in-progress asset-registry scan, then drains asset compilation "
		"(wait_for_asset_compilation=false skips). Non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_MapOpen::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// mapPath - required
	TSharedPtr<FJsonObject> MapPathProp = MakeShared<FJsonObject>();
	MapPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MapPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the map to open (e.g. /Game/Maps/MyLevel)"));
	Properties->SetObjectField(TEXT("mapPath"), MapPathProp);

	// wait_for_asset_compilation - optional, DEFAULT TRUE (parity with pie_start_async)
	TSharedPtr<FJsonObject> WaitCompileProp = MakeShared<FJsonObject>();
	WaitCompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
	WaitCompileProp->SetStringField(TEXT("description"),
		TEXT("Drain in-flight asset compilation (shaders, textures, post-process resources) after the "
			 "map loads, before the first frame renders. Default: true. Rendering a freshly-loaded map "
			 "mid-compile can hard-crash the editor on a render assert -- a cold-DDC ambient cubemap "
			 "hits FAmbientCubemapCompositePS with an unset AmbientCubemapSampler. Pass false only "
			 "when the DDC is known warm and the extra wait is unwanted."));
	Properties->SetObjectField(TEXT("wait_for_asset_compilation"), WaitCompileProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("mapPath")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MapOpen::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString MapPath;
	if (!Arguments->TryGetStringField(TEXT("mapPath"), MapPath) || MapPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: mapPath"));
	}

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(MapPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	MapPath = ResolveResult.ResolvedPath.Path;

	// Post-resolver: append object name for LoadMap (required by UEditorLoadingAndSavingUtils)
	if (!MapPath.Contains(TEXT(".")))
	{
		FString AssetName = FPaths::GetBaseFilename(MapPath);
		MapPath = MapPath + TEXT(".") + AssetName;
	}

	// Verify the asset exists in the registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(MapPath));
	if (!AssetData.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Map asset not found: %s"), *MapPath));
	}

	// Defaults to true: a hard editor crash is a worse failure than a slow call, and the crash
	// only occurs on the cold path where the wait was unavoidable anyway.
	bool bWaitForAssetCompilation = true;
	Arguments->TryGetBoolField(TEXT("wait_for_asset_compilation"), bWaitForAssetCompilation);

	TSharedPtr<FJsonObject> PayloadObj = MakeShared<FJsonObject>();
	PayloadObj->SetStringField(TEXT("mapPath"), MapPath);
	PayloadObj->SetBoolField(TEXT("waitForAssetCompilation"), bWaitForAssetCompilation);
	FString PayloadJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
	FJsonSerializer::Serialize(PayloadObj.ToSharedRef(), Writer);
	Writer->Close();

	// Enqueue — does NOT execute yet. The post-execution hook in ExecutePython
	// will run the GC barrier then call ExecuteDeferredLoadMap.
	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::LoadMap,
		PayloadJson
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("open_map"));
	Data->SetStringField(TEXT("map_path"), MapPath);

	const FString Summary = FString::Printf(TEXT("Map load queued: %s — executes after script completes"), *MapPath);
	return MakeSuccessResult(Data, Summary);
}

void ClaireonTool_MapOpen::ExecuteDeferredLoadMap(const FString& Payload)
{
	// Payload is the JSON object queued by Execute; a bare object path is
	// accepted as a fallback so raw-path callers keep working.
	FString MapPath = Payload;
	bool bWaitForAssetCompilation = true;
	{
		TSharedPtr<FJsonObject> PayloadObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
		if (FJsonSerializer::Deserialize(Reader, PayloadObj) && PayloadObj.IsValid())
		{
			PayloadObj->TryGetStringField(TEXT("mapPath"), MapPath);
			PayloadObj->TryGetBoolField(TEXT("waitForAssetCompilation"), bWaitForAssetCompilation);
		}
	}

	// Defer to next tick — LoadMap triggers world destruction and GC,
	// which asserts if called during a tick.
	FString CapturedPath = MapPath;
	TSharedRef<bool> bLoggedScanWait = MakeShared<bool>(false);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([CapturedPath, bWaitForAssetCompilation, bLoggedScanWait](float)
		{
			// Gate on the asset-registry scan: loading a map while the initial
			// scan is still running is the same crash class as loading assets
			// from init_unreal during startup. Re-tick until the scan drains.
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			if (AssetRegistry.IsLoadingAssets())
			{
				if (!*bLoggedScanWait)
				{
					UE_LOG(LogClaireon, Log,
						TEXT("[map_open_async] Asset registry scan in progress; map load waits for it to complete."));
					*bLoggedScanWait = true;
				}
				return true; // keep waiting
			}

			// Second-pass barrier: catch any UObject wrappers orphaned between
			// the post-execution barrier and this tick (e.g. from prior execute()
			// calls whose private namespaces haven't been GC'd yet).
			FClaireonBridge::RunWorldTransitionBarrier();

			// Composed guard: refuse the transition if any leaked World remains
			// OR if the editor has unsaved work that would be lost. The helper
			// formats and returns the structured error.
			FString GuardError;
			if (FClaireonBridge::ShouldAbortDeferredLoadMap(GuardError))
			{
				UE_LOG(LogClaireon, Error, TEXT("%s"), *GuardError);
				FClaireonBridge::ReportDeferredActionAbort(GuardError);
				return false; // do NOT call LoadMap
			}

			FEditorFileUtils::LoadMap(CapturedPath);

			// Drain async asset compilation before the next frame renders the
			// freshly-loaded world. Rendering mid-compile can fatally assert
			// (cold-DDC ambient cubemap reaches FAmbientCubemapCompositePS with
			// AmbientCubemapSampler unset), so this defaults on and is only
			// skipped when the caller opts out.
			if (bWaitForAssetCompilation)
			{
				const int32 NumRemaining = FAssetCompilingManager::Get().GetNumRemainingAssets();
				if (NumRemaining > 0)
				{
					UE_LOG(LogClaireon, Log,
						TEXT("[map_open_async] Draining %d compiling asset(s) after map load to avoid a "
							 "mid-compile render assert."), NumRemaining);
					FAssetCompilingManager::Get().FinishAllCompilation();
				}
			}
			return false; // one-shot
		}), 0.0f);
}
