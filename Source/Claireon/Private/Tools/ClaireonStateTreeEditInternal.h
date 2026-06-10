// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// File-local helpers shared across the decomposed State Tree edit tools.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ClaireonSettings.h"
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

	inline TOptional<EStateTreeTransitionType> TryParseTransitionType(const FString& TypeStr)
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
		if (TypeStr == TEXT("None"))
			return EStateTreeTransitionType::None;
		return {};
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

	// Resolves placement using precedence: index > insert_before > insert_after.
	// Negative or out-of-range `index` is silently clamped to [0, DefaultEndIndex].
	// If `insert_before` / `insert_after` GUIDs do not resolve to an entry in
	// `Children`, returns false with `OutError` describing the unknown GUID.
	// If none of the three params is supplied, returns true with `OutIndex = DefaultEndIndex`.
	// If multiple are supplied, the highest-precedence param drives `OutIndex` and
	// `OutResolutionNote` records the precedence resolution for the response envelope.
	inline bool ResolveInsertionIndex(
		const TArray<TObjectPtr<UStateTreeState>>& Children,
		const TSharedPtr<FJsonObject>& Args,
		int32 DefaultEndIndex,
		int32& OutIndex,
		FString& OutResolutionNote,
		FString& OutError)
	{
		OutIndex = DefaultEndIndex;
		OutResolutionNote.Reset();
		OutError.Reset();

		// Detect what was supplied.
		int32 RawIndex = INDEX_NONE;
		const bool bHasIndex = Args.IsValid() && Args->TryGetNumberField(TEXT("index"), RawIndex);

		FString InsertBeforeStr;
		const bool bHasInsertBefore = Args.IsValid()
			&& Args->TryGetStringField(TEXT("insert_before"), InsertBeforeStr)
			&& !InsertBeforeStr.IsEmpty();

		FString InsertAfterStr;
		const bool bHasInsertAfter = Args.IsValid()
			&& Args->TryGetStringField(TEXT("insert_after"), InsertAfterStr)
			&& !InsertAfterStr.IsEmpty();

		const int32 SuppliedCount = (bHasIndex ? 1 : 0) + (bHasInsertBefore ? 1 : 0) + (bHasInsertAfter ? 1 : 0);

		// index (highest precedence)
		if (bHasIndex)
		{
			OutIndex = FMath::Clamp(RawIndex, 0, DefaultEndIndex);
			if (SuppliedCount > 1)
			{
				OutResolutionNote = TEXT("placement resolved via 'index' over also-supplied lower-precedence params");
			}
			return true;
		}

		// insert_before
		if (bHasInsertBefore)
		{
			FGuid TargetGuid;
			if (!FGuid::Parse(InsertBeforeStr, TargetGuid))
			{
				OutError = FString::Printf(TEXT("Invalid insert_before GUID: %s"), *InsertBeforeStr);
				return false;
			}
			const int32 Found = Children.IndexOfByPredicate(
				[&TargetGuid](const TObjectPtr<UStateTreeState>& Child)
				{ return Child && Child->ID == TargetGuid; });
			if (Found == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("insert_before GUID %s does not match any sibling"), *InsertBeforeStr);
				return false;
			}
			OutIndex = Found;
			if (SuppliedCount > 1)
			{
				OutResolutionNote = TEXT("placement resolved via 'insert_before' over also-supplied 'insert_after'");
			}
			return true;
		}

		// insert_after (lowest precedence)
		if (bHasInsertAfter)
		{
			FGuid TargetGuid;
			if (!FGuid::Parse(InsertAfterStr, TargetGuid))
			{
				OutError = FString::Printf(TEXT("Invalid insert_after GUID: %s"), *InsertAfterStr);
				return false;
			}
			const int32 Found = Children.IndexOfByPredicate(
				[&TargetGuid](const TObjectPtr<UStateTreeState>& Child)
				{ return Child && Child->ID == TargetGuid; });
			if (Found == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("insert_after GUID %s does not match any sibling"), *InsertAfterStr);
				return false;
			}
			OutIndex = Found + 1;
			return true;
		}

		// Nothing supplied: append.
		return true;
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

	// Applies the spill-budget truncation policy to a single string field on `Data`.
	// Mutates `Data` in place:
	//   - InlineFieldName: written with a truncated-with-hint copy if `FullText` exceeds
	//     the inline budget, otherwise the full text byte-for-byte.
	//   - FullFieldName: written with the original full-fidelity text iff truncation
	//     triggered (so the gate can route it to disk if it alone exceeds threshold).
	//   - <InlineFieldName>_truncated, _truncation_bytes, _full_field: structured
	//     detection scalars, only set when truncation triggers.
	//
	// The inline budget is `UClaireonSettings::Get()->ResultSpillThresholdBytes / 4`,
	// re-read at every call so an operator's settings change takes effect on the next
	// tool invocation. The budget is measured in UTF-8 bytes; the slice respects UTF-8
	// code-point boundaries (it never lands in the middle of a multi-byte character).
	inline void ApplyStructuredSpill(
		FJsonObject& Data,
		const TCHAR* InlineFieldName,
		const TCHAR* FullFieldName,
		const FString& FullText)
	{
		const int32 ThresholdBytes = UClaireonSettings::Get()->ResultSpillThresholdBytes;
		const int32 BudgetBytes = ThresholdBytes / 4;

		// Convert to UTF-8 to measure the real byte cost.
		FTCHARToUTF8 Utf8(*FullText);
		const int32 Utf8Length = Utf8.Length();

		if (Utf8Length <= BudgetBytes)
		{
			// No truncation needed. Inline shape is byte-identical to today.
			Data.SetStringField(InlineFieldName, FullText);
			return;
		}

		// Find a UTF-8 code-point boundary at or before BudgetBytes. Bytes 0x80-0xBF
		// (binary 10xxxxxx) are continuation bytes; back up to a leading byte.
		const ANSICHAR* Utf8Bytes = Utf8.Get();
		int32 CutBytes = BudgetBytes;
		while (CutBytes > 0 && (static_cast<uint8>(Utf8Bytes[CutBytes]) & 0xC0) == 0x80)
		{
			--CutBytes;
		}

		// Convert the truncated UTF-8 prefix back to TCHAR for FString assembly.
		FUTF8ToTCHAR Truncated(Utf8Bytes, CutBytes);
		FString InlineText(Truncated.Length(), Truncated.Get());
		InlineText += FString::Printf(TEXT("\n[truncated; full text in field '%s']"), FullFieldName);

		Data.SetStringField(InlineFieldName, InlineText);
		Data.SetStringField(FullFieldName, FullText);

		// Structured detection trio. Agents must read these, not string-match the hint.
		const FString TruncatedKey = FString::Printf(TEXT("%s_truncated"), InlineFieldName);
		const FString TruncationBytesKey = FString::Printf(TEXT("%s_truncation_bytes"), InlineFieldName);
		const FString FullFieldKey = FString::Printf(TEXT("%s_full_field"), InlineFieldName);
		Data.SetBoolField(TruncatedKey, true);
		Data.SetNumberField(TruncationBytesKey, BudgetBytes);
		Data.SetStringField(FullFieldKey, FullFieldName);
	}
}
