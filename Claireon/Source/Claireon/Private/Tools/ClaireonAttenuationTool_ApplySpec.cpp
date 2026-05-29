// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAttenuationTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonAttenuationTool_ApplySpec::GetCategory() const { return TEXT("attenuation"); }
FString FClaireonAttenuationTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonAttenuationTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply an Attenuation declarative spec ({kind=\"Attenuation\", asset_path, properties}) "
				"in one shot. Non-session, stateless operation; no session_id required and no editor "
				"session is opened. Creates the target asset if absent, applies properties via "
				"reflection, then saves.");
}

TSharedPtr<FJsonObject> FClaireonAttenuationTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("Attenuation spec object (kind=\"Attenuation\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonAttenuationTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!Applicator.ApplyAttenuationSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
