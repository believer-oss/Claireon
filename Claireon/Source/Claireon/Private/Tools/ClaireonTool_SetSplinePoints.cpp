// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SetSplinePoints.h"
#include "ClaireonLog.h"

#include "Components/SplineComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_SetSplinePoints::GetName() const
{
	return TEXT("claireon.set_spline_points");
}

FString ClaireonTool_SetSplinePoints::GetCategory() const
{
	return TEXT("level");
}

FString ClaireonTool_SetSplinePoints::GetDescription() const
{
	return TEXT("Set the points on a USplineComponent attached to an actor in the editor world. "
				"Replaces all existing spline points with the provided array. "
				"Supports per-point spline type (Linear, Curve, Constant, CurveClamped, CurveCustomTangent), "
				"closed loop toggle, and undo via FScopedTransaction.");
}

TSharedPtr<FJsonObject> ClaireonTool_SetSplinePoints::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorLabelProp = MakeShared<FJsonObject>();
	ActorLabelProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorLabelProp->SetStringField(TEXT("description"),
		TEXT("Label of the actor containing the spline component"));
	Properties->SetObjectField(TEXT("actor_label"), ActorLabelProp);

	TSharedPtr<FJsonObject> ComponentNameProp = MakeShared<FJsonObject>();
	ComponentNameProp->SetStringField(TEXT("type"), TEXT("string"));
	ComponentNameProp->SetStringField(TEXT("description"),
		TEXT("Optional name of the spline component (uses first USplineComponent if omitted)"));
	Properties->SetObjectField(TEXT("component_name"), ComponentNameProp);

	TSharedPtr<FJsonObject> PointsProp = MakeShared<FJsonObject>();
	PointsProp->SetStringField(TEXT("type"), TEXT("array"));
	PointsProp->SetStringField(TEXT("description"),
		TEXT("Array of spline points, each with location {x,y,z} and optional type string"));
	Properties->SetObjectField(TEXT("points"), PointsProp);

	TSharedPtr<FJsonObject> ClosedLoopProp = MakeShared<FJsonObject>();
	ClosedLoopProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ClosedLoopProp->SetStringField(TEXT("description"),
		TEXT("Whether the spline forms a closed loop (default: false)"));
	Properties->SetObjectField(TEXT("closed_loop"), ClosedLoopProp);

	TSharedPtr<FJsonObject> DefaultTypeProp = MakeShared<FJsonObject>();
	DefaultTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	DefaultTypeProp->SetStringField(TEXT("description"),
		TEXT("Default spline point type for points without explicit type (default: Curve). "
			 "Valid values: Linear, Curve, Constant, CurveClamped, CurveCustomTangent"));
	Properties->SetObjectField(TEXT("default_type"), DefaultTypeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_label")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("points")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace
{
	bool ParseSplinePointType(const FString& TypeStr, ESplinePointType::Type& OutType)
	{
		if (TypeStr.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))
		{
			OutType = ESplinePointType::Linear;
			return true;
		}
		if (TypeStr.Equals(TEXT("Curve"), ESearchCase::IgnoreCase))
		{
			OutType = ESplinePointType::Curve;
			return true;
		}
		if (TypeStr.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))
		{
			OutType = ESplinePointType::Constant;
			return true;
		}
		if (TypeStr.Equals(TEXT("CurveClamped"), ESearchCase::IgnoreCase))
		{
			OutType = ESplinePointType::CurveClamped;
			return true;
		}
		if (TypeStr.Equals(TEXT("CurveCustomTangent"), ESearchCase::IgnoreCase))
		{
			OutType = ESplinePointType::CurveCustomTangent;
			return true;
		}
		return false;
	}
} // namespace

FToolResult ClaireonTool_SetSplinePoints::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	checkf(GEditor && GEditor->GetEditorWorldContext().World(),
		TEXT("RequiresEditorWorld() tool reached Execute without a valid world. This indicates a dispatch path that bypasses precondition checks."));
	UWorld* World = GEditor->GetEditorWorldContext().World();

	FString ActorLabel;
	if (!Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: actor_label"));
	}

	const TArray<TSharedPtr<FJsonValue>>* PointsArray = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("points"), PointsArray) || !PointsArray)
	{
		return MakeErrorResult(TEXT("Missing required field: points"));
	}

	FString ComponentName;
	Arguments->TryGetStringField(TEXT("component_name"), ComponentName);

	bool bClosedLoop = false;
	Arguments->TryGetBoolField(TEXT("closed_loop"), bClosedLoop);

	FString DefaultTypeStr = TEXT("Curve");
	Arguments->TryGetStringField(TEXT("default_type"), DefaultTypeStr);

	ESplinePointType::Type DefaultType = ESplinePointType::Curve;
	if (!ParseSplinePointType(DefaultTypeStr, DefaultType))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid default_type: %s"), *DefaultTypeStr));
	}

	// Find actor by label
	AActor* TargetActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == ActorLabel)
		{
			TargetActor = Actor;
			break;
		}
	}

	if (!TargetActor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found with label: %s"), *ActorLabel));
	}

	// Find spline component
	USplineComponent* SplineComp = nullptr;
	if (!ComponentName.IsEmpty())
	{
		SplineComp = FindObject<USplineComponent>(TargetActor, *ComponentName);
	}
	else
	{
		SplineComp = TargetActor->FindComponentByClass<USplineComponent>();
	}

	if (!SplineComp)
	{
		if (!ComponentName.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(TEXT("SplineComponent '%s' not found on actor '%s'"), *ComponentName, *ActorLabel));
		}
		return MakeErrorResult(FString::Printf(TEXT("No USplineComponent found on actor '%s'"), *ActorLabel));
	}

	// Begin transaction for undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Spline Points")));
	SplineComp->Modify();

	// Clear existing points
	SplineComp->ClearSplinePoints(/*bUpdateSpline=*/false);

	// Add each point
	int32 PointCount = 0;
	for (int32 Idx = 0; Idx < PointsArray->Num(); ++Idx)
	{
		const TSharedPtr<FJsonObject>* PointObj = nullptr;
		if (!(*PointsArray)[Idx]->TryGetObject(PointObj) || !PointObj)
		{
			continue;
		}

		// Parse location
		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		FVector Location = FVector::ZeroVector;
		if ((*PointObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			(*LocationObj)->TryGetNumberField(TEXT("x"), X);
			(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
			(*LocationObj)->TryGetNumberField(TEXT("z"), Z);
			Location = FVector(X, Y, Z);
		}

		SplineComp->AddSplinePoint(Location, ESplineCoordinateSpace::World, /*bUpdateSpline=*/false);

		// Parse per-point type
		FString PointTypeStr;
		ESplinePointType::Type PointType = DefaultType;
		if ((*PointObj)->TryGetStringField(TEXT("type"), PointTypeStr) && !PointTypeStr.IsEmpty())
		{
			if (!ParseSplinePointType(PointTypeStr, PointType))
			{
				PointType = DefaultType; // Fall back to default on invalid type
			}
		}

		SplineComp->SetSplinePointType(Idx, PointType, /*bUpdateSpline=*/false);
		PointCount++;
	}

	// Set closed loop
	SplineComp->SetClosedLoop(bClosedLoop, /*bUpdateSpline=*/false);

	// Update spline (no-argument virtual overload)
	SplineComp->UpdateSpline();

	// Mark package dirty
	TargetActor->MarkPackageDirty();

	// Build result
	int32 FinalPointCount = SplineComp->GetNumberOfSplinePoints();
	float SplineLength = SplineComp->GetSplineLength();
	bool bIsClosedLoop = SplineComp->IsClosedLoop();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetNumberField(TEXT("point_count"), FinalPointCount);
	Data->SetNumberField(TEXT("spline_length"), SplineLength);
	Data->SetBoolField(TEXT("closed_loop"), bIsClosedLoop);

	FString Summary = FString::Printf(TEXT("Set %d spline points on '%s' (length: %.1f, closed: %s)"),
		FinalPointCount, *ActorLabel, SplineLength, bIsClosedLoop ? TEXT("true") : TEXT("false"));

	return MakeSuccessResult(Data, Summary);
}
