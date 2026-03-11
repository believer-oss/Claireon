// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEStart.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Settings/LevelEditorPlaySettings.h"

FString ClaireonTool_PIEStart::GetName() const
{
	return TEXT("pie_start");
}

FString ClaireonTool_PIEStart::GetDescription() const
{
	return TEXT("Start a Play In Editor (PIE) session, optionally loading a map first");
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
		return MakeErrorResult(TEXT("A PIE session is already active. Stop it first with pie_stop."));
	}

	// Optional: load a map first
	FString MapPath;
	if (Arguments->TryGetStringField(TEXT("mapPath"), MapPath) && !MapPath.IsEmpty())
	{
		FEditorFileUtils::LoadMap(MapPath);
	}

	// Determine net mode
	FString NetModeStr;
	Arguments->TryGetStringField(TEXT("netMode"), NetModeStr);

	const TSet<FString>& DisabledModes = UClaireonSettings::Get()->DisabledPIENetModes;

	// Block disabled net modes
	if (!NetModeStr.IsEmpty() && DisabledModes.Contains(NetModeStr))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Net mode '%s' is disabled in MCP settings (Editor Preferences > Plugins > MCP REPL > PIE)."),
			*NetModeStr));
	}

	ULevelEditorPlaySettings* PlaySettings = GetMutableDefault<ULevelEditorPlaySettings>();
	if (NetModeStr == TEXT("Standalone"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
	}
	else if (NetModeStr == TEXT("ListenServer"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
	}
	else if (NetModeStr == TEXT("DedicatedServer"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	}
	else if (NetModeStr == TEXT("Client"))
	{
		PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
	}
	else
	{
		// Default: use first non-disabled mode, preferring Client
		if (!DisabledModes.Contains(TEXT("Client")))
		{
			PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Client);
			NetModeStr = TEXT("Client");
		}
		else if (!DisabledModes.Contains(TEXT("Standalone")))
		{
			PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
			NetModeStr = TEXT("Standalone");
		}
		else if (!DisabledModes.Contains(TEXT("ListenServer")))
		{
			PlaySettings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
			NetModeStr = TEXT("ListenServer");
		}
		else
		{
			return MakeErrorResult(TEXT("All PIE net modes are disabled in MCP settings."));
		}
	}

	// Determine the current map path before starting PIE
	FString CurrentMapPath;
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (EditorWorld)
	{
		CurrentMapPath = EditorWorld->GetPathName();
		// Use the map path provided, or fall back to the current map
		if (!MapPath.IsEmpty())
		{
			CurrentMapPath = MapPath;
		}
	}

	// Generate session ID and register before starting PIE
	FString SessionId = PIEManager.GenerateSessionId();
	PIEManager.OnPIEStarted(SessionId, CurrentMapPath, NetModeStr);

	// Disable CPU throttling so PIE runs at full speed
	PIEManager.DisableThrottleCPU();

	// Start PIE
	FRequestPlaySessionParams Params;
	GEditor->RequestPlaySession(Params);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("map"), CurrentMapPath);
	Data->SetStringField(TEXT("net_mode"), NetModeStr);
	Data->SetStringField(TEXT("status"), TEXT("starting"));

	FString Summary = FString::Printf(TEXT("PIE started on %s (net mode: %s)"), *CurrentMapPath, *NetModeStr);
	return MakeSuccessResult(Data, Summary);
}
