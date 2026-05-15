// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_SetKeyType.h"
#include "Tools/FToolSchemaBuilder.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_SetKeyType::GetName() const
{
	return TEXT("claireon.blackboard_set_key_type");
}

FString ClaireonBlackboardTool_SetKeyType::GetDescription() const
{
	return TEXT("Change the type of an existing blackboard key.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_SetKeyType::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("key_name"), TEXT("Name of the key to modify."), true);
	Builder.AddString(TEXT("key_type"), TEXT("New type (Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum)."), true);
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_SetKeyType::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UBlackboardData* BB = Data->BlackboardData.Get();

	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			FoundEntry = &Entry;
			break;
		}
	}

	if (!FoundEntry)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found"), *KeyName));
	}

	UBlackboardKeyType* NewKeyType = CreateKeyTypeForName(KeyType, BB, Error);
	if (!NewKeyType)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blackboard Key Type")));
	BB->Modify();

	FoundEntry->KeyType = NewKeyType;
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_key_type - '%s' -> %s"), *KeyName, *KeyType);
	return BuildStateResponse(SessionId, Data);
}
