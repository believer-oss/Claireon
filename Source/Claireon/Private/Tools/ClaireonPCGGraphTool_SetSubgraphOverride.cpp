// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_SetSubgraphOverride.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSubgraph.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_SetSubgraphOverride::GetOperation() const { return TEXT("set_subgraph_override"); }

FString ClaireonPCGGraphTool_SetSubgraphOverride::GetDescription() const
{
	return TEXT("Set a per-instance override for one graph parameter of a Subgraph node, within an "
				"open editing session -- what the node's Overrides panel writes. The value is stored "
				"on the node's own graph instance and flagged overridden, so a later parameter "
				"refresh will not revert it to the referenced graph's default. Pass override=false "
				"to drop back to that default. Requires session_id from pcg_open.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_SetSubgraphOverride::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Subgraph node identifier (index or name)."), true);
	Builder.AddString(TEXT("parameter"),
		TEXT("Name of a graph parameter declared on the referenced subgraph, as listed in its "
			 "Graph Parameters panel."), true);
	Builder.AddString(TEXT("value"),
		TEXT("New value, as text in the property's import format (for example 0.35, true, or "
			 "/Game/Env/SM_Tree.SM_Tree). Omit to flag the current value as overridden without "
			 "changing it."));
	Builder.AddBoolean(TEXT("override"),
		TEXT("Defaults to true. False clears the override and restores the referenced graph's "
			 "default; it cannot be combined with value."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_SetSubgraphOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeIdentifier;
	if (!Arguments->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node"));
	}

	FString ParamName;
	if (!Arguments->TryGetStringField(TEXT("parameter"), ParamName) || ParamName.TrimStartAndEnd().IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parameter"));
	}
	ParamName = ParamName.TrimStartAndEnd();

	bool bMarkAsOverridden = true;
	Arguments->TryGetBoolField(TEXT("override"), bMarkAsOverridden);

	FString NewValue;
	const bool bHasValue = Arguments->TryGetStringField(TEXT("value"), NewValue);
	if (!bMarkAsOverridden && bHasValue)
	{
		return MakeErrorResult(TEXT("override=false clears the override and restores the referenced "
									"graph's default, so it takes no value"));
	}

	UPCGGraph* OwningGraph = Data->PCGGraph.Get();
	int32 NodeIndex = INDEX_NONE;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(OwningGraph, NodeIdentifier, NodeIndex);
	if (!IsValid(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Node->GetSettings());
	if (!IsValid(SubgraphSettings))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Node '%s' is not a Subgraph node (its settings are %s, not a UPCGBaseSubgraphSettings)"),
			*NodeIdentifier,
			IsValid(Node->GetSettings()) ? *Node->GetSettings()->GetClass()->GetName() : TEXT("null")));
	}

	// Overrides live on the node's wrapper instance, even when no graph is assigned.
	UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(SubgraphSettings->GetSubgraphInterface());
	if (!IsValid(Instance))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Node '%s' has no graph instance to hold overrides"), *NodeIdentifier));
	}

	// UpdatePropertyOverride silently returns when no graph is assigned.
	if (!IsValid(Instance->GetGraph()))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Node '%s' references no subgraph, so it has no parameters to override. Call "
				 "pcg_set_subgraph first."), *NodeIdentifier));
	}

	// Use the override bag's descriptor; its GUID keys the override flag.
	const FInstancedPropertyBag* OverrideBag = Instance->GetUserParametersStruct();
	const FPropertyBagPropertyDesc* Desc =
		OverrideBag ? OverrideBag->FindPropertyDescByName(FName(*ParamName)) : nullptr;
	if (!Desc || !Desc->CachedProperty)
	{
		const UPCGGraph* Referenced = Instance->GetGraph();
		return MakeErrorResult(FString::Printf(
			TEXT("'%s' is not a graph parameter of %s. Declare it there with pcg_add_user_parameter first."),
			*ParamName,
			IsValid(Referenced) ? *Referenced->GetPathName() : TEXT("the referenced graph")));
	}
	const FProperty* Property = Desc->CachedProperty;

	const bool bWasOverridden = Instance->IsPropertyOverridden(Property);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set PCG Subgraph Override")));

	// Capture before the direct bag write as well as the engine-managed flag changes.
	Instance->Modify();

	if (!bMarkAsOverridden)
	{
		Instance->UpdatePropertyOverride(Property, false);

		ClaireonPCGGraphHelpers::NotifyGraphChanged(OwningGraph, EPCGChangeType::Settings);

		Data->LastOperationStatus = bWasOverridden
			? FString::Printf(TEXT("Cleared override %s on %s"), *ParamName,
				*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node))
			: FString::Printf(TEXT("%s on %s was not overridden; nothing to clear"), *ParamName,
				*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node));

		FToolResult Result = BuildStateResponse(SessionId, Data);
		if (Result.Data.IsValid())
		{
			Result.Data->SetStringField(TEXT("parameter"), ParamName);
			Result.Data->SetBoolField(TEXT("overridden"), false);
			Result.Data->SetBoolField(TEXT("was_overridden"), bWasOverridden);
		}
		return Result;
	}

	// Cancel discards the undo record without restoring objects. Capture prior state
	// and restore it explicitly on failure before cancelling.
	FString PriorSerializedValue;
	bool bHadPriorValue = false;
	if (const FInstancedPropertyBag* ReadBag = Instance->GetUserParametersStruct())
	{
		TValueOrError<FString, EPropertyBagResult> Prior =
			ReadBag->GetValueSerializedString(FName(*ParamName));
		if (Prior.HasValue())
		{
			PriorSerializedValue = Prior.GetValue();
			bHadPriorValue = true;
		}
	}

	auto RestorePriorState = [&]()
	{
		Instance->UpdatePropertyOverride(Property, bWasOverridden);
		if (bHadPriorValue)
		{
			if (FInstancedPropertyBag* RestoreBag = Instance->GetMutableUserParametersStruct_Unsafe())
			{
				RestoreBag->SetValueSerializedString(FName(*ParamName), PriorSerializedValue);
			}
		}
	};

	Instance->UpdatePropertyOverride(Property, true);

	bool bValueApplied = false;
	if (bHasValue)
	{
		// Write the text value through the bag, then emit the setter's notification.
		FInstancedPropertyBag* MutableBag = Instance->GetMutableUserParametersStruct_Unsafe();
		const EPropertyBagResult SetResult = MutableBag
			? MutableBag->SetValueSerializedString(FName(*ParamName), NewValue)
			: EPropertyBagResult::PropertyNotFound;

		if (SetResult != EPropertyBagResult::Success)
		{
			RestorePriorState();
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("value '%s' could not be parsed as the type of '%s'. The override flag and "
				     "the previous value were restored; nothing on this instance changed."),
				*NewValue, *ParamName));
		}

		Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::ValueModifiedLocally, FName(*ParamName));
		bValueApplied = true;
	}

	// Verify the descriptor GUID was marked overridden; otherwise refresh reverts the value.
	if (!Instance->IsPropertyOverridden(Property))
	{
		RestorePriorState();
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("Marking '%s' as overridden did not take effect on this instance. Any value "
			     "written by this call was restored to its previous state."), *ParamName));
	}

	// Overrides affect settings without changing graph topology or requiring reconstruction.
	ClaireonPCGGraphHelpers::NotifyGraphChanged(OwningGraph, EPCGChangeType::Settings);

	Data->LastOperationStatus = FString::Printf(TEXT("Overrode %s on %s%s"),
		*ParamName, *ClaireonPCGGraphHelpers::GetNodeDisplayName(Node),
		bValueApplied ? TEXT(" with a new value") : TEXT(" (value unchanged)"));

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetStringField(TEXT("parameter"), ParamName);
		Result.Data->SetBoolField(TEXT("overridden"), true);
		Result.Data->SetBoolField(TEXT("was_overridden"), bWasOverridden);
		Result.Data->SetBoolField(TEXT("value_applied"), bValueApplied);
	}
	return Result;
}
