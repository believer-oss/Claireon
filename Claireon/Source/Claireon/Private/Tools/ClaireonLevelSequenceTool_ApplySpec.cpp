// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_LevelSequence.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_ApplySpec::GetOperation() const { return TEXT("sequence_apply_spec"); }

FString ClaireonLevelSequenceTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON specification to a Level Sequence asset "
				"(creates/modifies bindings, tracks, sections, and keyframes atomically). "
				"Opens its own session when session_id is not provided.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Level Sequence asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object describing playback_range / bindings / tracks / sections / keyframes."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse instead of opening a new one."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	FString SessionId;
	Arguments->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_LevelSequence Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
