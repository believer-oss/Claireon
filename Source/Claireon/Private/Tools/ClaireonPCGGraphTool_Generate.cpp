// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Generate.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "PCGSubsystem.h"
#include "UObject/UObjectIterator.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Generate::GetCategory() const { return TEXT("pcg"); }
FString ClaireonPCGGraphTool_Generate::GetOperation() const { return TEXT("generate"); }

FString ClaireonPCGGraphTool_Generate::GetDescription() const
{
	return TEXT("Run PCG generation on the placed components using a graph and wait for it to "
				"finish before returning, reporting each component's own managed instance count. "
				"UPCGComponent::Generate() only schedules work, so measuring in the call that "
				"triggered it reads the previous generation and looks like a broken graph. "
				"Stateless / non-session: addressed by asset_path or actor, editor world only.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Generate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("PCG graph asset path; every placed component using it is generated."));
	S.AddString(TEXT("actor_label"), TEXT("Generate only this actor's PCG component, by Outliner label."));
	S.AddInteger(TEXT("timeout_ms"), TEXT("How long to wait for generation to finish. Defaults to 30000; capped at 300000."));
	S.AddBoolean(TEXT("force"), TEXT("Regenerate even when PCG believes the component is up to date. Defaults to true."));
	return S.Build();
}

FToolResult ClaireonPCGGraphTool_Generate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	UWorld* EditorWorld = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!IsValid(EditorWorld))
	{
		return MakeErrorResult(TEXT("No editor world is loaded"));
	}

	FString AssetPath, ActorLabel;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (AssetPath.IsEmpty() && ActorLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Supply asset_path (generate every component using that graph) or actor_label"));
	}

	int32 TimeoutMs = 30000;
	Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);
	TimeoutMs = FMath::Clamp(TimeoutMs, 1000, 300000);

	bool bForce = true;
	Arguments->TryGetBoolField(TEXT("force"), bForce);

	TArray<UPCGComponent*> Components;
	int32 Skipped = 0;

	if (!AssetPath.IsEmpty())
	{
		FString LoadError;
		UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, LoadError);
		if (!IsValid(Graph))
		{
			return MakeErrorResult(LoadError);
		}
		ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(Graph, Components, Skipped, MaxComponentsPerCall);
		if (Components.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No placed PCGComponent in the editor world uses '%s', so there is nothing to "
					 "generate. Place an actor with a PCG component bound to it first."),
				*AssetPath));
		}
	}
	else
	{
		for (TObjectIterator<UPCGComponent> It; It; ++It)
		{
			UPCGComponent* Component = *It;
			if (!IsValid(Component) || Component->IsTemplate() || Component->GetWorld() != EditorWorld)
			{
				continue;
			}
			const AActor* Owner = Component->GetOwner();
			if (IsValid(Owner) && Owner->GetActorLabel() == ActorLabel)
			{
				Components.Add(Component);
			}
		}
		if (Components.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No actor labelled '%s' in the editor world has a PCG component"), *ActorLabel));
		}
	}

	int32 Frames = 0;
	FString GenerateError;
	const bool bFinished =
		ClaireonPCGGraphHelpers::GenerateAndWait(Components, TimeoutMs, bForce, Frames, GenerateError);
	const bool bTimedOut = !bFinished;

	// Count component-managed instances separately from owner-wide ISMs.
	// Deduplicate owners when totaling the latter.
	TArray<TSharedPtr<FJsonValue>> PerComponent;
	int32 TotalInstances = 0;
	int32 OwnerInstancesTotal = 0;
	TSet<const AActor*> CountedOwners;
	for (const UPCGComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		const int32 Instances = ClaireonPCGGraphHelpers::CountManagedISMInstances(Component);
		TotalInstances += Instances;

		const AActor* Owner = Component->GetOwner();
		int32 OwnerInstances = 0;
		if (IsValid(Owner))
		{
			Owner->ForEachComponent<UInstancedStaticMeshComponent>(/*bIncludeFromChildActors=*/true,
				[&OwnerInstances](const UInstancedStaticMeshComponent* Ism)
				{
					if (IsValid(Ism))
					{
						OwnerInstances += Ism->GetInstanceCount();
					}
				});
			if (!CountedOwners.Contains(Owner))
			{
				CountedOwners.Add(Owner);
				OwnerInstancesTotal += OwnerInstances;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), IsValid(Owner) ? Owner->GetActorLabel() : TEXT("(none)"));
		Entry->SetNumberField(TEXT("instances"), Instances);
		Entry->SetNumberField(TEXT("owner_ism_instances"), OwnerInstances);
		Entry->SetBoolField(TEXT("still_generating"), Component->IsGenerating());
		PerComponent.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("components"), Components.Num());
	Data->SetNumberField(TEXT("total_instances"), TotalInstances);
	Data->SetNumberField(TEXT("owner_instances_total"), OwnerInstancesTotal);
	Data->SetNumberField(TEXT("frames_pumped"), Frames);
	Data->SetBoolField(TEXT("timed_out"), bTimedOut);
	Data->SetArrayField(TEXT("per_component"), PerComponent);
	if (Skipped > 0)
	{
		Data->SetNumberField(TEXT("skipped_over_cap"), Skipped);
	}

	if (bTimedOut)
	{
		return MakeErrorResult(GenerateError);
	}

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("Generated %d component(s) in %d frames: %d instance(s)"),
		Components.Num(), Frames, TotalInstances));
}
