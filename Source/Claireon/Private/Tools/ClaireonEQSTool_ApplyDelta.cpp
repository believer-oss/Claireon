// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_EQS.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonEQSTool_ApplyDelta::GetCategory() const { return TEXT("eqs"); }
FString FClaireonEQSTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonEQSTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch eqs modification. Removes and creates in one transactional call. Counterpart to eqs_apply_spec. Execution order: remove -> create. (No disconnect/connect phases -- options have no inter-option wiring.)");
}

TSharedPtr<FJsonObject> FClaireonEQSTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing eqs edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("EQS query asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: option index strings or {id|name} objects."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, kind: option|test, ...}; option requires generator.type."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonEQSTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_EQS Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply EQS Delta (batch)"));
}
