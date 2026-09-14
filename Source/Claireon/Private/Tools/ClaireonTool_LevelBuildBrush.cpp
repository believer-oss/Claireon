// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LevelBuildBrush.h"
#include "Tools/FToolSchemaBuilder.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Model.h"
#include "Engine/Polys.h"
#include "GameFramework/Volume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

namespace ClaireonLevelBuildBrush_Internal
{
	/** Match path, label, then class substring, as level_set_actor_property does. */
	AActor* FindActor(UWorld* World, const FString& Path, const FString& Label, const FString& ClassName, int32 Index)
	{
		int32 MatchCount = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor))
			{
				continue;
			}

			bool bMatch = false;
			if (!Path.IsEmpty())
			{
				bMatch = (Actor->GetPathName() == Path);
			}
			else if (!Label.IsEmpty())
			{
				bMatch = (Actor->GetActorLabel() == Label);
			}
			else if (!ClassName.IsEmpty())
			{
				bMatch = Actor->GetClass()->GetName().Contains(ClassName, ESearchCase::IgnoreCase);
			}

			if (bMatch)
			{
				if (MatchCount == Index)
				{
					return Actor;
				}
				++MatchCount;
			}
		}
		return nullptr;
	}

	double ReadAxis(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Default)
	{
		double Value = Default;
		return (Obj.IsValid() && Obj->TryGetNumberField(Key, Value)) ? Value : Default;
	}
}

FString ClaireonTool_LevelBuildBrush::GetCategory() const { return TEXT("level"); }
FString ClaireonTool_LevelBuildBrush::GetOperation() const { return TEXT("build_brush"); }

FString ClaireonTool_LevelBuildBrush::GetDescription() const
{
	return TEXT("Create box brush geometry on a placed Volume actor, addressed the same way as "
				"level_set_actor_property. A volume spawned by level_place_actor or "
				"audio_place_audio_volume has no brush, so its bounds are a zero-extent point and "
				"anything reading them (PCG generation domains, audio reverb, blocking) sees nothing. "
				"Stateless / non-session: edits the editor world inside a transaction.");
}

TSharedPtr<FJsonObject> ClaireonTool_LevelBuildBrush::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("actor_label"), TEXT("Actor label from the Outliner panel."));
	S.AddString(TEXT("actor_path"), TEXT("Full object path to the actor."));
	S.AddString(TEXT("actor_class"), TEXT("Class name for actor lookup, e.g. PCGVolume."));
	S.AddInteger(TEXT("actor_index"), TEXT("Disambiguation index when actor_class matches several actors (0-based). Defaults to 0."));
	S.AddObject(TEXT("extent"),
		TEXT("Half-size of the box in unscaled local units as {x, y, z}; the actor's own scale still "
			 "applies on top. Defaults to 100 on each axis (the engine's default 200-unit cube)."));
	return S.Build();
}

FToolResult ClaireonTool_LevelBuildBrush::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonLevelBuildBrush_Internal;

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	UWorld* World = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!IsValid(World))
	{
		return MakeErrorResult(TEXT("No editor world is loaded"));
	}

	FString ActorPath, ActorLabel, ActorClass;
	Arguments->TryGetStringField(TEXT("actor_path"), ActorPath);
	Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
	Arguments->TryGetStringField(TEXT("actor_class"), ActorClass);
	if (ActorPath.IsEmpty() && ActorLabel.IsEmpty() && ActorClass.IsEmpty())
	{
		return MakeErrorResult(TEXT("Supply one of actor_path, actor_label or actor_class"));
	}

	int32 ActorIndex = 0;
	Arguments->TryGetNumberField(TEXT("actor_index"), ActorIndex);

	AActor* Actor = FindActor(World, ActorPath, ActorLabel, ActorClass, ActorIndex);
	if (!IsValid(Actor))
	{
		return MakeErrorResult(TEXT("No actor matched the supplied label / path / class"));
	}

	AVolume* Volume = Cast<AVolume>(Actor);
	if (!IsValid(Volume))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Actor '%s' is a %s, not an AVolume; only volumes have buildable box geometry here "
				 "(additive/subtractive BSP brushes are a different, level-geometry workflow)"),
			*Actor->GetActorLabel(), *Actor->GetClass()->GetName()));
	}

	const TSharedPtr<FJsonObject>* ExtentObj = nullptr;
	Arguments->TryGetObjectField(TEXT("extent"), ExtentObj);
	const TSharedPtr<FJsonObject> Extent = ExtentObj ? *ExtentObj : nullptr;
	const double HalfX = ReadAxis(Extent, TEXT("x"), 100.0);
	const double HalfY = ReadAxis(Extent, TEXT("y"), 100.0);
	const double HalfZ = ReadAxis(Extent, TEXT("z"), 100.0);
	if (HalfX <= 0.0 || HalfY <= 0.0 || HalfZ <= 0.0)
	{
		return MakeErrorResult(TEXT("extent must be positive on every axis"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Build Brush")));
	Volume->Modify();

	UCubeBuilder* Builder = NewObject<UCubeBuilder>(Volume);
	// UCubeBuilder takes full dimensions; the tool accepts half-extents like GetActorBounds.
	Builder->X = HalfX * 2.0;
	Builder->Y = HalfY * 2.0;
	Builder->Z = HalfZ * 2.0;

	// Create the UModel and UPolys before building. Build alone reports success without
	// geometry when a newly spawned volume has no model.
	UActorFactory::CreateBrushForVolumeActor(Volume, Builder);

	ABrush* Brush = Volume;
	if (!IsValid(Brush->Brush) || !IsValid(Brush->Brush->Polys) || Brush->Brush->Polys->Element.Num() == 0)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("Brush geometry was not produced (no polys after build)"));
	}

	if (UBrushComponent* BrushComponent = Brush->GetBrushComponent(); IsValid(BrushComponent))
	{
		BrushComponent->Modify();
		BrushComponent->BuildSimpleBrushCollision();
		BrushComponent->MarkRenderStateDirty();
		BrushComponent->UpdateBounds();
	}
	Brush->MarkPackageDirty();

	FVector Origin = FVector::ZeroVector;
	FVector BoxExtent = FVector::ZeroVector;
	Brush->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, BoxExtent);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor"), Brush->GetActorLabel());
	Data->SetStringField(TEXT("actor_path"), Brush->GetPathName());
	TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
	BoundsJson->SetNumberField(TEXT("x"), BoxExtent.X);
	BoundsJson->SetNumberField(TEXT("y"), BoxExtent.Y);
	BoundsJson->SetNumberField(TEXT("z"), BoxExtent.Z);
	Data->SetObjectField(TEXT("world_extent"), BoundsJson);

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("Built box brush on %s (world half-extent %.0f x %.0f x %.0f)"),
		*Brush->GetActorLabel(), BoxExtent.X, BoxExtent.Y, BoxExtent.Z));
}
