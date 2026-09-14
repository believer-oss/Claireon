// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/IClaireonTool.h"
#include "UObject/Script.h"

class FCompilerResultsLog;
class UBlueprint;
class UEdGraph;
class UK2Node_FunctionEntry;

/** Shared function-flag updates and compiler-diagnostic capture for Blueprint authoring. */
namespace ClaireonBPFunctionRecipe
{
	/**
	 * Clear reliability along with net-mode flags so switching to non-networked
	 * does not leave FUNC_NetReliable set.
	 */
	inline constexpr int32 kNetModeClearMask =
		FUNC_Net | FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast | FUNC_NetReliable;

	/** Access flags cleared before applying the requested access mode. */
	inline constexpr int32 kAccessClearMask = FUNC_Public | FUNC_Protected | FUNC_Private;

	/** The function graph's UK2Node_FunctionEntry, or null. */
	CLAIREON_API UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* FunctionGraph);

	/** Caller-supplied phase wire strings and optional reconstruction steps. */
	struct FFlagRecipeSteps
	{
		/** Wire string journalled for the flag write itself. Required. */
		const TCHAR* FlagPhase = nullptr;

		/** Wire string journalled for the entry-node reconstruct. Required when bReconstructEntry. */
		const TCHAR* EntryReconstructionPhase = nullptr;

		/** Wire string journalled for the call-site broadcast. Required when bRefreshCallSites. */
		const TCHAR* CallsiteRefreshPhase = nullptr;

		/** Reconstruct the entry node with orphan-pin saving disabled when its shape or purity changes. */
		bool bReconstructEntry = true;

		/** Broadcast signature changes so loaded call sites reconstruct. */
		bool bRefreshCallSites = true;
	};

	/**
	 * Apply masks to entry ExtraFlags and the skeleton UFunction, then optionally
	 * reconstruct the entry without preserving orphan pins and refresh loaded, non-transient
	 * call sites. Mark structurally modified even when call-site refresh is skipped.
	 * Do not call during saving: HandleParameterDefaultValueChanged returns early then.
	 *
	 * @param SetMask Bits to set on both sites. May be zero.
	 * @param ClearMask Bits to clear before SetMask. May be zero.
	 * @return False with OutError populated before any writes if required inputs,
	 *         the entry node, or the skeleton UFunction are missing.
	 */
	CLAIREON_API bool ApplyFunctionFlagChange(
		UBlueprint* Blueprint,
		UEdGraph* FunctionGraph,
		int32 SetMask,
		int32 ClearMask,
		const FFlagRecipeSteps& Steps,
		FClaireonBPPhaseJournal& Journal,
		FString& OutError);

	/**
	 * Compiler diagnostic captured by value before reconstruction can invalidate weak
	 * node/pin references. Classify by Identifier, never node titles or formatted text.
	 */
	struct FCompilerDiagnostic
	{
		/** EMessageSeverity::Type. Lower is MORE severe: Error 1, PerformanceWarning 2, Warning 3, Info 4. */
		int32 Severity = 4;

		/** Wire label: "error" | "warning" | "note". */
		FString SeverityLabel;

		/** FTokenizedMessage::GetIdentifier(), or empty for an unclassified message. */
		FString Identifier;

		/** Formatted text. REPORTED, never matched on. */
		FString Message;

		/** True when at least one FEdGraphToken resolved to a graph node at collection time. */
		bool bHasNode = false;

		/** Captured from the token while the message was collected. */
		FGuid NodeGuid;

		/** Captured alongside NodeGuid when the token also carried a pin. */
		FString PinName;
	};

	/**
	 * Append diagnostics with graph identities captured by value. OutTokenless counts
	 * messages without a token resolving to a graph node.
	 */
	CLAIREON_API void CollectCompilerDiagnostics(
		const FCompilerResultsLog& Log,
		TArray<FCompilerDiagnostic>& OutDiagnostics,
		int32& OutTokenless);

	/** Bounded JSON render of the collected diagnostics. Nested Data only, never inline. */
	CLAIREON_API TArray<TSharedPtr<FJsonValue>> DiagnosticsToJson(
		const TArray<FCompilerDiagnostic>& Diagnostics);

	/**
	 * Attach diagnostics on every return that ran a compile. Counts and the truncation
	 * flag survive spills; the message array remains recoverable from the spill file.
	 */
	CLAIREON_API void AttachDiagnostics(
		IClaireonTool::FToolResult& Result,
		const TArray<FCompilerDiagnostic>& Diagnostics,
		int32 TokenlessDiagnostics);
}
