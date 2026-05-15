// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialApply.h"

#include "ClaireonLog.h"

#include "Materials/MaterialInterface.h"
#include "Components/MeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "HAL/PlatformTime.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	/** Find a UMeshComponent on the given actor whose GetName() matches ComponentName. */
	static UMeshComponent* FindMeshComponentOnActor(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TArray<UMeshComponent*> Mesh;
		Actor->GetComponents<UMeshComponent>(Mesh);
		for (UMeshComponent* Component : Mesh)
		{
			if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	/** Find a UMeshComponent SCS template on the given blueprint by SCS node variable name. */
	static UMeshComponent* FindMeshComponentOnBlueprint(UBlueprint* Blueprint, const FString& ComponentName)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			return nullptr;
		}
		const FName Wanted(*ComponentName);
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			if (Node->GetVariableName() == Wanted ||
				(Node->ComponentTemplate && Node->ComponentTemplate->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)))
			{
				if (UMeshComponent* AsMesh = Cast<UMeshComponent>(Node->ComponentTemplate))
				{
					return AsMesh;
				}
			}
		}
		return nullptr;
	}

	/** Resolve an actor in the editor world by label or path. */
	static AActor* FindActorInEditorWorld(UWorld* World, const FString& ActorName)
	{
		if (!World || ActorName.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase) ||
				Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(ActorName, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}
}

FString ClaireonTool_MaterialApply::GetName() const
{
	return TEXT("claireon.material_apply");
}

FString ClaireonTool_MaterialApply::GetDescription() const
{
	return TEXT("Apply a UMaterial or UMaterialInstanceConstant to a level actor or a blueprint's UMeshComponent SCS node.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialApply::GetInputSchema() const
{
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

	FString MaterialPath;
	if (!Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: material_path"));
	}

	const TSharedPtr<FJsonObject>* TargetObjPtr = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("target"), TargetObjPtr) || !TargetObjPtr || !TargetObjPtr->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required field: target (object)"));
	}
	const TSharedPtr<FJsonObject>& TargetObj = *TargetObjPtr;

	FString Kind, ComponentName;
	int32 ElementIndex = 0;
	if (!TargetObj->TryGetStringField(TEXT("kind"), Kind) || Kind.IsEmpty())
	{
		return MakeErrorResult(TEXT("target.kind is required ('actor' or 'blueprint')"));
	}
	if (!TargetObj->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return MakeErrorResult(TEXT("target.component_name is required"));
	}
	{
		double IndexDouble = 0.0;
		TargetObj->TryGetNumberField(TEXT("element_index"), IndexDouble);
		ElementIndex = static_cast<int32>(IndexDouble);
	}

	// Material resolution: load via direct LoadObject so we accept either UMaterial or UMaterialInstanceConstant.
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		// Fall back to FSoftObjectPath in case the path needs sub-object resolution.
		FSoftObjectPath SoftPath(MaterialPath);
		Material = Cast<UMaterialInterface>(SoftPath.TryLoad());
	}
	if (!Material)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load material '%s'"), *MaterialPath));
	}

	UMeshComponent* MeshComponent = nullptr;
	FString TargetLabel;
	UBlueprint* Blueprint = nullptr;
	AActor* Actor = nullptr;

	if (Kind.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
	{
		FString ActorName;
		if (!TargetObj->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
		{
			return MakeErrorResult(TEXT("target.actor_name is required when kind == 'actor'"));
		}
		if (!GEditor)
		{
			return MakeErrorResult(TEXT("GEditor is unavailable"));
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return MakeErrorResult(TEXT("No editor world available"));
		}
		Actor = FindActorInEditorWorld(World, ActorName);
		if (!Actor)
		{
			return MakeErrorResult(FString::Printf(TEXT("Actor not found in editor world: '%s'"), *ActorName));
		}
		MeshComponent = FindMeshComponentOnActor(Actor, ComponentName);
		if (!MeshComponent)
		{
			return MakeErrorResult(FString::Printf(TEXT("UMeshComponent '%s' not found on actor '%s'"),
				*ComponentName, *Actor->GetActorLabel()));
		}
		TargetLabel = FString::Printf(TEXT("actor %s"), *Actor->GetActorLabel());
	}
	else if (Kind.Equals(TEXT("blueprint"), ESearchCase::IgnoreCase))
	{
		FString BlueprintPath;
		if (!TargetObj->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("target.blueprint_path is required when kind == 'blueprint'"));
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!Blueprint)
		{
			FSoftObjectPath SoftBP(BlueprintPath);
			Blueprint = Cast<UBlueprint>(SoftBP.TryLoad());
		}
		if (!Blueprint)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load blueprint '%s'"), *BlueprintPath));
		}
		MeshComponent = FindMeshComponentOnBlueprint(Blueprint, ComponentName);
		if (!MeshComponent)
		{
			return MakeErrorResult(FString::Printf(TEXT("UMeshComponent SCS node '%s' not found on blueprint '%s'"),
				*ComponentName, *BlueprintPath));
		}
		TargetLabel = FString::Printf(TEXT("blueprint %s"), *BlueprintPath);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown target.kind '%s' (expected 'actor' or 'blueprint')"), *Kind));
	}

	const int32 SlotCount = MeshComponent->GetNumMaterials();
	const bool bAllSlots = (ElementIndex == -1);
	if (!bAllSlots && (ElementIndex < 0 || ElementIndex >= SlotCount))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("element_index %d out of range (component has %d material slot(s))"),
			ElementIndex, SlotCount));
	}

	// Capture prior materials before mutation.
	TArray<TPair<int32, FString>> PriorMaterials;
	auto CapturePrior = [&PriorMaterials, MeshComponent](int32 Slot)
	{
		UMaterialInterface* Prior = MeshComponent->GetMaterial(Slot);
		PriorMaterials.Add(TPair<int32, FString>(Slot, Prior ? Prior->GetPathName() : FString(TEXT("(none)"))));
	};

	const TCHAR* TransactionLabel = TEXT("[Claireon] Apply Material");
	FScopedTransaction Transaction(FText::FromString(TransactionLabel));
	MeshComponent->Modify();

	if (bAllSlots)
	{
		for (int32 Slot = 0; Slot < SlotCount; ++Slot)
		{
			CapturePrior(Slot);
			MeshComponent->SetMaterial(Slot, Material);
		}
	}
	else
	{
		CapturePrior(ElementIndex);
		MeshComponent->SetMaterial(ElementIndex, Material);
	}

	// Mark dirty + (if blueprint) compile.
	double CompileMs = -1.0;
	FString CompileStatus;
	if (Actor)
	{
		Actor->MarkPackageDirty();
	}
	if (Blueprint)
	{
		Blueprint->MarkPackageDirty();
		const double StartTime = FPlatformTime::Seconds();
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		CompileMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
		CompileStatus = FString::Printf(TEXT("Compile: OK (%.0f ms)"), CompileMs);
	}

	// Child-blueprint annotation (blueprint targets only).
	TArray<FString> ChildBlueprintPaths;
	if (Blueprint && Blueprint->GeneratedClass)
	{
		TArray<UClass*> ChildClasses;
		GetDerivedClasses(Blueprint->GeneratedClass, ChildClasses, /*bRecursive=*/true);
		for (UClass* Child : ChildClasses)
		{
			if (!Child) continue;
			UBlueprint* ChildBP = UBlueprint::GetBlueprintFromClass(Child);
			if (ChildBP && ChildBP != Blueprint)
			{
				ChildBlueprintPaths.AddUnique(ChildBP->GetPathName());
			}
		}
	}

	// Build response.
	FString Output;
	const FString SlotLabel = bAllSlots ? FString(TEXT("all")) : FString::FromInt(ElementIndex);
	Output += FString::Printf(TEXT("# Material Apply\n\n"));
	Output += FString::Printf(TEXT("- Target: %s\n"), *TargetLabel);
	Output += FString::Printf(TEXT("- Component: %s\n"), *ComponentName);
	Output += FString::Printf(TEXT("- Slot: %s\n"), *SlotLabel);
	Output += FString::Printf(TEXT("- New Material: %s\n"), *Material->GetPathName());
	for (const TPair<int32, FString>& Prior : PriorMaterials)
	{
		Output += FString::Printf(TEXT("- Prior Material (slot %d): %s\n"), Prior.Key, *Prior.Value);
	}
	if (Blueprint)
	{
		Output += FString::Printf(TEXT("- %s\n"), *CompileStatus);
	}
	if (ChildBlueprintPaths.Num() > 0)
	{
		Output += FString::Printf(TEXT("\n## Child Blueprints (not auto-propagated)\n\n"));
		for (const FString& Path : ChildBlueprintPaths)
		{
			Output += FString::Printf(TEXT("- %s\n"), *Path);
		}
	}

	// Build structured data.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("target_kind"), Kind);
	Data->SetStringField(TEXT("component_name"), ComponentName);
	Data->SetStringField(TEXT("slot"), SlotLabel);
	Data->SetStringField(TEXT("new_material_path"), Material->GetPathName());
	if (Actor)
	{
		Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	}
	if (Blueprint)
	{
		Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Data->SetNumberField(TEXT("compile_ms"), CompileMs);
		Data->SetStringField(TEXT("compile_status"), CompileStatus);
	}
	{
		TArray<TSharedPtr<FJsonValue>> PriorArr;
		for (const TPair<int32, FString>& Prior : PriorMaterials)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("slot"), Prior.Key);
			Entry->SetStringField(TEXT("prior_material_path"), Prior.Value);
			PriorArr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Data->SetArrayField(TEXT("prior_materials"), PriorArr);
	}
	if (ChildBlueprintPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildArr;
		for (const FString& Path : ChildBlueprintPaths)
		{
			ChildArr.Add(MakeShared<FJsonValueString>(Path));
		}
		Data->SetArrayField(TEXT("child_blueprints"), ChildArr);
	}

	const FString Summary = FString::Printf(TEXT("Applied %s to %s.%s slot %s"),
		*Material->GetName(), *TargetLabel, *ComponentName, *SlotLabel);

	UE_LOG(LogClaireon, Log, TEXT("[claireon.material_apply] %s"), *Summary);

	FToolResult Result = MakeSuccessResult(Data, Output);
	return Result;
}
