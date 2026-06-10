// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_RenameKey.h"
#include "Tools/FToolSchemaBuilder.h"
#include "BehaviorTree/BlackboardData.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_RenameKey::GetOperation() const { return TEXT("rename_key"); }

FString ClaireonBlackboardTool_RenameKey::GetDescription() const
{
	return TEXT("Rename an existing Blackboard key within an open editing session. Requires session_id "
				"from blackboard.open; the edit is transactional and only persists after save. "
				"Downstream BT references are not automatically rewritten.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_RenameKey::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("old_name"), TEXT("Current name of the key."), true);
	Builder.AddString(TEXT("new_name"), TEXT("New name for the key."), true);
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_RenameKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString OldName;
	if (!Arguments->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: old_name"));
	}

	FString NewName;
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();

	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*OldName))
		{
			FoundEntry = &Entry;
			break;
		}
	}

	if (!FoundEntry)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found"), *OldName));
	}

	// Check new name doesn't conflict
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*NewName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists"), *NewName));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Blackboard Key")));
	BB->Modify();

	FoundEntry->EntryName = FName(*NewName);
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("rename_key - '%s' -> '%s'"), *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}
