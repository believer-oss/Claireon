// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "PCGCommon.h"

class FJsonObject;
class UPCGData;
class UPCGGraph;
class UPCGComponent;
class UPCGNode;
class UPCGPin;
class UPCGSettings;

/**
 * Shared utility functions for PCG Graph MCP tools.
 * Provides asset loading, node lookup, formatting, and property manipulation.
 */
namespace ClaireonPCGGraphHelpers
{
	/** Load and validate a PCG Graph asset from an asset path. */
	UPCGGraph* LoadPCGGraphAsset(const FString& AssetPath, FString& OutError);

	/** Find a node by identifier (numeric index, node title, or settings class name). */
	UPCGNode* FindNodeByIdentifier(UPCGGraph* Graph, const FString& Identifier, int32& OutIndex);

	/** Get the display name for a node (title if set, otherwise settings class name). */
	FString GetNodeDisplayName(const UPCGNode* Node);

	/** Get a short name for a settings class (strips "PCG" prefix and "Settings" suffix). */
	FString GetSettingsShortName(const UPCGSettings* Settings);

	/** Resolve a settings class name to a UClass*. Accepts full, partial, or short names. */
	UClass* ResolveSettingsClass(const FString& ClassName, FString& OutError);

	/** Format the full graph structure as structured text. */
	FString FormatGraphStructure(const UPCGGraph* Graph, const FString& DetailLevel);

	/** Format a single node with its pins, connections, and optionally properties. */
	FString FormatNodeDetail(const UPCGGraph* Graph, const UPCGNode* Node, int32 NodeIndex, bool bIncludeProperties);

	/** Format pin info including connections. */
	FString FormatPinConnections(const UPCGGraph* Graph, const UPCGPin* Pin);

	/** Read all editable properties of a node's settings as formatted text. */
	FString ReadNodeProperties(const UPCGNode* Node);

	/**
	 * Set a dotted, optionally subscripted property path via ImportText, then run
	 * the engine settings-change path to rebuild pins. Refuse deprecated leaves: UHT
	 * strips their suffix, so they can shadow active properties.
	 * OutChangeType receives the engine-derived flags on success.
	 */
	bool SetNodeProperty(UPCGNode* Node, const FString& PropertyPath, const FString& PropertyValue,
		FString& OutError, EPCGChangeType& OutChangeType);

	/** Overload for callers that do not need the derived change type. */
	bool SetNodeProperty(UPCGNode* Node, const FString& PropertyPath, const FString& PropertyValue, FString& OutError);

	/** Graph-editing operations used to select change flags. */
	enum class EPCGGraphEditOp : uint8
	{
		Connect,
		Disconnect,
		DisconnectAll,
		AddNode,
		RemoveNode,
		/** Fallback only -- prefer the engine-derived type returned by SetNodeProperty. */
		SetNodeProperty,
		/** A multi-edit applicator pass, notified once under FPCGGraphNotifyPauseScope. */
		Batch,
		/** Sentinel for exhaustiveness tests; never passed to GetChangeTypeForOp. */
		Count
	};

	/** Map operations to conservative change flags to avoid stale compiled graphs. */
	EPCGChangeType GetChangeTypeForOp(EPCGGraphEditOp Op);

	/** Human-readable flag list ("Edge|Structural"), for tool results and logs. */
	FString ChangeTypeToString(EPCGChangeType ChangeType);

	/**
	 * Notify through ForceNotificationForEditor to invalidate compiled graphs and
	 * dependent parents. Accumulate flags while FPCGGraphNotifyPauseScope is active.
	 */
	void NotifyGraphChanged(UPCGGraph* Graph, EPCGChangeType ChangeType);

	/** Convenience overload: notify with the flags mapped from Op. */
	void NotifyGraphChanged(UPCGGraph* Graph, EPCGGraphEditOp Op);

	/**
	 * Accumulate change flags and emit their union when the last scope releases.
	 * Keep pause/release balanced: the engine asserts on an unmatched release.
	 */
	class CLAIREON_API FPCGGraphNotifyPauseScope
	{
	public:
		explicit FPCGGraphNotifyPauseScope(UPCGGraph* InGraph);
		~FPCGGraphNotifyPauseScope();

		FPCGGraphNotifyPauseScope(const FPCGGraphNotifyPauseScope&) = delete;
		FPCGGraphNotifyPauseScope& operator=(const FPCGGraphNotifyPauseScope&) = delete;

	private:
		TWeakObjectPtr<UPCGGraph> Graph;
		bool bPaused = false;
	};

	/** Assign the first free grid slot to a node at (0,0). Preserve other positions. */
	void AssignDefaultNodePosition(UPCGGraph* Graph, UPCGNode* Node);

	/**
	 * Compute a deterministic display position for an unpositioned node without
	 * dirtying the asset during editor-view reconstruction.
	 */
	void ComputeDisplayPosition(const UPCGGraph* Graph, const UPCGNode* Node, int32& OutX, int32& OutY);

	/** List all available PCG settings class names. */
	TArray<FString> GetAvailableSettingsClasses();

	/**
	 * Count valid, non-template, owned editor-world components using this graph,
	 * including GraphInstance wrappers. Uses the pcg_refresh predicate without its cap.
	 */
	int32 CountLiveComponentsUsingGraph(const UPCGGraph* Graph);

	/**
	 * Collect valid, non-template, owned editor-world components using this graph.
	 * Apply ActorLabelFilter before the result cap; OutSkipped counts matches beyond it.
	 */
	void CollectLiveComponentsUsingGraph(const UPCGGraph* Graph, TArray<UPCGComponent*>& OutComponents,
		int32& OutSkipped, int32 MaxComponents = 32, const FString& ActorLabelFilter = FString());

	/**
	 * Count instances in this component's managed ISM resources. Owner-level ISMs may
	 * belong to other components or have been placed by hand.
	 */
	int32 CountManagedISMInstances(const UPCGComponent* Component);

	/** Running min/max/mean over a numeric series. */
	struct FPointStat
	{
		double Min = TNumericLimits<double>::Max();
		double Max = TNumericLimits<double>::Lowest();
		double Sum = 0.0;
		int32 Count = 0;

		void Add(double Value);
		double Mean() const { return Count ? Sum / Count : 0.0; }
		TSharedPtr<FJsonObject> ToJson() const;
	};

	/** What AccumulatePointData gathers across one or more point-data objects. */
	struct FPointStatistics
	{
		int32 Points = 0;
		FPointStat Density;
		FPointStat PositionZ;
		TMap<FName, FPointStat> Attributes;
		TSet<FName> NonNumericAttributes;
	};

	/**
	 * Accumulate point data; return false without changes for other data types.
	 * From 5.6, read UPCGBasePointData accessors to support both point storage formats.
	 * Unset metadata entries contribute attribute defaults.
	 */
	bool AccumulatePointData(const UPCGData* Data, FPointStatistics& Out);

	/**
	 * Generate components and pump until completion or timeout. Tick the PCG subsystem
	 * directly: world or core-ticker pumping can re-enter MCP dispatch. Also pump Slate
	 * and editor timers, which do not advance FTSTicker.
	 */
	bool GenerateAndWait(const TArray<UPCGComponent*>& Components, int32 TimeoutMs, bool bForce,
		int32& OutFrames, FString& OutError);
} // namespace ClaireonPCGGraphHelpers
