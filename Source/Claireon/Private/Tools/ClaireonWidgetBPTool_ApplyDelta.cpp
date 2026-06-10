// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_WidgetBP.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonWidgetBPTool_ApplyDelta::GetCategory() const { return TEXT("widgetbp"); }
FString FClaireonWidgetBPTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonWidgetBPTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch widgetbp modification. Removes, creates, and reparents widgets in one transactional call. Counterpart to widgetbp_apply_spec. Execution order: remove -> create -> reparent. (No disconnect phase -- widgets always have exactly one parent.)");
}

TSharedPtr<FJsonObject> FClaireonWidgetBPTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing widgetbp edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("Widget Blueprint asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: widget names (strings) or {name|id} objects."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {id, type, parent_id?, name?, slot_properties?{}}. type is widget class (short or path)."));
	S.AddObject(TEXT("connections"), TEXT("Phase 4 entries (reparent, D5): {widget_id, new_parent_id, new_parent_slot_id?}. Delegates to ClaireonWidgetHelpers::MoveWidget."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonWidgetBPTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_WidgetBP Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply WidgetBP Delta (batch)"));
}
