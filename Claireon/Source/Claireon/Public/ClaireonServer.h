// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "TimerManager.h"
#include "ClaireonTypes.h"

class IClaireonTool;
class FClaireonServer;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnUserStopChanged, bool /*bIsActive*/);
DECLARE_MULTICAST_DELEGATE(FOnToolsChanged);

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
	 *
	 * Prefer TryStart / StartEphemeral. Start(uint32) is a thin wrapper around
	 * TryStart(Port, bExclusive=false) for callers that want the incremental
	 * retry behaviour; new callers (FClaireonModule::StartServer + the smoke
	 * test) take the explicit single-attempt path so EADDRINUSE can be detected
	 * and the editor can decide whether to auto-promote into proxy-attached mode.
	 */
	bool Start(uint32 Port);

	/**
	 * Single-attempt bind on the given port.
	 *
	 * @param Port The port to bind. Typically the per-worktree SHA port
	 *             returned by Claireon::DeriveDefaultMcpPort.
	 * @return true if the server bound and is now listening on `Port`.
	 *         false on bind failure (EADDRINUSE or otherwise); the caller
	 *         decides whether to abort, retry, or auto-promote.
	 *
	 * Note on SO_EXCLUSIVEADDRUSE: UE's IHttpRouter owns the underlying
	 * listener, so the editor cannot directly set socket options. The
	 * proxy-side bind enforces exclusivity for the SHA port (the proxy holds
	 * it whenever it runs). For the editor's direct-connect
	 * branch, the ordering of "TryStart -> EADDRINUSE -> probe 43017 ->
	 * auto-promote" is what guarantees we don't double-bind by accident.
	 */
	bool TryStart(uint16 Port);

	/**
	 * Auto-promote: bind a local listener on a kernel-picked
	 * ephemeral port (port 0). Returns the port the listener actually bound
	 * to, or 0 on failure. Used by the auto-promote path in StartServer
	 * when the proxy already owns the SHA port.
	 */
	uint16 StartEphemeral();

	/**
	 * Set the per-session bearer token required by every incoming HTTP request.
	 * Generated fresh at each StartServer call (see module wiring) and passed
	 * to the proxy via /editor/register. Empty token disables gating and is
	 * ONLY permitted for command-line overrides / direct-connect scenarios
	 * where the proxy is bypassed (logged as a warning on Start).
	 *
	 * Called by FClaireonModule before Start(); MUST be called from the
	 * game thread.
	 */
	void SetSessionToken(const FString& Token) { SessionToken = Token; }

	/** Return the active session token (empty when gating is disabled). */
	const FString& GetSessionToken() const { return SessionToken; }

	/** Stop the server and clean up routes */
	void Stop();

	/** Whether the server is currently running */
	bool IsRunning() const { return bIsRunning; }

	/** Get the port the server is listening on */
	uint32 GetPort() const { return BoundPort; }

	/**
	 * Register a tool with the server. Can be called during or after startup.
	 * @param Tool - The tool to register.
	 * @param SourceProvider - The provider name for source tracking. NAME_None if unspecified.
	 */
	CLAIREON_API void RegisterTool(TSharedPtr<IClaireonTool> Tool, FName SourceProvider = NAME_None);

	/** Unregister a single tool by name. No-op if tool is not found. */
	CLAIREON_API void UnregisterTool(const FString& ToolName);

	/** Unregister all tools registered by the given source provider. No-op if NAME_None. */
	CLAIREON_API void UnregisterToolsBySource(FName SourceProvider);

	/** Delegate broadcast when tools are added or removed. */
	FOnToolsChanged OnToolsChanged;

	/** Generation counter incremented each time the tool list changes. */
	uint32 GetToolListGeneration() const { return ToolListGeneration; }

	/** Get the tool source map (ToolName -> ProviderName). */
	const TMap<FString, FName>& GetToolSourceMap() const { return ToolSourceMap; }

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

	/**
	 * FPlatformTime::Seconds() timestamp of the most recent incoming request.
	 * Updated at the entry of HandlePostRequest on the game thread. Used by the
	 * toolbar status dot to drive the "processing" blink animation.
	 * Returns 0.0 if no request has been received yet.
	 */
	double GetLastRequestTime() const { return LastRequestTime; }

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
	/** In-memory registry entry loaded from a JSON file in Content/MCP/Prompts/. */
	struct FPromptTemplate
	{
		FString Description;
		FString Role;
		FString TextTemplate;
		FString SourcePath;
	};

	/** In-memory registry entry loaded from a JSON file in Content/MCP/Resources/. */
	struct FResourceTemplate
	{
		FString Name;
		FString Description;
		FString MimeType;
		FString TextTemplate;
		FString SourcePath;
	};

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
	TSharedPtr<FJsonObject> HandlePromptsList(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandlePromptsGet(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandleResourcesList(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandleResourcesRead(const FMCPRequestContext& Context);
	TSharedPtr<FJsonObject> HandleResourceTemplatesList(const FMCPRequestContext& Context);

	/** Dispatch a parsed JSON-RPC request to the appropriate handler */
	TSharedPtr<FJsonObject> DispatchRequest(const FMCPRequestContext& Context);

	/** Scan Content/MCP/Prompts and Content/MCP/Resources, populating the registries. */
	void LoadMCPContent();

	/** Load all *.json files under Directory, inserting each into LoadedPrompts keyed by relative-path-without-extension. */
	void LoadPromptsFromDirectory(const FString& Directory);

	/** Load all *.json files under Directory, inserting each into LoadedResources keyed by "claireon://" + relative-path-without-extension. */
	void LoadResourcesFromDirectory(const FString& Directory);

	/** Replace {{name}} tokens with matching values from Variables. Unknown tokens are left intact. */
	static FString SubstitutePlaceholders(const FString& Template, const TMap<FString, FString>& Variables);

	/** Build the runtime variable map used by prompt and resource templates (project.*, server.*). */
	TMap<FString, FString> BuildRuntimeVariables() const;

	/** Validate Origin and Host headers for localhost-only access */
	bool ValidateRequestHeaders(const FHttpServerRequest& Request) const;

	/**
	 * Constant-time bearer-token check against SessionToken.
	 * Returns true when:
	 *   - SessionToken is empty (gating disabled), OR
	 *   - Request carries Authorization: Bearer <SessionToken>.
	 */
	bool ValidateBearerToken(const FHttpServerRequest& Request) const;

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

	/** File-backed prompt templates. Key is the prompt name (relative path without .json). */
	TMap<FString, FPromptTemplate> LoadedPrompts;

	/** File-backed resource templates. Key is the full URI (e.g. claireon://project/info). */
	TMap<FString, FResourceTemplate> LoadedResources;

	/** Maps tool name to the provider that registered it */
	TMap<FString, FName> ToolSourceMap;

	/** Incremented each time the tool list changes (for future SSE notifications) */
	uint32 ToolListGeneration = 0;

	/** HTTP route handles for cleanup */
	TArray<FHttpRouteHandle> RouteHandles;

	/** The port the server is bound to */
	uint32 BoundPort = 0;

	/**
	 * Bearer token required on inbound requests. Set by the module before
	 * Start(). Empty = gating disabled (dev-only direct-connect path). Never
	 * logged. Regenerated per StartServer call so a torn-down + restarted
	 * editor does not share credentials with its predecessor.
	 */
	FString SessionToken;

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

	/** FPlatformTime::Seconds() of the most recent HandlePostRequest entry. Game-thread only. */
	double LastRequestTime = 0.0;

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

	/** Whether feedback_submit was called this session */
	bool bFeedbackSubmittedThisSession = false;

	/** Thresholds that trigger a feedback nudge */
	static constexpr int32 FeedbackNudgeErrorThreshold = 4;
	static constexpr int32 FeedbackNudgeTotalThreshold = 10;
};
