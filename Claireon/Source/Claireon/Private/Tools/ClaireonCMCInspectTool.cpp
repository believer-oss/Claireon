// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCMCInspectTool.h"

#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimationAsset.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/CharacterMovementReplication.h"
#include "GameFramework/RootMotionSource.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace ClaireonCMCInspectScratch
{
	// Decode EMovementMode (declared at
	// C:/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/EngineTypes.h:922).
	FString MovementModeToString(EMovementMode InMode)
	{
		switch (InMode)
		{
		case MOVE_None:      return TEXT("MOVE_None");
		case MOVE_Walking:   return TEXT("MOVE_Walking");
		case MOVE_NavWalking:return TEXT("MOVE_NavWalking");
		case MOVE_Falling:   return TEXT("MOVE_Falling");
		case MOVE_Swimming:  return TEXT("MOVE_Swimming");
		case MOVE_Flying:    return TEXT("MOVE_Flying");
		case MOVE_Custom:    return TEXT("MOVE_Custom");
		case MOVE_MAX:       return TEXT("MOVE_MAX");
		default:             return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InMode));
		}
	}

	void SetVectorField(const TSharedPtr<FJsonObject>& Parent, const FString& Key, const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		Parent->SetObjectField(Key, Obj);
	}

	void SetRotatorField(const TSharedPtr<FJsonObject>& Parent, const FString& Key, const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		Parent->SetObjectField(Key, Obj);
	}

	// Resolve PIE world or return null. Sets OutErrorMessage if no PIE.
	UWorld* ResolvePIEWorld(FString& OutErrorMessage)
	{
		if (!GEditor)
		{
			OutErrorMessage = TEXT("GEditor is not available");
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				return Context.World();
			}
		}

		OutErrorMessage = TEXT("No active PIE session");
		return nullptr;
	}

	// Common front-half: validate args, resolve PIE world, resolve actor, cast to ACharacter.
	// Returns:
	//   - On error: OutResult set, return false.
	//   - On non-Character: OutData set with hasCMC=false + reason, OutResult set; return false.
	//   - On success: OutCharacter populated, OutData allocated empty (with hasCMC=true preset),
	//                 return true.
	bool ResolveCharacterForTool(
		const TSharedPtr<FJsonObject>& Arguments,
		const TCHAR* HasFlagKey,
		FString& OutActorId,
		ACharacter*& OutCharacter,
		TSharedPtr<FJsonObject>& OutData,
		IClaireonTool::FToolResult& OutResult)
	{
		OutCharacter = nullptr;

		if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("actorId"), OutActorId))
		{
			OutResult = IClaireonTool::MakeErrorResult(TEXT("Missing required argument: actorId"));
			return false;
		}

		FString Err;
		UWorld* PIEWorld = ResolvePIEWorld(Err);
		if (!PIEWorld)
		{
			OutResult = IClaireonTool::MakeErrorResult(Err);
			return false;
		}

		AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(OutActorId, PIEWorld);
		if (!Actor)
		{
			OutResult = IClaireonTool::MakeErrorResult(
				FString::Printf(TEXT("Actor not found for ID: %s"), *OutActorId));
			return false;
		}

		OutData = MakeShared<FJsonObject>();
		ACharacter* Character = Cast<ACharacter>(Actor);
		if (!Character)
		{
			OutData->SetBoolField(HasFlagKey, false);
			OutData->SetStringField(TEXT("reason"),
				TEXT("Actor is not an ACharacter; no CharacterMovementComponent"));
			OutResult = IClaireonTool::MakeSuccessResult(OutData,
				TEXT("Actor has no CMC (not an ACharacter)"));
			return false;
		}

		OutCharacter = Character;
		OutData->SetBoolField(HasFlagKey, true);
		return true;
	}
}

// =============================================================================
// Tool 1: cmc_inspect_state
// =============================================================================

FString ClaireonTool_CMCInspectState::GetDescription() const
{
    return TEXT("Snapshot the authoritative UCharacterMovementComponent state on a paused-PIE pawn: movement mode, velocity/acceleration, gravity, walkable floor parameters, current floor hit, movement base, crouched, and FS-CMC custom fields. Read-only / non-session inspector. Targets stuck-mid-air, gravity-flip, and floor-detection bug classes.");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectState::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	TSharedPtr<FJsonObject> IncludeFloorProp = MakeShared<FJsonObject>();
	IncludeFloorProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeFloorProp->SetStringField(TEXT("description"),
		TEXT("If true, include CurrentFloor result. Default true."));
	IncludeFloorProp->SetBoolField(TEXT("default"), true);
	Properties->SetObjectField(TEXT("includeFloor"), IncludeFloorProp);

	TSharedPtr<FJsonObject> IncludePrevFloorProp = MakeShared<FJsonObject>();
	IncludePrevFloorProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludePrevFloorProp->SetStringField(TEXT("description"),
		TEXT("If true, include FS-CMC PreviousKnownFloor. Default false."));
	IncludePrevFloorProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("includePrevFloor"), IncludePrevFloorProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FString ClaireonTool_CMCInspectState::GetExampleUsage() const
{
	return TEXT("cmc_inspect_state(actorId=\"actor_0\", includeFloor=true)");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectState::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Tooltips->SetStringField(TEXT("includeFloor"),
		TEXT("Include CurrentFloor hit result (walkable, dist, normal, impact)."));
	Tooltips->SetStringField(TEXT("includePrevFloor"),
		TEXT("Include FS-CMC PreviousKnownFloor (only present on UFSCharacterMovementComponent)."));
	return Tooltips;
}

TArray<FString> ClaireonTool_CMCInspectState::GetSearchKeywords() const
{
	return {
		TEXT("stuck mid-air"),
		TEXT("movement mode"),
		TEXT("movement base"),
		TEXT("current floor"),
		TEXT("floor check"),
		TEXT("walkable"),
		TEXT("gravity"),
		TEXT("falling"),
		TEXT("walking"),
		TEXT("crouched")
	};
}

namespace ClaireonCMCInspectScratch
{
	// Build floor JSON. Shape:
	// { blockingHit, walkableFloor, lineTrace, floorDist, lineDist, hitNormal:{x,y,z},
	//   hitActor: <name|null>, hitBone, hitImpactPoint:{x,y,z}, hitTime }
	TSharedPtr<FJsonObject> BuildFloorJson(const FFindFloorResult& Floor)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("blockingHit"), Floor.bBlockingHit != 0);
		Obj->SetBoolField(TEXT("walkableFloor"), Floor.bWalkableFloor != 0);
		Obj->SetBoolField(TEXT("lineTrace"), Floor.bLineTrace != 0);
		Obj->SetNumberField(TEXT("floorDist"), Floor.FloorDist);
		Obj->SetNumberField(TEXT("lineDist"), Floor.LineDist);

		const FHitResult& Hit = Floor.HitResult;
		SetVectorField(Obj, TEXT("hitNormal"), Hit.Normal);
		SetVectorField(Obj, TEXT("hitImpactPoint"), Hit.ImpactPoint);
		Obj->SetNumberField(TEXT("hitTime"), Hit.Time);

		AActor* HitActor = Hit.GetActor();
		if (HitActor)
		{
			Obj->SetStringField(TEXT("hitActor"), HitActor->GetName());
		}
		else
		{
			Obj->SetField(TEXT("hitActor"), MakeShared<FJsonValueNull>());
		}
		Obj->SetStringField(TEXT("hitBone"), Hit.BoneName.ToString());
		return Obj;
	}
}

IClaireonTool::FToolResult ClaireonTool_CMCInspectState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonCMCInspectScratch;

	FString ActorId;
	ACharacter* Character = nullptr;
	TSharedPtr<FJsonObject> Data;
	IClaireonTool::FToolResult EarlyResult;
	if (!ResolveCharacterForTool(Arguments, TEXT("hasCMC"), ActorId, Character, Data, EarlyResult))
	{
		return EarlyResult;
	}

	UCharacterMovementComponent* CMC = Character->GetCharacterMovement();
	if (!CMC)
	{
		Data->SetBoolField(TEXT("hasCMC"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Character has no CharacterMovementComponent (GetCharacterMovement() returned null)"));
		return MakeSuccessResult(Data, TEXT("Character has no CMC"));
	}

	bool bIncludeFloor = true;
	Arguments->TryGetBoolField(TEXT("includeFloor"), bIncludeFloor);
	bool bIncludePrevFloor = false;
	Arguments->TryGetBoolField(TEXT("includePrevFloor"), bIncludePrevFloor);

	// Movement mode
	Data->SetStringField(TEXT("movementMode"), MovementModeToString(CMC->MovementMode.GetValue()));
	Data->SetNumberField(TEXT("customMovementMode"), static_cast<int32>(CMC->CustomMovementMode));

	// Gravity
	Data->SetNumberField(TEXT("gravityScale"), CMC->GravityScale);
	SetVectorField(Data, TEXT("gravityDirection"), CMC->GetGravityDirection());

	// Velocity / acceleration / requested velocity
	SetVectorField(Data, TEXT("velocity"), CMC->Velocity);
	SetVectorField(Data, TEXT("acceleration"), CMC->GetCurrentAcceleration());
	SetVectorField(Data, TEXT("requestedVelocity"), CMC->RequestedVelocity);

	// State booleans
	Data->SetBoolField(TEXT("falling"), CMC->IsFalling());
	Data->SetBoolField(TEXT("walking"), CMC->IsWalking());
	Data->SetBoolField(TEXT("flying"), CMC->IsFlying());
	Data->SetBoolField(TEXT("swimming"), CMC->IsSwimming());
	Data->SetBoolField(TEXT("crouched"), Character->bIsCrouched != 0);

	// Walkable floor parameters
	Data->SetNumberField(TEXT("walkableFloorAngle"), CMC->GetWalkableFloorAngle());
	Data->SetNumberField(TEXT("walkableFloorZ"), CMC->GetWalkableFloorZ());
	Data->SetNumberField(TEXT("maxStepHeight"), CMC->MaxStepHeight);

	Data->SetBoolField(TEXT("forceNextFloorCheck"), CMC->bForceNextFloorCheck != 0);
	Data->SetBoolField(TEXT("alwaysCheckFloor"), CMC->bAlwaysCheckFloor != 0);

	// Movement base
	{
		TSharedPtr<FJsonObject> BaseObj = MakeShared<FJsonObject>();
		const FBasedMovementInfo& BasedMove = Character->GetBasedMovement();
		AActor* BaseOwner = BasedMove.MovementBase ? BasedMove.MovementBase->GetOwner() : nullptr;
		if (BaseOwner)
		{
			BaseObj->SetStringField(TEXT("actor"), BaseOwner->GetName());
		}
		else
		{
			BaseObj->SetField(TEXT("actor"), MakeShared<FJsonValueNull>());
		}
		BaseObj->SetStringField(TEXT("boneName"), BasedMove.BoneName.ToString());
		BaseObj->SetBoolField(TEXT("ignoreBaseRotation"), CMC->bIgnoreBaseRotation != 0);
		Data->SetObjectField(TEXT("movementBase"), BaseObj);
	}

	// Current floor
	if (bIncludeFloor)
	{
		Data->SetObjectField(TEXT("currentFloor"), BuildFloorJson(CMC->CurrentFloor));
	}

	// Project-specific custom fields, nested under fsCustom to prevent collisions. Detected by
	// walking the parent class chain by name so this tool does not need to link the
	// project's character-movement module. Replace "FSCharacterMovementComponent" with your
	// project's subclass name to surface its custom UPROPERTYs in the snapshot.
	auto DerivesFromFSCMC = [](const UClass* Class) -> bool
	{
		for (const UClass* Cur = Class; Cur != nullptr; Cur = Cur->GetSuperClass())
		{
			if (Cur->GetName() == TEXT("FSCharacterMovementComponent"))
			{
				return true;
			}
		}
		return false;
	};

	if (DerivesFromFSCMC(CMC->GetClass()))
	{
		TSharedPtr<FJsonObject> FsCustom = MakeShared<FJsonObject>();
		UClass* CMCClass = CMC->GetClass();

		// Read AscendingGravityScale and DescendingGravityScale via reflection. These are
		// example project-specific UPROPERTYs; the call is a no-op if they are absent.
		auto ReadFloatProp = [CMC](FName PropName, const TSharedPtr<FJsonObject>& Out, const FString& OutKey)
		{
			if (FProperty* P = CMC->GetClass()->FindPropertyByName(PropName))
			{
				if (FFloatProperty* FP = CastField<FFloatProperty>(P))
				{
					const void* ValuePtr = P->ContainerPtrToValuePtr<void>(CMC);
					Out->SetNumberField(OutKey, FP->GetPropertyValue(ValuePtr));
				}
				else if (FDoubleProperty* DP = CastField<FDoubleProperty>(P))
				{
					const void* ValuePtr = P->ContainerPtrToValuePtr<void>(CMC);
					Out->SetNumberField(OutKey, DP->GetPropertyValue(ValuePtr));
				}
			}
		};
		ReadFloatProp(TEXT("AscendingGravityScale"), FsCustom, TEXT("ascendingGravityScale"));
		ReadFloatProp(TEXT("DescendingGravityScale"), FsCustom, TEXT("descendingGravityScale"));

		// Discover any additional project-specific Custom_-prefixed UPROPERTYs via reflection.
		// Walk class metadata in full so multi-level subclass chains that contribute
		// Custom_ properties are picked up.
		for (TFieldIterator<FProperty> PropIt(CMCClass); PropIt; ++PropIt)
		{
			const FProperty* Prop = *PropIt;
			if (!Prop)
			{
				continue;
			}
			const FString PropName = Prop->GetName();
			if (!PropName.StartsWith(TEXT("Custom_")))
			{
				continue;
			}

			// Best-effort: emit numeric properties directly. Other types are skipped (would
			// require a full struct serializer; out of scope for this snapshot tool).
			if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CMC);
				if (NumProp->IsFloatingPoint())
				{
					FsCustom->SetNumberField(PropName, NumProp->GetFloatingPointPropertyValue(ValuePtr));
				}
				else if (NumProp->IsInteger())
				{
					FsCustom->SetNumberField(PropName,
						static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
				}
			}
			else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
			{
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CMC);
				FsCustom->SetBoolField(PropName, BoolProp->GetPropertyValue(ValuePtr));
			}
		}

		// Previous known floor (project-specific). Reflected via FStructProperty so this
		// tool does not need a direct C++ dependency on the project's movement module.
		if (bIncludePrevFloor)
		{
			if (FProperty* PrevFloorProp = CMCClass->FindPropertyByName(TEXT("PreviousKnownFloor")))
			{
				if (FStructProperty* StructProp = CastField<FStructProperty>(PrevFloorProp))
				{
					if (StructProp->Struct && StructProp->Struct->GetName() == TEXT("FindFloorResult"))
					{
						const FFindFloorResult* PrevFloor =
							StructProp->ContainerPtrToValuePtr<FFindFloorResult>(CMC);
						if (PrevFloor)
						{
							FsCustom->SetObjectField(TEXT("previousKnownFloor"),
								BuildFloorJson(*PrevFloor));
						}
					}
				}
			}
		}

		Data->SetObjectField(TEXT("fsCustom"), FsCustom);
	}

	const FString Summary = FString::Printf(
		TEXT("CMC state for %s: %s, vel=(%.0f,%.0f,%.0f)"),
		*ActorId,
		*MovementModeToString(CMC->MovementMode.GetValue()),
		CMC->Velocity.X, CMC->Velocity.Y, CMC->Velocity.Z);

	return MakeSuccessResult(Data, Summary);
}

// =============================================================================
// Tool 2: cmc_inspect_root_motion (implementation in S4)
// =============================================================================

FString ClaireonTool_CMCInspectRootMotion::GetDescription() const
{
    return TEXT("Snapshot CMC root motion state: CurrentRootMotion / ServerCorrectionRootMotion source groups, anim root motion, and last applied root motion delta. Read-only / non-session inspector. Targets stuck-mid-air bug class where root motion is desyncing.");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectRootMotion::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FString ClaireonTool_CMCInspectRootMotion::GetExampleUsage() const
{
	return TEXT("cmc_inspect_root_motion(actorId=\"actor_0\")");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectRootMotion::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	return Tooltips;
}

TArray<FString> ClaireonTool_CMCInspectRootMotion::GetSearchKeywords() const
{
	return {
		TEXT("root motion server"),
		TEXT("root motion source"),
		TEXT("stuck mid-air"),
		TEXT("RootMotionSourceGroup"),
		TEXT("anim root motion")
	};
}

namespace ClaireonCMCInspectScratch
{
	// Cap on per-source enumeration to avoid pathological blowup. Mirror of the
	// MaxProperties = 50 pattern at ClaireonTool_PIEGetComponent.cpp:187.
	inline constexpr int32 MaxRootMotionSourcesPerGroup = 32;

	FString AccumulateModeToString(ERootMotionAccumulateMode Mode)
	{
		switch (Mode)
		{
		case ERootMotionAccumulateMode::Override: return TEXT("Override");
		case ERootMotionAccumulateMode::Additive: return TEXT("Additive");
		default:                                  return FString::Printf(TEXT("Unknown(%d)"),
															 static_cast<int32>(Mode));
		}
	}

	FString FinishVelocityModeToString(ERootMotionFinishVelocityMode Mode)
	{
		switch (Mode)
		{
		case ERootMotionFinishVelocityMode::MaintainLastRootMotionVelocity:
			return TEXT("MaintainLastRootMotionVelocity");
		case ERootMotionFinishVelocityMode::SetVelocity:
			return TEXT("SetVelocity");
		case ERootMotionFinishVelocityMode::ClampVelocity:
			return TEXT("ClampVelocity");
		default:
			return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(Mode));
		}
	}

	void AppendStatusFlagStrings(const FRootMotionSourceStatus& Status, TArray<TSharedPtr<FJsonValue>>& OutFlags)
	{
		if (Status.HasFlag(ERootMotionSourceStatusFlags::Prepared))
		{
			OutFlags.Add(MakeShared<FJsonValueString>(TEXT("Prepared")));
		}
		if (Status.HasFlag(ERootMotionSourceStatusFlags::Finished))
		{
			OutFlags.Add(MakeShared<FJsonValueString>(TEXT("Finished")));
		}
		if (Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval))
		{
			OutFlags.Add(MakeShared<FJsonValueString>(TEXT("MarkedForRemoval")));
		}
	}

	// Build a JSON description of one FRootMotionSourceGroup (CurrentRootMotion or
	// ServerCorrectionRootMotion). Shape:
	//   { count, hasAdditiveSources, hasOverrideSources, sources: [...], truncated }
	TSharedPtr<FJsonObject> BuildSourceGroupJson(const FRootMotionSourceGroup& Group)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("count"), Group.RootMotionSources.Num());
		Obj->SetBoolField(TEXT("hasAdditiveSources"), Group.bHasAdditiveSources != 0);
		Obj->SetBoolField(TEXT("hasOverrideSources"), Group.bHasOverrideSources != 0);

		TArray<TSharedPtr<FJsonValue>> SourcesArr;
		const int32 NumToEmit = FMath::Min(Group.RootMotionSources.Num(), MaxRootMotionSourcesPerGroup);
		for (int32 Index = 0; Index < NumToEmit; ++Index)
		{
			const TSharedPtr<FRootMotionSource>& SourcePtr = Group.RootMotionSources[Index];
			if (!SourcePtr.IsValid())
			{
				continue;
			}
			const FRootMotionSource& Source = *SourcePtr;
			TSharedPtr<FJsonObject> SrcObj = MakeShared<FJsonObject>();
			SrcObj->SetNumberField(TEXT("id"), Source.LocalID);
			SrcObj->SetStringField(TEXT("instanceName"), Source.InstanceName.ToString());
			SrcObj->SetStringField(TEXT("accumulateMode"), AccumulateModeToString(Source.AccumulateMode));
			SrcObj->SetStringField(TEXT("finishVelocityMode"),
				FinishVelocityModeToString(Source.FinishVelocityParams.Mode));
			SrcObj->SetNumberField(TEXT("time"), Source.GetTime());
			SrcObj->SetNumberField(TEXT("duration"), Source.Duration);
			SrcObj->SetNumberField(TEXT("priority"), Source.Priority);

			TArray<TSharedPtr<FJsonValue>> StatusFlags;
			AppendStatusFlagStrings(Source.Status, StatusFlags);
			SrcObj->SetArrayField(TEXT("sourceFlags"), StatusFlags);

			SourcesArr.Add(MakeShared<FJsonValueObject>(SrcObj));
		}
		Obj->SetArrayField(TEXT("sources"), SourcesArr);
		Obj->SetBoolField(TEXT("truncated"),
			Group.RootMotionSources.Num() > MaxRootMotionSourcesPerGroup);
		return Obj;
	}
}

IClaireonTool::FToolResult ClaireonTool_CMCInspectRootMotion::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonCMCInspectScratch;

	FString ActorId;
	ACharacter* Character = nullptr;
	TSharedPtr<FJsonObject> Data;
	IClaireonTool::FToolResult EarlyResult;
	if (!ResolveCharacterForTool(Arguments, TEXT("hasCMC"), ActorId, Character, Data, EarlyResult))
	{
		return EarlyResult;
	}

	UCharacterMovementComponent* CMC = Character->GetCharacterMovement();
	if (!CMC)
	{
		Data->SetBoolField(TEXT("hasCMC"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Character has no CharacterMovementComponent (GetCharacterMovement() returned null)"));
		return MakeSuccessResult(Data, TEXT("Character has no CMC"));
	}

	// Source groups.
	Data->SetObjectField(TEXT("currentRootMotion"), BuildSourceGroupJson(CMC->CurrentRootMotion));
	Data->SetObjectField(TEXT("serverCorrectionRootMotion"),
		BuildSourceGroupJson(CMC->ServerCorrectionRootMotion));

	// Anim root motion.
	{
		TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
		// IsPlayingNetworkedRootMotionMontage is a method on ACharacter (Character.h:1033),
		// not a UPROPERTY field.
		const bool bPlayingNetworkedRM = Character->IsPlayingNetworkedRootMotionMontage();
		AnimObj->SetBoolField(TEXT("isPlayingNetworkedRootMotionMontage"), bPlayingNetworkedRM);

		// currentMontage: read from GetRootMotionAnimMontageInstance(); null if no networked
		// root motion montage is active.
		FAnimMontageInstance* MontageInstance = Character->GetRootMotionAnimMontageInstance();
		if (MontageInstance && MontageInstance->Montage)
		{
			TSharedPtr<FJsonObject> MontageObj = MakeShared<FJsonObject>();
			MontageObj->SetStringField(TEXT("name"), MontageInstance->Montage->GetName());
			MontageObj->SetNumberField(TEXT("position"), MontageInstance->GetPosition());
			MontageObj->SetNumberField(TEXT("weight"), MontageInstance->GetWeight());
			AnimObj->SetObjectField(TEXT("currentMontage"), MontageObj);
		}
		else
		{
			AnimObj->SetField(TEXT("currentMontage"), MakeShared<FJsonValueNull>());
		}

		// lastAppliedRootMotionDelta: from CMC's RootMotionParams (engine-public, transient).
		TSharedPtr<FJsonObject> DeltaObj = MakeShared<FJsonObject>();
		const FTransform& RMTransform = CMC->RootMotionParams.GetRootMotionTransform();
		SetVectorField(DeltaObj, TEXT("translation"), RMTransform.GetTranslation());
		SetRotatorField(DeltaObj, TEXT("rotation"), RMTransform.GetRotation().Rotator());
		AnimObj->SetObjectField(TEXT("lastAppliedRootMotionDelta"), DeltaObj);

		Data->SetObjectField(TEXT("animRootMotion"), AnimObj);
	}

	Data->SetBoolField(TEXT("ignoreClientMovementErrorChecksAndCorrection"),
		CMC->bIgnoreClientMovementErrorChecksAndCorrection != 0);

	const FString Summary = FString::Printf(
		TEXT("Root motion for %s: current=%d source(s), serverCorrection=%d source(s), animRootMotion=%s"),
		*ActorId,
		CMC->CurrentRootMotion.RootMotionSources.Num(),
		CMC->ServerCorrectionRootMotion.RootMotionSources.Num(),
		Character->IsPlayingNetworkedRootMotionMontage() ? TEXT("playing") : TEXT("none"));

	return MakeSuccessResult(Data, Summary);
}

// =============================================================================
// Tool 3: cmc_inspect_prediction_data (implementation in S5)
// =============================================================================

FString ClaireonTool_CMCInspectPredictionData::GetDescription() const
{
    return TEXT("Snapshot CMC network prediction data: server clock, client clock, saved-move buffer, pending adjustment, last client adjustment / good move ack times. Read-only / non-session inspector. Targets replication desync, saved-move-limit, and client-correction bug classes.");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectPredictionData::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FString ClaireonTool_CMCInspectPredictionData::GetExampleUsage() const
{
	return TEXT("cmc_inspect_prediction_data(actorId=\"actor_0\")");
}

TSharedPtr<FJsonObject> ClaireonTool_CMCInspectPredictionData::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	return Tooltips;
}

TArray<FString> ClaireonTool_CMCInspectPredictionData::GetSearchKeywords() const
{
	return {
		TEXT("replication desync"),
		TEXT("saved moves"),
		TEXT("client time stamp"),
		TEXT("server prediction"),
		TEXT("saved move limit"),
		TEXT("96 saved moves"),
		TEXT("client correction"),
		TEXT("server move"),
		TEXT("client adjustment")
	};
}

namespace ClaireonCMCInspectScratch
{
	FString NetRoleToString(ENetRole Role)
	{
		switch (Role)
		{
		case ROLE_None:            return TEXT("None");
		case ROLE_SimulatedProxy:  return TEXT("SimulatedProxy");
		case ROLE_AutonomousProxy: return TEXT("AutonomousProxy");
		case ROLE_Authority:       return TEXT("Authority");
		default:                   return FString::Printf(TEXT("Unknown(%d)"),
															 static_cast<int32>(Role));
		}
	}

	// Build pendingAdjustment object from FClientAdjustment. Mirror of fields at
	// CharacterMovementReplication.h:256-288.
	TSharedPtr<FJsonObject> BuildPendingAdjustmentJson(
		const FClientAdjustment& Adj,
		UCharacterMovementComponent* CMC)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("timeStamp"), Adj.TimeStamp);
		Obj->SetNumberField(TEXT("deltaTime"), Adj.DeltaTime);
		SetVectorField(Obj, TEXT("newLoc"), Adj.NewLoc);
		SetVectorField(Obj, TEXT("newVel"), Adj.NewVel);
		SetRotatorField(Obj, TEXT("newRot"), Adj.NewRot);
		SetVectorField(Obj, TEXT("gravityDirection"), Adj.GravityDirection);

		if (Adj.NewBase)
		{
			AActor* BaseOwner = Adj.NewBase->GetOwner();
			Obj->SetStringField(TEXT("newBase"),
				BaseOwner ? BaseOwner->GetName() : Adj.NewBase->GetName());
		}
		else
		{
			Obj->SetField(TEXT("newBase"), MakeShared<FJsonValueNull>());
		}

		Obj->SetStringField(TEXT("newBaseBoneName"), Adj.NewBaseBoneName.ToString());
		Obj->SetBoolField(TEXT("ackGoodMove"), Adj.bAckGoodMove);
		Obj->SetBoolField(TEXT("baseRelativePosition"), Adj.bBaseRelativePosition);
		Obj->SetBoolField(TEXT("baseRelativeVelocity"), Adj.bBaseRelativeVelocity);

		// Decode the packed MovementMode via the CMC's UnpackNetworkMovementMode method
		// (declared at CharacterMovementComponent.h:1270). M1 review resolution.
		TEnumAsByte<EMovementMode> OutMode = MOVE_None;
		uint8 OutCustomMode = 0;
		TEnumAsByte<EMovementMode> OutGroundMode = MOVE_None;
		if (CMC)
		{
			CMC->UnpackNetworkMovementMode(Adj.MovementMode, OutMode, OutCustomMode, OutGroundMode);
		}
		Obj->SetStringField(TEXT("movementMode"), MovementModeToString(OutMode.GetValue()));
		Obj->SetNumberField(TEXT("customMovementMode"), static_cast<int32>(OutCustomMode));

		return Obj;
	}
}

IClaireonTool::FToolResult ClaireonTool_CMCInspectPredictionData::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonCMCInspectScratch;

	FString ActorId;
	ACharacter* Character = nullptr;
	TSharedPtr<FJsonObject> Data;
	IClaireonTool::FToolResult EarlyResult;
	if (!ResolveCharacterForTool(Arguments, TEXT("hasCMC"), ActorId, Character, Data, EarlyResult))
	{
		return EarlyResult;
	}

	UCharacterMovementComponent* CMC = Character->GetCharacterMovement();
	if (!CMC)
	{
		Data->SetBoolField(TEXT("hasCMC"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Character has no CharacterMovementComponent (GetCharacterMovement() returned null)"));
		return MakeSuccessResult(Data, TEXT("Character has no CMC"));
	}

	const ENetRole LocalRole = Character->GetLocalRole();
	Data->SetStringField(TEXT("netRole"), NetRoleToString(LocalRole));

	const bool bHasAuthority = (LocalRole == ROLE_Authority);
	const bool bIsLocallyControlled = Character->IsLocallyControlled();

	// Probe predictiondata-server lazily. The accessor lazy-allocates on first call.
	bool bNoMovesProcessed = false;

	if (bHasAuthority)
	{
		FNetworkPredictionData_Server_Character* ServerData =
			CMC->GetPredictionData_Server_Character();
		if (ServerData)
		{
			bNoMovesProcessed =
				(ServerData->LastUpdateTime == 0.f) &&
				(ServerData->CurrentClientTimeStamp == 0.f);

			TSharedPtr<FJsonObject> ServerObj = MakeShared<FJsonObject>();
			ServerObj->SetNumberField(TEXT("currentClientTimeStamp"), ServerData->CurrentClientTimeStamp);
			ServerObj->SetNumberField(TEXT("lastReceivedClientTimeStamp"),
				ServerData->LastReceivedClientTimeStamp);
			ServerObj->SetNumberField(TEXT("serverAccumulatedClientTimeStamp"),
				ServerData->ServerAccumulatedClientTimeStamp);
			ServerObj->SetNumberField(TEXT("lastUpdateTime"), ServerData->LastUpdateTime);
			ServerObj->SetNumberField(TEXT("serverTimeStampLastServerMove"),
				ServerData->ServerTimeStampLastServerMove);
			ServerObj->SetNumberField(TEXT("maxMoveDeltaTime"), ServerData->MaxMoveDeltaTime);
			ServerObj->SetBoolField(TEXT("forceClientUpdate"), ServerData->bForceClientUpdate != 0);

			ServerObj->SetObjectField(TEXT("pendingAdjustment"),
				BuildPendingAdjustmentJson(ServerData->PendingAdjustment, CMC));

			ServerObj->SetNumberField(TEXT("lifetimeRawTimeDiscrepancy"),
				ServerData->LifetimeRawTimeDiscrepancy);
			ServerObj->SetNumberField(TEXT("timeDiscrepancy"), ServerData->TimeDiscrepancy);
			ServerObj->SetBoolField(TEXT("resolvingTimeDiscrepancy"),
				ServerData->bResolvingTimeDiscrepancy);

			// reconciliation: the tracker-card "LastClientAdjustmentTime" /
			// "LastClientGoodMoveAckTime" fields live on the CMC itself, not on
			// FNetworkPredictionData_Server_Character. CMC fields are protected
			// UPROPERTY(Transient) at CharacterMovementComponent.h:662 / :666.
			// Read via FProperty reflection (same pattern as ClaireonPropertyUtils.cpp).
			auto ReadCMCFloatProp = [CMC](FName PropName, const TSharedPtr<FJsonObject>& Out, const FString& OutKey)
			{
				if (FProperty* P = CMC->GetClass()->FindPropertyByName(PropName))
				{
					if (FFloatProperty* FP = CastField<FFloatProperty>(P))
					{
						Out->SetNumberField(OutKey, FP->GetPropertyValue(P->ContainerPtrToValuePtr<void>(CMC)));
					}
					else if (FDoubleProperty* DP = CastField<FDoubleProperty>(P))
					{
						Out->SetNumberField(OutKey, DP->GetPropertyValue(P->ContainerPtrToValuePtr<void>(CMC)));
					}
				}
			};
			ReadCMCFloatProp(TEXT("ServerLastClientAdjustmentTime"), ServerObj,
				TEXT("serverLastClientAdjustmentTime"));
			ReadCMCFloatProp(TEXT("ServerLastClientGoodMoveAckTime"), ServerObj,
				TEXT("serverLastClientGoodMoveAckTime"));

			Data->SetObjectField(TEXT("server"), ServerObj);
		}
	}

	if (bIsLocallyControlled)
	{
		FNetworkPredictionData_Client_Character* ClientData =
			CMC->GetPredictionData_Client_Character();
		if (ClientData)
		{
			TSharedPtr<FJsonObject> ClientObj = MakeShared<FJsonObject>();
			ClientObj->SetNumberField(TEXT("currentTimeStamp"), ClientData->CurrentTimeStamp);
			ClientObj->SetNumberField(TEXT("lastReceivedAckRealTime"),
				ClientData->LastReceivedAckRealTime);
			ClientObj->SetNumberField(TEXT("clientUpdateRealTime"), ClientData->ClientUpdateRealTime);

			const int32 SavedMovesCount = ClientData->SavedMoves.Num();
			ClientObj->SetNumberField(TEXT("savedMovesCount"), SavedMovesCount);
			ClientObj->SetNumberField(TEXT("freeMovesCount"), ClientData->FreeMoves.Num());
			ClientObj->SetNumberField(TEXT("maxSavedMoveCount"), ClientData->MaxSavedMoveCount);
			ClientObj->SetNumberField(TEXT("maxFreeMoveCount"), ClientData->MaxFreeMoveCount);

			// Saved-move timestamp probes. Read FSavedMove_Character::TimeStamp (public,
			// CharacterMovementComponent.h:2928).
			if (SavedMovesCount > 0 && ClientData->SavedMoves[0].IsValid())
			{
				ClientObj->SetNumberField(TEXT("oldestSavedMoveTimeStamp"),
					ClientData->SavedMoves[0]->TimeStamp);
			}
			else
			{
				ClientObj->SetField(TEXT("oldestSavedMoveTimeStamp"), MakeShared<FJsonValueNull>());
			}
			if (SavedMovesCount > 0 && ClientData->SavedMoves.Last().IsValid())
			{
				ClientObj->SetNumberField(TEXT("newestSavedMoveTimeStamp"),
					ClientData->SavedMoves.Last()->TimeStamp);
			}
			else
			{
				ClientObj->SetField(TEXT("newestSavedMoveTimeStamp"), MakeShared<FJsonValueNull>());
			}

			if (ClientData->LastAckedMove.IsValid())
			{
				ClientObj->SetNumberField(TEXT("lastAckedMoveTimeStamp"),
					ClientData->LastAckedMove->TimeStamp);
			}
			else
			{
				ClientObj->SetField(TEXT("lastAckedMoveTimeStamp"), MakeShared<FJsonValueNull>());
			}

			if (ClientData->PendingMove.IsValid())
			{
				ClientObj->SetNumberField(TEXT("pendingMoveTimeStamp"),
					ClientData->PendingMove->TimeStamp);
			}
			else
			{
				ClientObj->SetField(TEXT("pendingMoveTimeStamp"), MakeShared<FJsonValueNull>());
			}

			ClientObj->SetBoolField(TEXT("updatePosition"), ClientData->bUpdatePosition != 0);
			ClientObj->SetNumberField(TEXT("maxMoveDeltaTime"), ClientData->MaxMoveDeltaTime);
			ClientObj->SetBoolField(TEXT("savedMovesNearLimit"),
				SavedMovesCount >= (ClientData->MaxSavedMoveCount - 4));

			Data->SetObjectField(TEXT("client"), ClientObj);
		}
	}

	Data->SetBoolField(TEXT("noMovesProcessed"), bNoMovesProcessed);

	const FString Summary = FString::Printf(
		TEXT("Prediction data for %s: role=%s%s%s"),
		*ActorId,
		*NetRoleToString(LocalRole),
		bHasAuthority ? TEXT(", server-side") : TEXT(""),
		bIsLocallyControlled ? TEXT(", client-side") : TEXT(""));

	return MakeSuccessResult(Data, Summary);
}
