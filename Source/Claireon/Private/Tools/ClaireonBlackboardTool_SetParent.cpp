// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_SetParent.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BlackboardData.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_SetParent::GetOperation() const { return TEXT("set_parent"); }

FString ClaireonBlackboardTool_SetParent::GetDescription() const
{
	return TEXT("Set or clear the parent Blackboard for key inheritance within an open editing "
				"session. Pass empty parent_path to clear. Requires session_id from blackboard.open; "
				"the edit is transactional and only persists after save.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_SetParent::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parent_path"), TEXT("Path to the parent Blackboard Data asset. Empty to clear parent."));
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_SetParent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParentPath;
	Arguments->TryGetStringField(TEXT("parent_path"), ParentPath);

	UBlackboardData* BB = Data->BlackboardData.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blackboard Parent")));
	BB->Modify();

	if (ParentPath.IsEmpty())
	{
		BB->Parent = nullptr;
		BB->UpdateKeyIDs();
		BB->MarkPackageDirty();

		Data->LastOperationStatus = TEXT("set_parent - Cleared parent");
		return BuildStateResponse(SessionId, Data);
	}

	UBlackboardData* ParentBB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(ParentPath, Error);
	if (!ParentBB)
	{
		return MakeErrorResult(Error);
	}

	if (ParentBB == BB)
	{
		return MakeErrorResult(TEXT("Cannot set a blackboard as its own parent"));
	}

	BB->Parent = ParentBB;
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_parent - Set parent to %s"), *ParentPath);
	return BuildStateResponse(SessionId, Data);
}
