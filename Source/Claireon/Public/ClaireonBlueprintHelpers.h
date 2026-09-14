// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h" // EBlueprintType, for CreateBlueprint's kind parameter
#include "Tools/ClaireonBPSnapshot.h" // FClaireonBPSnapshot, the per-session pre-op snapshot
#include "ClaireonScopedAssetEditor.h" // the family-agnostic opener FScopedBlueprintEditor is built on

class UBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UPackage;
class SGraphEditor;
class FBlueprintEditor;
class SDockTab;

/** Blueprint-specific editor access, including graph widgets. */
class CLAIREON_API FScopedBlueprintEditor : public FClaireonScopedAssetEditor
{
public:
	/**
	 * Open a Blueprint editor.
	 *
	 * @param InBlueprint The Blueprint to open
	 * @param bInSilent Ignored; retained for compatibility
	 * @param bInCloseOnDestroy Close on destruction only if this object opened the editor
	 */
	explicit FScopedBlueprintEditor(UBlueprint* InBlueprint, bool bInSilent = true, bool bInCloseOnDestroy = true);

	/**
	 * Get the graph editor, creating its document tab if absent.
	 *
	 * @param Graph The graph to get the editor for
	 * @return The graph editor, or nullptr if unavailable
	 */
	TSharedPtr<SGraphEditor> GetGraphEditor(UEdGraph* Graph);

	/** Whether the Blueprint editor is available. */
	bool IsValid() const { return GetBlueprintEditor().IsValid(); }

	/** Get the underlying Blueprint editor */
	TSharedPtr<FBlueprintEditor> GetBlueprintEditor() const;
};

/** Why a recorded editor binding no longer resolves. */
enum class EClaireonEditorBindingStatus : uint8
{
	Valid,
	NotBound,
	BoundEditorClosed,
	GraphTabClosed,
	GraphRemoved,
};

CLAIREON_API const TCHAR* ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus Status);

/**
 * Weak binding to one editor instance, revalidated on every call.
 * A dead instance requires a new session; it is never silently rebound.
 * Switching graphs may open a tab in the bound instance, but a closed tab for the
 * current graph is reported as graph_tab_closed rather than reopened.
 */
struct CLAIREON_API FClaireonBlueprintEditorBinding
{
	TWeakObjectPtr<UBlueprint>   Blueprint;
	TWeakObjectPtr<UEdGraph>     BoundGraph;
	TWeakPtr<FBlueprintEditor>   Editor;
	TWeakPtr<SGraphEditor>       GraphEditor;

	/** Opaque identity of the bound instance. Invalid means this session bound to nothing. */
	FGuid InstanceId;

	bool IsBound() const { return InstanceId.IsValid(); }
	void Clear();

	/** Record the instance and widget a session open produced. */
	void BindTo(UBlueprint* InBlueprint, UEdGraph* InGraph,
		TSharedPtr<FBlueprintEditor> InEditor, TSharedPtr<SGraphEditor> InGraphEditor);

	/** Resolve through the bound instance. Returns null on failure and always sets OutStatus. */
	TSharedPtr<SGraphEditor> ResolveGraphEditor(UEdGraph* SessionGraph, EClaireonEditorBindingStatus& OutStatus);

	/** Revalidation alone, for callers that only need the status. */
	EClaireonEditorBindingStatus Revalidate(UEdGraph* SessionGraph) const;

#if WITH_CLAIREON_TESTS
	/** Bind to a registered test candidate; normal editor opening reuses the existing instance. */
	void BindToSeamInstance(UBlueprint* InBlueprint, UEdGraph* InGraph, const FGuid& InInstanceId);
#endif

	/** True when InstanceId names a seam candidate rather than a live toolkit. */
	bool bSeamBound = false;
};

#if WITH_CLAIREON_TESTS
/** Test candidates for editor and widget identity binding. */
namespace ClaireonBlueprintEditorBindingSeam
{
	/** Register a candidate instance hosting Widget for Graph. Returns its instance id. */
	CLAIREON_API FGuid RegisterInstance(UEdGraph* Graph, TSharedPtr<SGraphEditor> Widget);

	/** Drop the whole candidate -- the editor instance closed. */
	CLAIREON_API void CloseInstance(const FGuid& InstanceId);

	/** Drop only the widget -- the graph's document tab closed, the instance lives. */
	CLAIREON_API void CloseGraphTab(const FGuid& InstanceId);

	/** The widget a candidate hosts, or null when the id is unknown or its tab is closed. */
	CLAIREON_API TSharedPtr<SGraphEditor> FindWidget(const FGuid& InstanceId);

	/** True while the id names a registered candidate, tab open or not. */
	CLAIREON_API bool IsInstanceRegistered(const FGuid& InstanceId);

	/** Forget every candidate. */
	CLAIREON_API void Reset();

	/** Reset seam candidates on scope exit. */
	struct CLAIREON_API FScopedSeam
	{
		FScopedSeam() = default;
		~FScopedSeam();

		FScopedSeam(const FScopedSeam&) = delete;
		FScopedSeam& operator=(const FScopedSeam&) = delete;
	};
}
#endif // WITH_CLAIREON_TESTS

/** Graph and node identity for a cursor history entry. */
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

	// Nodes affected by the last mutation, used by response_mode="changed".
	// Cleared at the start of each operation.
	TSet<FGuid> LastOperationAffectedNodes;

	// Pre-mutation snapshot for BuildStateResponse in "changed" mode.
	FClaireonBPSnapshot PreOpSnapshot;

	// Stale-to-current GUID corrections from FindNodeByGuid, returned so clients can update references.
	TMap<FGuid, FGuid> GuidCorrections;

	// Consecutive asset_path calls; explicit session_id resets the count.
	// Session hints fire at calls 6, 11, 16, and so on.
	int32 ConsecutiveAssetPathCalls = 0;

	// Editor-opening outcome recorded at session creation and returned with every response.
	FClaireonEditorOpenOutcome EditorWindow;

	// The one editor instance this session drives. Unbound in a commandlet, where there is
	// no window; every consumer revalidates it rather than trusting it.
	FClaireonBlueprintEditorBinding EditorBinding;

	bool IsValid() const
	{
		return Blueprint.IsValid() && Graph.IsValid();
	}
};

/**
 * Helper functions for Blueprint graph manipulation
 */
class USCS_Node;
class UActorComponent;

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
	 * Resolve a node_guid string against a graph, accepting either a full GUID or a
	 * bare hex prefix (>= 8 hex characters, hyphens stripped, case-insensitive).
	 * This is the uniform resolution shared by every node_guid consumer; its
	 * semantics mirror the proven apply_delta resolver.
	 *
	 * Resolution order:
	 *  1. FGuid::Parse succeeds -> FindNodeByGuid exact lookup (with the A-field
	 *     recompile-recovery fallback). If the fallback fired and OutCorrectedFullGuid
	 *     is non-null, the recovered full GUID is written there so callers can keep
	 *     their GuidCorrections bookkeeping firing. Not found -> structured error.
	 *  2. Full parse failed but the input is 8..32 hex chars (dashes stripped) ->
	 *     prefix-match every node's Digits GUID. Exactly one hit resolves; zero hits
	 *     is a not-found error naming the graph; more than one hit is an ambiguity
	 *     error listing every candidate's DigitsWithHyphens GUID.
	 *  3. Anything else (too short / non-hex) is a genuine "Invalid <field> format" error.
	 *
	 * @param Graph                 Graph to search.
	 * @param GuidStr               Full GUID or >=8-hex-char prefix.
	 * @param OutNode               Receives the resolved node on success.
	 * @param OutError              Human-readable failure detail on false.
	 * @param FieldNameForErrors    Field name used in the "Invalid <field> format" text
	 *                              (e.g. "source_node_guid"); defaults to "node_guid".
	 * @param OutCorrectedFullGuid  Optional. Written only when the input parsed as a full
	 *                              GUID and the A-field recompile-recovery fallback fired;
	 *                              never written for the prefix-match branch.
	 * @return True on success.
	 */
	bool ResolveNodeGuidString(const UEdGraph* Graph, const FString& GuidStr, UEdGraphNode*& OutNode,
	                           FString& OutError, const TCHAR* FieldNameForErrors = TEXT("node_guid"),
	                           FGuid* OutCorrectedFullGuid = nullptr);

	/**
	 * Fixed-point sweep that resolves wildcard pins from their linked neighbours.
	 *
	 * For every pin still holding category=wildcard that HAS a link, copy the pin type
	 * of the first linked neighbour whose category is already resolved, then let the
	 * owning K2 node re-coerce its siblings. Repeats until nothing changes, capped to
	 * guard against pathological cycles in macro graphs.
	 *
	 * Strictly link-driven: an unconnected wildcard pin is never touched. Use
	 * PromoteWildcardContainerElementPin for the literal-write case, which has no link
	 * to propagate from.
	 *
	 * @param SeedNodes     Nodes to start from; the scan expands to nodes reachable
	 *                      through the seeds' current links.
	 * @param MaxIterations Fixed-point iteration cap.
	 */
	void PropagateWildcardTypesViaLinks(TArray<UEdGraphNode*> SeedNodes, int32 MaxIterations = 16);

	/**
	 * Promote a single unconnected wildcard pin to a concrete type, propagating the
	 * result across the owning node the way a real connection would.
	 *
	 * The engine's wildcard machinery (UK2Node_MakeContainer::NotifyPinConnectionListChanged,
	 * and the link sweep above) only reacts to pins that have links, so writing a literal
	 * into a wildcard pin cannot reuse either. This does the propagation manually:
	 * element pin type, the container output pin (key or value half for MakeMap), and the
	 * still-wildcard sibling element pins.
	 *
	 * Safe on any wildcard pin: nodes that are not UK2Node_MakeContainer just get the
	 * direct pin-type set with no sibling walk.
	 *
	 * Call this BEFORE writing the pin's default value. It intentionally skips
	 * NotifyPinConnectionListChanged, which on an unlinked pin resets the node back to
	 * wildcard while the default still matches the autogenerated one.
	 *
	 * @param ElementPin   The wildcard pin being written to.
	 * @param InferredType Type inferred from the literal. Its ContainerType is ignored --
	 *                     the pin's own container kind is preserved.
	 */
	void PromoteWildcardContainerElementPin(UEdGraphPin* ElementPin, const FEdGraphPinType& InferredType);

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
	 * Find nodes whose title matches ignoring spaces, underscores, and case.
	 *
	 * This is a SUGGESTION-ONLY search. Do not bind an edit to its result: callers that
	 * silently accept a normalized match can retarget an edit onto a different node (asking
	 * for "SetHealth" in a graph that only has the "Set Health" variable setter). Use it to
	 * build a "did you mean" hint after FindNodesByTitle has already returned nothing.
	 *
	 * Motivation: node titles render friendly ("Print String") in the interactive editor but
	 * raw ("PrintString") in headless commandlets, since friendly-name humanization is an
	 * editor style setting.
	 *
	 * @param Graph The graph to search
	 * @param NodeTitle The title to search for
	 * @param bAllowPartial If true, also returns titles that merely contain the normalized
	 *                      search text (catches truncations such as "PrintStr")
	 * @return Array of candidate nodes
	 */
	TArray<UEdGraphNode*> FindNodesByNormalizedTitle(UEdGraph* Graph, const FString& NodeTitle, bool bAllowPartial = false);

	/**
	 * Build a "did you mean" fragment for a title that matched no node exactly.
	 *
	 * Returns an empty string when nothing plausible is nearby, so callers can fall back to
	 * listing the graph. Use this when the caller's own not-found sentence already carries
	 * context the generic one would lose (local IDs, GUID prefixes); otherwise prefer
	 * FormatTitleMatchFailure, which composes the whole message.
	 *
	 * @param Graph The graph that was searched
	 * @param RequestedTitle The title the caller asked for
	 * @param DisambiguationParam Name of the parameter the caller should pass instead
	 * @return A "Did you mean: ..." fragment, or an empty string
	 */
	FString FormatTitleSuggestions(
		UEdGraph* Graph,
		const FString& RequestedTitle,
		const FString& DisambiguationParam = TEXT("node_guid"));

	/**
	 * Compose the error for a node lookup that matched more than one node.
	 *
	 * The GUID-prefix path and the title path produce the same answer shape -- N candidates,
	 * each needing its GUID listed, resolvable by re-issuing the call with a narrower
	 * reference -- so both share this one formatter rather than each carrying its own
	 * near-identical sentence. Candidates are rendered as full node descriptions (title plus
	 * GUID), capped, because a bare GUID list does not tell the caller which node it picked.
	 *
	 * This is the surface the structured-hint `options` field will adopt: the candidate set
	 * here is exactly the option set there. Keep the two in step -- if this message starts
	 * carrying information the option entries do not, they have diverged.
	 *
	 * @param Graph The graph that was searched
	 * @param LookupKind What was being matched, for the message -- e.g. "node title", "GUID prefix"
	 * @param RequestedValue The value the caller asked for
	 * @param Matches The matched nodes (expected to be more than one)
	 * @param Remedy A caller-ready sentence naming how to narrow the reference
	 * @return A formatted, caller-ready error message
	 */
	FString FormatAmbiguousNodeMatch(
		UEdGraph* Graph,
		const FString& LookupKind,
		const FString& RequestedValue,
		const TArray<UEdGraphNode*>& Matches,
		const FString& Remedy);

	/**
	 * Build the error message for a title lookup that did not resolve to exactly one node.
	 *
	 * Both outcomes are routine -- duplicate titles are the normal state of a graph (several
	 * "Branch" nodes, several "Get Health") -- so both answers carry the GUIDs the caller
	 * needs to make the follow-up call succeed:
	 *  - more than one exact match: lists the matched GUIDs to disambiguate with
	 *  - no exact match: offers normalized "did you mean" candidates, then the available nodes
	 *
	 * @param Graph The graph that was searched
	 * @param RequestedTitle The title the caller asked for
	 * @param ExactMatches The result of FindNodesByTitle (must not be exactly one element)
	 * @param DisambiguationParam Name of the parameter the caller should pass instead,
	 *                            e.g. "node_guid" or "source_node_guid"
	 * @return A formatted, caller-ready error message
	 */
	FString FormatTitleMatchFailure(
		UEdGraph* Graph,
		const FString& RequestedTitle,
		const TArray<UEdGraphNode*>& ExactMatches,
		const FString& DisambiguationParam = TEXT("node_guid"));

	/**
	 * Find a graph by name, including nested graphs through GetAllGraphs.
	 *
	 * @param Blueprint The Blueprint to search
	 * @param GraphName The graph name
	 * @return The graph, or nullptr if not found
	 */
	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);

	/** Top-level graph kinds. */
	enum class EClaireonGraphKind : uint8
	{
		Ubergraph,
		Function,
		Macro,
		DelegateSignature,
		InterfaceImplementation,
		Extension,
	};

	/** The wire spelling: "ubergraph" | "function" | "macro" | "delegate_signature" | "interface_implementation" | "extension". */
	const TCHAR* GraphKindToWireString(EClaireonGraphKind Kind);

	/** Parse a wire spelling back. False for anything not in the set -- callers must error, not guess. */
	bool ParseGraphKind(const FString& Wire, EClaireonGraphKind& OutKind);

	struct FGraphEnumerationEntry
	{
		UEdGraph* Graph = nullptr;

		EClaireonGraphKind GraphKind = EClaireonGraphKind::Ubergraph;

		/** Populated only when GraphKind is InterfaceImplementation: the implemented interface's class path. Empty otherwise. */
		FString OwningInterface;
	};

	/**
	 * Enumerate ubergraph, function, macro, delegate-signature, interface-implementation,
	 * and extension-owned graphs. Kind comes from collection membership, since
	 * GetGraphType cannot distinguish interface implementations from functions.
	 * Exclude nested composites: their names need not be unique for graph_name addressing.
	 */
	TArray<FGraphEnumerationEntry> EnumerateTopLevelGraphs(UBlueprint* Blueprint);

	/**
	 * Find all pins on a node that are compatible with the given pin.
	 *
	 * @param Node The node to search
	 * @param Pin The pin to find compatible pins for
	 * @return Array of compatible pins
	 */
	TArray<UEdGraphPin*> FindCompatiblePins(UEdGraphNode* Node, UEdGraphPin* Pin);

	/** Variable-type parsing result with structured failure details. */
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
	/** Return DisplayName metadata when present, otherwise FriendlyName. */
	FString ResolveVariableDisplayName(const struct FBPVariableDescription& Var);

	/** Format a type for ParseVariableType, including containers, map values, and enum-bound bytes. */
	FString FormatVariableTypeString(const struct FEdGraphPinType& PinType);

	/** True when PinType is a single-cast or multicast delegate. */
	bool IsDelegateVariableType(const struct FEdGraphPinType& PinType);

	/**
	 * Resolve the member reference first, then try dispatcher signature names on the
	 * generated and skeleton classes. Blueprint dispatchers may have empty references.
	 * Without OwningBlueprint, only member-reference resolution is available.
	 */
	class UFunction* ResolveDelegateSignatureFunction(const struct FEdGraphPinType& PinType,
		class UBlueprint* OwningBlueprint = nullptr, FName VariableName = NAME_None);

	/**
	 * Return a replayable delegate signature path, or empty for non-delegates and
	 * unresolved signatures. An unresolved delegate cannot round-trip through the parser.
	 */
	FString GetDelegateSignatureFunctionPath(const struct FEdGraphPinType& PinType,
		class UBlueprint* OwningBlueprint = nullptr, FName VariableName = NAME_None);

	/** Write type and round-trip status, plus signature_function and variable_type_spec for delegates. */
	void WriteVariableTypeJson(const struct FEdGraphPinType& PinType, const TSharedPtr<FJsonObject>& Out,
		class UBlueprint* OwningBlueprint = nullptr, FName VariableName = NAME_None);

	/**
	 * Write shared variable fields: name, type, display name, category, flags, replication,
	 * tooltip, and metadata. Callers add envelope-specific fields.
	 */
	void WriteVariableCoreJson(const struct FBPVariableDescription& Var, const TSharedPtr<FJsonObject>& Out,
		class UBlueprint* OwningBlueprint = nullptr);

	/** True for a non-exec data output on a FunctionEntry. */
	bool IsFunctionParameterSource(const UEdGraphPin* SourcePin);

	/** Name conflicts that prevent creating a Custom Event. */
	struct FCustomEventNameConflict
	{
		enum class EKind : uint8
		{
			/** No conflict: the name is free. */
			None,
			/** The parent chain already implements it as an override event in this Blueprint. */
			ExistingOverrideEvent,
			/** Another Custom Event in this Blueprint already has the name. */
			ExistingCustomEvent,
			/** A BlueprintImplementableEvent or BlueprintNativeEvent on the parent chain. */
			ParentBlueprintEvent,
			/** A parent-class function that is not an event, so the name is still taken. */
			ParentFunction,
			/** A function on an implemented interface. */
			InterfaceFunction,
			/** A function graph in this Blueprint. */
			LocalFunctionGraph,
		};

		EKind Kind = EKind::None;

		/** The real function the requested name resolves to. */
		FName ResolvedFunctionName;

		/** Class or interface declaring it, or the graph name for a local conflict. */
		FString OwnerName;

		/** Populated for ExistingOverrideEvent / ExistingCustomEvent. */
		FString ExistingNodeGuid;
		FString ExistingGraphName;

		/** True for a BlueprintNativeEvent, which needs add_function_override rather than an event node. */
		bool bNativeEvent = false;

		/** How the requested name matched, when it was not an exact match. */
		FString ResolutionNote;

		FString Explanation;

		/** The call to make instead. */
		FString Remedy;

		bool IsConflict() const { return Kind != EKind::None; }
	};

	/**
	 * Find a Custom Event name conflict using the shared alias and function-name resolver,
	 * without substring matching.
	 *
	 * @param IgnoreNode Exclude this node when auditing existing content; null when checking a new name.
	 */
	FCustomEventNameConflict FindCustomEventNameConflict(
		UBlueprint* Blueprint,
		const FString& EventName,
		const UEdGraphNode* IgnoreNode = nullptr);

	/** Stable wire name for a conflict kind. Part of bp_lint's evidence contract. */
	const TCHAR* ToString(FCustomEventNameConflict::EKind Kind);

	/**
	 * Distance threshold shared with bp_lint's distant-get rule.
	 * Keep inline constexpr: static constexpr at header scope fails the Linux v2 build.
	 */
	inline constexpr double LocalGetDistanceUnits = 512.0;

	/**
	 * True for a pure, receiver-less variable read beyond MaxDistance from its consumer.
	 * Validated gets and reads with wired or default-object targets are ineligible.
	 */
	bool IsDistantVariableGetSource(const UEdGraphPin* SourcePin, const UEdGraphPin* TargetPin, double MaxDistance);

	/**
	 * Copy the variable reference beside TargetPin and return its data output, or null
	 * with no node left in Graph. The caller owns keeping or destroying a returned node.
	 * Copy the full reference to preserve scope, class, self-context, and GUID identity.
	 */
	class UEdGraphPin* EmitAdjacentVariableGet(class UEdGraph* Graph, class UEdGraphPin* SourcePin,
	                                           class UEdGraphPin* TargetPin, int32 StackIndex);

	/**
	 * Emit a local parameter get beside TargetPin, staggered by StackIndex.
	 * Return its output pin, or null with no node left in Graph. The caller owns the node.
	 * Do not use this to store computed values; that changes demand-time evaluation.
	 */
	class UEdGraphPin* EmitLocalParameterGet(class UEdGraph* Graph, class UEdGraphPin* SourcePin,
	                                         class UEdGraphPin* TargetPin, int32 StackIndex);

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
	 * @param BlueprintType Blueprint kind. BPTYPE_MacroLibrary and BPTYPE_Interface get no
	 *                      ubergraph at all (FBlueprintEditorUtils::DoesSupportEventGraphs is
	 *                      false for them), so OutResult.EventGraph is legitimately null for
	 *                      those -- callers must not treat that as failure.
	 */
	void CreateBlueprint(const FString& AssetPath, UClass* ParentClass, FCreateBlueprintResult& OutResult,
	                     EBlueprintType BlueprintType = BPTYPE_Normal);

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

	/**
	 * Result of resolving a Blueprint component by variable name for inspection or edit.
	 * A component can live in this Blueprint's own SCS, or be inherited from a parent
	 * Blueprint's SCS. Inherited components are edited through this Blueprint's
	 * InheritableComponentHandler override template, never through the parent's template.
	 */
	struct FResolvedComponentTemplate
	{
		/** The SCS node that defines the component (in this Blueprint or an ancestor). */
		USCS_Node* Node = nullptr;
		/** The template to inspect or edit for THIS Blueprint. */
		UActorComponent* Template = nullptr;
		/** True when the defining SCS node lives in an ancestor Blueprint. */
		bool bInherited = false;
		/** True when Template is this Blueprint's own override of an inherited component. */
		bool bOverridden = false;
	};

	/**
	 * Resolve a component by variable name, searching this Blueprint's SCS first and then
	 * every ancestor Blueprint's SCS. For an inherited component, bForWrite=true creates
	 * (or reuses) the override template in this Blueprint's InheritableComponentHandler,
	 * matching what the Details panel does; bForWrite=false returns the existing override
	 * if there is one, else the parent's template, without creating anything.
	 * Native (C++) default subobjects are not SCS components and are not resolved here.
	 *
	 * @param Blueprint The Blueprint being inspected or edited
	 * @param ComponentName SCS variable name of the component
	 * @param bForWrite Create the child override template when the component is inherited
	 * @param OutResolved Filled on success
	 * @param OutError Human-readable reason on failure
	 * @return True if a node and a usable template were found
	 */
	bool ResolveComponentTemplate(UBlueprint* Blueprint, FName ComponentName, bool bForWrite, FResolvedComponentTemplate& OutResolved, FString& OutError);

	// Defined in ClaireonBlueprintGraphEditToolBase_Internal.cpp (file-local alias table).
	// Returns the high-level add_node alias for a given node UClass (e.g. "CallFunction"
	// for UK2Node_CallFunction, including subclasses via IsChildOf). Returns empty FString
	// if no alias is registered.
	FString GetNodeTypeAliasForClass(const UClass* NodeClass);
} // namespace ClaireonBlueprintHelpers
