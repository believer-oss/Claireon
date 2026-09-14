// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;

/**
 * Value snapshots used to classify durable mutation after a failure.
 * Capture immediately before the first mutation and again on failure, outside the
 * tool-owned transaction. Phase journals record actions but do not prove their effects.
 * Snapshots retain identities and values rather than pointers to nodes that mutation may delete.
 */

/** Durable-effect classes used by the operation delta and result state. */
enum class EClaireonBPDurableEffectClass : uint8
{
	/** Read directly: graph nodes and pins, keyed by NodeGuid and PinId. */
	GraphTopology,

	/** CONSTRUCTED PROXY. See FClaireonBPGeneratedClassSnapshot. */
	GeneratedClass,

	/** CONSTRUCTED PROXY. See FClaireonBPPackageSnapshot. */
	PackageState,

	/** Read, but unobservable headless. See FClaireonBPTransactionSnapshot. */
	TransactionBuffer,

	/** The family-specific property class. See EClaireonBPSnapshotFamily. */
	FamilyProperties,
};

/**
 * Selects family-specific evidence. Node positions count for formatting only;
 * asynchronous formatter movement must not appear as an extraction or setter mutation.
 */
enum class EClaireonBPSnapshotFamily : uint8
{
	/** Items 2 and 3. Function flags and purity, entry signature, gateway pins, graph and function names. */
	Extraction,

	/** Item 4. Function flag mask, entry node extra flags, category, tooltip, access specifier, net flag mask. */
	Setter,

	/** Item 5. Node positions, comment bounds and containment sets, knot membership. */
	Format,
};

/** How a delta entry differs between the two snapshots. */
enum class EClaireonBPDeltaKind : uint8
{
	Added,
	Removed,
	Changed,
};

/** Pin-link endpoint identified by node GUID and pin ID. */
struct FClaireonBPLinkEndpoint
{
	FGuid NodeGuid;
	FGuid PinId;
};

/**
 * One pin, by value.
 *
 * PinId is stable across reconstruction: it is TRANSFERRED onto the new pin by the
 * persistent-data move, so a PinId-keyed diff does not report a spurious change across
 * an entry-reconstruction phase.
 */
struct FClaireonBPPinSnapshot
{
	FGuid PinId;
	FName PinName;

	/** Canonical pin type; empty and unobserved in a bChangedDiffOnly capture. */
	FString PinType;

	/** True for an input pin. Direction is part of identity, not decoration. */
	bool bIsInput = false;

	/**
	 * True for PC_Exec, including in changed-diff captures where PinType is absent.
	 * Used by structural assertions; category changes are diffed through PinType.
	 */
	bool bIsExec = false;

	/**
	 * Unmatched reconstructed pins can retain their PinId as orphans; presence alone
	 * does not detect that semantic loss.
	 */
	bool bOrphaned = false;

	/**
	 * Canonical literal, object, or text default. Empty if unset or unobserved
	 * (bChangedDiffOnly). Default changes belong to GraphTopology.
	 */
	FString DefaultValue;

	/** Both directions are recorded, so a broken link is visible from either end. */
	TArray<FClaireonBPLinkEndpoint> Links;
};

/**
 * One node, by value.
 *
 * NodeGuid is stable across reconstruction: it is written only at creation and by
 * legacy load fixups, and node reconstruction never touches it. Node POSITION is
 * deliberately absent -- it belongs to the format family's property class.
 */
struct FClaireonBPNodeSnapshot
{
	FGuid NodeGuid;

	/** Class path, not title. Titles are display state and change under a rename. */
	FString NodeClassPath;

	TMap<FGuid, FClaireonBPPinSnapshot> Pins;
};

/** One graph's topology, by value. */
struct FClaireonBPGraphTopologySnapshot
{
	FGuid GraphGuid;
	FName GraphName;
	TMap<FGuid, FClaireonBPNodeSnapshot> Nodes;
};

/**
 * Generated-class evidence: class identities, skeleton function names, and Blueprint state.
 * Skeleton regeneration can preserve the class pointer, so identity alone is insufficient.
 */
struct FClaireonBPGeneratedClassSnapshot
{
	/** Opaque pointer identity, never dereferenced. Zero when the class is null. */
	uint64 SkeletonGeneratedClassIdentity = 0;

	/** Opaque pointer identity, never dereferenced. Zero when the class is null. */
	uint64 GeneratedClassIdentity = 0;

	/** Recorded alongside the identities so a delta can name what changed. */
	FString SkeletonGeneratedClassPath;
	FString GeneratedClassPath;

	/**
	 * Sorted names of the skeleton class's own UFunctions, excluding inherited functions.
	 * Diffed as a set to detect in-place changes without treating declaration order as a change.
	 */
	TArray<FString> SkeletonFunctionNames;

	FGuid BlueprintGuid;

	/** EBlueprintStatus by its underlying value, so this header stays free of Engine includes. */
	uint8 Status = 0;

	bool bCachedDependenciesUpToDate = false;
};

/** Package-state proxy using the dirty bit and persistent GUID; not a per-edit revision. */
struct FClaireonBPPackageSnapshot
{
	FString PackageName;
	bool bIsDirty = false;
	FGuid PersistentGuid;
};

/**
 * Transaction-buffer evidence; unobservable when the buffer is absent.
 * Compare head-transaction GUIDs, not queue length, which eviction can change.
 * Capture after the tool-owned transaction is destroyed or canceled.
 */
struct FClaireonBPTransactionSnapshot
{
	/** False when the transaction buffer does not exist. The diff reports unobservable, never empty. */
	bool bObservable = false;

	/** Recorded for diagnosis only. Never the change signal -- see the class comment. */
	int32 QueueLength = 0;

	/** The change signal. */
	FGuid HeadTransactionId;
	FGuid HeadTransactionOperationId;
	FString HeadTransactionTitle;

	/** Titles of transactions this tool opened, so Claireon-owned effects are separable. */
	TArray<FString> ClaireonOwnedTitles;

	/** A caller-owned group is active. Activity alone is NOT authority to roll back. */
	bool bCallerGroupActive = false;
	FString CallerGroupLabel;
};

/** Fifth class, extraction family (items 2 and 3). */
struct FClaireonBPExtractionPropertySnapshot
{
	/** Function name to its flags mask. Purity lives in this mask. */
	TMap<FName, uint32> FunctionFlags;

	/** Function name to its entry-node signature, as an ordered list of canonical pin type strings. */
	TMap<FName, TArray<FString>> EntrySignatures;

	/** Gateway node GUID to its pin ids, so a lost gateway pin is a delta rather than a silence. */
	TMap<FGuid, TArray<FGuid>> GatewayPins;

	/** Graph GUID to graph name, so a rename reads as a rename and not as a disappearance. */
	TMap<FGuid, FName> GraphNames;
};

/** Function-property setter evidence, including entry flags and RPC mode. */
struct FClaireonBPSetterPropertySnapshot
{
	/** Function name to its FunctionFlags mask. */
	TMap<FName, uint32> FunctionFlags;

	/** Function name to its entry node's extra flags. */
	TMap<FName, int32> EntryExtraFlags;

	TMap<FName, FString> Categories;
	TMap<FName, FString> Tooltips;
	TMap<FName, FString> AccessSpecifiers;

	/** Function name to the net flag bits only, so an RPC transition is legible on its own. */
	TMap<FName, uint32> NetFlagMasks;
};

/** Fifth class, format family (item 5). Positions are the evidence here, and only here. */
struct FClaireonBPFormatPropertySnapshot
{
	TMap<FGuid, FIntPoint> NodePositions;

	/** Comment node GUID to its bounds. */
	TMap<FGuid, FIntRect> CommentBounds;

	/**
	 * Comment node GUID to the nodes it contains. Containment is asserted SEPARATELY
	 * from semantic topology: a comment spanning two islands is a refusal reason for
	 * the island being formatted, and neither invariant sees comments.
	 */
	TMap<FGuid, TArray<FGuid>> CommentContainment;

	/** Knot membership. Knot count may change under a semantic-topology-preserving format. */
	TSet<FGuid> KnotNodes;
};

/**
 * Operation-start or failure snapshot. Captures all shared classes and only
 * the property block selected by Family.
 */
struct FClaireonBPSnapshot
{
	EClaireonBPSnapshotFamily Family = EClaireonBPSnapshotFamily::Extraction;

	/** True once a capture has actually run. A default-constructed snapshot is not a baseline. */
	bool bCaptured = false;

	/** Class 1, keyed by GraphGuid. Covers every graph the operation may touch, not only the focused one. */
	TMap<FGuid, FClaireonBPGraphTopologySnapshot> Graphs;

	/** Class 2. */
	FClaireonBPGeneratedClassSnapshot GeneratedClass;

	/** Class 3. */
	FClaireonBPPackageSnapshot Package;

	/** Class 4. */
	FClaireonBPTransactionSnapshot Transactions;

	/** Class 5, extraction. Populated only when Family is Extraction. */
	FClaireonBPExtractionPropertySnapshot ExtractionProperties;

	/** Class 5, setter. Populated only when Family is Setter. */
	FClaireonBPSetterPropertySnapshot SetterProperties;

	/** Class 5, format. Populated only when Family is Format. */
	FClaireonBPFormatPropertySnapshot FormatProperties;

	/**
	 * Graph-only capture for response_mode=changed: pin identity, orphan state, and links.
	 * PinType and DefaultValue are unobserved. Diff computes display entries but does not
	 * accept this capture as durable-effect evidence.
	 */
	bool bChangedDiffOnly = false;

	/** Locate one node across every captured graph. Null when it was not captured. */
	const FClaireonBPNodeSnapshot* FindNode(const FGuid& NodeGuid) const;
};

/** Diagnostic record of an attempted action; only the snapshot delta selects result state. */
struct FClaireonBPPhaseJournalEntry
{
	/** Wire string of the phase that acted. See ClaireonBPMutation::ToWireString. */
	FString Phase;

	/** What the phase believes it did. */
	FString Action;

	/** Stable identifier for what it acted on. */
	FString Target;

	/** Free-form supporting detail. */
	FString Detail;
};

/**
 * Append-only action journal. Engine and third-party mutations may appear only in the
 * snapshot delta, which determines result state.
 */
struct FClaireonBPPhaseJournal
{
	/** Append-only. Never cleared mid-operation, never rewritten. */
	TArray<FClaireonBPPhaseJournalEntry> Entries;

	/** Append an action without taking a snapshot. */
	void Append(FClaireonBPPhaseJournalEntry&& Entry) { Entries.Add(MoveTemp(Entry)); }

	int32 Num() const { return Entries.Num(); }
	bool IsEmpty() const { return Entries.Num() == 0; }

	/**
	 * The failing phase's own records, which is the source of the phase-local delta.
	 * Phase is a wire string from one of the three phase enums.
	 */
	TArray<FClaireonBPPhaseJournalEntry> EntriesForPhase(const FString& Phase) const;

	/** Serialized only on a failure path, into nested Data. Never an inline scalar. */
	TSharedPtr<FJsonObject> ToJson() const;
};

/** One observed difference between two snapshots, in the vocabulary of its class. */
struct FClaireonBPDeltaEntry
{
	EClaireonBPDurableEffectClass EffectClass = EClaireonBPDurableEffectClass::GraphTopology;
	EClaireonBPDeltaKind Kind = EClaireonBPDeltaKind::Changed;

	/** Stable identifier, addressed by identity: graph GUID, node GUID, pin id, or property name. */
	FString Target;

	/** Value before and after, as data. Empty on Added and Removed respectively. */
	FString Before;
	FString After;
};

/**
 * The observed difference between two snapshots.
 *
 * This is what selects the result state. A delta that cannot be computed -- because one
 * side was never captured -- is NOT an empty delta, and HasDurableEffect() must not
 * answer false for it.
 */
struct FClaireonBPSnapshotDelta
{
	TArray<FClaireonBPDeltaEntry> Entries;

	/**
	 * False when the transaction-buffer class could not be read on either side, which
	 * is the normal headless case. Report the class as unobservable; never as empty.
	 */
	bool bTransactionClassObservable = false;

	/**
	 * False when either input snapshot was never captured. The caller must resolve that
	 * to the retained state rather than to a proven-quiescent one.
	 */
	bool bComparable = false;

	bool IsEmpty() const { return Entries.Num() == 0; }

	/**
	 * True when the diff proves something durable remains. Returns true when the diff is
	 * not comparable, because uncertainty resolves to the retained state.
	 */
	bool HasDurableEffect() const { return !bComparable || Entries.Num() > 0; }

	/** Serialized only on a failure path, into nested Data. Never an inline scalar. */
	TSharedPtr<FJsonObject> ToJson() const;
};

namespace ClaireonBPSnapshot
{
	/**
	 * Capture one snapshot over the supplied graphs plus the Blueprint's non-graph state.
	 *
	 * Called exactly twice per failing operation and exactly once per successful one:
	 * immediately before the first mutating API call, and again in the failure handler.
	 * Never per phase, never per island.
	 *
	 * @param Blueprint  The Blueprint whose class, package, and property state is read.
	 * @param Graphs     Every graph the operation may touch, not only the focused one.
	 * @param Family     Selects which property block of the fifth class is populated.
	 * @param OutSnapshot  Populated on success; bCaptured stays false on failure.
	 * @return True when the snapshot is a usable baseline.
	 */
	CLAIREON_API bool Capture(
		UBlueprint* Blueprint,
		const TArray<UEdGraph*>& Graphs,
		EClaireonBPSnapshotFamily Family,
		FClaireonBPSnapshot& OutSnapshot);

	/**
	 * Capture pin identity, orphan state, and links for response_mode=changed.
	 * Sets bChangedDiffOnly and omits pin types and defaults; usable for display,
	 * not for proving that no durable mutation remains.
	 */
	CLAIREON_API bool CaptureForChangedDiff(const TArray<UEdGraph*>& Graphs, FClaireonBPSnapshot& OutSnapshot);

	/**
	 * Diff two snapshots into the operation delta.
	 *
	 * Both sides must come from the same family; a cross-family diff is not meaningful
	 * and is reported as not comparable rather than as an empty delta.
	 */
	CLAIREON_API FClaireonBPSnapshotDelta Diff(const FClaireonBPSnapshot& Before, const FClaireonBPSnapshot& After);

	/** Wire vocabulary for the delta. The C++ enum names are not the contract; these are. */
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPDurableEffectClass EffectClass);
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPDeltaKind Kind);
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPSnapshotFamily Family);
}

/**
 * Test-only phase failures routed through the normal failure handler.
 * Compiled out of shipping builds. Arm with FScopedFault so early test returns disarm it.
 */
namespace ClaireonBPFaultInjection
{
	/** True when the fault-injection seam is compiled in; available in all configurations. */
	constexpr bool IsCompiledIn()
	{
#if WITH_CLAIREON_TESTS
		return true;
#else
		return false;
#endif
	}
}

#if WITH_CLAIREON_TESTS
static_assert(!UE_BUILD_SHIPPING,
	"WITH_CLAIREON_TESTS is set in a shipping build. The Blueprint fault-injection seam "
	"must not exist in a shipping build at all -- see Claireon.Build.cs.");
#endif

#if WITH_CLAIREON_TESTS
namespace ClaireonBPFaultInjection
{
	/** Arm the seam for one phase wire string. Prefer FScopedFault. */
	CLAIREON_API void Arm(const FString& PhaseWireString);

	/** Disarm. Idempotent. */
	CLAIREON_API void Disarm();

	/** The armed phase, or empty when disarmed. */
	CLAIREON_API const FString& GetArmedPhase();

	/**
	 * True when this exact phase is armed. Called by the phase runners at the point
	 * where the phase would otherwise have succeeded, so the failure it produces travels
	 * the genuine failure path rather than a synthetic one.
	 */
	CLAIREON_API bool ShouldFail(const TCHAR* PhaseWireString);

	/** RAII arming. Disarms on scope exit even when the test returns early. */
	struct CLAIREON_API FScopedFault
	{
		explicit FScopedFault(const FString& PhaseWireString) { Arm(PhaseWireString); }
		~FScopedFault() { Disarm(); }

		FScopedFault(const FScopedFault&) = delete;
		FScopedFault& operator=(const FScopedFault&) = delete;
	};
}

/** Phase failure hook; expands to false without a seam reference when tests are disabled. */
#define CLAIREON_BP_SHOULD_INJECT_FAILURE(PhaseWireString) \
	(ClaireonBPFaultInjection::ShouldFail(PhaseWireString))

#else

#define CLAIREON_BP_SHOULD_INJECT_FAILURE(PhaseWireString) (false)

#endif // WITH_CLAIREON_TESTS
