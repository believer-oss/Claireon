// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "IClaireonTool.h"
#include "ClaireonBlueprintHelpers.h"

struct FMCPSessionClosedInfo;

/**
 * Single source of truth for the "bp" category string returned by all
 * Blueprint-related Claireon tools (graph edit family, translate family, CDO
 * helpers, etc.). Using a shared constant keeps a future category rename to
 * a one-line edit. Declared inline constexpr (not static constexpr at
 * namespace scope) to avoid -Wunused-const-variable on Linux clang strict
 * when a TU references only a subset of consts.
 */
inline constexpr TCHAR kBPCategory[] = TEXT("bp");

/**
 * Shared base class for decomposed Blueprint graph editor MCP tools.
 *
 * Each operation is its own top-level MCP tool (ClaireonBlueprintGraphTool_Open,
 * ClaireonBlueprintGraphTool_AddNode, etc.). The shared session management, tool
 * data map, and common helpers live on this base class; each decomposed tool's
 * cpp hosts its own operation body as an out-of-line member function definition.
 *
 * The base class itself is NOT MCP-registered. All session lifecycle (touching,
 * lock tracking) remains in FClaireonSessionManager.
 */
class CLAIREON_API ClaireonBlueprintGraphEditToolBase : public IClaireonTool
{
public:
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetCategory() const override { return kBPCategory; }

protected:
	// ========================================================================
	// Session Delegate
	// ========================================================================

	/** Called by FClaireonSessionManager when any session is closed; cleans up our tool data. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Whether we have registered our delegate with the session manager. */
	static bool bDelegateRegistered;

	// ========================================================================
	// Helpers (defined in ClaireonBlueprintGraphEditToolBase.cpp)
	// ========================================================================

	FToolResult BuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data);

	/**
	 * Shared pre-op wrapping used by every session-requiring decomposed tool.
	 * Handles: params unwrap (legacy nested "params"), suppress_output/response_mode
	 * parsing, ResolveOrOpenSession, TouchSession, PreOpPinConnections snapshot,
	 * and LastOperationAffectedNodes clearing. Returns false on error (OutError populated).
	 */
	bool BeginSessionOp(
		const TSharedPtr<FJsonObject>& Arguments,
		const FString& OperationName,
		TSharedPtr<FJsonObject>& OutParams,
		FString& OutSessionId,
		FBlueprintEditToolData*& OutData,
		FToolResult& OutError);

	/**
	 * Debug warning helper used by mutation-style handlers: logs a warning if the
	 * handler succeeded in response_mode=changed but didn't record any affected nodes.
	 */
	FToolResult CheckMutationAffectedNodes(const FString& OpName, FBlueprintEditToolData* Data, const FToolResult& Result);

	bool ResolveOrOpenSession(
		const TSharedPtr<FJsonObject>& Params,
		const FString& OperationName,
		FString& OutSessionId,
		FBlueprintEditToolData*& OutData,
		FToolResult& OutError);

	void InitToolDataForSession(const FString& SessionId, UBlueprint* Blueprint, UEdGraph* Graph);

	void ValidateCursor(FBlueprintEditToolData* Data);

	FString BuildAvailableGraphsList(const UBlueprint* Blueprint) const;

	/**
	 * Resolve the target node for an operation that accepts either node_guid or
	 * node_title in its params. On success OutNode is set and returns true. On
	 * failure OutError carries a populated error result (caller returns it directly).
	 *
	 * Sole responsibility is (Params, Graph) -> (Node, Error). Does not mutate
	 * session data, does not participate in GUID-correction tracking, and does
	 * not auto-correct -- callers that need GUID correction keep using the
	 * session-aware path in ClaireonBPGraphInternal::FindNodeForOperation.
	 */
	static bool ResolveTargetNode(
		const TSharedPtr<FJsonObject>& Params,
		UEdGraph* Graph,
		UEdGraphNode*& OutNode,
		FToolResult& OutError);

	// Tool-specific data storage (keyed by session ID from FClaireonSessionManager).
	static TMap<FString, FBlueprintEditToolData> ToolData;

public:
	static FBlueprintEditToolData* FindToolData(const FString& SessionId) { return ToolData.Find(SessionId); }

	/**
	 * Public forwarder for the protected BuildStateResponse helper. Used by the
	 * AddInterface/ImplementInterface shared body that lives outside the class
	 * hierarchy (ClaireonBlueprintGraphInterfaceImpl::ApplyAddInterface). Internal
	 * callers should keep using the protected member directly.
	 */
	FToolResult PublicBuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data)
	{
		return BuildStateResponse(SessionId, Data);
	}
};

// Macro used by the decomposed tool headers to reduce boilerplate.
#define DECLARE_BPGRAPH_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonBlueprintGraphEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}

// Variant for read-only inspection tools that are safe during PIE. The default base
// class blocks all bp_graph ops while a PIE session is live; this variant relaxes
// that for tools that never mutate Blueprint state. apply_spec uses this too and
// gates non-dry-run inside Execute (see ClaireonBlueprintGraphTool_ApplySpec.cpp).
#define DECLARE_BPGRAPH_TOOL_PIE_OK(ClassName) \
	class CLAIREON_API ClassName : public ClaireonBlueprintGraphEditToolBase \
	{ \
	public: \
		bool RequiresNoPIE() const override { return false; } \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
