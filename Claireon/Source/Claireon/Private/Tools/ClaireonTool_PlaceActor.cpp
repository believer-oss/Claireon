// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PlaceActor.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonPropertyResolver.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_PlaceActor::GetName() const
{
	return TEXT("claireon.place_actor");
}

FString ClaireonTool_PlaceActor::GetCategory() const
{
	return TEXT("level");
}

FString ClaireonTool_PlaceActor::GetDescription() const
{
	return TEXT("Place one or more actors in the editor world by class path. "
				"Supports Blueprint and native class paths, batch placement via an actors array, "
				"optional location/rotation/scale/label/properties per actor, and automatic ISM mode "
				"when the class_path points to a static mesh asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_PlaceActor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actors array
	TSharedPtr<FJsonObject> ActorsProp = MakeShared<FJsonObject>();
	ActorsProp->SetStringField(TEXT("type"), TEXT("array"));
	ActorsProp->SetStringField(TEXT("description"),
		TEXT("Array of actors to place. Each element has class_path (required), location, rotation, scale, label, properties."));
	Properties->SetObjectField(TEXT("actors"), ActorsProp);

	// class_path for single-actor convenience
	TSharedPtr<FJsonObject> ClassPathProp = MakeShared<FJsonObject>();
	ClassPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassPathProp->SetStringField(TEXT("description"),
		TEXT("Class path for single-actor placement (alternative to actors array)"));
	Properties->SetObjectField(TEXT("class_path"), ClassPathProp);

	// location for single-actor convenience
	TSharedPtr<FJsonObject> LocationProp = MakeShared<FJsonObject>();
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"),
		TEXT("World location {x, y, z} for single-actor placement"));
	Properties->SetObjectField(TEXT("location"), LocationProp);

	// rotation for single-actor convenience
	TSharedPtr<FJsonObject> RotationProp = MakeShared<FJsonObject>();
	RotationProp->SetStringField(TEXT("type"), TEXT("object"));
	RotationProp->SetStringField(TEXT("description"),
		TEXT("World rotation {pitch, yaw, roll} for single-actor placement"));
	Properties->SetObjectField(TEXT("rotation"), RotationProp);

	// scale for single-actor convenience
	TSharedPtr<FJsonObject> ScaleProp = MakeShared<FJsonObject>();
	ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
	ScaleProp->SetStringField(TEXT("description"),
		TEXT("Scale {x, y, z} for single-actor placement"));
	Properties->SetObjectField(TEXT("scale"), ScaleProp);

	// label for single-actor convenience
	TSharedPtr<FJsonObject> LabelProp = MakeShared<FJsonObject>();
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"),
		TEXT("Actor label for single-actor placement"));
	Properties->SetObjectField(TEXT("label"), LabelProp);

	// properties for single-actor convenience
	TSharedPtr<FJsonObject> PropsProp = MakeShared<FJsonObject>();
	PropsProp->SetStringField(TEXT("type"), TEXT("object"));
	PropsProp->SetStringField(TEXT("description"),
		TEXT("Property name/value pairs for single-actor placement"));
	Properties->SetObjectField(TEXT("properties"), PropsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

namespace
{
	FVector ParseVector(const TSharedPtr<FJsonObject>& Obj, double DefaultX = 0.0, double DefaultY = 0.0, double DefaultZ = 0.0)
	{
		double X = DefaultX, Y = DefaultY, Z = DefaultZ;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("z"), Z);
		}
		return FVector(X, Y, Z);
	}

	FRotator ParseRotator(const TSharedPtr<FJsonObject>& Obj)
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("pitch"), Pitch);
			Obj->TryGetNumberField(TEXT("yaw"), Yaw);
			Obj->TryGetNumberField(TEXT("roll"), Roll);
		}
		return FRotator(Pitch, Yaw, Roll);
	}
} // namespace

FToolResult ClaireonTool_PlaceActor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	checkf(GEditor && GEditor->GetEditorWorldContext().World(),
		TEXT("RequiresEditorWorld() tool reached Execute without a valid world. This indicates a dispatch path that bypasses precondition checks."));
	UWorld* World = GEditor->GetEditorWorldContext().World();

	// Normalize input: detect array vs flat object, build actor specs array
	TArray<TSharedPtr<FJsonValue>> ActorSpecs;
	const TArray<TSharedPtr<FJsonValue>>* ActorsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("actors"), ActorsArray) && ActorsArray)
	{
		ActorSpecs = *ActorsArray;
	}
	else
	{
		// Single-actor convenience: wrap the arguments object as a single-element array
		ActorSpecs.Add(MakeShared<FJsonValueObject>(Arguments));
	}

	if (ActorSpecs.Num() == 0)
	{
		return MakeErrorResult(TEXT("No actor specs provided. Pass an 'actors' array or flat object with 'class_path'."));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Place Actor")));

	TArray<TSharedPtr<FJsonValue>> ResultDetails;
	int32 SuccessCount = 0;

	// Track ISM actors by mesh path for reuse within this batch
	TMap<FString, AActor*> ISMActorMap;
	TMap<FString, UInstancedStaticMeshComponent*> ISMComponentMap;

	for (int32 Idx = 0; Idx < ActorSpecs.Num(); ++Idx)
	{
		TSharedPtr<FJsonObject> Spec;
		{
			const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
			if (!ActorSpecs[Idx]->TryGetObject(SpecPtr) || !SpecPtr)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
				ErrObj->SetNumberField(TEXT("index"), Idx);
				ErrObj->SetStringField(TEXT("error"), TEXT("Invalid actor spec (not an object)"));
				ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
				continue;
			}
			Spec = *SpecPtr;
		}

		FString ClassPath;
		if (!Spec->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
			ErrObj->SetNumberField(TEXT("index"), Idx);
			ErrObj->SetStringField(TEXT("error"), TEXT("Missing required field: class_path"));
			ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Parse transform fields
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		Spec->TryGetObjectField(TEXT("location"), LocationObj);
		FVector Location = ParseVector(LocationObj ? *LocationObj : nullptr);

		const TSharedPtr<FJsonObject>* RotationObj = nullptr;
		Spec->TryGetObjectField(TEXT("rotation"), RotationObj);
		FRotator Rotation = ParseRotator(RotationObj ? *RotationObj : nullptr);

		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		Spec->TryGetObjectField(TEXT("scale"), ScaleObj);
		FVector Scale = ParseVector(ScaleObj ? *ScaleObj : nullptr, 1.0, 1.0, 1.0);

		FString Label;
		Spec->TryGetStringField(TEXT("label"), Label);

		// Load the object
		UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassPath);
		if (!LoadedObject)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
			ErrObj->SetNumberField(TEXT("index"), Idx);
			ErrObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not load object: %s"), *ClassPath));
			ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Check if this is a static mesh -> ISM path
		UStaticMesh* LoadedMesh = Cast<UStaticMesh>(LoadedObject);
		if (LoadedMesh)
		{
			// ISM path: find or create ISM actor for this mesh
			FString MeshName = LoadedMesh->GetName();
			FString ISMLabel = FString::Printf(TEXT("ISM_%s"), *MeshName);

			AActor* ISMActor = nullptr;
			UInstancedStaticMeshComponent* ISMComponent = nullptr;

			if (AActor** FoundActor = ISMActorMap.Find(ClassPath))
			{
				ISMActor = *FoundActor;
				ISMComponent = ISMComponentMap.FindChecked(ClassPath);
			}
			else
			{
				// Search existing actors in the level for a matching ISM actor
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if ((*It)->GetActorLabel() == ISMLabel)
					{
						ISMActor = *It;
						ISMComponent = ISMActor->FindComponentByClass<UInstancedStaticMeshComponent>();
						break;
					}
				}

				if (!ISMActor)
				{
					// Create a new actor with an ISM component
					FActorSpawnParameters SpawnParams;
					SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					ISMActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
					if (ISMActor)
					{
						ISMActor->SetActorLabel(ISMLabel, /*bMarkDirty=*/true);

						ISMComponent = NewObject<UInstancedStaticMeshComponent>(ISMActor, NAME_None, RF_Transactional);
						ISMComponent->RegisterComponent();
						ISMActor->AddInstanceComponent(ISMComponent);
						ISMComponent->SetStaticMesh(LoadedMesh);
					}
				}

				if (ISMActor && ISMComponent)
				{
					ISMActorMap.Add(ClassPath, ISMActor);
					ISMComponentMap.Add(ClassPath, ISMComponent);
				}
			}

			if (!ISMActor || !ISMComponent)
			{
				TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
				ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
				ErrObj->SetNumberField(TEXT("index"), Idx);
				ErrObj->SetStringField(TEXT("error"), TEXT("Failed to create ISM actor"));
				ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
				continue;
			}

			FTransform InstanceTransform(Rotation, Location, Scale);
			int32 InstanceIndex = ISMComponent->AddInstance(InstanceTransform, /*bWorldSpace=*/true);

			ISMActor->MarkPackageDirty();

			TSharedPtr<FJsonObject> SuccObj = MakeShared<FJsonObject>();
			SuccObj->SetStringField(TEXT("status"), TEXT("success"));
			SuccObj->SetNumberField(TEXT("index"), Idx);
			SuccObj->SetStringField(TEXT("mode"), TEXT("ism"));
			SuccObj->SetStringField(TEXT("ism_actor"), ISMLabel);
			SuccObj->SetNumberField(TEXT("instance_index"), InstanceIndex);
			SuccObj->SetStringField(TEXT("mesh"), ClassPath);

			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), Location.X);
			LocObj->SetNumberField(TEXT("y"), Location.Y);
			LocObj->SetNumberField(TEXT("z"), Location.Z);
			SuccObj->SetObjectField(TEXT("location"), LocObj);

			ResultDetails.Add(MakeShared<FJsonValueObject>(SuccObj));
			SuccessCount++;
			continue;
		}

		// Normal actor path: resolve class
		UClass* SpawnClass = nullptr;
		if (UBlueprint* BP = Cast<UBlueprint>(LoadedObject))
		{
			SpawnClass = BP->GeneratedClass;
		}
		else if (UClass* DirectClass = Cast<UClass>(LoadedObject))
		{
			SpawnClass = DirectClass;
		}

		if (!SpawnClass)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
			ErrObj->SetNumberField(TEXT("index"), Idx);
			ErrObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not resolve class from: %s"), *ClassPath));
			ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		if (!SpawnClass->IsChildOf(AActor::StaticClass()))
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
			ErrObj->SetNumberField(TEXT("index"), Idx);
			ErrObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Class %s is not an Actor subclass"), *SpawnClass->GetName()));
			ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Spawn the actor
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

		AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnClass, Location, Rotation, SpawnParams);
		if (!SpawnedActor)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetStringField(TEXT("status"), TEXT("failed"));
			ErrObj->SetNumberField(TEXT("index"), Idx);
			ErrObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to spawn actor of class: %s"), *SpawnClass->GetName()));
			ResultDetails.Add(MakeShared<FJsonValueObject>(ErrObj));
			continue;
		}

		// Set scale
		SpawnedActor->SetActorScale3D(Scale);

		// Set label
		if (!Label.IsEmpty())
		{
			SpawnedActor->SetActorLabel(Label, /*bMarkDirty=*/true);
		}

		// Apply optional properties via resolver (component-aware)
		const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
		if (Spec->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj)
		{
			for (const auto& Pair : (*PropertiesObj)->Values)
			{
				// Coerce JSON value to string (caller responsibility per M4)
				FString ValueStr;
				if (!Pair.Value->TryGetString(ValueStr))
				{
					double NumVal;
					bool BoolVal;
					if (Pair.Value->TryGetNumber(NumVal))
					{
						ValueStr = FString::SanitizeFloat(NumVal);
					}
					else if (Pair.Value->TryGetBool(BoolVal))
					{
						ValueStr = BoolVal ? TEXT("True") : TEXT("False");
					}
				}

				ClaireonPropertyResolver::FResolvedProperty Resolved;
				FString WriteError;
				bool bOk = ClaireonPropertyResolver::WritePropertyOnActor(SpawnedActor, Pair.Key, ValueStr, Resolved, WriteError);
				if (!bOk)
				{
					UE_LOG(LogClaireon, Warning, TEXT("[place_actor] %s"), *WriteError);
				}
			}
		}

		SpawnedActor->MarkPackageDirty();

		// Build per-actor success result
		FString ActorLabel = SpawnedActor->GetActorLabel();
		FVector ActorLocation = SpawnedActor->GetActorLocation();

		TSharedPtr<FJsonObject> SuccObj = MakeShared<FJsonObject>();
		SuccObj->SetStringField(TEXT("status"), TEXT("success"));
		SuccObj->SetNumberField(TEXT("index"), Idx);
		SuccObj->SetStringField(TEXT("label"), ActorLabel);
		SuccObj->SetStringField(TEXT("class"), SpawnedActor->GetClass()->GetName());
		SuccObj->SetStringField(TEXT("path"), SpawnedActor->GetPathName());

		TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), ActorLocation.X);
		LocObj->SetNumberField(TEXT("y"), ActorLocation.Y);
		LocObj->SetNumberField(TEXT("z"), ActorLocation.Z);
		SuccObj->SetObjectField(TEXT("location"), LocObj);

		ResultDetails.Add(MakeShared<FJsonValueObject>(SuccObj));
		SuccessCount++;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("total"), ActorSpecs.Num());
	Data->SetNumberField(TEXT("succeeded"), SuccessCount);
	Data->SetNumberField(TEXT("failed"), ActorSpecs.Num() - SuccessCount);
	Data->SetArrayField(TEXT("results"), ResultDetails);

	FString Summary = FString::Printf(TEXT("Placed %d/%d actors"), SuccessCount, ActorSpecs.Num());
	return MakeSuccessResult(Data, Summary);
}
