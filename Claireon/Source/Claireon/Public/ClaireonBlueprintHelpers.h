// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class SGraphEditor;
class FBlueprintEditor;

/**
 * Scoped helper that opens a Blueprint editor temporarily for operations requiring graph editors.
 * Automatically closes the editor on destruction if it wasn't already open.
 */
class FScopedBlueprintEditor
{
public:
	/**
	 * Opens a Blueprint editor for the specified Blueprint.
	 *
	 * @param InBlueprint The Blueprint to open
	 * @param bInSilent If true, opens editor without UI (default: true)
	 * @param bInCloseOnDestroy If true, closes editor on destruction (default: true)
	 */
	explicit FScopedBlueprintEditor(UBlueprint* InBlueprint, bool bInSilent = true, bool bInCloseOnDestroy = true);

	/** Destructor - closes the editor if we opened it */
	~FScopedBlueprintEditor();

	/**
	 * Get the graph editor for a specific graph.
	 *
	 * @param Graph The graph to get the editor for
	 * @return Shared pointer to the graph editor, or nullptr if not available
	 */
	TSharedPtr<SGraphEditor> GetGraphEditor(UEdGraph* Graph);

	/** Check if the Blueprint editor was successfully opened */
	bool IsValid() const { return BlueprintEditor.IsValid(); }

	/** Get the underlying Blueprint editor */
	TSharedPtr<FBlueprintEditor> GetBlueprintEditor() const { return BlueprintEditor; }

private:
	TWeakObjectPtr<UBlueprint> Blueprint;
	TSharedPtr<FBlueprintEditor> BlueprintEditor;
	bool bWasAlreadyOpen;
	bool bCloseOnDestroy;
};

/**
 * Represents the AI's current focus point in a Blueprint graph during editing.
 */
struct FBlueprintEditCursor
{
	/** Name of the graph currently being edited */
	FString GraphName;

	/** GUID of the currently focused node */
	FGuid FocusedNodeGuid;

	/** Name of the currently focused pin on the focused node */
	FName FocusedPinName;

	/** Direction of the focused pin (input or output) */
	EEdGraphPinDirection FocusedPinDirection;

	/** Current viewport center position (for node placement) */
	FVector2D ViewportCenter;

	/** History of visited nodes for cursor_back navigation */
	TArray<FGuid> CursorHistory;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Status message from the last operation */
	FString LastOperationStatus;

	/** Push current node to history before moving cursor */
	void PushHistory()
	{
		if (FocusedNodeGuid.IsValid())
		{
			if (CursorHistory.Num() >= MaxHistorySize)
			{
				CursorHistory.RemoveAt(0);
			}
			CursorHistory.Add(FocusedNodeGuid);
		}
	}

	/** Pop previous node from history (for cursor_back) */
	bool PopHistory(FGuid& OutGuid)
	{
		if (CursorHistory.Num() > 0)
		{
			OutGuid = CursorHistory.Pop();
			return true;
		}
		return false;
	}

	/** Default constructor */
	FBlueprintEditCursor()
		: FocusedPinDirection(EGPD_Output)
		, ViewportCenter(0.0f, 0.0f)
	{
	}
};

/**
 * Tool-specific data for an active Blueprint editing session.
 * Session lifecycle (ID, expiry, locking) is managed by FClaireonSessionManager.
 */
struct FBlueprintEditToolData
{
	/** Weak pointer to the Blueprint being edited */
	TWeakObjectPtr<UBlueprint> Blueprint;

	/** Weak pointer to the current graph being edited */
	TWeakObjectPtr<UEdGraph> Graph;

	/** Current cursor state */
	FBlueprintEditCursor Cursor;

	/** When true, BuildStateResponse returns minimal output instead of full graph state */
	bool bSuppressOutput = false;

	/** Output verbosity mode for BuildStateResponse. "full", "changed" (default), or "status". */
	FString ResponseMode = TEXT("changed");

	// Nodes affected by the last mutation op — used by response_mode="changed"
	// GUIDs of nodes whose pin connections changed in the last operation.
	// Used by response_mode="changed" to compute the pin-level diff.
	// Cleared at the start of each operation, then populated by mutation handlers.
	TSet<FGuid> LastOperationAffectedNodes;

	// Pre-operation pin connections snapshot — [NodeGuid -> [PinName -> [connected node titles]]]
	// Snapshotted before each mutation op; consumed by BuildStateResponse in "changed" mode.
	TMap<FGuid, TMap<FName, TArray<FString>>> PreOpPinConnections;

	// GUID corrections from the current operation (stale GUID -> current GUID).
	// Populated by FindNodeByGuid's A-field fallback when a blueprint was recompiled
	// between get and edit calls.  Surfaced in BuildStateResponse so the MCP client
	// can update its references.
	TMap<FGuid, FGuid> GuidCorrections;

	/** Check if the session is still valid (Blueprint and Graph are still loaded) */
	bool IsValid() const
	{
		return Blueprint.IsValid() && Graph.IsValid();
	}
};

/**
 * Helper functions for Blueprint graph manipulation
 */
namespace ClaireonBlueprintHelpers
{
	/**
	 * Get all execution pins from a node.
	 *
	 * @param Node The node to get execution pins from
	 * @param bInputOnly If true, only return input exec pins
	 * @param bOutputOnly If true, only return output exec pins
	 * @return Array of execution pins
	 */
	TArray<UEdGraphPin*> GetExecPins(UEdGraphNode* Node, bool bInputOnly = false, bool bOutputOnly = false);

	/**
	 * Check if a node has any input execution pins.
	 *
	 * @param Node The node to check
	 * @return True if the node has at least one input exec pin
	 */
	bool HasExecInputPins(UEdGraphNode* Node);

	/**
	 * Check if a node has any output execution pins.
	 *
	 * @param Node The node to check
	 * @return True if the node has at least one output exec pin
	 */
	bool HasExecOutputPins(UEdGraphNode* Node);

	/**
	 * Find potential root nodes in a graph (nodes with exec output but no exec input).
	 * These are typically Event nodes or entry points.
	 *
	 * @param Graph The graph to search
	 * @return Array of potential root nodes
	 */
	TArray<UEdGraphNode*> FindRootNodes(UEdGraph* Graph);

	/**
	 * Validate that a Blueprint asset path starts with /Game/.
	 *
	 * @param AssetPath The asset path to validate
	 * @param OutError Error message if validation fails
	 * @return True if the path is valid
	 */
	bool ValidateAssetPath(const FString& AssetPath, FString& OutError);

	/**
	 * Find a node in a graph by its GUID.
	 *
	 * @param Graph The graph to search
	 * @param NodeGuid The GUID of the node to find
	 * @param OutCorrectedGuid If non-null and a fallback match was used, receives the node's actual GUID so callers can surface the correction.
	 * @return The node, or nullptr if not found
	 */
	UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FGuid& NodeGuid, FGuid* OutCorrectedGuid = nullptr);

	/**
	 * Find nodes in a graph by their title (display name).
	 * May return multiple nodes if they share the same title.
	 *
	 * @param Graph The graph to search
	 * @param NodeTitle The title to search for (case-insensitive partial match)
	 * @param bExactMatch If true, requires exact match; if false, allows partial match
	 * @return Array of matching nodes
	 */
	TArray<UEdGraphNode*> FindNodesByTitle(UEdGraph* Graph, const FString& NodeTitle, bool bExactMatch = true);

	/**
	 * Find a graph by name in a Blueprint.
	 * Searches EventGraph, FunctionGraphs, and UbergraphPages.
	 *
	 * @param Blueprint The Blueprint to search
	 * @param GraphName The name of the graph to find
	 * @return The graph, or nullptr if not found
	 */
	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

	/**
	 * Find all pins on a node that are compatible with the given pin.
	 *
	 * @param Node The node to search
	 * @param Pin The pin to find compatible pins for
	 * @return Array of compatible pins
	 */
	TArray<UEdGraphPin*> FindCompatiblePins(UEdGraphNode* Node, UEdGraphPin* Pin);

	/**
	 * Parse a variable type string (e.g., "float", "Array<Actor>") into an FEdGraphPinType.
	 *
	 * @param TypeString The type string to parse
	 * @return The parsed pin type
	 */
	FEdGraphPinType ParseVariableType(const FString& TypeString);

	/**
	 * Parse property flags from an array of flag strings.
	 *
	 * @param FlagStrings Array of flag names (e.g., "BlueprintReadWrite", "EditAnywhere")
	 * @return Combined property flags
	 */
	uint64 ParsePropertyFlags(const TArray<FString>& FlagStrings);

	/**
	 * Get the first output pin on a node (for cursor positioning).
	 *
	 * @param Node The node to get the output pin from
	 * @return The first output pin, or nullptr if none exist
	 */
	UEdGraphPin* GetFirstOutputPin(UEdGraphNode* Node);

	/** Format a list of available nodes in a graph for error messages. Returns up to MaxCount nodes. */
	FString FormatAvailableNodes(UEdGraph* Graph, int32 MaxCount = 20);

	/** Format a list of available pins on a node for error messages. */
	FString FormatAvailablePins(UEdGraphNode* Node);

	/** Find the closest matching pin name on a node using simple string similarity. Returns empty string if no close match. */
	FString FindClosestPinName(UEdGraphNode* Node, const FString& SearchName);

	/** Find nodes by class name and/or title. Used for stale GUID recovery. */
	TArray<UEdGraphNode*> FindNodesByClassAndTitle(UEdGraph* Graph, const FString& ClassName, const FString& Title);
} // namespace ClaireonBlueprintHelpers
