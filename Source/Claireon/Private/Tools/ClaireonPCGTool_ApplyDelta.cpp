// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_PCGGraph.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonPCGTool_ApplyDelta::GetCategory() const { return TEXT("pcg"); }
FString FClaireonPCGTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonPCGTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch pcg modification. Disconnects, removes, creates, and connects in one transactional call. Counterpart to pcg_apply_spec. Execution order: disconnect -> remove -> create -> connect. (Pin names are exact-match -- no fuzzy resolution.)");
}

TSharedPtr<FJsonObject> FClaireonPCGTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing pcg edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("PCG Graph asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("disconnect"), TEXT("Phase 1 entries: {source_node, source_pin, target_node, target_pin} (exact-match pin names)."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: node id strings or {id|name} objects."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, type, properties?}."));
	S.AddObject(TEXT("connections"), TEXT("Phase 4 entries: {source_node, source_pin, target_node, target_pin} (exact-match)."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonPCGTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_PCGGraph Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply PCG Delta (batch)"));
}
