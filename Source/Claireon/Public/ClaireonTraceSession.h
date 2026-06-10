// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

namespace TraceServices
{
class IAnalysisSession;
class IAnalysisService;
class ITimingProfilerProvider;
class IFrameProvider;
class IThreadProvider;
}

/**
 * Holds one active trace analysis session.
 * Provider access requires the caller to hold a FAnalysisSessionReadScope.
 */
struct FClaireonTraceSession
{
	FString SessionId;
	FString FilePath;
	TSharedPtr<const TraceServices::IAnalysisSession> AnalysisSession;
	FDateTime LastAccessTime;

	/** Returns true if the session has been idle longer than the timeout. */
	bool IsExpired(double TimeoutSeconds = 1800.0) const;

	/** Touch the session to reset the idle timer. */
	void Touch();

	/**
	 * Provider accessors — caller MUST hold a FAnalysisSessionReadScope.
	 * These resolve providers on each call from the session (no caching).
	 */
	const TraceServices::ITimingProfilerProvider* GetTimingProvider() const;
	const TraceServices::IFrameProvider* GetFrameProvider() const;
	const TraceServices::IThreadProvider* GetThreadProvider() const;
};

/**
 * Manages active trace analysis sessions for MCP tools.
 * Sessions are identified by string IDs and auto-expire after 30 minutes of inactivity.
 */
class FClaireonTraceSessionManager
{
public:
	static FClaireonTraceSessionManager& Get();

	/**
	 * Open a .utrace file, run analysis, and return the session.
	 * Blocks until analysis completes.
	 * @param FilePath - Absolute path to the .utrace file
	 * @param OutError - Error message if the operation fails
	 * @return Pointer to the session, or nullptr on failure
	 */
	FClaireonTraceSession* OpenSession(const FString& FilePath, FString& OutError);

	/** Find an existing session by ID, or nullptr if not found/expired. */
	FClaireonTraceSession* FindSession(const FString& SessionId);

	/** Close and remove a session. Returns true if the session existed. */
	bool CloseSession(const FString& SessionId);

	/** Remove all expired sessions. Called periodically. */
	void CleanupExpiredSessions();

private:
	TMap<FString, TSharedPtr<FClaireonTraceSession>> Sessions;
	int32 SessionCounter = 0;
	mutable FCriticalSection CriticalSection;

	FString GenerateSessionId();
};
