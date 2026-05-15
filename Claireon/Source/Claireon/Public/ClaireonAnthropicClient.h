// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Dom/JsonObject.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Ticker.h"

class FClaireonServer;
class FClaireonREPLLogger;

/**
 * Event types broadcast to the REPL widget for live display.
 */
enum class EREPLEventType : uint8
{
	AssistantText,		   // A text content block from Claude
	ToolCallStarted,	   // A tool_use block received — tool about to execute
	ToolCallCompleted,	   // Tool execution finished
	ToolCallCancelled,	   // Tool was skipped due to cancellation
	Cancelled,			   // Full request was cancelled
	Error,				   // API or network error
	Finished,			   // Conversation turn complete (end_turn)
	FreshContextSuggested, // Claude included [SUGGEST_FRESH_CONTEXT] marker
	Retrying,			   // Retry in progress (informational, not a failure)
};

/** Single event dispatched to the widget. */
struct FREPLEvent
{
	EREPLEventType Type;
	FString Text;				 // Message text or tool result content
	FString ToolName;			 // For tool events
	FString ToolUseId;			 // For matching start/complete pairs
	FString ToolArgsJson;		 // Pretty-printed tool arguments
	double DurationMs = 0.0;	 // For ToolCallCompleted
	bool bIsError = false;		 // For ToolCallCompleted, Error
	FString FreshContextHandoff; // For FreshContextSuggested
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnREPLEvent, const FREPLEvent&);

/** Semantic classification of Anthropic API HTTP failures. */
enum class EAnthropicErrorCategory : uint8
{
	Success,	 // HTTP 200
	Network,	 // bConnectedSuccessfully == false && FailureReason != TimedOut
	Timeout,	 // FailureReason == EHttpFailureReason::TimedOut
	RateLimit,	 // HTTP 429
	Overloaded,	 // HTTP 529
	ServerError, // HTTP 500, 502, 503, 504
	AuthError,	 // HTTP 401, 403
	ClientError, // HTTP 400 (bad request body)
};

/**
 * Anthropic Messages API client for the MCP REPL.
 *
 * All public methods must be called from the game thread.
 * HTTP requests are async; tool execution is marshaled to the game thread.
 * Settings are read fresh from UClaireonSettings on each SendMessage() call.
 */
class FClaireonAnthropicClient : public TSharedFromThis<FClaireonAnthropicClient>
{
public:
	explicit FClaireonAnthropicClient(
		FClaireonServer* InServer,
		TSharedPtr<FClaireonREPLLogger> InLogger);

	/** Send a user message and start the conversation loop. */
	void SendMessage(const FString& UserText, const FString& ConversationId);

	/** Cancel the active request (Escape / Ctrl+. / Stop button). */
	void CancelActiveRequest();

	/** Reset conversation history (New Topic). */
	void ResetConversation();

	/** Whether a request is currently in flight. */
	bool IsRequestInFlight() const { return bRequestInFlight; }

	/** Get approximate token count for context indicator (messages + system prompt + tool definitions). */
	int32 GetApproximateTokenCount() const;

	/** Number of tools sent to the API (Code Mode: 2 — execute + search_tools). */
	int32 GetAPIToolCount() const;

	/** Delegate broadcast for every REPL event (for live display). */
	FOnREPLEvent OnREPLEvent;

	/**
	 * Always returns the live server from the module, not the stale constructor pointer.
	 * The tab may have been opened before the server was started, making the stored
	 * Server pointer null even though the server is now running.
	 */
	FClaireonServer* GetCurrentServer() const;

	/** Classify an HTTP response for retry decisions. Public for test access. */
	static EAnthropicErrorCategory ClassifyHTTPResponse(
		bool bConnectedSuccessfully,
		int32 HttpStatusCode,
		EHttpRequestStatus::Type RequestStatus,
		EHttpFailureReason FailureReason);

	/** Whether a given error category is retryable. */
	static bool IsRetryable(EAnthropicErrorCategory Category);

	/** Calculate retry delay. Public for test access. */
	static float CalculateRetryDelay(EAnthropicErrorCategory Category,
		const FString& RetryAfterHeader, int32 Attempt,
		float InitialDelay, float MaxDelay);

	/** Clean up ticker handles. Call from widget destructor before releasing shared pointer. */
	void Shutdown();

private:
	/** Scans ConversationMessages for assistant tool_use blocks without matching
	 *  tool_result blocks in the following user message. Injects synthetic error
	 *  tool_result blocks for any unmatched tool_use IDs. Handles both complete-miss
	 *  (zero tool_results) and partial-miss (M < N tool_results for N tool_uses).
	 *  Called at the top of PostToAPI() as a defensive invariant check. */
	// Repair tool_use/tool_result pairing violations before sending to API
	void SanitizeConversationHistory();

	/** Build the tool definitions array from registered server tools. */
	TArray<TSharedPtr<FJsonValue>> BuildToolDefinitions() const;

	/** Build the full request body JSON. */
	TSharedPtr<FJsonObject> BuildRequestBody() const;

	/** Send the current message array to the API. Async. */
	void PostToAPI(TSharedPtr<FThreadSafeBool> CancelToken, int32 Depth);

	/** Handle a completed HTTP response. Called on game thread. */
	void OnHTTPResponse(FHttpRequestPtr Request, FHttpResponsePtr Response,
		bool bConnectedSuccessfully, TSharedPtr<FThreadSafeBool> CancelToken, int32 Depth);

	/** Execute all tool_use blocks from a response. Returns false if cancelled. */
	bool ExecuteToolUses(const TArray<TSharedPtr<FJsonValue>>& ContentArray,
		TSharedPtr<FThreadSafeBool> CancelToken,
		TArray<TSharedPtr<FJsonValue>>& OutToolResults);

	/** Parse [SUGGEST_FRESH_CONTEXT] and [FRESH_CONTEXT_HANDOFF] markers from text. */
	static void ParseContextMarkers(FString& InOutText,
		FString& OutHandoff, bool& bOutSuggestFresh);

	/** Serialize JSON to string. */
	static FString SerializeJson(const TSharedPtr<FJsonObject>& Obj);

	/** Broadcast a REPL event on the game thread. */
	void BroadcastEvent(FREPLEvent&& Event);

	/** Clean up after a turn completes or is cancelled. */
	void FinalizeTurn();

	/**
	 * Anthropic API requires tool names matching ^[a-zA-Z0-9_-]{1,128}$.
	 * Our MCP tools use dot-separated names (e.g. "editor.assets.list").
	 * This map translates sanitized API names (dots→underscores) back to
	 * the original registered tool name for lookup during execution.
	 * Rebuilt on every BuildToolDefinitions() call.
	 */
	mutable TMap<FString, FString> SanitizedToOriginalToolName;

	/** Stored pointer (may be null if tab was opened before server started — use GetCurrentServer()). */
	FClaireonServer* Server = nullptr;
	TSharedPtr<FClaireonREPLLogger> Logger;

	/** Full conversation message history. */
	TArray<TSharedPtr<FJsonObject>> ConversationMessages;

	/** Active cancellation token. Replaced on each SendMessage(). */
	TSharedPtr<FThreadSafeBool> ActiveCancelToken;

	/** Active HTTP request for CancelRequest(). */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveHttpRequest;

	/** Whether a request is currently in flight. */
	bool bRequestInFlight = false;

	/** Current conversation ID string (conv_001, etc.). */
	FString CurrentConversationId;

	/** Conversation counter for ID generation. */
	int32 ConversationCounter = 0;

	// --- Retry state ---
	int32 CurrentRetryAttempt = 0;
	FTSTicker::FDelegateHandle RetryTickerHandle;
	TSharedPtr<FThreadSafeBool> PendingRetryCancelToken;
	int32 PendingRetryDepth = 0;

	// --- Rate limiter ---
	double LastRequestTimestamp = 0.0;
	FTSTicker::FDelegateHandle RateLimitTickerHandle;

	/** Guard for the one-shot connect-time stale-spill sweep (see SendMessage). */
	bool bHasSwept = false;
};
