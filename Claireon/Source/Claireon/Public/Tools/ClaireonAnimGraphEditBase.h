// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "ClaireonBlueprintHelpers.h"

class UAnimBlueprint;
class UEdGraph;
struct FMCPSessionClosedInfo;

/**
 * Per-session data for an active animation graph editing session.
 * Session lifecycle is managed by FClaireonSessionManager.
 */
struct FAnimGraphEditToolData
{
	TWeakObjectPtr<UAnimBlueprint> AnimBlueprint;
	TWeakObjectPtr<UEdGraph> CurrentGraph;
	FBlueprintEditCursor Cursor;
	FString ResponseMode = TEXT("changed");
	TSet<FGuid> LastOperationAffectedNodes;
	TMap<FGuid, TMap<FName, TArray<FString>>> PreOpPinConnections;
	TMap<FGuid, FGuid> GuidCorrections;

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	bool IsValid() const { return AnimBlueprint.IsValid() && CurrentGraph.IsValid(); }
};

/**
 * Base class for all animation graph editing MCP tools.
 * Provides shared session management, state response building, and common helpers.
 */
class CLAIREON_API ClaireonAnimGraphEditToolBase : public IClaireonTool
{
public:
	static const TCHAR* AnimGraphSessionToolName;
	static TMap<FString, FAnimGraphEditToolData> ToolData;
	static bool bDelegateRegistered;

	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }
	FString GetCategory() const override { return TEXT("animgraph"); }

protected:
	/**
	 * Validate session and retrieve tool data. Reads session_id from Arguments.
	 * Also clears per-operation state and snapshots pin connections for "changed" mode.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FAnimGraphEditToolData*& OutData,
		FToolResult& OutError);

	/** Build the standard state response with graph nodes and cursor. */
	FToolResult BuildStateResponse(const FString& SessionId, FAnimGraphEditToolData* Data);

	/** Snapshot pin connections before a mutation (for "changed" response mode). */
	void SnapshotPinConnections(FAnimGraphEditToolData* Data);

	/**
	 * Refresh the Blueprint editor in-place without closing/reopening it (no tab switching).
	 * Finds the open FBlueprintEditor for the asset and calls RefreshEditors().
	 * No-op if the editor is not open.
	 */
	static void RefreshBlueprintEditorInPlace(UBlueprint* Blueprint);
};

// Macro to reduce declaration boilerplate for individual animgraph edit tool classes.
#define DECLARE_ANIMGRAPH_EDIT_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonAnimGraphEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
