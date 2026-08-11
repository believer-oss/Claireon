// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AudioApply.h"
#include "Tools/ClaireonAudioTool_PlaceAmbientSound.h"
#include "Tools/ClaireonAudioTool_PlaceAudioVolume.h"
#include "Tools/ClaireonAudioTool_AttachAudioComponent.h"
#include "Tools/ClaireonAudioTool_SetAudioProperty.h"
#include "Tools/ClaireonAudioApplyHelpers.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonTool_AudioApply::GetCategory() const { return TEXT("audio"); }
FString FClaireonTool_AudioApply::GetOperation() const { return TEXT("apply"); }

FString FClaireonTool_AudioApply::GetDescription() const
{
	return TEXT("Apply an audio edit by dispatching on the 'operation' field. DEPRECATED: use the per-op "
	            "tools instead -- audio_place_ambient_sound, audio_place_audio_volume, "
	            "audio_attach_audio_component, audio_set_audio_property. Stateless / non-session: forwards "
	            "to the per-op tool, which edits the current editor world directly. Requires an editor "
	            "world and refuses to run during PIE.");
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

	// Per-op fields kept loose-typed for backwards compatibility. Each still
	// carries a description: an undescribed parameter is invisible in help and
	// in the MCP schema, so a caller can only discover it by reading the source.
	ClaireonAudioSchema::AddString(Properties, TEXT("sound_asset_path"),
		TEXT("Sound asset to place or attach. Used by place_ambient_sound and attach_audio_component."));
	ClaireonAudioSchema::AddString(Properties, TEXT("actor_name"),
		TEXT("Target actor, by label or name. Used by attach_audio_component and set_audio_property."));
	ClaireonAudioSchema::AddString(Properties, TEXT("component_path"),
		TEXT("Path to the component to write. Used by set_audio_property."));
	ClaireonAudioSchema::AddString(Properties, TEXT("field_name"),
		TEXT("Property name to write on the resolved component. Used by set_audio_property."));
	ClaireonAudioSchema::AddString(Properties, TEXT("component_name"),
		TEXT("Name for the newly attached component. Used by attach_audio_component."));
	ClaireonAudioSchema::AddString(Properties, TEXT("label"),
		TEXT("Actor label for the placed actor. Used by place_ambient_sound and place_audio_volume."));
	ClaireonAudioSchema::AddBoolean(Properties, TEXT("auto_activate"),
		TEXT("Whether the created audio component auto-activates. Used by place_ambient_sound and attach_audio_component."));
	ClaireonAudioSchema::AddObject(Properties, TEXT("transform"),
		TEXT("Placement transform {location, rotation, scale}. Used by place_ambient_sound and place_audio_volume."));
	ClaireonAudioSchema::AddObject(Properties, TEXT("properties"),
		TEXT("Property name/value map applied to the placed actor. Used by place_audio_volume."));
	ClaireonAudioSchema::AddAnyType(Properties, TEXT("value"),
		TEXT("New value for field_name. Accepts whatever JSON type the target property takes. Used by set_audio_property."));

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
