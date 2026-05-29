// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/**
 * Asynchronous JSONL conversation logger for the MCP REPL.
 *
 * All public methods are game-thread only.
 * File I/O is dispatched to a background thread — the game thread is never blocked.
 *
 * Log files: Saved/Logs/Claireon/repl_YYYY-MM-DD_HHMMSS.jsonl (one per editor session)
 */
class FClaireonREPLLogger
{
public:
    FClaireonREPLLogger();
    ~FClaireonREPLLogger();

    /** Initialize: create log file path, start flush timer. Call once after construction. */
    void Initialize();

    /** Flush any remaining entries and stop the timer. Call before destruction. */
    void Shutdown();

    /** Log a session start event with settings snapshot. */
    void LogSessionStart(const FString& ModelId, const FString& SystemPrompt, int32 ToolCount);

    /** Log a user message. */
    void LogUserMessage(const FString& Content, const FString& ConversationId);

    /** Log a full assistant response (raw JSON content array as string, usage info). */
    void LogAssistantMessage(const FString& ContentJson, const FString& StopReason,
        int32 InputTokens, int32 OutputTokens, double DurationMs, const FString& ConversationId);

    /** Log a tool call + result. */
    void LogToolResult(const FString& ToolName, const FString& ToolUseId,
        const FString& Content, double DurationMs, bool bIsError, const FString& ConversationId);

    /** Log a cancellation event. */
    void LogCancelled(const FString& Phase, const TArray<FString>& ToolsCompleted,
        const TArray<FString>& ToolsSkipped, const FString& ConversationId);

    /** Log the global emergency stop (Ctrl+.). */
    void LogEmergencyStop(const FString& ConversationId);

    /** Log a fresh context transition. */
    void LogFreshContext(const FString& OldConversationId, const FString& NewConversationId,
        const FString& Trigger, const FString& HandoffSummary);

    /** Log a settings change. */
    void LogSettingsChanged(const TArray<FString>& ChangedFields);

    /** Log an error. */
    void LogError(const FString& ErrorType, const FString& Message, const FString& ConversationId);

    /** Log an API error with structured diagnostic fields. */
    void LogError(const FString& ErrorType, const FString& Message,
        int32 HttpStatusCode, const FString& ResponseBody,
        const FString& FailureReason, int32 RetryAttempt,
        double RequestDurationMs, const FString& ConversationId);

    /** Log a retry attempt as a separate JSONL entry (type: "retry_attempt"). */
    void LogRetryAttempt(const FString& ErrorCategory, int32 HttpStatusCode,
        int32 AttemptNumber, float DelaySeconds, const FString& ConversationId);

    /** Force an immediate flush (e.g., on conversation reset). */
    void ForceFlush();

private:
    /** Append a JSON line to the pending buffer. Thread: game thread only. */
    void EnqueueLine(FString&& JsonLine);

    /** Ticker callback: swap buffer and dispatch async write. Returns true to keep ticking. */
    bool OnFlushTick(float DeltaTime);

    /** Serialize a simple JSON object from a TMap<FString,FString>. */
    static FString MakeJsonLine(const TMap<FString, FString>& Fields);

    /** Get current ISO 8601 timestamp string. */
    static FString NowISO8601();

    /** The log file path for this session. */
    FString LogFilePath;

    /** Pending lines awaiting flush. Protected by PendingLock. */
    TArray<FString> PendingLines;

    /** Lock protecting PendingLines. */
    FCriticalSection PendingLock;

    /** Whether we've been initialized. */
    bool bInitialized = false;

    /** Ticker handle for periodic flush. */
    FTSTicker::FDelegateHandle TickerHandle;
};
