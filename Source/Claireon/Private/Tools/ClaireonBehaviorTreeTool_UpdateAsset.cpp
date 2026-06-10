// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_UpdateAsset.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_UpdateAsset::GetOperation() const { return TEXT("update_asset"); }

FString ClaireonBehaviorTreeTool_UpdateAsset::GetDescription() const
{
	return TEXT("Compile the Behavior Tree by rebuilding the runtime tree from the EdGraph within the "
				"current session (BT equivalent of compile). Requires session_id from behavior_tree.open; "
				"the session stays open so subsequent transactional edits and the eventual save observe "
				"the rebuilt structure.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_UpdateAsset::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_UpdateAsset::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBehaviorTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	Graph->UpdateAsset();

	Data->LastOperationStatus = TEXT("update_asset - Rebuilt runtime BT from graph");
	return BuildStateResponse(SessionId, Data);
}
