// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_RemoveKey.h"
#include "Tools/FToolSchemaBuilder.h"
#include "BehaviorTree/BlackboardData.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_RemoveKey::GetOperation() const { return TEXT("remove_key"); }

FString ClaireonBlackboardTool_RemoveKey::GetDescription() const
{
	return TEXT("Remove a key from the blackboard by name.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_RemoveKey::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("key_name"), TEXT("Name of the key to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_RemoveKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString KeyName;
	if (!Arguments->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_name"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();

	int32 KeyIndex = INDEX_NONE;
	for (int32 i = 0; i < BB->Keys.Num(); ++i)
	{
		if (BB->Keys[i].EntryName == FName(*KeyName))
		{
			KeyIndex = i;
			break;
		}
	}

	if (KeyIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found in this blackboard's own keys"), *KeyName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blackboard Key")));
	BB->Modify();

	BB->Keys.RemoveAt(KeyIndex);
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_key - Removed '%s'"), *KeyName);
	return BuildStateResponse(SessionId, Data);
}
