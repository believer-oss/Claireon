// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_Niagara.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonNiagaraTool_ApplyDelta::GetCategory() const { return TEXT("niagara"); }
FString FClaireonNiagaraTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonNiagaraTool_ApplyDelta::GetDescription() const
{
	return TEXT("Apply an atomic batch of Niagara User-parameter edits in one transactional call. Execution "
				"order: remove -> create; any phase failure cancels the transaction so nothing lands. No "
				"disconnect/connect phases, and emitters/modules/renderers are not supported yet. Counterpart "
				"to niagara_apply_spec. Pass session_id for an already open niagara session (niagara_open), "
				"or asset_path for a temporary one.");
}

TSharedPtr<FJsonObject> FClaireonNiagaraTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing niagara edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("Niagara System asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: strings or {name|id} objects. Per AR6, accepts both local-ids (resolved via id_map) and literal 'User.<Name>'."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, name, type, value?, kind?}. kind defaults to 'parameter'; anything else returns the L4 emitter-stub error (F1 backlog)."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonNiagaraTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_Niagara Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply Niagara Delta (batch)"));
}
