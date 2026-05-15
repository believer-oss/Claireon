// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_UpdateAsset.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_UpdateAsset::GetName() const
{
	return TEXT("claireon.behaviortree_update_asset");
}

FString ClaireonBehaviorTreeTool_UpdateAsset::GetDescription() const
{
	return TEXT("Rebuild the runtime Behavior Tree from the graph (BT equivalent of compile).");
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
