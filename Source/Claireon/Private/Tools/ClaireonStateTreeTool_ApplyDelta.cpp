// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_StateTree.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonStateTreeTool_ApplyDelta::GetCategory() const { return TEXT("statetree"); }
FString FClaireonStateTreeTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonStateTreeTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch statetree modification. Disconnects, removes, creates, and connects in one transactional call. Counterpart to statetree_apply_spec. Execution order: disconnect -> remove -> create -> connect. (Accepts top-level transitions[] OR phase-4 connections[]; both are unioned and deduped.)");
}

TSharedPtr<FJsonObject> FClaireonStateTreeTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing statetree edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("StateTree asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("disconnect"), TEXT("Phase 1 entries: transition id strings (removes named transitions)."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: {kind: state|evaluator|global_task, id}. 'transition' is rejected here -- use disconnect[]."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: kind-tagged union {kind, id, ...kind-specific fields}."));
	S.AddObject(TEXT("connections"), TEXT("Phase 4 entries: transitions {id?, from_state, to_state, trigger?}."));
	S.AddObject(TEXT("transitions"), TEXT("AR4 alternative-shape: top-level transitions[] is unioned with phase-4 connections[]; top-level wins on tie."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonStateTreeTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_StateTree Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply StateTree Delta (batch)"));
}
