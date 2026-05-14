// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_ApplySpec::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonSoundCueTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a SoundCue declarative spec ({kind=\"SoundCue\", asset_path, ...}). "
				"Two-pass cross-reference handling preserved (load referenced attenuation/concurrency/sound_class).");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("SoundCue spec object (kind=\"SoundCue\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	const TSharedPtr<FJsonObject>* SpecObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj || !SpecObj->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: spec"));
	}

	FClaireonSpecApplicator_Audio Applicator;
	FString Summary, Error;
	if (!Applicator.ApplySoundCueSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
