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
	virtual FString GetCategory() const override { return TEXT("blueprint_graph"); }

protected:
	// ========================================================================
	// Session Delegate
	// ========================================================================

	/** Called by FClaireonSessionManager when any session is closed; cleans up our tool data. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Whether we have registered our delegate with the session manager. */
	static bool bDelegateRegistered;

	// ========================================================================
	// Operation member-function declarations.
	//
	// These stay on the base class so every decomposed tool's Execute() can
	// invoke a single consistent signature for its underlying operation. Each
	// body is defined in the corresponding decomposed tool's cpp (e.g.
	// Operation_AddComponent lives in ClaireonBlueprintGraphTool_AddComponent.cpp
	// as an out-of-line definition of ClaireonBlueprintGraphEditToolBase::
	// Operation_AddComponent). Cross-TU linkage is normal C++: member functions
	// can be defined in any translation unit that has the class declaration.
	// ========================================================================

	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ListGraphs(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNodeStateless(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReconstructNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReconstructNodeStateless(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ConnectPins(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_DisconnectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetPinValue(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddVariable(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetVariableProperties(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveVariable(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetProperty(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetGameplayTags(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveCursor(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_CursorBack(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SwitchGraph(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_InspectNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SelectNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SelectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SelectNearestNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetState(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ImportNodes(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Compile(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Format(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemovePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SplitPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RecombinePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReparentComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetRootComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetComponentDetails(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddFunctionOverride(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddInterface(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveInterface(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SuggestNode(const TSharedPtr<FJsonObject>& Params);

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

	// Tool-specific data storage (keyed by session ID from FClaireonSessionManager).
	static TMap<FString, FBlueprintEditToolData> ToolData;

public:
	static FBlueprintEditToolData* FindToolData(const FString& SessionId) { return ToolData.Find(SessionId); }
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
