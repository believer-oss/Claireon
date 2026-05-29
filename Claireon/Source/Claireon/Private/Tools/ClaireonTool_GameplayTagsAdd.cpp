// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GameplayTagsAdd.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

FString ClaireonTool_GameplayTagsAdd::GetCategory() const { return TEXT("gameplay"); }
FString ClaireonTool_GameplayTagsAdd::GetOperation() const { return TEXT("tags_add"); }

FString ClaireonTool_GameplayTagsAdd::GetDescription() const
{
    return TEXT("Add one or more gameplay tags to a configured ini source. Batches via UGameplayTagsManager Suspend/Resume for a single coalesced refresh. Each entry: {tag, dev_comment?}. Optional tag_source defaults to DefaultGameplayTags.ini. Stateless / non-session: writes the ini file directly.");
}

TSharedPtr<FJsonObject> ClaireonTool_GameplayTagsAdd::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// tags - required array of objects
	{
		TSharedPtr<FJsonObject> TagsProp = MakeShared<FJsonObject>();
		TagsProp->SetStringField(TEXT("type"), TEXT("array"));
		TagsProp->SetStringField(TEXT("description"), TEXT("Tags to add. Each entry: {tag: 'A.B.C', dev_comment?: '...'}"));

		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> ItemProps = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> TagField = MakeShared<FJsonObject>();
		TagField->SetStringField(TEXT("type"), TEXT("string"));
		ItemProps->SetObjectField(TEXT("tag"), TagField);
		TSharedPtr<FJsonObject> DevField = MakeShared<FJsonObject>();
		DevField->SetStringField(TEXT("type"), TEXT("string"));
		ItemProps->SetObjectField(TEXT("dev_comment"), DevField);
		Items->SetObjectField(TEXT("properties"), ItemProps);

		TArray<TSharedPtr<FJsonValue>> ItemRequired;
		ItemRequired.Add(MakeShared<FJsonValueString>(TEXT("tag")));
		Items->SetArrayField(TEXT("required"), ItemRequired);

		TagsProp->SetObjectField(TEXT("items"), Items);
		Properties->SetObjectField(TEXT("tags"), TagsProp);
	}

	// tag_source - optional string
	{
		TSharedPtr<FJsonObject> SourceProp = MakeShared<FJsonObject>();
		SourceProp->SetStringField(TEXT("type"), TEXT("string"));
		SourceProp->SetStringField(TEXT("description"), TEXT("Ini source filename (e.g. 'DefaultGameplayTags.ini'). Defaults to FGameplayTagSource::GetDefaultName(). If a path with / or \\ is supplied, the basename is extracted. NOTE: if UGameplayTagsDeveloperSettings.DeveloperConfigName is set in the operator's EditorPerProjectUserSettings.ini, the engine silently redirects a default-source request to that per-user ini; the tool reports the actual source per row and emits a warning when the redirect occurs."));
		Properties->SetObjectField(TEXT("tag_source"), SourceProp);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("tags")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace ClaireonTool_GameplayTagsAdd_Impl
{
	// Discriminator-prefixed to survive unity batching collisions across the module.
	static FName ResolveSourceName(const FString& Arg)
	{
		if (Arg.IsEmpty())
		{
			return FGameplayTagSource::GetDefaultName();
		}

		// If the caller passed a path with separators, extract the basename.
		if (Arg.Contains(TEXT("/")) || Arg.Contains(TEXT("\\")))
		{
			return FName(*FPaths::GetCleanFilename(Arg));
		}
		return FName(*Arg);
	}
}

IClaireonTool::FToolResult ClaireonTool_GameplayTagsAdd::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Reject unknown top-level args.
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("claireon.gameplay_tags_add requires a 'tags' argument."));
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

	// Required field: tags must be present and an array.
	const TArray<TSharedPtr<FJsonValue>>* TagsArrayPtr = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("tags"), TagsArrayPtr) || !TagsArrayPtr)
	{
		return MakeErrorResult(TEXT("'tags' is required and must be an array of {tag, dev_comment?} objects."));
	}

	// 2. Editor guard.
	if (!GIsEditor || !IGameplayTagsEditorModule::IsAvailable())
	{
		return MakeErrorResult(TEXT("claireon.gameplay_tags_add requires the editor + GameplayTagsEditor module."));
	}

	// 3. Resolve source name.
	FString TagSourceArg;
	Arguments->TryGetStringField(TEXT("tag_source"), TagSourceArg);
	const FName ResolvedSourceName = ClaireonTool_GameplayTagsAdd_Impl::ResolveSourceName(TagSourceArg);

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	IGameplayTagsEditorModule& EditorModule = IGameplayTagsEditorModule::Get();

	// 4. Suspend refresh - guarantees a single coalesced rebuild on resume.
	const FGuid SuspendToken = FGuid::NewGuid();
	Manager.SuspendEditorRefreshGameplayTagTree(SuspendToken);

	TArray<TSharedPtr<FJsonValue>> AddedArr;
	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	// 5. Per-entry loop.
	for (const TSharedPtr<FJsonValue>& Entry : *TagsArrayPtr)
	{
		const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(EntryObjPtr) || !EntryObjPtr)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), TEXT(""));
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("reason"), TEXT("invalid_string"));
			Row->SetStringField(TEXT("error_text"), TEXT("Entry was not a JSON object."));
			Row->SetStringField(TEXT("fixed_string"), TEXT(""));
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		const TSharedPtr<FJsonObject>& EntryObj = *EntryObjPtr;

		FString Tag;
		if (!EntryObj->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("reason"), TEXT("invalid_string"));
			Row->SetStringField(TEXT("error_text"), TEXT("Missing or empty 'tag' field."));
			Row->SetStringField(TEXT("fixed_string"), TEXT(""));
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		FString DevComment;
		EntryObj->TryGetStringField(TEXT("dev_comment"), DevComment);

		// 5a. Pre-validate the tag string.
		FText ErrorText;
		FString FixedString;
		if (!Manager.IsValidGameplayTagString(Tag, &ErrorText, &FixedString))
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("reason"), TEXT("invalid_string"));
			Row->SetStringField(TEXT("error_text"), ErrorText.ToString());
			Row->SetStringField(TEXT("fixed_string"), FixedString);
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		// 5b. Pre-check duplicate.
		if (Manager.IsDictionaryTag(FName(*Tag)))
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("status"), TEXT("skipped"));
			Row->SetStringField(TEXT("reason"), TEXT("already_exists"));
			SkippedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		// 5c. Engine call.
		const bool bAddOk = EditorModule.AddNewGameplayTagToINI(Tag, DevComment, ResolvedSourceName, /*bIsRestrictedTag*/false, /*bAllowNonRestrictedChildren*/true);
		if (!bAddOk)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("tag"), Tag);
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("reason"), Manager.ShouldImportTagsFromINI() ? TEXT("unknown") : TEXT("manager_rejected"));
			FailedArr.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		// 5e. Resolve actual source via FindTagNode.
		FName ActualSourceName = ResolvedSourceName;
		TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(FName(*Tag));
		if (Node.IsValid())
		{
			const FName NodeSource = Node->GetFirstSourceName();
			if (!NodeSource.IsNone())
			{
				ActualSourceName = NodeSource;
			}
		}

		if (ActualSourceName != ResolvedSourceName)
		{
			WarningsArr.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Requested source '%s' was overridden to '%s' by UGameplayTagsDeveloperSettings.DeveloperConfigName."),
				*ResolvedSourceName.ToString(),
				*ActualSourceName.ToString())));
		}

		// 5f. Record success row.
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("tag"), Tag);
		Row->SetStringField(TEXT("status"), TEXT("added"));
		Row->SetStringField(TEXT("source"), ActualSourceName.ToString());
		Row->SetStringField(TEXT("dev_comment"), DevComment);
		AddedArr.Add(MakeShared<FJsonValueObject>(Row));
	}

	// 6. Resume - fires a single coalesced rebuild if any AddNewGameplayTagToINI call requested one.
	Manager.ResumeEditorRefreshGameplayTagTree(SuspendToken);

	// 7. Build result.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("added"), AddedArr);
	Data->SetArrayField(TEXT("skipped"), SkippedArr);
	Data->SetArrayField(TEXT("failed"), FailedArr);
	Data->SetStringField(TEXT("requested_source"), ResolvedSourceName.ToString());
	Data->SetArrayField(TEXT("warnings"), WarningsArr);

	const FString Summary = FString::Printf(
		TEXT("gameplay_tags_add: %d added, %d skipped, %d failed (source=%s)"),
		AddedArr.Num(), SkippedArr.Num(), FailedArr.Num(), *ResolvedSourceName.ToString());

	return MakeSuccessResult(Data, Summary);
}
