// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonConcurrencyTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonConcurrencyTool_ApplySpec::GetCategory() const { return TEXT("concurrency"); }
FString FClaireonConcurrencyTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonConcurrencyTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a Concurrency declarative spec ({kind=\"Concurrency\", asset_path, properties}). "
				"Creates the asset if absent, applies properties via reflection.");
}

TSharedPtr<FJsonObject> FClaireonConcurrencyTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("Concurrency spec object (kind=\"Concurrency\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonConcurrencyTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	const TSharedPtr<FJsonObject>* SpecObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj || !SpecObj->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: spec"));
	}

	FClaireonSpecApplicator_Audio Applicator;
	FString Summary, Error;
	if (!Applicator.ApplyConcurrencySpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
