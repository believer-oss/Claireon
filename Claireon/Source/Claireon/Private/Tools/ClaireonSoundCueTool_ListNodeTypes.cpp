// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_ListNodeTypes.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_ListNodeTypes::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_ListNodeTypes::GetOperation() const { return TEXT("list_node_types"); }

FString FClaireonSoundCueTool_ListNodeTypes::GetDescription() const
{
	return TEXT("Enumerate all available USoundNode subclasses with their short_name and class_path. "
				"Non-session, read-only query; no session_id required. Use the returned short_name "
				"or class_path with soundcue.add_node to insert nodes into an open session.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_ListNodeTypes::GetInputSchema() const
{
	FToolSchemaBuilder S;
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_ListNodeTypes::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	const TMap<FName, UClass*>& Registry = ClaireonAudioHelpers::GetSoundNodeClassRegistry();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Names;
	for (const auto& Pair : Registry)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("short_name"), Pair.Key.ToString());
		E->SetStringField(TEXT("class_path"), Pair.Value ? Pair.Value->GetPathName() : FString());
		Names.Add(MakeShared<FJsonValueObject>(E));
	}
	Data->SetArrayField(TEXT("sound_node_types"), Names);
	Data->SetNumberField(TEXT("count"), Names.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("%d sound node types"), Names.Num()));
}
