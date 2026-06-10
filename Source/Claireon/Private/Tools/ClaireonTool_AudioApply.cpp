// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AudioApply.h"
#include "Tools/ClaireonAudioTool_PlaceAmbientSound.h"
#include "Tools/ClaireonAudioTool_PlaceAudioVolume.h"
#include "Tools/ClaireonAudioTool_AttachAudioComponent.h"
#include "Tools/ClaireonAudioTool_SetAudioProperty.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonTool_AudioApply::GetCategory() const { return TEXT("audio"); }
FString FClaireonTool_AudioApply::GetOperation() const { return TEXT("apply"); }

FString FClaireonTool_AudioApply::GetDescription() const
{
	return TEXT("DEPRECATED: dispatches on 'operation'. Use the per-op tools instead: "
	            "audio_place_ambient_sound, audio_place_audio_volume, audio_attach_audio_component, audio_set_audio_property. "
	            "RequiresNoPIE AND RequiresEditorWorld.");
}

TSharedPtr<FJsonObject> FClaireonTool_AudioApply::GetInputSchema() const
{
	// Preserved schema for backwards-compatible callers; new callers should target the per-op tools directly.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"),
		TEXT("Operation: 'place_ambient_sound', 'place_audio_volume', 'attach_audio_component', or 'set_audio_property'."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumVals;
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("place_ambient_sound")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("place_audio_volume")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("attach_audio_component")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("set_audio_property")));
		OpProp->SetArrayField(TEXT("enum"), EnumVals);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// Per-op fields kept loose-typed for backwards compatibility.
	for (const TCHAR* Field : { TEXT("sound_asset_path"), TEXT("actor_name"), TEXT("component_path"),
								TEXT("field_name"), TEXT("component_name"), TEXT("label") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		Properties->SetObjectField(Field, P);
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("boolean"));
		Properties->SetObjectField(TEXT("auto_activate"), P);
	}
	for (const TCHAR* Field : { TEXT("transform"), TEXT("properties") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("object"));
		Properties->SetObjectField(Field, P);
	}
	{
		// value -- any type
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("value"), P);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FClaireonTool_AudioApply::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	if (Operation == TEXT("place_ambient_sound"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[audio_apply] DEPRECATED: forward this call to 'audio_place_ambient_sound' (per-op tool). "
			     "The dispatcher will be removed in a future release."));
		FClaireonAudioTool_PlaceAmbientSound Tool;
		return Tool.Execute(Arguments);
	}
	if (Operation == TEXT("place_audio_volume"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[audio_apply] DEPRECATED: forward this call to 'audio_place_audio_volume' (per-op tool). "
			     "The dispatcher will be removed in a future release."));
		FClaireonAudioTool_PlaceAudioVolume Tool;
		return Tool.Execute(Arguments);
	}
	if (Operation == TEXT("attach_audio_component"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[audio_apply] DEPRECATED: forward this call to 'audio_attach_audio_component' (per-op tool). "
			     "The dispatcher will be removed in a future release."));
		FClaireonAudioTool_AttachAudioComponent Tool;
		return Tool.Execute(Arguments);
	}
	if (Operation == TEXT("set_audio_property"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[audio_apply] DEPRECATED: forward this call to 'audio_set_audio_property' (per-op tool). "
			     "The dispatcher will be removed in a future release."));
		FClaireonAudioTool_SetAudioProperty Tool;
		return Tool.Execute(Arguments);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation '%s'"), *Operation));
}
