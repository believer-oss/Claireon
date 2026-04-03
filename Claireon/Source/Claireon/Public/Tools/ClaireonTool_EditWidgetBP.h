// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

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
 * Session-based MCP tool for editing Widget Blueprint designer surfaces.
 * Supports full CRUD on widget trees, property editing, bindings, and more.
 */
class ClaireonTool_EditWidgetBP : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FWidgetBPEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

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

	// Animation operations
	FToolResult Operation_CreateAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_DeleteAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_DuplicateAnimation(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetAnimationDetails(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddAnimationBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddAnimationTrack(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddAnimationKeyframe(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveAnimationKeyframe(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetAnimationProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

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

	// Helpers
	FToolResult BuildStateResponse(const FString& SessionId, FWidgetBPEditToolData* Data);
};
