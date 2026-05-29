// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_Material.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonMaterialTool_ApplyDelta::GetCategory() const { return TEXT("material"); }
FString FClaireonMaterialTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonMaterialTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch material modification. Disconnects, removes, creates, and connects in one transactional call. Counterpart to material_apply_spec. Execution order: disconnect -> remove -> create -> connect. (Note: applies to UMaterial only; UMaterialInstanceConstant uses material_instance_* discrete tools.)");
}

TSharedPtr<FJsonObject> FClaireonMaterialTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing material edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("UMaterial asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("disconnect"), TEXT("Phase 1 entries: {from_expr_id?, from_output?, to_expr_id, to_input}. Disconnect ToInput on ToExpr."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: expression ids (strings) or {id|name} objects."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, type, x?, y?, properties?{}}. type is expression class (short or full path)."));
	S.AddObject(TEXT("connections"), TEXT("Phase 4 entries: {from, from_output?, to, to_input?} OR {from, from_output?, attribute}. Dispatch on presence of 'attribute' field."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMaterialTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_Material Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply Material Delta (batch)"));
}
