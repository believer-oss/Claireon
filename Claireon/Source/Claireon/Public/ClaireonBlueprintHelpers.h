// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UPackage;
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
 * Identifies a single cursor history entry: the graph name the cursor was on
 * and the node GUID within that graph.
 */
struct FGraphCursorHistoryEntry
{
	FString GraphName;
	FGuid   NodeGuid;

	bool operator==(const FGraphCursorHistoryEntry& Other) const
	{
		return GraphName == Other.GraphName && NodeGuid == Other.NodeGuid;
	}
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

	/** History of visited (graph, node) pairs for cursor_back navigation */
	TArray<FGraphCursorHistoryEntry> CursorHistory;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Status message from the last operation */
	FString LastOperationStatus;

	/**
	 * Push the currently focused node to history.
	 *
	 * @param InGraphName Graph name the focused node belongs to. Callers
	 *        normally pass Data->Cursor.GraphName, but handlers that mutate
	 *        Cursor.GraphName before pushing (e.g. Operation_AddFunctionOverride
	 *        function path) must capture the previous graph name and pass it
	 *        here so the history entry references the correct graph.
	 */
	void PushHistory(const FString& InGraphName)
	{
		if (FocusedNodeGuid.IsValid() && !InGraphName.IsEmpty())
		{
			if (CursorHistory.Num() >= MaxHistorySize)
			{
				CursorHistory.RemoveAt(0);
			}
			CursorHistory.Add(FGraphCursorHistoryEntry{ InGraphName, FocusedNodeGuid });
		}
	}

	/** Pop the most recent history entry. Returns false if history is empty. */
	bool PopHistory(FGraphCursorHistoryEntry& OutEntry)
	{
		if (CursorHistory.Num() > 0)
		{
			OutEntry = CursorHistory.Pop();
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

	// GUID corrections from the current operation (stale GUID → current GUID).
	// Populated by FindNodeByGuid's A-field fallback when a blueprint was recompiled
	// between get and edit calls.  Surfaced in BuildStateResponse so the MCP client
	// can update its references.
	TMap<FGuid, FGuid> GuidCorrections;

	// Consecutive calls that resolved the session via asset_path (auto-open) rather
	// than an explicit session_id. Resets to 0 whenever the caller passes session_id.
	// BuildStateResponse emits a Data.session_hint nudge when this passes 5 (first
	// at call 6, then every 5 past that: 11, 16, ...) to steer agents toward
	// explicit open/close discipline.
	int32 ConsecutiveAssetPathCalls = 0;

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
	 * Rich result carrier for variable-type parsing. Replaces the pre-existing
	 * silent-fallback behavior of ParseVariableType so callers can surface
	 * structured errors instead of quietly coercing unknown inputs to PC_String.
	 */
	struct FParseVariableTypeResult
	{
		/** True only when a concrete PinType was resolved from the input. */
		bool bSucceeded = false;

		/** The parsed pin type. Only meaningful when bSucceeded == true. */
		FEdGraphPinType PinType;

		/** Human-readable failure detail. Empty on success. */
		FString Error;

		/** Fuzzy-resolution note for class/struct/enum lookups. May be set on success. */
		FString ResolutionNote;
	};

	/**
	 * Parse a variable type string (e.g., "float", "Array<Actor>", "Map<FString,int>") into a
	 * structured result. On unknown input, returns bSucceeded=false with a clear error -- no
	 * silent PC_String fallback.
	 *
	 * Supported short-form strings (case-insensitive for names, keywords are case-sensitive
	 * where noted):
	 *  - float, double, int, int32, int64, byte, bool, string, name, text
	 *  - Array<T>, Set<T>, Map<K,V>  (Map parser is comma/bracket-depth aware)
	 *  - softclass:/Game/.../X.X_C, SoftClass<Class>
	 *  - softobject:/Game/.../X.X,  SoftObject<Class>
	 *  - instancedstruct, gameplaytag, gameplaytagcontainer
	 *  - Class / Struct / Enum names resolved via ClaireonNameResolver
	 *
	 * Types that require extra data (delegate family) must go through ParseVariableTypeSpec.
	 */
	FParseVariableTypeResult ParseVariableTypeChecked(const FString& TypeString);

	/**
	 * Parse a variable type from a long-form JSON spec. Used by add_variable to carry
	 * additional fields the short-form string cannot express (e.g. delegate signature
	 * function path). Schema:
	 *   {
	 *     "base": "<type name>",                 (required)
	 *     "signature_function": "/Script/.../X__DelegateSignature", (required for delegate family)
	 *     "subtype": "/Script/.../X"             (optional; subcategory hint for softclass/softobject/instancedstruct)
	 *   }
	 */
	FParseVariableTypeResult ParseVariableTypeSpec(const TSharedPtr<FJsonObject>& Spec);

	/**
	 * Legacy entry point retained as a lenient wrapper. On failure, logs a warning and
	 * returns a default-constructed FEdGraphPinType. Prefer ParseVariableTypeChecked.
	 *
	 * @param TypeString The type string to parse
	 * @return The parsed pin type (default-constructed on failure)
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
	 * Decompose a CPF bitmask into human-readable flag strings.
	 * Inverse of ParsePropertyFlags, handling compound flags.
	 *
	 * @param PropertyFlags The CPF bitmask to decompose
	 * @return Array of human-readable flag strings
	 */
	TArray<FString> FormatPropertyFlags(uint64 PropertyFlags);

	/**
	 * Side-effect report for ApplyVariableProperties.
	 * Populated only when a non-nullptr OutResult is supplied.
	 */
	struct FApplyVariableResult
	{
		/** Name of the RepNotify handler graph that was created or reused. NAME_None if no RepNotify work ran. */
		FName RepNotifyHandlerGraph;

		/** True iff a new UEdGraph was created this call. False if the handler already existed. */
		bool bRepNotifyGraphCreated = false;
	};

	/**
	 * Result of CreateBlueprint. Populated on both success and failure paths.
	 */
	struct FCreateBlueprintResult
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* EventGraph = nullptr;
		UPackage* Package = nullptr;
		FString Error;
		TArray<FString> Warnings;

		bool IsOk() const { return Error.IsEmpty() && Blueprint != nullptr; }
	};

	/**
	 * Create a new Blueprint asset at the given asset path with the given parent class.
	 * Handles package creation, externally-referenceable flag, asset-registry notification,
	 * and overwrites any existing file on disk at that path.
	 *
	 * @param AssetPath UE asset path (e.g. "/Game/MyBlueprint" or "/Game/MyBlueprint.MyBlueprint")
	 * @param ParentClass The parent UClass for the new Blueprint.
	 * @param OutResult Receives Blueprint + EventGraph + Package handles and warnings/error.
	 */
	void CreateBlueprint(const FString& AssetPath, UClass* ParentClass, FCreateBlueprintResult& OutResult);

	/**
	 * Apply optional variable properties (category, tooltip, replication, flags, metadata)
	 * to an existing Blueprint variable. Used by both set_variable_properties and add_variable.
	 *
	 * @param Blueprint The Blueprint containing the variable
	 * @param VarName Name of the variable to configure
	 * @param Params JSON object with optional property fields
	 * @param OutResult Optional. When non-null, receives the result of RepNotify handler-graph
	 *                  creation or reuse. Anim-graph callers pass nullptr.
	 */
	void ApplyVariableProperties(UBlueprint* Blueprint, FName VarName,
	                             const TSharedPtr<FJsonObject>& Params,
	                             FApplyVariableResult* OutResult = nullptr);

	/**
	 * Create a new FBPVariableDescription on the Blueprint from a JSON spec, then call
	 * ApplyVariableProperties to populate the full option set.
	 *
	 * Accepts both factory-canonical and applicator-legacy field names:
	 *   - "variable_name" or "name" (required)
	 *   - "variable_type" or "type" (legacy applicator alias) or "variable_type_spec" (object form)
	 *   - "default_value" (optional)
	 *   - "flags" (optional string array)
	 *   - All keys consumed by ApplyVariableProperties.
	 *
	 * Default PropertyFlags = CPF_Edit | CPF_BlueprintVisible.
	 *
	 * @return true on success.
	 */
	bool CreateVariableFromSpec(UBlueprint* Blueprint,
	                            const TSharedPtr<FJsonObject>& Params,
	                            FApplyVariableResult* OutResult,
	                            FString& OutError);

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

	/** Find nodes by class name and/or title. Used for stale GUID recovery. */
	TArray<UEdGraphNode*> FindNodesByClassAndTitle(UEdGraph* Graph, const FString& ClassName, const FString& Title);

	/**
	 * Pick the specialized K2Node subclass for a resolved UFunction by inspecting
	 * its metadata. Canonical order mirrors UBlueprintFunctionNodeSpawner::Create
	 * (BlueprintFunctionNodeSpawner.cpp) plus an AsyncAction prefix:
	 *   AsyncAction (UBlueprintAsyncActionBase factory + 4-conjunct guard)
	 *     -> CommutativeAssociativeBinaryOperator (pure)
	 *     -> MaterialParameterCollectionFunction
	 *     -> CallDataTableFunction
	 *     -> CallArrayFunction
	 *     -> plain UK2Node_CallFunction.
	 * Returns UK2Node_CallFunction::StaticClass() when Function is null.
	 * Note: UK2Node_AsyncAction is the only return value that is NOT a
	 * UK2Node_CallFunction subclass; callers must branch on the result type.
	 */
	UClass* PickK2NodeClassForFunction(const UFunction* Function);

	/**
	 * Returns true if the given asset class short-name is in the Blueprint family
	 * allowlist used by D1 Branch 1 class validation.
	 * Allowlist = { "Blueprint", "AnimBlueprint", "WidgetBlueprint" }.
	 * This is a pure string check; it does not load the asset or inspect inheritance.
	 * For IsChildOf<UBlueprint>-style checks (D1 Branch 2), load the asset and use
	 * UClass::IsChildOf(UBlueprint::StaticClass()) directly.
	 *
	 * @param ClassName The AssetData.AssetClassPath.GetAssetName().ToString() value.
	 * @return True if ClassName is one of the three allowlist values.
	 */
	bool IsBlueprintAssetClass(const FString& ClassName);

	// Defined in ClaireonBlueprintGraphEditToolBase_Internal.cpp (file-local alias table).
	// Returns the high-level add_node alias for a given node UClass (e.g. "CallFunction"
	// for UK2Node_CallFunction, including subclasses via IsChildOf). Returns empty FString
	// if no alias is registered.
	FString GetNodeTypeAliasForClass(const UClass* NodeClass);
} // namespace ClaireonBlueprintHelpers
