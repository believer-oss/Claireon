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
	return TEXT("Enumerate available USoundNode subclasses (short_name + class_path). Stateless.");
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
