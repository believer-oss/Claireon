// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UAnimMontage;
class UAnimSequenceBase;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active animation edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FAnimEditToolData
{
	/** Weak reference to the animation being edited */
	TWeakObjectPtr<UAnimSequenceBase> Animation;

	/** Detected asset type: "AnimSequence", "AnimMontage", or "AnimComposite" */
	FString AssetType;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output instead of full animation state */
	bool bSuppressOutput = false;

	/** Index of the currently focused notify, or -1 if none */
	int32 FocusedNotifyIndex = -1;

	/** Check if the tool data is still valid (animation is still loaded) */
	bool IsValid() const { return Animation.IsValid(); }
};

/**
 * Lightweight JSON Schema builder for tool inputSchema definitions.
 * Reduces boilerplate when declaring per-tool parameter schemas.
 */
struct CLAIREON_API FToolSchemaBuilder
{
	TSharedPtr<FJsonObject> Schema;
	TSharedPtr<FJsonObject> Properties;
	TArray<TSharedPtr<FJsonValue>> RequiredFields;

	FToolSchemaBuilder();

	void AddString(const FString& Name, const FString& Description, bool bRequired = false);
	void AddNumber(const FString& Name, const FString& Description, bool bRequired = false);
	void AddInteger(const FString& Name, const FString& Description, bool bRequired = false);
	void AddBoolean(const FString& Name, const FString& Description, bool bRequired = false);
	void AddObject(const FString& Name, const FString& Description, bool bRequired = false);
	void AddEnum(const FString& Name, const FString& Description, const TArray<FString>& Values, bool bRequired = false);

	/** Finalize and return the schema. Sets the required array if any fields were marked required. */
	TSharedPtr<FJsonObject> Build();

	/** Add the common session_id + suppress_output parameters (session_id is required). */
	void AddSessionParams();
};

/**
 * Base class for all animation editing MCP tools.
 * Provides shared session management, state response building, and common helpers.
 */
class CLAIREON_API ClaireonAnimEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* AnimSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FAnimEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All anim tools share the "anim" category. */
	FString GetCategory() const override { return TEXT("anim"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FAnimEditToolData*& OutData,
		FToolResult& OutError);

	/** Builds the standard state response with animation structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FAnimEditToolData* Data);

	/** Gets a montage from tool data, or returns nullptr and sets OutError. */
	UAnimMontage* RequireMontage(FAnimEditToolData* Data, FToolResult& OutError);

	/** Requires the asset to be an AnimSequence. Returns nullptr and sets OutError if not. */
	class UAnimSequence* RequireAnimSequence(FAnimEditToolData* Data, FToolResult& OutError);
};
