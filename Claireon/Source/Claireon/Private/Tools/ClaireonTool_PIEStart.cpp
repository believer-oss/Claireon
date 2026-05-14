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

FString ClaireonTool_PIEStart::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEStart::GetOperation() const { return TEXT("start_async"); }

FString ClaireonTool_PIEStart::GetDescription() const
{
	return TEXT("Start a Play In Editor (PIE) session, optionally loading a map first. "
		"The PIE start is deferred until after the current script finishes. "
		"If mapPath is supplied, a leaked-World guard runs before the pre-PIE map load: "
		"a leaked World aborts the PIE start with a structured error -- "
		"use duplicate_and_open_map_async if you need to PIE into a freshly-duplicated map.");
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
	if (!GEditor)
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

	// Serialize validated args as JSON payload for deferred execution
	TSharedPtr<FJsonObject> PayloadObj = MakeShared<FJsonObject>();
	PayloadObj->SetStringField(TEXT("netMode"), NetModeStr);
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

	FString Summary = FString::Printf(TEXT("PIE start queued (net mode: %s) — executes after script completes"), *NetModeStr);
	return MakeSuccessResult(Data, Summary);
}

void ClaireonTool_PIEStart::ExecuteDeferredPIEStart(const FString& Payload)
{
	if (!GEditor)
	{
		return;
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

	// Optional: load a map first. Run the leaked-World guard BEFORE
	// LoadMap so a duplicate-asset leak crashes loud-and-clean instead
	// of fatally asserting in EditorDestroyWorld(). [RESOLVED] D1.
	if (!MapPath.IsEmpty())
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

		FEditorFileUtils::LoadMap(MapPath);
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
	if (EditorWorld)
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
