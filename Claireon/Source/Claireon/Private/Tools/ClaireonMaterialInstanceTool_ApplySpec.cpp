// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_MaterialInstance.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_ApplySpec::GetOperation() const { return TEXT("instance_apply_spec"); }

FString ClaireonMaterialInstanceTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON spec to a UMaterialInstanceConstant (parent, parameter overrides, clears). Opens its own session if session_id is not provided.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the MaterialInstance asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object describing parent/overrides/clears."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse instead of opening a new one."));
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FClaireonSpecApplicator_MaterialInstance Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
