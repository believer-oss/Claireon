// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Shared compile-time constants for the Claireon MCP always-on proxy.
 *
 * SOURCE-OF-TRUTH for values duplicated into the Python proxy
 * (Plugins/Claireon/Content/Python/claireon_proxy.py). The Python side
 * carries a sentinel comment (PROXY_REG_PORT_SOURCE_OF_TRUTH) referencing
 * this header; a pre-commit/CI check parses both files and asserts equality.
 * Any change to a value here MUST be reflected in claireon_proxy.py, and the
 * stage/commit making the change MUST mention the sync.
 *
 * See sibling docs under Docs/llm/work/always-on-mcp-proxy/:
 *   - ALWAYS_ON_MCP_PROXY_PROTOCOL.md  (wire contract)
 *   - ALWAYS_ON_MCP_PROXY_CPP.md       (C++ plugin spec)
 *   - ALWAYS_ON_MCP_PROXY_PYTHON.md    (Python proxy spec)
 */
namespace ClaireonProxy
{
	// Fixed TCP port on 127.0.0.1 where the proxy exposes its
	// /editor/register, /editor/heartbeat, /editor/unregister endpoints.
	// Compile-time constant shared with Python. Any change requires bumping
	// both sides. Chosen from the IANA registered range; expected to be free
	// on developer workstations.
	//
	// All constants here use `inline constexpr` (C++17) rather than
	// `static constexpr` so they have external linkage and do NOT trip
	// clang's -Wunused-const-variable / -Wunused-variable in any including
	// translation unit that references only a subset of them. Non-unity
	// Linux clang strict (-Werror) treats internal-linkage unused const
	// variables as hard errors; unity Linux and MSVC silently accept them.
	inline constexpr int32 PROXY_REG_PORT = 43017;

	// Heartbeat cadence (editor -> proxy). See PROTOCOL.md "Shared constants".
	inline constexpr int32 HEARTBEAT_INTERVAL_SECONDS = 5;

	// Proxy-side staleness threshold. A session is evicted after this many
	// seconds with no heartbeat. Must exceed PythonExecutionTimeoutSeconds
	// (Editor Preferences > Plugins > Claireon) so an in-flight python_execute
	// call cannot evict the session before the watchdog fires.
	inline constexpr int32 HEARTBEAT_STALENESS_SECONDS = 180;

	// IDLE_AUTO_EXIT_HOURS removed. The singleton proxy runs until
	// SIGINT/SIGTERM; port-as-lock is the source of truth for liveness, so a
	// long-lived idle proxy is harmless.

	// Default timeout (seconds) for the proxy's outbound MCP forward to the
	// editor. Overridden by UClaireonSettings::ProxyForwardTimeoutSeconds
	// at runtime. Long default so slow tools (asset cook, large map
	// validations) don't trip it.
	inline constexpr int32 FORWARD_DEFAULT_TIMEOUT_SECONDS = 600;

	// Endpoint path constants. Declared here so the editor-side proxy client
	// and any C++ diagnostics never drift from the wire contract.
	inline constexpr const TCHAR* RegisterEndpoint   = TEXT("/editor/register");
	inline constexpr const TCHAR* HeartbeatEndpoint  = TEXT("/editor/heartbeat");
	inline constexpr const TCHAR* UnregisterEndpoint = TEXT("/editor/unregister");
	// Lightweight GET endpoint used as a reachability probe. Hitting
	// /editor/register with an empty body would also tell us the listener
	// is up, but it generates a "register rejected reason=malformed_request"
	// warning in the proxy log on every editor boot, polluting real
	// validation failures. /health is the cheap, side-effect-free alternative.
	inline constexpr const TCHAR* HealthEndpoint     = TEXT("/health");
	// Editor POSTs this after FClaireonBridge::EnsureRegistered()
	// to signal that the tool catalog is populated and the Python bridge is
	// initialized. Until this POST is received the proxy returns a
	// "warming up" error rather than "build and launch editor first".
	inline constexpr const TCHAR* ReadyEndpoint      = TEXT("/editor/ready");
	// Admin endpoint that asks the proxy to bind a worktree's SHA-derived
	// MCP port. The editor calls this after EnsureProxyRunning so a proxy
	// spawned by a different worktree still ends up holding our SHA port --
	// without it, the editor's TryStart on that port succeeds, the
	// auto-promote branch never fires, and a token-gated DirectConnect
	// editor rejects Claude Code's requests with 401.
	inline constexpr const TCHAR* EnsureWorktreeEndpoint = TEXT("/admin/ensure_worktree");

	// Loopback host used by the proxy client when assembling absolute URLs.
	inline constexpr const TCHAR* LoopbackHost = TEXT("127.0.0.1");
}
