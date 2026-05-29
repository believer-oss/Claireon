// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialApply.h"
#include "Tools/ClaireonMaterialTool_ApplyToActor.h"
#include "Tools/ClaireonMaterialTool_ApplyToBlueprint.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_MaterialApply::GetCategory() const { return TEXT("material"); }
FString ClaireonTool_MaterialApply::GetOperation() const { return TEXT("apply"); }

FString ClaireonTool_MaterialApply::GetDescription() const
{
	return TEXT("DEPRECATED: dispatches on target.kind. Use the per-kind tools instead: "
	            "material_apply_to_actor, material_apply_to_blueprint. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialApply::GetInputSchema() const
{
	// Preserved schema for backwards-compatible callers; new callers should target the per-kind tools directly.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> MaterialPathProp = MakeShared<FJsonObject>();
	MaterialPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MaterialPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the UMaterialInterface to apply (UMaterial or UMaterialInstanceConstant)."));
	Properties->SetObjectField(TEXT("material_path"), MaterialPathProp);

	TSharedPtr<FJsonObject> TargetProp = MakeShared<FJsonObject>();
	TargetProp->SetStringField(TEXT("type"), TEXT("object"));
	TargetProp->SetStringField(TEXT("description"),
		TEXT("Target descriptor: { kind: 'actor'|'blueprint', actor_name?, blueprint_path?, component_name, element_index }. element_index = -1 applies to every slot."));
	Properties->SetObjectField(TEXT("target"), TargetProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("material_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("target")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_MaterialApply::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("target"), TargetObjPtr) || !TargetObjPtr || !TargetObjPtr->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required field: target (object). DEPRECATED: prefer material_apply_to_actor / material_apply_to_blueprint."));
	}
	const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;

	FString Kind, ComponentName;
	if (!TargetObj->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		return MakeErrorResult(TEXT("target.kind is required ('actor' or 'blueprint')"));
	}
	if (!TargetObj->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return MakeErrorResult(TEXT("target.component_name is required"));
	}

	// Build per-kind arguments and forward.
	TSharedPtr<FJsonObject> ForwardedArgs = MakeShared<FJsonObject>();
	ForwardedArgs->SetStringField(TEXT("material_path"), Arguments->GetStringField(TEXT("material_path")));
	ForwardedArgs->SetStringField(TEXT("component_name"), ComponentName);
	{
		double IndexDouble = 0.0;
		TargetObj->TryGetNumberField(TEXT("element_index"), IndexDouble);
		ForwardedArgs->SetNumberField(TEXT("element_index"), IndexDouble);
	}

	if (Kind.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
	{
		FString ActorName;
		if (!TargetObj->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
		{
			return MakeErrorResult(TEXT("target.actor_name is required when kind == 'actor'"));
		}
		ForwardedArgs->SetStringField(TEXT("actor_name"), ActorName);

		UE_LOG(LogClaireon, Warning,
			TEXT("[material_apply] DEPRECATED: forward this call to 'material_apply_to_actor' (per-kind tool). "
			     "The dispatcher will be removed in a future release."));
		ClaireonMaterialTool_ApplyToActor Tool;
		return Tool.Execute(ForwardedArgs);
	}
	if (Kind.Equals(TEXT("blueprint"), ESearchCase::IgnoreCase))
	{
		FString BlueprintPath;
		if (!TargetObj->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("target.blueprint_path is required when kind == 'blueprint'"));
		}
		ForwardedArgs->SetStringField(TEXT("blueprint_path"), BlueprintPath);

		UE_LOG(LogClaireon, Warning,
			TEXT("[material_apply] DEPRECATED: forward this call to 'material_apply_to_blueprint' (per-kind tool). "
			     "The dispatcher will be removed in a future release."));
		ClaireonMaterialTool_ApplyToBlueprint Tool;
		return Tool.Execute(ForwardedArgs);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown target.kind '%s' (expected 'actor' or 'blueprint')"), *Kind));
}
