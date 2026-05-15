// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveBinding.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveBinding::GetName() const
{
	return TEXT("claireon.statetree_remove_binding");
}

FString ClaireonStateTreeTool_RemoveBinding::GetDescription() const
{
	return TEXT("Remove all property bindings to a target property.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveBinding::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("target_node_id"), TEXT("GUID of the target node."), true);
	Builder.AddString(TEXT("target_property"), TEXT("Target property path."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid TargetNodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString TargetProperty;
	if (!Arguments->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing parameter: target_property"));

#if WITH_EDITORONLY_DATA
	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Property Binding")));
	Data->StateTree->Modify();
	EditorData->EditorBindings.RemovePropertyBindings(TargetPath);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_binding -> Removed binding to %s"), *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("remove_binding -> Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}
