// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

/**
 * Proxy registration lifecycle state. Driven by the heartbeat ticker after
 * StartServer hands the client to RetryRegister; transitions are documented
 */
enum class EClaireonProxyState : uint8
{
	/** StartServer has not been called or proxy wiring is disabled. */
	Unstarted,
	/**
	 * Heartbeat ticker is trying to register. The first attempt may fail
	 * because the proxy is mid-spawn or unreachable; subsequent ticks back
	 * off and retry rather than tearing down the editor's MCP server.
	 */
	RetryRegister,
	/** /editor/register succeeded; heartbeats are flowing. */
	Registered,
	/**
	 * Terminal: a 4xx malformed/auth rejection from /editor/register OR a
	 * heartbeat reply with evicted_by from a newer same-worktree editor
	 * (D6 newest-wins). Manual reconnect required (RequestReconnect).
	 */
	Failed,
};

/** Outcome of a single /editor/register attempt. */
enum class ERegisterResult : uint8
{
	/** Server responded {accepted: true}; session is live. */
	Accepted,
	/**
	 * Transport failure (status==0, 5xx) or transient body shape we can
	 * legitimately retry. The state machine schedules another attempt.
	 */
	Transient,
	/**
	 * 4xx malformed body or auth-style rejection that retrying will not
	 * fix. The state machine moves to Failed and waits for an operator
	 * Reconnect.
	 */
	TerminalAuthOrMalformed,
};

/** Why a heartbeat tick failed (for the state machine). */
enum class EHeartbeatReason : uint8
{
	/** Heartbeat 200 OK; nothing to do. */
	None,
	/**
	 * Heartbeat reply carried unknown_session without an evicted_by field.
	 * We treat this as proxy-side staleness (proxy restarted, session GC'd,
	 * etc.) and bounce to RetryRegister.
	 */
	UnknownSessionStale,
	/**
	 * Heartbeat reply carried unknown_session WITH an evicted_by field
	 * (D6 newest-wins). Terminal: another editor took our worktree slot.
	 */
	UnknownSessionEvicted,
	/** Transport failed (5xx or no response). Bounce to RetryRegister. */
	TransportFailure,
};

/** Parsed result of a single /editor/heartbeat call. */
struct FHeartbeatResult
{
	/** True iff the proxy responded with {ok: true}. */
	bool Ok = false;
	/** Why the heartbeat failed (None when Ok). */
	EHeartbeatReason Reason = EHeartbeatReason::None;
	/** evicted_by.pid (newest-wins). 0 when not present. */
	uint32 EvictedByPid = 0;
	/** evicted_by.start_time_ns (newest-wins). 0 when not present. */
	int64 EvictedByStartTimeNs = 0;
};

/**
 * Editor-side client for the always-on Claireon MCP proxy.
 *
 * The proxy itself is a long-running Python process
 * (Claireon/Content/Python/claireon_proxy.py) that outlives the
 * editor. This class is responsible for:
 *   - Spawning the proxy if no live instance is registered for this worktree.
 *     Uses FPlatformProcess::CreateProc with bLaunchDetached=true so the
 *     proxy survives editor exit. A documented cmd.exe /c start /b fallback
 *     lives in the cpp file to handle cases where DETACHED_PROCESS alone
 *     fails to orphan the child.
 *   - POSTing /editor/register once the proxy is reachable.
 *   - Driving a 5-second FTSTicker-based heartbeat to /editor/heartbeat.
 *   - POSTing /editor/unregister on clean editor shutdown (the proxy itself
 *     keeps running so subsequent editor restarts are near-instant).
 *
 * This client does NOT own the proxy's lifetime and does NOT forward tool
 * calls. Claude Code connects directly to the proxy on its MCP_PORT; the
 * proxy in turn MCP-clients back into the editor on editor_mcp_port.
 *
 * current implementation contract. The next-iteration design (singleton
 * proxy serving all worktrees, port-as-lock, editor reconnect on register
 */
class FClaireonProxyClient
{
public:
	FClaireonProxyClient();
	~FClaireonProxyClient();

	FClaireonProxyClient(const FClaireonProxyClient&) = delete;
	FClaireonProxyClient& operator=(const FClaireonProxyClient&) = delete;

	/**
	 * Ensure a proxy process is running for this worktree. If /editor/register
	 * on PROXY_REG_PORT responds within the attach budget, reuses the existing
	 * proxy. Otherwise spawns a fresh process via FPlatformProcess::CreateProc
	 * (detached + hidden). Polls with exponential backoff up to ~3.2s.
	 *
	 * Returns true if the proxy is reachable when the call returns. A false
	 * return means the caller should abort StartServer.
	 */
	bool EnsureProxyRunning();

	/**
	 * Ask the proxy to bind the SHA-derived MCP port for this worktree via
	 * POST /admin/ensure_worktree. Idempotent on the proxy side; safe to
	 * call repeatedly.
	 *
	 * Why this is needed even after EnsureProxyRunning(): a proxy spawned
	 * by some other worktree's editor only holds its own worktree's SHA
	 * port; it does NOT auto-claim ports for worktrees it has never been
	 * told about. Without this call, the editor's TryStart on its SHA port
	 * succeeds, the editor falls into DirectConnect mode (token-gated when
	 * bEnableProxy was opted in), and Claude Code requests to .mcp.json's
	 * port land at the editor directly and get rejected as 401 because the
	 * proxy is not actually in the path.
	 *
	 * Passing MCPPortHint > 0 forwards the editor's preferred port to the
	 * proxy via the body's `mcp_port` field; the proxy uses the hint
	 * instead of re-deriving the SHA. Pass 0 to let the proxy derive.
	 *
	 * Returns true on 200 (proxy bound the port for us) or already-bound.
	 * Returns false on transport failure or any non-2xx response (e.g.
	 * 409 port_held_externally). A false return is informational; the
	 * caller can still proceed into TryStart and rely on the existing
	 * abort branch if the port is genuinely unavailable.
	 */
	bool EnsureWorktreeBound(const FString& WorktreeRoot, int32 MCPPortHint = 0);

	/**
	 * Register this editor instance with the proxy. Must be called after a
	 * successful EnsureProxyRunning(). Sends PID, worktree root, the editor
	 * process's start_time_ns (D8 newest-wins discriminator), build ID,
	 * proxy-script SHA-256 hash, the editor's internal MCP port, and the
	 * bearer token the proxy must present on subsequent forwarded tool calls.
	 *
	 * On {accepted: true} -> true; starts the heartbeat ticker. The caller
	 * must have populated StartTimeNs before this call (typically via
	 * BeginRetryRegister at module startup).
	 * On 4xx malformed/auth -> false (terminal); caller surfaces in
	 * diagnostics.
	 * On any other rejection / transport failure -> false (transient);
	 * the heartbeat ticker retries.
	 */
	bool Register(int32 EditorMCPPort, const FString& EditorMCPToken, const FString& BuildId);

	/**
	 * Send /editor/unregister and stop the heartbeat ticker. Does NOT kill
	 * the proxy process -- the proxy is expected to keep running for the
	 * next editor launch.
	 */
	void Unregister();

	/** True while a /editor/register has succeeded and no Unregister has run. */
	bool IsRegistered() const { return bIsRegistered; }

	/** Current process start_time_ns (0 before the first BeginRetryRegister). */
	int64 GetStartTimeNs() const { return StartTimeNs; }

	/**
	 * Per-host process start time (D8 newest-wins discriminator).
	 *
	 * Windows: GetProcessTimes FILETIME, packed as (high<<32)|low. Units are
	 * 100ns ticks since 1601.
	 * Linux: /proc/self/stat field 22 (1-indexed) in clock-tick units since
	 * boot.
	 *
	 * Mirrors claireon_proxy.py::_process_start_time_ns so the (pid,
	 * start_time_ns) tuple compares cleanly between editor and proxy on
	 * the same host. The tuple is never compared across hosts, so unit
	 * consistency only matters per host.
	 *
	 * Returns 0 on failure; the proxy accepts a 0 start_time_ns (pid alone
	 * is unique on a live host) so a failed query degrades gracefully.
	 */
	static int64 GetSelfStartTimeNs();

	/** Current proxy registration lifecycle state (see EClaireonProxyState). */
	EClaireonProxyState GetState() const { return ProxyState; }

	/**
	 * Move the state machine into RetryRegister and clear any backoff so the
	 * next heartbeat tick attempts a fresh /editor/register. Called by the
	 * diagnostics widget's Reconnect button when ProxyState is Failed; safe
	 * to call from any state (a no-op for Unstarted).
	 *
	 * Does NOT touch the editor's MCP server -- only the proxy registration
	 * is being retried.
	 */
	void RequestReconnect();

	/**
	 * Begin the RetryRegister lifecycle: cache the editor's port/token/build
	 * info, set ProxyState to RetryRegister, kick the heartbeat ticker. Does
	 * NOT block on register success -- the ticker drives the state machine
	 * and tolerates a proxy that is mid-spawn or temporarily unreachable.
	 *
	 * Replaces the previous synchronous EnsureProxyRunning + Register call
	 * sequence at module-startup time per D4.
	 */
	void BeginRetryRegister(int32 EditorMCPPort, const FString& EditorMCPToken, const FString& BuildId);

	/** PROXY_REG_PORT derivative used for composing editor-registration URLs. */
	static FString GetRegistrationBaseUrl();

	/**
	 * Stage 010 (auto-promote): GET http://127.0.0.1:43017/admin/health with a
	 * tight timeout and parse the response. On 200 with a `version_hash` field
	 * present, caches the proxy's reported pid in CachedProxyPid for the
	 * diagnostics tab and returns true. On any failure (transport error, non-
	 * 200, missing `version_hash`) returns false and leaves CachedProxyPid
	 * untouched.
	 *
	 * The probe distinguishes "Claireon's proxy is on 43017" from "some
	 * unrelated process is on 43017"; a non-Claireon occupant either does not
	 * answer /admin/health or answers without the expected JSON shape.
	 */
	bool PingProxyHealth();

	/**
	 * POST /admin/ensure_worktree so the proxy binds (or confirms it already
	 * binds) this worktree's SHA-derived MCP port. Required before the editor
	 * attempts an auto-promote handshake: without this call the proxy holds
	 * only PROXY_REG_PORT (43017) and the SHA port is unowned, so the editor's
	 * SHA-port TryStart wins the race and DirectConnect mode silently strands
	 * the proxy with no way to route traffic for this worktree.
	 *
	 * Returns true on a 200 {bound:true} response. Any non-200 (incl. 409
	 * port_held_by_*) returns false; the caller falls back to the legacy race.
	 */
	bool EnsureWorktreeBound(const FString& WorktreeRoot);

	/** Pid the proxy reported via /admin/health (0 if no successful probe yet). */
	uint32 GetCachedProxyPid() const { return CachedProxyPid; }

	/**
	 * Stage 010: tell the client which port the editor's local MCP listener
	 * is bound to. Called by the auto-promote path in StartServer after
	 * StartEphemeral picks a port; used by the next /editor/register payload.
	 */
	void SetEditorMcpPort(int32 EditorMCPPort) { CachedEditorMCPPort = EditorMCPPort; }

	// ----- Test seams (do not call from production code) -----

	/**
	 * Inject overrides for the two transport calls (RegisterOnce, SendHeartbeat).
	 * When non-null, the corresponding override is invoked instead of the real
	 * HTTP path. Pass empty TFunction() to clear an override. Used by
	 * ClaireonRetryRegister.spec.cpp to drive the state machine deterministically
	 * without spawning a real proxy. Production code never sets these.
	 */
	void SetTransportOverrides_TestOnly(
		TFunction<ERegisterResult()> InRegisterOverride,
		TFunction<FHeartbeatResult()> InHeartbeatOverride);

	/**
	 * Run a single state-machine pulse with NowSeconds substituted for
	 * FPlatformTime::Seconds(). Skips EnsureProxyRunning so the test does
	 * not need a live proxy or a vendored Python. Mirrors HeartbeatTick's
	 * dispatch but is callable from a synchronous test body.
	 */
	void TickForTest(double NowSeconds);

	/** Direct state setter for tests. No-ops on production code paths. */
	void SetState_TestOnly(EClaireonProxyState NewState) { ProxyState = NewState; }

	/** Read NextRegisterAttemptSeconds for backoff-doubling assertions. */
	double GetNextRegisterAttemptSeconds_TestOnly() const { return NextRegisterAttemptSeconds; }

	/** Read CurrentBackoffSeconds for backoff-doubling assertions. */
	double GetCurrentBackoffSeconds_TestOnly() const { return CurrentBackoffSeconds; }

	/** Seed cached registration parameters without firing the HTTP call. */
	void SeedCachedRegistration_TestOnly(int32 EditorMCPPort, const FString& EditorMCPToken, const FString& BuildId)
	{
		CachedEditorMCPPort = EditorMCPPort;
		CachedEditorMCPToken = EditorMCPToken;
		CachedBuildId = BuildId;
	}

private:
	/**
	 * Resolve the vendored Python executable path under EngineDir/Binaries/
	 * ThirdParty/Python3/Win64. Returns empty string if the path does not
	 * exist (caller must log CRITICAL).
	 */
	static FString ResolveVendoredPythonExe();

	/** Resolve the absolute path of claireon_proxy.py inside the plugin's Content/Python dir. */
	static FString ResolveProxyScriptPath();

	/**
	 * Returns the absolute path of a runtime copy of claireon_proxy.py under
	 * `<ProjectSaved>/Claireon/runtime/`. Copies the source script there if
	 * the runtime copy is missing or its bytes differ from the source.
	 *
	 * Spawning python.exe with the script in `Claireon/Content/Python/`
	 * lets antivirus hold a scan handle on a git-tracked file, which then
	 * blocks `git pull` from updating it. The runtime copy lives under Saved/
	 * (gitignored), so AV's handle never collides with git updates.
	 */
	static FString ResolveOrCreateRuntimeScriptPath();

	/**
	 * Spawn claireon_proxy.py as a detached child. First attempts CreateProc
	 * with bLaunchDetached=true; on Windows, falls back to cmd.exe /c start
	 * /b if the primary spawn reports failure. Returns a valid handle on
	 * success; returned handle is immediately closed (we do not wait on it)
	 * but a boolean success is propagated by the return value.
	 */
	bool SpawnDetachedProxy();

	/**
	 * Poll POST http://127.0.0.1:PROXY_REG_PORT/ with a lightweight request
	 * until the listener accepts a TCP connection or the attempt budget is
	 * exhausted. Starts at 100ms, doubles, 10 attempts total (~3.2s).
	 */
	bool WaitForProxyReachable();

	/** Compute sha256 of claireon_proxy.py; returned as lower-case hex. */
	static FString ComputeProxyScriptHash();

	/** Start / stop / tick the heartbeat ticker. */
	void StartHeartbeatTicker();
	void StopHeartbeatTicker();
	bool HeartbeatTick(float DeltaTime);

	/**
	 * Single attempt at /editor/register. Returns Accepted on {accepted:true},
	 * TerminalAuthOrMalformed on a 4xx with an "auth"/"malformed_request"-shaped
	 * reason, Transient otherwise. Stage 005 dropped the version_mismatch
	 * respawn path: the proxy now treats version drift as advisory (D5), so
	 * the editor never sees that rejection. Used by both BeginRetryRegister
	 * (initial wiring) and the heartbeat tick state machine.
	 *
	 * The legacy public Register(...) bool wrapper still exists for the existing
	 * smoke tests; it forwards to RegisterOnce and returns true iff Accepted.
	 */
	ERegisterResult RegisterOnce();

	/**
	 * Single /editor/heartbeat probe. Parses the proxy's reply into a struct
	 * carrying both the ok/reason discriminator AND any evicted_by metadata
	 * (D6 newest-wins). Wire-defensive: today's per-worktree proxy never
	 * sets evicted_by, so an absent field is treated as staleness, which is
	 * the correct fallback for that proxy.
	 */
	FHeartbeatResult SendHeartbeat();

	/**
	 * Update NextRegisterAttemptSeconds with exponential backoff (250ms
	 * doubling to a 5s cap). Logs a single Warning every 60s during a
	 * sustained outage via LastWarnedAtSeconds.
	 */
	void ScheduleRetry(double NowSeconds);

	/** Current registration lifecycle state (D4). */
	EClaireonProxyState ProxyState = EClaireonProxyState::Unstarted;

	/** Earliest FPlatformTime::Seconds() at which the next register attempt may run. */
	double NextRegisterAttemptSeconds = 0.0;

	/** Last seconds-time at which we emitted a Warning during sustained RetryRegister. */
	double LastWarnedAtSeconds = 0.0;

	/** Current backoff interval, doubled on each transient failure (capped at 5s). */
	double CurrentBackoffSeconds = 0.25;

	/**
	 * Cached registration parameters so the heartbeat tick can re-register
	 * after an "unknown_session" eviction or a proxy respawn without the
	 * caller having to re-supply them.
	 */
	int32 CachedEditorMCPPort = 0;
	FString CachedEditorMCPToken;
	FString CachedBuildId;

	/**
	 * This editor process's start_time_ns (D8 newest-wins discriminator).
	 * Populated once at BeginRetryRegister via GetSelfStartTimeNs(); reused
	 * for every register/heartbeat/unregister body. Defaults to 0 so the
	 * proxy still accepts a register if the OS query failed (pid alone is
	 * unique on a live host).
	 */
	int64 StartTimeNs = 0;

	/** Current proxy PID, if we have one (used for version_mismatch kill path). */
	uint32 ProxyPid = 0;

	/**
	 * Pid the proxy reported via /admin/health, cached so the diagnostics
	 * widget can surface "Joined proxy session because pid=N was already
	 * running" without re-probing. 0 until PingProxyHealth() succeeds.
	 */
	uint32 CachedProxyPid = 0;

	/** Heartbeat ticker handle. */
	FTSTicker::FDelegateHandle HeartbeatTickerHandle;

	/** True between successful Register() and Unregister() / shutdown. */
	bool bIsRegistered = false;

	/**
	 * Optional transport overrides for ClaireonRetryRegister.spec.cpp.
	 * Production paths leave these empty and the real HTTP code runs.
	 */
	TFunction<ERegisterResult()> RegisterTransportOverride;
	TFunction<FHeartbeatResult()> HeartbeatTransportOverride;
};
