// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// File-local helpers shared across the decomposed State Tree edit tools.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonStateTreeEditToolBase.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeTypes.h"

namespace ClaireonStateTreeEditInternal
{
	inline bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid, FString& OutError)
	{
		FString GuidStr;
		if (!Params->TryGetStringField(FieldName, GuidStr) || GuidStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required parameter: %s"), *FieldName);
			return false;
		}
		if (!FGuid::Parse(GuidStr, OutGuid))
		{
			OutError = FString::Printf(TEXT("Invalid GUID for %s: %s"), *FieldName, *GuidStr);
			return false;
		}
		return true;
	}

	inline UStateTreeEditorData* GetEditorDataFromSession(FStateTreeEditToolData* Data, FString& OutError)
	{
		if (!Data || !Data->IsValid())
		{
			OutError = TEXT("Session is invalid");
			return nullptr;
		}
		return ClaireonStateTreeHelpers::GetEditorData(Data->StateTree.Get(), OutError);
	}

	inline EStateTreeStateType ParseStateType(const FString& TypeStr)
	{
		if (TypeStr == TEXT("Group"))
			return EStateTreeStateType::Group;
		if (TypeStr == TEXT("Linked"))
			return EStateTreeStateType::Linked;
		if (TypeStr == TEXT("LinkedAsset"))
			return EStateTreeStateType::LinkedAsset;
		if (TypeStr == TEXT("Subtree"))
			return EStateTreeStateType::Subtree;
		return EStateTreeStateType::State;
	}

	inline EStateTreeStateSelectionBehavior ParseSelectionBehavior(const FString& BehaviorStr)
	{
		if (BehaviorStr == TEXT("None"))
			return EStateTreeStateSelectionBehavior::None;
		if (BehaviorStr == TEXT("TryEnterState"))
			return EStateTreeStateSelectionBehavior::TryEnterState;
		if (BehaviorStr == TEXT("TrySelectChildrenAtRandom"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
		if (BehaviorStr == TEXT("TrySelectChildrenWithHighestUtility"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
		if (BehaviorStr == TEXT("TrySelectChildrenAtRandomWeightedByUtility"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility;
		if (BehaviorStr == TEXT("TryFollowTransitions"))
			return EStateTreeStateSelectionBehavior::TryFollowTransitions;
		return EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
	}

	inline EStateTreeTransitionTrigger ParseTransitionTrigger(const FString& TriggerStr)
	{
		if (TriggerStr == TEXT("OnStateCompleted"))
			return EStateTreeTransitionTrigger::OnStateCompleted;
		if (TriggerStr == TEXT("OnStateSucceeded"))
			return EStateTreeTransitionTrigger::OnStateSucceeded;
		if (TriggerStr == TEXT("OnStateFailed"))
			return EStateTreeTransitionTrigger::OnStateFailed;
		if (TriggerStr == TEXT("OnTick"))
			return EStateTreeTransitionTrigger::OnTick;
		if (TriggerStr == TEXT("OnEvent"))
			return EStateTreeTransitionTrigger::OnEvent;
		return EStateTreeTransitionTrigger::None;
	}

	inline EStateTreeTransitionType ParseTransitionType(const FString& TypeStr)
	{
		if (TypeStr == TEXT("GotoState"))
			return EStateTreeTransitionType::GotoState;
		if (TypeStr == TEXT("NextState"))
			return EStateTreeTransitionType::NextState;
		if (TypeStr == TEXT("NextSelectableState"))
			return EStateTreeTransitionType::NextSelectableState;
		if (TypeStr == TEXT("Succeeded"))
			return EStateTreeTransitionType::Succeeded;
		if (TypeStr == TEXT("Failed"))
			return EStateTreeTransitionType::Failed;
		return EStateTreeTransitionType::None;
	}

	inline EStateTreeTransitionPriority ParseTransitionPriority(const FString& PriorityStr)
	{
		if (PriorityStr == TEXT("Low"))
			return EStateTreeTransitionPriority::Low;
		if (PriorityStr == TEXT("Medium"))
			return EStateTreeTransitionPriority::Medium;
		if (PriorityStr == TEXT("High"))
			return EStateTreeTransitionPriority::High;
		if (PriorityStr == TEXT("Critical"))
			return EStateTreeTransitionPriority::Critical;
		if (PriorityStr == TEXT("None"))
			return EStateTreeTransitionPriority::None;
		return EStateTreeTransitionPriority::Normal;
	}

	inline void SetInitialProperties(FStateTreeEditorNode& Node, const TSharedPtr<FJsonObject>& PropsObj, UObject* Outer)
	{
		if (!PropsObj.IsValid())
			return;
		for (const auto& Pair : PropsObj->Values)
		{
			FString Value;
			if (Pair.Value->TryGetString(Value))
			{
				FString Error;
				// Try on node struct first, then instance data
				if (!ClaireonStateTreeHelpers::SetNodeProperty(Node, Pair.Key, Value, false, Error))
				{
					ClaireonStateTreeHelpers::SetNodeProperty(Node, Pair.Key, Value, true, Error);
				}
			}
		}
	}
}
