// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_InspectData.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Graph/PCGStackContext.h"
#include "Misc/EngineVersionComparison.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGPoint.h"
#include "PCGSubsystem.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_InspectData::GetCategory() const { return TEXT("pcg"); }
FString ClaireonPCGGraphTool_InspectData::GetOperation() const { return TEXT("inspect_data"); }

FString ClaireonPCGGraphTool_InspectData::GetDescription() const
{
	return TEXT("Retrieve the runtime data on a node's output pin after generation: point count, "
				"density and Z ranges, and min/max/mean for each numeric attribute. pcg_inspect "
				"reports static structure only, so checking whether a graph actually moved points "
				"otherwise meant spawning meshes and measuring instance transforms. "
				"Stateless / non-session: generates first unless told not to, editor world only.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_InspectData::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("PCG graph asset path. Required."));
	S.AddString(TEXT("node"), TEXT("Node index, title, or 'Input'/'Output'. Required."));
	S.AddString(TEXT("pin"), TEXT("Output pin label. Defaults to the node's first output pin."));
	S.AddString(TEXT("actor_label"), TEXT("Inspect only this actor's component; defaults to the first component using the graph."));
	S.AddBoolean(TEXT("generate"), TEXT("Regenerate before reading. Defaults to true; inspection data only exists for a generation that ran with inspection enabled."));
	S.AddInteger(TEXT("timeout_ms"), TEXT("How long to wait for that generation. Defaults to 30000."));
	return S.Build();
}

FToolResult ClaireonPCGGraphTool_InspectData::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString AssetPath, NodeId, PinLabel, ActorLabel;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("node"), NodeId);
	Arguments->TryGetStringField(TEXT("pin"), PinLabel);
	Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
	if (AssetPath.IsEmpty() || NodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("asset_path and node are both required"));
	}

	bool bGenerate = true;
	Arguments->TryGetBoolField(TEXT("generate"), bGenerate);
	int32 TimeoutMs = 30000;
	Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs);

	FString LoadError;
	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, LoadError);
	if (!IsValid(Graph))
	{
		return MakeErrorResult(LoadError);
	}

	int32 NodeIndex = INDEX_NONE;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Graph, NodeId, NodeIndex);
	if (!IsValid(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeId));
	}

	UPCGPin* Pin = nullptr;
	if (!PinLabel.IsEmpty())
	{
		Pin = Node->GetOutputPin(FName(*PinLabel));
		if (!IsValid(Pin))
		{
			TArray<FString> Available;
			for (const TObjectPtr<UPCGPin>& Candidate : Node->GetOutputPins())
			{
				if (Candidate)
				{
					Available.Add(Candidate->Properties.Label.ToString());
				}
			}
			return MakeErrorResult(FString::Printf(
				TEXT("Output pin '%s' not found on node %s. Available: %s"),
				*PinLabel, *ClaireonPCGGraphHelpers::GetNodeDisplayName(Node),
				Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
		}
	}
	else
	{
		const TArray<TObjectPtr<UPCGPin>>& OutputPins = Node->GetOutputPins();
		if (OutputPins.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Node %s has no output pins to inspect"),
				*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node)));
		}
		Pin = OutputPins[0];
	}

	// Apply actor filtering before the result cap.
	TArray<UPCGComponent*> Components;
	int32 SkippedOverCap = 0;
	ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(Graph, Components, SkippedOverCap,
		/*MaxComponents=*/32, ActorLabel);
	if (Components.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No placed PCGComponent in the editor world uses '%s'%s, so there is no runtime "
				 "data to inspect."),
			*AssetPath,
			ActorLabel.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" on actor '%s'"), *ActorLabel)));
	}

	// Inspection records only while enabled; enable it before generation.
	for (UPCGComponent* Component : Components)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		Component->EnableInspection();
#else
		Component->GetExecutionState().GetInspection().EnableInspection();
#endif
	}

	int32 Frames = 0;
	if (bGenerate)
	{
		FString GenerateError;
		if (!ClaireonPCGGraphHelpers::GenerateAndWait(Components, TimeoutMs, /*bForce=*/true, Frames, GenerateError))
		{
			return MakeErrorResult(GenerateError);
		}
	}

	UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(EditorWorld);
	if (!IsValid(Subsystem))
	{
		return MakeErrorResult(TEXT("The editor world has no UPCGSubsystem"));
	}

	int32 CollectionsFound = 0;
	ClaireonPCGGraphHelpers::FPointStatistics Stats;

	// Count non-point collections as found so missing data differs from zero points.
	auto Accumulate = [&](const FPCGDataCollection& Collection)
	{
		++CollectionsFound;
		for (const FPCGTaggedData& Tagged : Collection.TaggedData)
		{
			ClaireonPCGGraphHelpers::AccumulatePointData(Tagged.Data, Stats);
		}
	};

	for (UPCGComponent* Component : Components)
	{
		// Use recorded execution stacks as inspection keys.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		// Match the graph frame explicitly; nullptr matches no executed stacks.
		const TArray<FPCGStack> ExecutedStacks = Subsystem->GetExecutedStacks(Component, Graph);
		for (const FPCGStack& BaseStack : ExecutedStacks)
		{
			FPCGStack Stack = BaseStack;
			TArray<FPCGStackFrame>& Frames2 = Stack.GetStackFramesMutable();
			Frames2.Reserve(Frames2.Num() + 2);
			Frames2.Emplace(Node);
			Frames2.Emplace(Pin);

			if (const FPCGDataCollection* Collection = Component->GetInspectionData(Stack))
			{
				Accumulate(*Collection);
			}
		}
#else
		// From 5.8, inspect recorded node stacks through the execution source. Data is
		// valid only within the callback; append the pin frame before reading.
		const FPCGGraphExecutionInspection& Inspection = Component->GetExecutionState().GetInspection();
		for (const FPCGGraphExecutionInspection::FNodeExecutedNotificationData& Executed :
			Inspection.GetExecutedNodeStacks(Node))
		{
			FPCGStack Stack = Executed.Stack;
			Stack.GetStackFramesMutable().Emplace(Pin);
			Inspection.InspectData(Stack, [&Accumulate](const FPCGDataCollection& Collection)
			{
				Accumulate(Collection);
			});
		}
#endif
	}

	if (CollectionsFound == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No inspection data was recorded for %s.\"%s\". The node may not have executed on "
				 "this generation -- a node downstream of a filter that rejected everything, or one "
				 "on a branch that did not run, records nothing. Check pcg_inspect for the node's "
				 "connections, and confirm generate was not passed as false."),
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *Pin->Properties.Label.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node"), ClaireonPCGGraphHelpers::GetNodeDisplayName(Node));
	Data->SetStringField(TEXT("pin"), Pin->Properties.Label.ToString());
	Data->SetNumberField(TEXT("components"), Components.Num());
	Data->SetNumberField(TEXT("data_collections"), CollectionsFound);
	Data->SetNumberField(TEXT("points"), Stats.Points);
	Data->SetNumberField(TEXT("frames_pumped"), Frames);

	if (Stats.Points > 0)
	{
		Data->SetObjectField(TEXT("density"), Stats.Density.ToJson());
		Data->SetObjectField(TEXT("position_z"), Stats.PositionZ.ToJson());
	}

	TSharedPtr<FJsonObject> Attributes = MakeShared<FJsonObject>();
	for (const TPair<FName, ClaireonPCGGraphHelpers::FPointStat>& Pair : Stats.Attributes)
	{
		if (Pair.Value.Count > 0)
		{
			Attributes->SetObjectField(Pair.Key.ToString(), Pair.Value.ToJson());
		}
	}
	Data->SetObjectField(TEXT("attributes"), Attributes);

	if (Stats.NonNumericAttributes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FName& Name : Stats.NonNumericAttributes)
		{
			Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		Data->SetArrayField(TEXT("non_numeric_attributes"), Names);
	}

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("%s.\"%s\": %d point(s) across %d data collection(s)"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *Pin->Properties.Label.ToString(),
		Stats.Points, CollectionsFound));
}
