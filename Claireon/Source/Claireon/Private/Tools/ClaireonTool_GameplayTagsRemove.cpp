// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GameplayTagsRemove.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "Misc/App.h"

FString ClaireonTool_GameplayTagsRemove::GetCategory() const { return TEXT("gameplay"); }
FString ClaireonTool_GameplayTagsRemove::GetOperation() const { return TEXT("tags_remove"); }

FString ClaireonTool_GameplayTagsRemove::GetDescription() const
{
	return TEXT("Delete one or more gameplay tags from a configured ini source via IGameplayTagsEditorModule::DeleteTagsFromINI (engine coalesces the in-memory refresh for the whole batch). tag_source is accepted for operator clarity but does not influence the engine call - the source comes from the FGameplayTagNode itself. Returns {removed, failed}.");
}

TSharedPtr<FJsonObject> ClaireonTool_GameplayTagsRemove::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// tags - required array of strings
	{
		TSharedPtr<FJsonObject> TagsProp = MakeShared<FJsonObject>();
		TagsProp->SetStringField(TEXT("type"), TEXT("array"));
		TagsProp->SetStringField(TEXT("description"), TEXT("Tag names to delete (e.g. ['A.B.C', 'X.Y'])."));

		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));
		TagsProp->SetObjectField(TEXT("items"), Items);

		Properties->SetObjectField(TEXT("tags"), TagsProp);
	}

	// tag_source - optional string
	{
		TSharedPtr<FJsonObject> SourceProp = MakeShared<FJsonObject>();
		SourceProp->SetStringField(TEXT("type"), TEXT("string"));
		SourceProp->SetStringField(TEXT("description"), TEXT("Optional ini source filename (e.g. 'DefaultGameplayTags.ini'). Used only for validation context - DeleteTagsFromINI determines the actual source from the tag node."));
		Properties->SetObjectField(TEXT("tag_source"), SourceProp);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("tags")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GameplayTagsRemove::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Reject unknown top-level args.
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("claireon.gameplay_tags_remove requires a 'tags' argument."));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values)
	{
		const FString& Key = Pair.Key;
		if (!Key.Equals(TEXT("tags"), ESearchCase::IgnoreCase)
			&& !Key.Equals(TEXT("tag_source"), ESearchCase::IgnoreCase))
		{
			return MakeErrorResult(FString::Printf(TEXT("Unknown argument: %s"), *Key));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TagsArrayPtr = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("tags"), TagsArrayPtr) || !TagsArrayPtr)
	{
		return MakeErrorResult(TEXT("'tags' is required and must be an array of tag-name strings."));
	}

	// 2. Editor guard.
	if (!GIsEditor || !IGameplayTagsEditorModule::IsAvailable())
	{
		return MakeErrorResult(TEXT("claireon.gameplay_tags_remove requires the editor + GameplayTagsEditor module."));
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

	// 3. Resolve nodes.
	TArray<TSharedPtr<FGameplayTagNode>> ValidNodes;
	TArray<FString> ValidTagStrings;
	TArray<TSharedPtr<FJsonValue>> FailedArr;

	for (const TSharedPtr<FJsonValue>& Entry : *TagsArrayPtr)
	{
		FString Tag;
		if (!Entry.IsValid() || !Entry->TryGetString(Tag) || Tag.IsEmpty())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("reason"), TEXT("unknown_tag"));
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(FName(*Tag));
		if (!Node.IsValid())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("reason"), TEXT("unknown_tag"));
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		ValidNodes.Add(Node);
		ValidTagStrings.Add(Tag);
	}

	// 4. Batch delete - engine coalesces the refresh for the whole batch.
	if (ValidNodes.Num() > 0)
	{
		IGameplayTagsEditorModule::Get().DeleteTagsFromINI(ValidNodes);
	}

	// 5. Build result.
	TArray<TSharedPtr<FJsonValue>> RemovedArr;
	RemovedArr.Reserve(ValidTagStrings.Num());
	for (const FString& T : ValidTagStrings)
	{
		RemovedArr.Add(MakeShared<FJsonValueString>(T));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("removed"), RemovedArr);
	Data->SetArrayField(TEXT("failed"), FailedArr);

	const FString Summary = FString::Printf(
		TEXT("gameplay_tags_remove: %d removed, %d failed"),
		RemovedArr.Num(), FailedArr.Num());

	return MakeSuccessResult(Data, Summary);
}
