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
	return TEXT("Return the world that is actually live right now: the PIE world during play, otherwise the editor "
		"world; errors when neither exists. Prefer it over the Python paths, which both return null in PIE "
		"-- unreal.EditorLevelLibrary.get_editor_world() (deprecated) and "
		"UnrealEditorSubsystem.get_editor_world(). Stateless / read-only, opens no session.");
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
	if (!IsValid(GEngine))
	{
		return MakeErrorResult(TEXT("GEngine not available (editor not yet initialized)"));
	}

	// Walk world contexts -- PIE first (per the I6 priority order: PIE > Editor).
	UWorld* SelectedWorld = nullptr;
	EWorldType::Type SelectedType = EWorldType::None;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
		{
			SelectedWorld = WorldContext.World();
			SelectedType = EWorldType::PIE;
			break;
		}
	}
	if (!IsValid(SelectedWorld))
	{
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::Editor && IsValid(WorldContext.World()))
			{
				SelectedWorld = WorldContext.World();
				SelectedType = EWorldType::Editor;
				break;
			}
		}
	}

	if (!IsValid(SelectedWorld))
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
