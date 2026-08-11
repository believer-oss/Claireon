// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_ApplyDelta.h"
#include "Tools/FClaireonDeltaApplicator_LevelSequence.h"
#include "Tools/FToolSchemaBuilder.h"

FString FClaireonLevelSequenceTool_ApplyDelta::GetCategory() const { return TEXT("level_sequence"); }
FString FClaireonLevelSequenceTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString FClaireonLevelSequenceTool_ApplyDelta::GetDescription() const
{
	return TEXT("Apply a batch of level_sequence removals and creations in one atomic transactional call; "
	            "counterpart to level_sequence_apply_spec. Order: remove -> create (no disconnect/connect "
	            "phases). remove_nodes entries use composite identity binding_label+track_name+row_index+"
	            "start_frame; omitted fields select the parent. Pass session_id, or asset_path to open a "
	            "temporary session.");
}

TSharedPtr<FJsonObject> FClaireonLevelSequenceTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing level_sequence edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("LevelSequence asset path; opens a temporary session if no session_id is provided. Fail-on-missing."));
	S.AddObject(TEXT("remove_nodes"), TEXT("Phase 2 entries: composite-id refs {binding_label, track_name?, row_index?, start_frame?}. Sorted DEEPEST-FIRST before applying (M4)."));
	S.AddObject(TEXT("nodes"), TEXT("Phase 3 entries: {kind: \"binding\", id, label, object_class}."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonLevelSequenceTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonDeltaApplicator_LevelSequence Applicator;
	return Applicator.ApplyDelta(Arguments, TEXT("[Claireon] Apply LevelSequence Delta (batch)"));
}
