// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_AddNode.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_AddNode::GetOperation() const { return TEXT("add_node"); }

FString ClaireonPCGGraphTool_AddNode::GetDescription() const
{
	return TEXT("Add a new node to the PCG graph using a settings class name. Focuses on the new node and returns the updated graph state.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("settings_class"), TEXT("Name of the UPCGSettings class to instantiate (e.g. 'PCGSurfaceSamplerSettings')."), true);
	Builder.AddString(TEXT("node_title"), TEXT("Optional title to assign to the new node."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString SettingsClassName;
	if (!Arguments->TryGetStringField(TEXT("settings_class"), SettingsClassName) || SettingsClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: settings_class"));
	}

	UClass* SettingsClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(SettingsClassName, Error);
	if (!SettingsClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add PCG Node")));

	UPCGSettings* DefaultSettings = nullptr;
	UPCGNode* NewNode = Data->PCGGraph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to add node of type: %s"), *SettingsClassName));
	}

	// Set optional title
	FString NodeTitle;
	if (Arguments->TryGetStringField(TEXT("node_title"), NodeTitle) && !NodeTitle.IsEmpty())
	{
		NewNode->NodeTitle = *NodeTitle;
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	int32 NewIndex = Data->PCGGraph->GetNodes().IndexOfByKey(NewNode);
	Data->LastOperationStatus = FString::Printf(TEXT("Added node [%d] %s (%s)"),
		NewIndex, *ClaireonPCGGraphHelpers::GetNodeDisplayName(NewNode), *SettingsClass->GetName());

	// Focus on the new node
	Data->PushHistory();
	Data->FocusedNodeIndex = NewIndex;

	return BuildStateResponse(SessionId, Data);
}
