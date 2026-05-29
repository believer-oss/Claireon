// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_BehaviorTree.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonBehaviorTreeTool_ApplyDelta::GetCategory() const { return TEXT("behaviortree"); }
FString FClaireonBehaviorTreeTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonBehaviorTreeTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch behaviortree modification. Disconnects, removes, creates, and connects in one transactional call. Counterpart to behaviortree_apply_spec. Execution order: disconnect -> remove -> create -> connect.");
}

TSharedPtr<FJsonObject> FClaireonBehaviorTreeTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing behaviortree edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("BehaviorTree asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("disconnect"), TEXT("Phase 1 entries: {parent_id, child_id} detaching child from parent."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: node id strings or {id|name} objects."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, class, parent_id?|parent_local_id?, properties?}."));
	S.AddObject(TEXT("connections"), TEXT("Phase 4 entries: {parent_id, child_id, child_index?}."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonBehaviorTreeTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_BehaviorTree Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply BehaviorTree Delta (batch)"));
}
