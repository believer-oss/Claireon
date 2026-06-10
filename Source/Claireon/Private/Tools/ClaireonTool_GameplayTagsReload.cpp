// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GameplayTagsReload.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_GameplayTagsReload::GetCategory() const { return TEXT("gameplay"); }
FString ClaireonTool_GameplayTagsReload::GetOperation() const { return TEXT("tags_reload"); }

FString ClaireonTool_GameplayTagsReload::GetDescription() const
{
    return TEXT("Refresh the in-memory gameplay tag tree by reloading DefaultGameplayTags.ini from disk via GConfig->LoadFile and forcing the engine through EditorRefreshGameplayTagTree. Use after an out-of-band file-system edit. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_GameplayTagsReload::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GameplayTagsReload::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Reject unknown top-level args.
	if (Arguments.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values)
		{
			return MakeErrorResult(FString::Printf(TEXT("Unknown argument: %s"), *Pair.Key));
		}
	}

	// 2. Editor guard.
	if (!GIsEditor)
	{
		return MakeErrorResult(TEXT("claireon.gameplay_tags_reload requires an editor context."));
	}

	// 3-5. Read settings, force a disk re-read of the default config, then rebuild the tree.
	UGameplayTagsSettings* Settings = GetMutableDefault<UGameplayTagsSettings>();
	if (!Settings)
	{
		return MakeErrorResult(TEXT("Failed to resolve UGameplayTagsSettings default object."));
	}

	const FString DefaultConfigFilename = Settings->GetDefaultConfigFilename();
	if (GConfig)
	{
		GConfig->LoadFile(DefaultConfigFilename);
	}
	UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();

	// 6. Build result.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("refreshed"), true);
	Data->SetStringField(TEXT("source"), DefaultConfigFilename);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Reloaded gameplay tag tree from %s"), *DefaultConfigFilename));
}
