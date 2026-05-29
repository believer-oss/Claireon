// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_WorldGetActive.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

FString ClaireonTool_WorldGetActive::GetCategory() const { return TEXT("world"); }
FString ClaireonTool_WorldGetActive::GetOperation() const { return TEXT("get_active_world"); }

FString ClaireonTool_WorldGetActive::GetDescription() const
{
	return TEXT("Return the world that's actually live right now. "
		"During PIE returns the PIE world; otherwise returns the editor world. "
		"Errors when neither is available (editor still initializing). "
		"Prefer this over the two python-side paths to the editor world: "
		"unreal.EditorLevelLibrary.get_editor_world() is deprecated and returns null in PIE, and "
		"unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world() (the modern "
		"replacement) still returns null in PIE. claireon.world_get_active_world covers both cases. "
		"To inspect AWorldSettings, call world.get_world_settings() in Python -- "
		"world.get_editor_property('persistent_level') / .get_editor_property('world_settings') fail "
		"because those properties are engine-protected.");
}

TSharedPtr<FJsonObject> ClaireonTool_WorldGetActive::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_WorldGetActive::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	if (!GEngine)
	{
		return MakeErrorResult(TEXT("GEngine not available (editor not yet initialized)"));
	}

	// Walk world contexts -- PIE first (per the I6 priority order: PIE > Editor).
	UWorld* SelectedWorld = nullptr;
	EWorldType::Type SelectedType = EWorldType::None;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			SelectedWorld = WorldContext.World();
			SelectedType = EWorldType::PIE;
			break;
		}
	}
	if (!SelectedWorld)
	{
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::Editor && WorldContext.World())
			{
				SelectedWorld = WorldContext.World();
				SelectedType = EWorldType::Editor;
				break;
			}
		}
	}

	if (!SelectedWorld)
	{
		return MakeErrorResult(TEXT("No live world: neither a PIE world nor an editor world is currently loaded. "
			"Open a map with map.open_async first."));
	}

	const bool bIsPie = (SelectedType == EWorldType::PIE);
	const FString WorldPath = SelectedWorld->GetPathName();
	const FString WorldName = SelectedWorld->GetName();

	const TCHAR* TypeStr = TEXT("Unknown");
	switch (SelectedType)
	{
	case EWorldType::Editor: TypeStr = TEXT("Editor"); break;
	case EWorldType::PIE:    TypeStr = TEXT("PIE"); break;
	default: break;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_path"), WorldPath);
	Data->SetStringField(TEXT("world_name"), WorldName);
	Data->SetStringField(TEXT("world_type"), TypeStr);
	Data->SetBoolField(TEXT("is_pie"), bIsPie);
	Data->SetBoolField(TEXT("has_begun_play"), SelectedWorld->HasBegunPlay());

	const FString Summary = FString::Printf(TEXT("Active %s world: %s"), TypeStr, *WorldName);
	return MakeSuccessResult(Data, Summary);
}
