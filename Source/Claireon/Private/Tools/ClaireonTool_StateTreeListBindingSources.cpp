// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeListBindingSources.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreePropertyBindings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_StateTreeListBindingSources::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeListBindingSources::GetOperation() const { return TEXT("list_binding_sources"); }

FString ClaireonTool_StateTreeListBindingSources::GetDescription() const
{
    return TEXT("List binding sources for a State Tree asset. Walks UStateTreeEditorData::VisitGlobalNodes for tree-wide sources (Context, TreeParameters, GlobalTask, Evaluator). When state_id is provided, also walks VisitStateNodes(state_id) which includes ancestor-chain parameters/tasks plus the StateEvent record. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeListBindingSources::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the State Tree."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> StateIdProp = MakeShared<FJsonObject>();
	StateIdProp->SetStringField(TEXT("type"), TEXT("string"));
	StateIdProp->SetStringField(TEXT("description"), TEXT("Optional FGuid of a state. When provided, the response additionally includes state-scoped sources walked via VisitStateNodes (ancestor chain, state parameters, tasks, and StateEvent if bHasRequiredEventToEnter)."));
	Properties->SetObjectField(TEXT("state_id"), StateIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_StateTreeListBindingSources::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString StateIdStr;
	const bool bHasStateId = Arguments->TryGetStringField(TEXT("state_id"), StateIdStr) && !StateIdStr.IsEmpty();
	FGuid StateGuid;
	if (bHasStateId && !FGuid::Parse(StateIdStr, StateGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid state_id format: %s"), *StateIdStr));
	}

	FString Error;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, Error);
	if (!StateTree)
	{
		return MakeErrorResult(Error);
	}
	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(StateTree, Error);
	if (!EditorData)
	{
		return MakeErrorResult(Error);
	}

	TArray<TSharedPtr<FJsonValue>> SourceArray;

	EditorData->VisitGlobalNodes(
		[&SourceArray](const UStateTreeState* /*State*/,
					   const FStateTreeBindableStructDesc& Desc,
					   const FStateTreeDataView /*Value*/) -> EStateTreeVisitor
		{
			SourceArray.Add(MakeShared<FJsonValueObject>(
				ClaireonStateTreeHelpers::EmitBindingSourceRecord(Desc)));
			return EStateTreeVisitor::Continue;
		});

	if (bHasStateId)
	{
		UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateGuid);
		if (!State)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("State '%s' not found in tree '%s'"), *StateIdStr, *AssetPath));
		}

		EditorData->VisitStateNodes(*State,
			[&SourceArray](const UStateTreeState* /*State*/,
						   const FStateTreeBindableStructDesc& Desc,
						   const FStateTreeDataView /*Value*/) -> EStateTreeVisitor
			{
				SourceArray.Add(MakeShared<FJsonValueObject>(
					ClaireonStateTreeHelpers::EmitBindingSourceRecord(Desc)));
				return EStateTreeVisitor::Continue;
			});
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	if (bHasStateId)
	{
		Data->SetStringField(TEXT("state_id"), StateIdStr);
	}
	else
	{
		Data->SetField(TEXT("state_id"), MakeShared<FJsonValueNull>());
	}
	Data->SetArrayField(TEXT("sources"), SourceArray);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Listed %d binding sources."), SourceArray.Num()));
}
