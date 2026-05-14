// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonMetaSoundTool_ApplySpec::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonMetaSoundTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a MetaSoundSource declarative spec ({kind=\"MetaSoundSource\", asset_path, ...}). "
				"Creates or locates the asset.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("MetaSoundSource spec object (kind=\"MetaSoundSource\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	const TSharedPtr<FJsonObject>* SpecObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj || !SpecObj->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: spec"));
	}

	FClaireonSpecApplicator_Audio Applicator;
	FString Summary, Error;
	if (!Applicator.ApplyMetaSoundSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
