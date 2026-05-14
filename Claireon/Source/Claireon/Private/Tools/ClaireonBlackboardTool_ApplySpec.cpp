// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Blackboard.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString ClaireonBlackboardTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON specification to a Blackboard Data asset (creates/modifies keys atomically). "
				"Opens its own session when session_id is not provided.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Blackboard Data asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object describing parent_blackboard and keys[]."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse instead of opening a new one."));
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
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

	FClaireonSpecApplicator_Blackboard Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
