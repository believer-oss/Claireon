// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimInspectTool.h"

#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "MotionWarpingComponent.h"
#include "RootMotionModifier.h"
#include "UObject/UnrealType.h"

namespace ClaireonAnimInspectScratch
{
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

	bool ResolveActorForTool(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutActorId,
		AActor*& OutActor,
		IClaireonTool::FToolResult& OutResult)
	{
		OutActor = nullptr;

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
		OutActor = Actor;
		return true;
	}

	FString VisibilityBasedAnimTickOptionToString(EVisibilityBasedAnimTickOption InOption)
	{
		switch (InOption)
		{
		case EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones:
			return TEXT("AlwaysTickPoseAndRefreshBones");
		case EVisibilityBasedAnimTickOption::AlwaysTickPose:
			return TEXT("AlwaysTickPose");
		case EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered:
			return TEXT("OnlyTickMontagesWhenNotRendered");
		case EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered:
			return TEXT("OnlyTickPoseWhenRendered");
		default:
			return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InOption));
		}
	}
}

// =============================================================================
// Tool 4: anim_inspect_montages
// =============================================================================

FString ClaireonTool_AnimInspectMontages::GetDescription() const
{
	return TEXT("Snapshot the UAnimInstance montage state on a paused-PIE pawn: enumerate every ")
	       TEXT("FAnimMontageInstance with position, weight, blend time, current section, and play/active/")
	       TEXT("stopped state. Includes the skeletal mesh component's tick + visibility settings. ")
	       TEXT("Targets anim-not-ticking, montage-not-advancing, and stuck-blending-out bug classes.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimInspectMontages::GetInputSchema() const
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

FString ClaireonTool_AnimInspectMontages::GetExampleUsage() const
{
	return TEXT("anim_inspect_montages(actorId=\"actor_0\")");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimInspectMontages::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	return Tooltips;
}

TArray<FString> ClaireonTool_AnimInspectMontages::GetSearchKeywords() const
{
	return {
		TEXT("anim not ticking"),
		TEXT("montage"),
		TEXT("blending out"),
		TEXT("FAnimMontageInstance"),
		TEXT("current section")
	};
}

IClaireonTool::FToolResult ClaireonTool_AnimInspectMontages::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonAnimInspectScratch;

	FString ActorId;
	AActor* Actor = nullptr;
	IClaireonTool::FToolResult EarlyResult;
	if (!ResolveActorForTool(Arguments, ActorId, Actor, EarlyResult))
	{
		return EarlyResult;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	ACharacter* Character = Cast<ACharacter>(Actor);
	if (!Character)
	{
		Data->SetBoolField(TEXT("hasAnimInstance"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Actor is not an ACharacter; no skeletal mesh / AnimInstance"));
		return MakeSuccessResult(Data, TEXT("Actor has no AnimInstance (not an ACharacter)"));
	}

	USkeletalMeshComponent* Mesh = Character->GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;

	if (!AnimInstance)
	{
		Data->SetBoolField(TEXT("hasAnimInstance"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Character mesh has no UAnimInstance bound (GetAnimInstance() returned null)"));
		return MakeSuccessResult(Data, TEXT("Character has no AnimInstance"));
	}

	Data->SetBoolField(TEXT("hasAnimInstance"), true);
	Data->SetStringField(TEXT("animInstanceClass"), AnimInstance->GetClass()->GetPathName());
	UClass* ParentClass = AnimInstance->GetClass()->GetSuperClass();
	Data->SetStringField(TEXT("animInstanceParentClass"),
		ParentClass ? ParentClass->GetPathName() : FString());

	// Skeletal mesh extras.
	{
		TSharedPtr<FJsonObject> MeshObj = MakeShared<FJsonObject>();
		MeshObj->SetStringField(TEXT("name"), Mesh->GetName());
		MeshObj->SetBoolField(TEXT("componentTickEnabled"), Mesh->IsComponentTickEnabled());
		MeshObj->SetStringField(TEXT("visibilityBasedAnimTickOption"),
			VisibilityBasedAnimTickOptionToString(Mesh->VisibilityBasedAnimTickOption));
		MeshObj->SetBoolField(TEXT("recentlyRendered"), Mesh->WasRecentlyRendered());
		Data->SetObjectField(TEXT("skeletalMeshComponent"), MeshObj);
	}

	// Active montage (top-of-stack).
	UAnimMontage* ActiveMontage = AnimInstance->GetCurrentActiveMontage();
	if (ActiveMontage)
	{
		Data->SetStringField(TEXT("activeMontageName"), ActiveMontage->GetName());
	}
	else
	{
		Data->SetField(TEXT("activeMontageName"), MakeShared<FJsonValueNull>());
	}

	// Enumerate montage instances. The MontageInstances array on UAnimInstance is public
	// (AnimInstance.h:770, public block opened at :733).
	bool bAnyPlaying = false;
	TArray<TSharedPtr<FJsonValue>> InstancesArr;
	for (FAnimMontageInstance* Instance : AnimInstance->MontageInstances)
	{
		if (!Instance || !Instance->IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> InstObj = MakeShared<FJsonObject>();
		InstObj->SetNumberField(TEXT("instanceId"), Instance->GetInstanceID());

		const UAnimMontage* Montage = Instance->Montage;
		InstObj->SetStringField(TEXT("montageName"),
			Montage ? Montage->GetName() : FString(TEXT("<null>")));

		const float Position = Instance->GetPosition();
		const float Length = Montage ? Montage->GetPlayLength() : 0.f;
		InstObj->SetNumberField(TEXT("position"), Position);
		InstObj->SetNumberField(TEXT("length"), Length);
		InstObj->SetNumberField(TEXT("playRate"), Instance->GetPlayRate());
		InstObj->SetNumberField(TEXT("weight"), Instance->GetWeight());
		InstObj->SetNumberField(TEXT("desiredWeight"), Instance->GetDesiredWeight());

		const float BlendTime = Instance->GetBlendTime();
		InstObj->SetNumberField(TEXT("blendTime"), BlendTime);
		// Derived blendingOut heuristic: stopped (desiredWeight==0) and still has a blend time.
		// Documented in per-area doc as the only public way to distinguish blend-in vs blend-out.
		InstObj->SetBoolField(TEXT("blendingOut"), Instance->IsStopped() && BlendTime > 0.f);

		InstObj->SetStringField(TEXT("currentSection"), Instance->GetCurrentSection().ToString());

		const bool bActive = Instance->IsActive();
		const bool bStopped = Instance->IsStopped();
		const bool bPlaying = Instance->IsPlaying();
		InstObj->SetBoolField(TEXT("active"), bActive);
		InstObj->SetBoolField(TEXT("stopped"), bStopped);
		InstObj->SetBoolField(TEXT("playing"), bPlaying);
		InstObj->SetBoolField(TEXT("rootMotionDisabled"), Instance->IsRootMotionDisabled());

		if (Montage)
		{
			InstObj->SetStringField(TEXT("montageGroupName"), Montage->GetGroupName().ToString());
		}
		else
		{
			InstObj->SetStringField(TEXT("montageGroupName"), FString());
		}

		if (bPlaying)
		{
			bAnyPlaying = true;
		}

		InstancesArr.Add(MakeShared<FJsonValueObject>(InstObj));
	}
	Data->SetBoolField(TEXT("anyMontagePlaying"), bAnyPlaying);
	Data->SetArrayField(TEXT("montageInstances"), InstancesArr);

	const FString Summary = FString::Printf(
		TEXT("Anim montages for %s: %d instance(s), %s"),
		*ActorId,
		InstancesArr.Num(),
		bAnyPlaying ? TEXT("anyPlaying=true") : TEXT("anyPlaying=false"));

	return MakeSuccessResult(Data, Summary);
}

// =============================================================================
// Tool 5: anim_inspect_motion_warping (implementation in S7)
// =============================================================================

FString ClaireonTool_AnimInspectMotionWarping::GetDescription() const
{
	return TEXT("Snapshot UMotionWarpingComponent state on a paused-PIE pawn: enumerate active root ")
	       TEXT("motion modifiers (with state, animation, time window, weight) and warp targets ")
	       TEXT("(with location, rotation, follow component, offsets). Targets motion-warp-not-firing ")
	       TEXT("and warp-target-stale bug classes.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimInspectMotionWarping::GetInputSchema() const
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

FString ClaireonTool_AnimInspectMotionWarping::GetExampleUsage() const
{
	return TEXT("anim_inspect_motion_warping(actorId=\"actor_0\")");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimInspectMotionWarping::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	return Tooltips;
}

TArray<FString> ClaireonTool_AnimInspectMotionWarping::GetSearchKeywords() const
{
	return {
		TEXT("motion warping"),
		TEXT("warp target"),
		TEXT("root motion warp"),
		TEXT("landing target")
	};
}

namespace ClaireonAnimInspectScratch
{
	FString RootMotionModifierStateToString(ERootMotionModifierState State)
	{
		switch (State)
		{
		case ERootMotionModifierState::Waiting:          return TEXT("Waiting");
		case ERootMotionModifierState::Active:           return TEXT("Active");
		case ERootMotionModifierState::MarkedForRemoval: return TEXT("MarkedForRemoval");
		case ERootMotionModifierState::Disabled:         return TEXT("Disabled");
		default: return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(State));
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
}

IClaireonTool::FToolResult ClaireonTool_AnimInspectMotionWarping::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonAnimInspectScratch;

	FString ActorId;
	AActor* Actor = nullptr;
	IClaireonTool::FToolResult EarlyResult;
	if (!ResolveActorForTool(Arguments, ActorId, Actor, EarlyResult))
	{
		return EarlyResult;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	UMotionWarpingComponent* WarpComp = Actor->FindComponentByClass<UMotionWarpingComponent>();
	if (!WarpComp)
	{
		Data->SetBoolField(TEXT("hasMotionWarpingComponent"), false);
		Data->SetStringField(TEXT("reason"),
			TEXT("Actor has no UMotionWarpingComponent"));
		return MakeSuccessResult(Data, TEXT("Actor has no MotionWarpingComponent"));
	}

	Data->SetBoolField(TEXT("hasMotionWarpingComponent"), true);

	// Read bSearchForWindowsInAnimsWithinMontages via reflection (it's a public protected
	// UPROPERTY at MotionWarpingComponent.h:96; emit defensively in case it's protected).
	if (FProperty* SearchProp = WarpComp->GetClass()->FindPropertyByName(
			TEXT("bSearchForWindowsInAnimsWithinMontages")))
	{
		if (FBoolProperty* BP = CastField<FBoolProperty>(SearchProp))
		{
			Data->SetBoolField(TEXT("searchForWindowsInAnimsWithinMontages"),
				BP->GetPropertyValue(SearchProp->ContainerPtrToValuePtr<void>(WarpComp)));
		}
	}

	// Modifiers via the public GetModifiers() accessor.
	const TArray<URootMotionModifier*>& Modifiers = WarpComp->GetModifiers();
	Data->SetNumberField(TEXT("modifierCount"), Modifiers.Num());

	TArray<TSharedPtr<FJsonValue>> ModifiersArr;
	for (URootMotionModifier* Modifier : Modifiers)
	{
		if (!Modifier)
		{
			continue;
		}
		TSharedPtr<FJsonObject> ModObj = MakeShared<FJsonObject>();
		ModObj->SetStringField(TEXT("className"), Modifier->GetClass()->GetName());

		const UAnimSequenceBase* Anim = Modifier->Animation.Get();
		if (Anim)
		{
			ModObj->SetStringField(TEXT("animation"), Anim->GetPathName());
		}
		else
		{
			ModObj->SetField(TEXT("animation"), MakeShared<FJsonValueNull>());
		}

		ModObj->SetNumberField(TEXT("startTime"), Modifier->StartTime);
		ModObj->SetNumberField(TEXT("endTime"), Modifier->EndTime);
		ModObj->SetNumberField(TEXT("previousPosition"), Modifier->PreviousPosition);
		ModObj->SetNumberField(TEXT("currentPosition"), Modifier->CurrentPosition);
		ModObj->SetNumberField(TEXT("weight"), Modifier->Weight);
		ModObj->SetNumberField(TEXT("actualStartTime"), Modifier->ActualStartTime);

		const ERootMotionModifierState State = Modifier->GetState();
		ModObj->SetStringField(TEXT("state"), RootMotionModifierStateToString(State));
		ModObj->SetBoolField(TEXT("active"), State == ERootMotionModifierState::Active);

		// WarpTargetName is only on URootMotionModifier_Warp subclass.
		if (URootMotionModifier_Warp* WarpMod = Cast<URootMotionModifier_Warp>(Modifier))
		{
			ModObj->SetStringField(TEXT("warpTargetName"), WarpMod->WarpTargetName.ToString());
		}
		else
		{
			ModObj->SetField(TEXT("warpTargetName"), MakeShared<FJsonValueNull>());
		}

		ModifiersArr.Add(MakeShared<FJsonValueObject>(ModObj));
	}
	Data->SetArrayField(TEXT("modifiers"), ModifiersArr);

	// WarpTargets via reflection on the protected UPROPERTY at MotionWarpingComponent.h:215.
	// Mirror of ClaireonPropertyUtils.cpp:103-110, 401-404, 504, 538, 593 pattern.
	TArray<TSharedPtr<FJsonValue>> WarpTargetsArr;
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(
			WarpComp->GetClass()->FindPropertyByName(TEXT("WarpTargets"))))
	{
		FScriptArrayHelper ArrayHelper(ArrayProp,
			ArrayProp->ContainerPtrToValuePtr<void>(WarpComp));

		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			const FMotionWarpingTarget* Target =
				reinterpret_cast<const FMotionWarpingTarget*>(ArrayHelper.GetRawPtr(Index));
			if (!Target)
			{
				continue;
			}

			TSharedPtr<FJsonObject> TgtObj = MakeShared<FJsonObject>();
			TgtObj->SetStringField(TEXT("name"), Target->Name.ToString());
			SetVectorField(TgtObj, TEXT("location"), Target->Location);
			SetRotatorField(TgtObj, TEXT("rotation"), Target->Rotation);

			const USceneComponent* Comp = Target->Component.Get();
			AActor* CompOwner = Comp ? Comp->GetOwner() : nullptr;
			if (CompOwner)
			{
				TgtObj->SetStringField(TEXT("componentOwner"), CompOwner->GetName());
			}
			else
			{
				TgtObj->SetField(TEXT("componentOwner"), MakeShared<FJsonValueNull>());
			}

			TgtObj->SetStringField(TEXT("boneName"), Target->BoneName.ToString());
			TgtObj->SetBoolField(TEXT("followComponent"), Target->bFollowComponent);
			SetVectorField(TgtObj, TEXT("locationOffset"), Target->LocationOffset);
			SetRotatorField(TgtObj, TEXT("rotationOffset"), Target->RotationOffset);

			WarpTargetsArr.Add(MakeShared<FJsonValueObject>(TgtObj));
		}
	}
	Data->SetArrayField(TEXT("warpTargets"), WarpTargetsArr);

	const FString Summary = FString::Printf(
		TEXT("Motion warping for %s: %d modifier(s), %d warp target(s)"),
		*ActorId,
		ModifiersArr.Num(),
		WarpTargetsArr.Num());

	return MakeSuccessResult(Data, Summary);
}
