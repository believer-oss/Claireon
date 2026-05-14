// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "ClaireonSessionManager.h"
#include "Tools/IClaireonTool.h"

/**
 * RAII per-asset session lock for use inside an IClaireonTool::Execute() body.
 *
 * Usage:
 *   FClaireonScopedAssetLock Lock(AssetPath, GetName());
 *   if (!Lock.IsAcquired())
 *   {
 *       return Lock.GetError();
 *   }
 *   // ... mutate the asset ...
 *
 * If construction blocks because another tool holds the lock, IsAcquired()
 * returns false and GetError() returns an FToolResult shaped like the
 * existing per-asset error message (template at
 * Tools/ClaireonBlueprintGraphTool_Open.cpp:184-194).
 *
 * On destruction, if a session was successfully acquired, CloseSession is called.
 * Reused-existing-session counts as acquired but does NOT close on destruction
 * (because some other call already owns the lifetime).
 *
 * NOTE: Editor-wide sessions held when this helper is constructed will cause
 * acquisition to fail with BlockedByOtherTool (per stage 002's OpenSession
 * extension), which is the correct behavior.
 */
class CLAIREON_API FClaireonScopedAssetLock
{
public:
	FClaireonScopedAssetLock(const FString& AssetPath, const FString& ToolName, double TimeoutMinutes = 60.0);
	~FClaireonScopedAssetLock();

	// Non-copyable, non-movable.
	FClaireonScopedAssetLock(const FClaireonScopedAssetLock&) = delete;
	FClaireonScopedAssetLock& operator=(const FClaireonScopedAssetLock&) = delete;
	FClaireonScopedAssetLock(FClaireonScopedAssetLock&&) = delete;
	FClaireonScopedAssetLock& operator=(FClaireonScopedAssetLock&&) = delete;

	bool IsAcquired() const { return bAcquired; }

	/** Get the error result to return from Execute() if !IsAcquired(). */
	const IClaireonTool::FToolResult& GetError() const { return ErrorResult; }

	/** The session id obtained, or empty if !IsAcquired(). */
	const FString& GetSessionId() const { return SessionId; }

private:
	FString SessionId;
	FString HeldAssetPath;
	bool bAcquired = false;
	bool bShouldCloseOnDestroy = false;
	IClaireonTool::FToolResult ErrorResult;
};
