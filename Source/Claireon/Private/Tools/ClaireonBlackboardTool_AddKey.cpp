// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_AddKey.h"
#include "Tools/FToolSchemaBuilder.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_AddKey::GetOperation() const { return TEXT("add_key"); }

FString ClaireonBlackboardTool_AddKey::GetDescription() const
{
	return TEXT("Add a new key to the Blackboard within an open editing session. Supported types: "
				"Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum. Requires "
				"session_id from blackboard.open; the edit is transactional and only persists after "
				"save.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_AddKey::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("key_name"), TEXT("Name of the key to add."), true);
	Builder.AddString(TEXT("key_type"), TEXT("Type of the key (Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum)."), true);
	Builder.AddBoolean(TEXT("instance_synced"), TEXT("Whether the key is instance-synced (default false)."));
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_AddKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString KeyType;
	if (!Arguments->TryGetStringField(TEXT("key_type"), KeyType) || KeyType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_type"));
	}

	bool bInstanceSynced = false;
	Arguments->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced);

	UBlackboardData* BB = Data->BlackboardData.Get();

	// Check for duplicate key name
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists"), *KeyName));
		}
	}
	for (const FBlackboardEntry& Entry : BB->ParentKeys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists in parent blackboard"), *KeyName));
		}
	}

	UBlackboardKeyType* NewKeyType = CreateKeyTypeForName(KeyType, BB, Error);
	if (!NewKeyType)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blackboard Key")));
	BB->Modify();

	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(*KeyName);
	NewEntry.KeyType = NewKeyType;
	NewEntry.bInstanceSynced = bInstanceSynced;

	BB->Keys.Add(NewEntry);
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_key - Added '%s' (%s)"), *KeyName, *KeyType);
	return BuildStateResponse(SessionId, Data);
}
