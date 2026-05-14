// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Material.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString ClaireonMaterialTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON spec to a UMaterial (expressions, connections, attribute_connections, parameter_defaults, shading_model, blend_mode). Opens its own session if session_id is not provided.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Material asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FClaireonSpecApplicator_Material Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
