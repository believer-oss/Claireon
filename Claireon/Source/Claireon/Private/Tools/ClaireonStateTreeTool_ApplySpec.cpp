// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_StateTree.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_ApplySpec::GetName() const
{
	return TEXT("claireon.statetree_apply_spec");
}

FString ClaireonStateTreeTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a declarative JSON specification to a State Tree asset atomically. Requires open session_id from claireon.statetree_open OR pass asset_path to auto-open a temporary session. Transactional. The spec runs as one rollback unit; partial failures revert all spec operations together. Saves on success.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the State Tree asset to apply the spec to."), true);
	Builder.AddObject(TEXT("spec"), TEXT("Declarative spec object."), true);
	Builder.AddString(TEXT("session_id"), TEXT("Optional existing session to reuse."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FClaireonSpecApplicator_StateTree Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
