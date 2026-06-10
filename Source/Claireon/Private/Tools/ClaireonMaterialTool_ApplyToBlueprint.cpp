// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ApplyToBlueprint.h"
#include "Tools/ClaireonMaterialApplyHelpers.h"
#include "ClaireonScopedAssetLock.h"

#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_ApplyToBlueprint::GetCategory() const { return TEXT("material"); }
FString ClaireonMaterialTool_ApplyToBlueprint::GetOperation() const { return TEXT("apply_to_blueprint"); }

FString ClaireonMaterialTool_ApplyToBlueprint::GetDescription() const
{
	return TEXT("Apply a UMaterial or UMaterialInstanceConstant to a UMeshComponent SCS node on a blueprint. "
	            "element_index = -1 applies to every slot. Compiles the blueprint after the change and "
	            "reports child blueprints that may need attention. Stateless / non-session; wrapped in "
	            "FScopedTransaction so editor undo works.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_ApplyToBlueprint::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> MaterialPathProp = MakeShared<FJsonObject>();
	MaterialPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MaterialPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the UMaterialInterface to apply (UMaterial or UMaterialInstanceConstant)."));
	Properties->SetObjectField(TEXT("material_path"), MaterialPathProp);

	TSharedPtr<FJsonObject> BPProp = MakeShared<FJsonObject>();
	BPProp->SetStringField(TEXT("type"), TEXT("string"));
	BPProp->SetStringField(TEXT("description"), TEXT("Asset path of the target UBlueprint."));
	Properties->SetObjectField(TEXT("blueprint_path"), BPProp);

	TSharedPtr<FJsonObject> CompProp = MakeShared<FJsonObject>();
	CompProp->SetStringField(TEXT("type"), TEXT("string"));
	CompProp->SetStringField(TEXT("description"), TEXT("Name of the UMeshComponent SCS node on the blueprint."));
	Properties->SetObjectField(TEXT("component_name"), CompProp);

	TSharedPtr<FJsonObject> IndexProp = MakeShared<FJsonObject>();
	IndexProp->SetStringField(TEXT("type"), TEXT("integer"));
	IndexProp->SetStringField(TEXT("description"), TEXT("Material slot index; -1 means all slots."));
	Properties->SetObjectField(TEXT("element_index"), IndexProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("material_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("component_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonMaterialTool_ApplyToBlueprint::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString MaterialPath;
	if (!Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: material_path"));
	}
	FString BlueprintPath;
	if (!Arguments->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: blueprint_path"));
	}
	FString ComponentName;
	if (!Arguments->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	int32 ElementIndex = 0;
	{
		double IndexDouble = 0.0;
		Arguments->TryGetNumberField(TEXT("element_index"), IndexDouble);
		ElementIndex = static_cast<int32>(IndexDouble);
	}

	FString LoadError;
	UMaterialInterface* Material = ClaireonMaterialApplyHelpers::LoadMaterialByPath(MaterialPath, LoadError);
	if (!Material) return MakeErrorResult(LoadError);

	FClaireonScopedAssetLock Lock(BlueprintPath, GetName());
	if (!Lock.IsAcquired())
	{
		return Lock.GetError();
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint)
	{
		FSoftObjectPath SoftBP(BlueprintPath);
		Blueprint = Cast<UBlueprint>(SoftBP.TryLoad());
	}
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load blueprint '%s'"), *BlueprintPath));
	}
	UMeshComponent* MeshComponent = ClaireonMaterialApplyHelpers::FindMeshComponentOnBlueprint(Blueprint, ComponentName);
	if (!MeshComponent)
	{
		return MakeErrorResult(FString::Printf(TEXT("UMeshComponent SCS node '%s' not found on blueprint '%s'"),
			*ComponentName, *BlueprintPath));
	}

	const FString TargetLabel = FString::Printf(TEXT("blueprint %s"), *BlueprintPath);
	return ClaireonMaterialApplyHelpers::ApplyMaterialToMeshComponent(
		Material, MeshComponent, ElementIndex, TargetLabel, ComponentName, /*Actor=*/nullptr, Blueprint);
}
