// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonSoundMixTool_ApplySpec::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonSoundMixTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a SoundMix declarative spec ({kind=\"SoundMix\", asset_path, envelope, "
				"class_adjusters}) in one shot. Non-session, stateless operation; no session_id "
				"required and no editor session is opened. Creates the target asset if absent, "
				"writes envelope and class-adjuster entries, then saves.");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("SoundMix spec object (kind=\"SoundMix\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!Applicator.ApplySoundMixSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
