// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_PCGGraph.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_ApplySpec::GetName() const
{
	return TEXT("claireon.pcg_apply_spec");
}

FString ClaireonPCGGraphTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON specification to a PCG Graph asset (creates nodes and edges atomically). "
				"Opens its own session when session_id is not provided.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the PCG Graph asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object describing nodes[] and connections[]."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse instead of opening a new one."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FClaireonSpecApplicator_PCGGraph Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
