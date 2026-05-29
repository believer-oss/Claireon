// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class UWidgetBlueprint;

struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active Widget Blueprint edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FWidgetBPEditToolData
{
	TWeakObjectPtr<UWidgetBlueprint> WidgetBlueprint;
	FName FocusedWidget;
	FDateTime LastCommandTime;
	bool bModified = false;

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	bool IsValid() const
	{
		return WidgetBlueprint.IsValid();
	}
};

/**
 * Shared base class for decomposed Widget Blueprint editor MCP tools.
 *
 * Each operation is its own top-level MCP tool (ClaireonWidgetBPTool_Open,
 * ClaireonWidgetBPTool_AddWidget, etc.). The shared session management, tool
 * data map, and per-operation implementations live on this base class so every
 * decomposed tool stays consistent without duplicating logic.
 *
 * The base class itself is NOT MCP-registered. Concrete decomposed tools
 * invoke their Operation_<Name> member directly from Execute. Operation bodies
 * are defined out-of-line in the matching ClaireonWidgetBPTool_<Name>.cpp (stage
 * 024 moved them out of the former dispatcher). All session lifecycle
 * (touching, lock tracking) remains in FClaireonSessionManager.
 */
class CLAIREON_API ClaireonWidgetBPEditToolBase : public IClaireonTool
{
public:
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetCategory() const override { return TEXT("widgetbp"); }

protected:
	// ========================================================================
	// Session Delegate
	// ========================================================================

	/** Called by FClaireonSessionManager when any session is closed; cleans up our tool data. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Whether we have registered our delegate with the session manager. */
	static bool bDelegateRegistered;

	// Helpers
	FToolResult BuildStateResponse(const FString& SessionId, FWidgetBPEditToolData* Data);

	/**
	 * Shared pre-op wrapping used by every session-requiring decomposed widgetbp tool.
	 * Handles: params unwrap (legacy nested "params"), session_id resolution,
	 * session validation, tool data lookup, and TouchSession. Returns false on
	 * error (OutError populated).
	 */
	bool BeginSessionOp(
		const TSharedPtr<FJsonObject>& Arguments,
		const FString& OperationName,
		TSharedPtr<FJsonObject>& OutParams,
		FString& OutSessionId,
		FWidgetBPEditToolData*& OutData,
		FToolResult& OutError);

	// Tool-specific data storage (keyed by session ID from FClaireonSessionManager).
	static TMap<FString, FWidgetBPEditToolData> ToolData;
};

// Macro used by the decomposed tool headers to reduce boilerplate.
#define DECLARE_WIDGETBP_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonWidgetBPEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
