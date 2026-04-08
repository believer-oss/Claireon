// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "ClaireonTypes.h"

class IClaireonTool;
class FClaireonServer;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnUserStopChanged, bool /*bIsActive*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnClaireonServerStarted, FClaireonServer& /*Server*/);

/**
 * Core MCP HTTP server. Binds HTTP routes, dispatches JSON-RPC requests,
 * and manages the MCP protocol lifecycle.
 */
class FClaireonServer : public TSharedFromThis<FClaireonServer>
{
public:
	FClaireonServer();
	~FClaireonServer();

	/**
	 * Start the server on the given port.
	 * @param Port - The port to bind to. If binding fails, retries with incremented ports.
	 * @return true if the server started successfully
	 */
	bool Start(uint32 Port);

	/** Stop the server and clean up routes */
	void Stop();

	/** Whether the server is currently running */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port the server is listening on */
	uint32 GetPort() const { return BoundPort; }

	/** Register a tool with the server. Can be called during or after startup. */
	CLAIREON_API void RegisterTool(TSharedPtr<IClaireonTool> Tool);

	/**
	 * Delegate broadcast after the server starts and all built-in tools are registered.
	 * External modules (e.g. game-specific plugins) can bind to this to register
	 * their own tools:
	 *
	 *   FClaireonServer::OnServerStarted().AddLambda([](FClaireonServer& Server) {
	 *       Server.RegisterTool(MakeShared<MyCustomTool>());
	 *   });
	 */
	static CLAIREON_API FOnClaireonServerStarted& OnServerStarted();

	/** Get the diagnostics log entries (ring buffer, newest last) */
	const TArray<FMCPDiagnosticsEntry>& GetDiagnosticsEntries() const { return DiagnosticsEntries; }

	/** Clear the diagnostics log */
	void ClearDiagnostics();

	/** Get total request count since server start */
	int32 GetTotalRequestCount() const { return TotalRequestCount; }

	/** Get error count since server start */
	int32 GetErrorCount() const { return ErrorCount; }

	/** Get the server start time */
	FDateTime GetStartTime() const { return StartTime; }

	/** Get the registered tools map (for REPL API client). */
	const TMap<FString, TSharedPtr<IClaireonTool>>& GetTools() const { return Tools; }

	/** Find a registered tool by name. Returns nullptr if not found. */
	TSharedPtr<IClaireonTool> FindTool(const FString& ToolName) const;

	/**
	 * Activate user stop mode (Ctrl+.).
	 * While active, all tools/call requests immediately return an error.
	 * Auto-clears after UserStopCooldownSeconds of no incoming tools/call.
	 */
	void ActivateUserStop();

	/** Whether user stop mode is currently active. */
	bool IsUserStopActive() const { return bUserStopActive; }

	/** Manually clear user stop mode (Resume button). */
	void ClearUserStop();

	/** Delegate broadcast when user stop state changes (for UI updates). */
	FOnUserStopChanged OnUserStopChanged;

	/** Delegate broadcast when a new diagnostics entry is added (may fire from background thread) */
	FOnMCPDiagnosticsEntryAdded OnDiagnosticsEntryAdded;

	/** Maximum diagnostics entries to retain */
	static constexpr int32 MaxDiagnosticsEntries = 200;

	/** Add a diagnostics entry to the ring buffer */
	void AddDiagnosticsEntry(FMCPDiagnosticsEntry&& Entry);

private:
	/** HTTP request handlers */
	bool HandlePostRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleGetRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleDeleteRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	/** MCP method handlers */
	TSharedPtr<FJsonObject> HandleInitialize(const FMCPRequestContext& Context);
	void HandleInitialized(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandleToolsList(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandleToolsCall(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandlePing(const FMCPRequestContext& Context);

	/** Dispatch a parsed JSON-RPC request to the appropriate handler */
	TSharedPtr<FJsonObject> DispatchRequest(const FMCPRequestContext& Context);

	/** Validate Origin and Host headers for localhost-only access */
	bool ValidateRequestHeaders(const FHttpServerRequest& Request) const;

	/** Serialize a JSON object to a UTF-8 string */
	static FString SerializeJson(const TSharedPtr<FJsonObject>& JsonObject);

	/** Write the port file to Saved/MCPServer.json */
	void WritePortFile() const;

	/** Delete the port file */
	void DeletePortFile() const;

	/** Get the path to the port file */
	FString GetPortFilePath() const;

	/** Registered tools */
	TMap<FString, TSharedPtr<IClaireonTool>> Tools;

	/** HTTP route handles for cleanup */
	TArray<FHttpRouteHandle> RouteHandles;

	/** The port the server is bound to */
	uint32 BoundPort = 0;

	/** Whether the server is running */
	bool bIsRunning = false;

	/** Whether the MCP initialization handshake has completed */
	bool bInitialized = false;

	/** Critical section for serializing game-thread tool execution */
	FCriticalSection GameThreadCriticalSection;

	/** Diagnostics ring buffer */
	TArray<FMCPDiagnosticsEntry> DiagnosticsEntries;

	/** Total requests served since server start */
	int32 TotalRequestCount = 0;

	/** Total errors since server start */
	int32 ErrorCount = 0;

	/** When the server started */
	FDateTime StartTime;

	/** Whether the user has activated emergency stop (Ctrl+.). */
	bool bUserStopActive = false;

	/** Time of last tools/call request, used for cooldown. */
	double LastToolsCallTime = 0.0;

	/** Timer handle for user-stop auto-cooldown. */
	FTimerHandle UserStopCooldownHandle;

	// --- Feedback nudge state (reset per MCP session on initialize) ---

	/** Total tools/call invocations this session */
	int32 SessionToolCallCount = 0;

	/** Failed tools/call invocations this session */
	int32 SessionToolErrorCount = 0;

	/** Whether the feedback nudge has already been sent this session */
	bool bFeedbackNudgeSent = false;

	/** Whether claireon.feedback_submit was called this session */
	bool bFeedbackSubmittedThisSession = false;

	/** Thresholds that trigger a feedback nudge */
	static constexpr int32 FeedbackNudgeErrorThreshold = 4;
	static constexpr int32 FeedbackNudgeTotalThreshold = 10;
};
