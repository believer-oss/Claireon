// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "IClaireonTool.h"
#include "ClaireonBlueprintHelpers.h"

struct FMCPSessionClosedInfo;

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
	virtual FString GetCategory() const override { return TEXT("bp"); }

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
