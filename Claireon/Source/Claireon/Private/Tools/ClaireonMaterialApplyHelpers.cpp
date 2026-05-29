// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialApplyHelpers.h"
#include "ClaireonLog.h"

#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"

namespace ClaireonMaterialApplyHelpers
{
	UMeshComponent* FindMeshComponentOnActor(AActor* Actor, const FString& ComponentName)
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

	UMeshComponent* FindMeshComponentOnBlueprint(UBlueprint* Blueprint, const FString& ComponentName)
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

	AActor* FindActorInEditorWorld(UWorld* World, const FString& ActorName)
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

	UMaterialInterface* LoadMaterialByPath(const FString& MaterialPath, FString& OutError)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Material)
		{
			FSoftObjectPath SoftPath(MaterialPath);
			Material = Cast<UMaterialInterface>(SoftPath.TryLoad());
		}
		if (!Material)
		{
			OutError = FString::Printf(TEXT("Failed to load material '%s'"), *MaterialPath);
		}
		return Material;
	}

	IClaireonTool::FToolResult ApplyMaterialToMeshComponent(
		UMaterialInterface* Material,
		UMeshComponent* MeshComponent,
		int32 ElementIndex,
		const FString& TargetLabel,
		const FString& ComponentName,
		AActor* Actor,
		UBlueprint* Blueprint)
	{
		using FToolResult = IClaireonTool::FToolResult;
		if (!Material || !MeshComponent)
		{
			return IClaireonTool::MakeErrorResult(TEXT("ApplyMaterialToMeshComponent: null material or component"));
		}

		const int32 SlotCount = MeshComponent->GetNumMaterials();
		const bool bAllSlots = (ElementIndex == -1);
		if (!bAllSlots && (ElementIndex < 0 || ElementIndex >= SlotCount))
		{
			return IClaireonTool::MakeErrorResult(FString::Printf(
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

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("component_name"), ComponentName);
		Data->SetStringField(TEXT("slot"), SlotLabel);
		Data->SetStringField(TEXT("new_material_path"), Material->GetPathName());
		if (Actor)
		{
			Data->SetStringField(TEXT("target_kind"), TEXT("actor"));
			Data->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		}
		if (Blueprint)
		{
			Data->SetStringField(TEXT("target_kind"), TEXT("blueprint"));
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

		UE_LOG(LogClaireon, Log, TEXT("[material_apply] %s"), *Summary);

		FToolResult Result = IClaireonTool::MakeSuccessResult(Data, Output);
		return Result;
	}
}
