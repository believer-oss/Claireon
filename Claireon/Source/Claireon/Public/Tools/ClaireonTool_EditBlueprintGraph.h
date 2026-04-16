// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "IClaireonTool.h"
#include "ClaireonBlueprintHelpers.h"

struct FMCPSessionClosedInfo;

/**
 * MCP tool for interactive Blueprint graph editing using a cursor-based model.
 * Supports creating/opening Blueprints, adding/removing nodes, connecting pins,
 * and managing Blueprint variables and components.
 *
 * Session lifecycle is managed by FClaireonSessionManager. This tool stores only
 * tool-specific data (Blueprint/Graph pointers, cursor, response mode) keyed
 * by the manager's session ID.
 */
class ClaireonTool_EditBlueprintGraph : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// ========================================================================
	// Session Delegate
	// ========================================================================

	/** Called by FClaireonSessionManager when any session is closed; cleans up our tool data. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Whether we have registered our delegate with the session manager. */
	static bool bDelegateRegistered;

	// ========================================================================
	// Operations
	// ========================================================================

	/** Open an existing Blueprint for editing */
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);

	/** Create a new Blueprint from scratch */
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);

	/** List all graphs in a Blueprint (stateless) */
	FToolResult Operation_ListGraphs(const TSharedPtr<FJsonObject>& Params);

	/** Add a node to the graph */
	FToolResult Operation_AddNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Remove a node from the graph (session-based) */
	FToolResult Operation_RemoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Remove a node from the graph (stateless) */
	FToolResult Operation_RemoveNodeStateless(const TSharedPtr<FJsonObject>& Params);

	/** Reconstruct a node (session-based) */
	FToolResult Operation_ReconstructNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Reconstruct a node (stateless) */
	FToolResult Operation_ReconstructNodeStateless(const TSharedPtr<FJsonObject>& Params);

	/** Connect two pins */
	FToolResult Operation_ConnectPins(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Disconnect a pin */
	FToolResult Operation_DisconnectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Set a pin's default value */
	FToolResult Operation_SetPinValue(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Add a variable to the Blueprint */
	FToolResult Operation_AddVariable(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Set properties on an existing Blueprint variable */
	FToolResult Operation_SetVariableProperties(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Add a component to the Blueprint */
	FToolResult Operation_AddComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Set a property on a component or CDO */
	FToolResult Operation_SetProperty(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Surgically add/remove gameplay tags from a FGameplayTagContainer property on a Blueprint CDO (stateless) */
	FToolResult Operation_SetGameplayTags(const TSharedPtr<FJsonObject>& Params);

	/** Move the cursor */
	FToolResult Operation_MoveCursor(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Navigate cursor back in history */
	FToolResult Operation_CursorBack(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Select a specific node */
	FToolResult Operation_SelectNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Select a specific pin */
	FToolResult Operation_SelectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Select the nearest node to a position */
	FToolResult Operation_SelectNearestNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Get current state without modification */
	FToolResult Operation_GetState(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Import nodes from T3D text */
	FToolResult Operation_ImportNodes(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Compile the Blueprint */
	FToolResult Operation_Compile(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Save the Blueprint to disk */
	FToolResult Operation_Save(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Format the current graph */
	FToolResult Operation_Format(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Close the editing session */
	FToolResult Operation_Close(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Add a dynamic pin to a node (Sequence, MakeArray, Switch, etc.) */
	FToolResult Operation_AddPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Remove a dynamic pin from a node */
	FToolResult Operation_RemovePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Split a struct pin into its component members */
	FToolResult Operation_SplitPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Recombine a previously split struct pin */
	FToolResult Operation_RecombinePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Move a node to a new position */
	FToolResult Operation_MoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Remove a component from the Blueprint SCS hierarchy */
	FToolResult Operation_RemoveComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Reparent a component within the Blueprint SCS hierarchy */
	FToolResult Operation_ReparentComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Rename a component variable in the Blueprint SCS */
	FToolResult Operation_RenameComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Set a new root component in the Blueprint SCS hierarchy */
	FToolResult Operation_SetRootComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Get detailed information about a specific component */
	FToolResult Operation_GetComponentDetails(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	/** Create a function override graph for a BlueprintNativeEvent or BlueprintImplementableEvent */
	FToolResult Operation_AddFunctionOverride(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// apply_spec
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);

	// ========================================================================
	// Helpers
	// ========================================================================

	/**
	 * Build the response content for the current session state.
	 *
	 * @param SessionId The session ID (for inclusion in output)
	 * @param Data The tool-specific data
	 * @return Tool result with graph state and cursor position
	 */
	FToolResult BuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data);

	/**
	 * Validate and reset invalid cursor state.
	 *
	 * @param Data The tool data to validate
	 */
	void ValidateCursor(FBlueprintEditToolData* Data);

	// Tool-specific data storage (keyed by session ID from FClaireonSessionManager)
	static TMap<FString, FBlueprintEditToolData> ToolData;
};
