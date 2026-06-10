// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundClassTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonSoundClassTool_ApplySpec::GetCategory() const { return TEXT("soundclass"); }
FString FClaireonSoundClassTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonSoundClassTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a SoundClass declarative spec ({kind=\"SoundClass\", asset_path, properties, "
				"child_classes}) in one shot. Non-session, stateless operation; no session_id required "
				"and no editor session is opened. Creates the target asset if absent, applies "
				"properties via reflection, then saves.");
}

TSharedPtr<FJsonObject> FClaireonSoundClassTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("SoundClass spec object (kind=\"SoundClass\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundClassTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!Applicator.ApplySoundClassSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
