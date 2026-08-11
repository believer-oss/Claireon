// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** Result of an OpenSession call. */
UENUM()
enum class EOpenSessionResult : uint8
{
	Success,
	ReusedExistingSession,
	BlockedByOtherTool,
	InvalidAssetPath
};

/**
 * Default session inactivity timeout, in minutes.
 *
 * Every tool operation resolves its session through FindSession, which
 * Touch()es it, so the clock only runs while a session sits genuinely idle --
 * no single operation approaches this bound. 10 minutes keeps abandoned
 * sessions from blocking editor-wide tools for the better part of an hour
 * (the previous 60-minute default; see Work #6704 friction 5).
 * `inline constexpr` (not `static constexpr` class member) per the v2/Linux
 * header rules.
 */
inline constexpr double ClaireonDefaultSessionTimeoutMinutes = 10.0;

/** Represents an active MCP editing session. The session IS the lock. */
struct FMCPSession
{
	FString SessionId;
	FString ToolName;
	FString AssetPath;
	FDateTime CreatedTime;
	FDateTime LastAccessTime;
	double TimeoutMinutes = ClaireonDefaultSessionTimeoutMinutes;

	bool IsExpired() const
	{
		return (FDateTime::UtcNow() - LastAccessTime).GetTotalMinutes() > TimeoutMinutes;
	}

	void Touch()
	{
		LastAccessTime = FDateTime::UtcNow();
	}
};

/** Rich result from OpenSession containing session ID and optional blocking info. */
struct FMCPOpenSessionResult
{
	EOpenSessionResult Result = EOpenSessionResult::InvalidAssetPath;
	FString SessionId;
	TOptional<FMCPSession> BlockingSession;
};

/** Information passed to the session-closed delegate. */
struct FMCPSessionClosedInfo
{
	FString SessionId;
	FString AssetPath;
	FString ToolName;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPSessionClosed, const FMCPSessionClosedInfo&);

/**
 * Unified session/lock manager for all MCP edit tools.
 *
 * Replaces FMCPAssetLockManager + per-tool session management with a single
 * system where the session IS the lock. Provides exclusive-per-asset locking,
 * session lifecycle, expiry cleanup, and a delegate for tool-specific teardown.
 *
 * Thread safety: All public APIs are self-locking via FCriticalSection.
 *
 * The FMCPSession* returned by FindSession points into the internal TMap.
 * The caller must use it before any other manager call that could modify the map.
 * This is safe because MCP calls are serialized per-client.
 */
class CLAIREON_API FClaireonSessionManager
{
public:
	/** Get the singleton instance. */
	static FClaireonSessionManager& Get();

	/**
	 * Open a new session or reuse an existing one for the same tool on the same asset.
	 * @param AssetPath Asset path (will be canonicalized internally)
	 * @param ToolName Tool identifier, must follow "claireon.<category>_<action>" convention (matches MCP tool name)
	 * @param TimeoutMinutes Session inactivity timeout (default 10 minutes; every
	 *        operation on the session resets the clock via Touch)
	 * @param bAllowUnsavedWorldPackage Pass true ONLY when AssetPath identifies the CURRENT EDITOR
	 *        WORLD's own package/level (e.g. World->GetOutermost()->GetName() or
	 *        World->PersistentLevel->GetPathName()), not a /Game/ asset. An unsaved level (File >
	 *        New Level) lives at /Temp/Untitled_N until its first save; without this, every
	 *        world-locking tool errors immediately on a fresh level with no recovery but saving
	 *        first. Default false so every other caller keeps rejecting non-/Game/ paths unchanged.
	 * @return Result with session ID and optional blocking session info
	 */
	FMCPOpenSessionResult OpenSession(const FString& AssetPath, const FString& ToolName,
		double TimeoutMinutes = ClaireonDefaultSessionTimeoutMinutes,
		bool bAllowUnsavedWorldPackage = false);

	/**
	 * Open the editor-wide session. Returns BlockedByOtherTool (with BlockingSession
	 * populated) immediately if any per-asset session OR another editor-wide session
	 * is already held. Mirrors OpenSession's return shape.
	 *
	 * Acquire-time policy is FAIL-FAST: no queue, no wait, no force-release.
	 * Operators can call session_release on the blocking session id to recover.
	 */
	FMCPOpenSessionResult OpenEditorWideSession(const FString& ToolName,
		double TimeoutMinutes = ClaireonDefaultSessionTimeoutMinutes);

	/** Close the editor-wide session by id. Returns true if the supplied id matched the active editor-wide session. */
	bool CloseEditorWideSession(const FString& SessionId);

	/** Returns true if an editor-wide session is currently held. */
	bool IsEditorWideSessionHeld() const;

	/**
	 * Find an active session by ID. Returns nullptr if not found or expired.
	 * The returned pointer is valid until the next manager call that modifies state.
	 */
	FMCPSession* FindSession(const FString& SessionId);

	/** Close a session. Returns true if the session existed and was closed. */
	bool CloseSession(const FString& SessionId);

	/** Touch a session to prevent expiry. */
	void TouchSession(const FString& SessionId);

	/** List all active sessions, optionally filtered by tool name. */
	TArray<FMCPSession> ListSessions(const FString& ToolName = TEXT("")) const;

	/** Check whether an asset is currently locked by any session. */
	bool IsAssetLocked(const FString& AssetPath) const;

	/** Release the session holding a lock on the given asset path. Returns the number of sessions released (0 or 1). */
	int32 ReleaseByAssetPath(const FString& AssetPath);

	/** Force-release all sessions. Returns the number released. */
	int32 ForceReleaseAll();

	/** Clean up expired sessions. */
	void CleanupExpired();

	/** Access the session-closed delegate. */
	FOnMCPSessionClosed& OnSessionClosed();

	/**
	 * Canonicalize an asset path (collapse //, normalize backslashes, strip suffixes).
	 * Returns empty string for invalid paths.
	 * @param bAllowUnsavedWorldPackage Explicit, opt-in allowance for a /Temp/ path (in addition to
	 *        /Game/) -- see OpenSession's parameter of the same name for when to pass true. This is
	 *        NOT a blanket relaxation of the /Game/ requirement for other asset types: a /Temp/
	 *        path is accepted only when the caller has explicitly identified it as the world
	 *        package being locked.
	 */
	static FString CanonicalizePath(const FString& InPath, bool bAllowUnsavedWorldPackage = false);

private:
	FClaireonSessionManager();
	~FClaireonSessionManager();

	/** Internal cleanup that assumes CriticalSection is already held. Returns info about removed sessions. */
	TArray<FMCPSessionClosedInfo> CleanupExpiredInternal();

	TMap<FString, FMCPSession> Sessions; // SessionId -> Session
	TMap<FString, FString> AssetLocks;	 // CanonicalAssetPath -> SessionId
	TOptional<FMCPSession> EditorWideSession; // Single sentinel slot; AssetPath is "<editor-wide>".
	mutable FCriticalSection CriticalSection;
	FOnMCPSessionClosed OnSessionClosedDelegate;
	FTSTicker::FDelegateHandle CleanupTickerHandle;
};
