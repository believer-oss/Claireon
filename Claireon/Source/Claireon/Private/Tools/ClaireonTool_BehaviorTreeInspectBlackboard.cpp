// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BehaviorTreeInspectBlackboard.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonLog.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_BehaviorTreeInspectBlackboard::GetName() const
{
	return TEXT("editor.behaviortree.inspectBlackboard");
}

FString ClaireonTool_BehaviorTreeInspectBlackboard::GetDescription() const
{
	return TEXT("Read a Blackboard Data asset and list all keys with their types, "
				"sync settings, and parent blackboard references.");
}

TSharedPtr<FJsonObject> ClaireonTool_BehaviorTreeInspectBlackboard::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the Blackboard Data asset (e.g. /Game/AI/BB_AI_Default)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for key names and types, 'full' for complete details (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BehaviorTreeInspectBlackboard::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);
	const bool bFullDetail = !DetailLevel.Equals(TEXT("summary"), ESearchCase::IgnoreCase);

	FString LoadError;
	UBlackboardData* BlackboardData = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(AssetPath, LoadError);
	if (!BlackboardData)
	{
		return MakeErrorResult(LoadError);
	}

	const FString AssetName = BlackboardData->GetName();

	// Generate formatted structure text
	FString StructureText = ClaireonBehaviorTreeHelpers::FormatBlackboardData(BlackboardData, bFullDetail);

	// Build structured key list
	TArray<TSharedPtr<FJsonValue>> OwnKeysArray;
	for (const FBlackboardEntry& Entry : BlackboardData->Keys)
	{
		FString TypeName = Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("Unknown");
		TypeName.RemoveFromStart(TEXT("BlackboardKeyType_"));

		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		KeyObj->SetStringField(TEXT("type"), TypeName);
		KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced);
		OwnKeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
	}

	TArray<TSharedPtr<FJsonValue>> InheritedKeysArray;
	for (const FBlackboardEntry& Entry : BlackboardData->ParentKeys)
	{
		FString TypeName = Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("Unknown");
		TypeName.RemoveFromStart(TEXT("BlackboardKeyType_"));

		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		KeyObj->SetStringField(TEXT("type"), TypeName);
		KeyObj->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced);
		InheritedKeysArray.Add(MakeShared<FJsonValueObject>(KeyObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_name"), AssetName);
	Data->SetNumberField(TEXT("own_key_count"), BlackboardData->Keys.Num());
	Data->SetNumberField(TEXT("inherited_key_count"), BlackboardData->ParentKeys.Num());
	Data->SetNumberField(TEXT("total_key_count"), BlackboardData->Keys.Num() + BlackboardData->ParentKeys.Num());
	Data->SetStringField(TEXT("parent_blackboard"), BlackboardData->Parent ? BlackboardData->Parent->GetPathName() : TEXT(""));
	Data->SetArrayField(TEXT("own_keys"), OwnKeysArray);
	Data->SetArrayField(TEXT("inherited_keys"), InheritedKeysArray);
	Data->SetStringField(TEXT("structure"), StructureText);

	const int32 TotalKeys = BlackboardData->Keys.Num() + BlackboardData->ParentKeys.Num();
	const FString Summary = FString::Printf(TEXT("%s: %d keys (%d own, %d inherited)"),
		*AssetName, TotalKeys, BlackboardData->Keys.Num(), BlackboardData->ParentKeys.Num());

	return MakeSuccessResult(Data, Summary);
}
