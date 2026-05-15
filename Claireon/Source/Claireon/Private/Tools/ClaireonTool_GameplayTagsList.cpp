// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GameplayTagsList.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_GameplayTagsList::GetName() const
{
	return TEXT("claireon.gameplay_tags_list");
}

FString ClaireonTool_GameplayTagsList::GetDescription() const
{
	return TEXT("Enumerate registered gameplay tags via UGameplayTagsManager. Optional case-sensitive prefix filter and include_source toggle for per-tag source attribution.");
}

TSharedPtr<FJsonObject> ClaireonTool_GameplayTagsList::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// prefix - optional
	TSharedPtr<FJsonObject> PrefixProp = MakeShared<FJsonObject>();
	PrefixProp->SetStringField(TEXT("type"), TEXT("string"));
	PrefixProp->SetStringField(TEXT("description"), TEXT("Case-sensitive prefix filter (e.g. \"Ability.\")."));
	Properties->SetObjectField(TEXT("prefix"), PrefixProp);

	// include_source - optional
	TSharedPtr<FJsonObject> IncludeSourceProp = MakeShared<FJsonObject>();
	IncludeSourceProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeSourceProp->SetStringField(TEXT("description"), TEXT("If true, include per-tag source attribution (default: false)."));
	Properties->SetObjectField(TEXT("include_source"), IncludeSourceProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GameplayTagsList::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Reject unknown fields (case-insensitive match against the declared schema)
	if (Arguments.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values)
		{
			const FString& Key = Pair.Key;
			if (!Key.Equals(TEXT("prefix"), ESearchCase::IgnoreCase)
				&& !Key.Equals(TEXT("include_source"), ESearchCase::IgnoreCase))
			{
				return MakeErrorResult(FString::Printf(TEXT("Unknown argument: %s"), *Key));
			}
		}
	}

	FString Prefix;
	const bool bHasPrefix = Arguments.IsValid() && Arguments->TryGetStringField(TEXT("prefix"), Prefix);

	bool bIncludeSource = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetBoolField(TEXT("include_source"), bIncludeSource);
	}

	// Query all tags from the manager
	FGameplayTagContainer OutContainer;
	UGameplayTagsManager::Get().RequestAllGameplayTags(OutContainer, /*OnlyIncludeDictTags=*/false);

	TArray<FGameplayTag> AllTags;
	OutContainer.GetGameplayTagArray(AllTags);

	// Filter by prefix (case-sensitive) if provided
	TArray<FGameplayTag> FilteredTags;
	FilteredTags.Reserve(AllTags.Num());
	for (const FGameplayTag& Tag : AllTags)
	{
		const FString TagStr = Tag.GetTagName().ToString();
		if (bHasPrefix && !TagStr.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			continue;
		}
		FilteredTags.Add(Tag);
	}

	// Sort alphabetically by tag string for deterministic output
	FilteredTags.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.GetTagName().ToString() < B.GetTagName().ToString();
	});

	// Build the tags array
	TArray<TSharedPtr<FJsonValue>> TagsArray;
	TagsArray.Reserve(FilteredTags.Num());

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	for (const FGameplayTag& Tag : FilteredTags)
	{
		const FName TagName = Tag.GetTagName();
		const FString TagStr = TagName.ToString();

		if (bIncludeSource)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), TagStr);

			FString SourceStr = TEXT("Unknown");
			TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(Tag);
			if (Node.IsValid())
			{
				const FName SourceName = Node->GetFirstSourceName();
				if (!SourceName.IsNone())
				{
					SourceStr = SourceName.ToString();
				}
			}
			Entry->SetStringField(TEXT("source"), SourceStr);

			TagsArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
		else
		{
			TagsArray.Add(MakeShared<FJsonValueString>(TagStr));
		}
	}

	const int32 Count = TagsArray.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Count);
	Data->SetArrayField(TEXT("tags"), TagsArray);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Returned %d tags"), Count));
}
