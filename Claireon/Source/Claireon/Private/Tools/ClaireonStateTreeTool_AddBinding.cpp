// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddBinding.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddBinding::GetOperation() const { return TEXT("add_binding"); }

FString ClaireonStateTreeTool_AddBinding::GetDescription() const
{
	return TEXT("Add a property binding between a source node's output and a target node's input in the open State Tree editing session. Requires open session_id from statetree_open. Transactional. Common pitfall: source and target property types must match (or be implicitly convertible); incompatible types error and the transaction rolls back.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddBinding::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("source_node_id"), TEXT("GUID of the source node."), true);
	Builder.AddString(TEXT("target_node_id"), TEXT("GUID of the target node."), true);
	Builder.AddString(TEXT("source_property"), TEXT("Source property path."), true);
	Builder.AddString(TEXT("target_property"), TEXT("Target property path."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeEditInternal::GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid SourceNodeId, TargetNodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("source_node_id"), SourceNodeId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString SourceProperty, TargetProperty;
	if (!Arguments->TryGetStringField(TEXT("source_property"), SourceProperty))
		return MakeErrorResult(TEXT("Missing parameter: source_property"));
	if (!Arguments->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing parameter: target_property"));

#if WITH_EDITORONLY_DATA
	FStateTreePropertyPath SourcePath(SourceNodeId);
	SourcePath.FromString(SourceProperty);

	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Property Binding")));
	Data->StateTree->Modify();

	FStateTreePropertyPathBinding Binding(SourcePath, TargetPath);
	EditorData->EditorBindings.AddPropertyBinding(Binding);

	Data->LastOperationStatus = FString::Printf(TEXT("add_binding -> Bound %s -> %s"), *SourceProperty, *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("add_binding -> Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}
