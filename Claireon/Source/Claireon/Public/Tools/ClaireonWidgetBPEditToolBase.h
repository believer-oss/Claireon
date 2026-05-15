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

	// ========================================================================
	// Operations
	// ========================================================================
	//
	// Declarations remain here so that decomposed ClaireonWidgetBPTool_<Name>.cpp
	// can define each Operation_<Name> body out-of-line. This mirrors standard
	// C++ member-definition-across-TUs and is the minimum surface that keeps
	// the shared contract intact.

	// Session lifecycle operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetState(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Focus(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Compile(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Widget CRUD operations
	FToolResult Operation_AddWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReplaceWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Property operations
	FToolResult Operation_SetWidgetProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetSlotProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetWidgetDetails(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Advanced operations
	FToolResult Operation_AddBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ListAnimations(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	FToolResult Operation_ImportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ExportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ListWidgetClasses(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// MVVM ViewModel operations
	FToolResult Operation_ListMVVMViewModels(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// MVVM Binding operations
	FToolResult Operation_ListMVVMBindings(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_EditMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMVVMBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// apply_spec
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);

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
