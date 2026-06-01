"""Claireon MCP Proxy.

Long-running Python proxy between Claude Code and the UE plugin's MCP server.
Keeps Claude's MCP connection alive across editor restarts, crashes, and
live-coding reloads. Presents a static minimal tool surface (search,
execute) and forwards tool calls to the currently-registered editor.

Runtime: UE's vendored Python 3
  <EngineDir>/Binaries/ThirdParty/Python3/Win64/python.exe claireon_proxy.py

Dependencies: stdlib only.

per-worktree design (current behaviour):
  - ALWAYS_ON_MCP_PROXY_PROPOSAL.md (overview)
  - ALWAYS_ON_MCP_PROXY_PROTOCOL.md (wire contract)
  - ALWAYS_ON_MCP_PROXY_PYTHON.md (this file's spec)
  - ALWAYS_ON_MCP_PROXY_CPP.md (plugin-side spec)
  - ALWAYS_ON_MCP_PROXY_TESTS.md (validation)

The next iteration -- one singleton proxy serving all worktrees, port-as-lock,
no idle-exit, editor-side reconnect on register failure -- is specified in
document supersedes the operational decisions above when implemented.

PROXY_REG_PORT_SOURCE_OF_TRUTH: ClaireonProxyConstants.h
The PROXY_REG_PORT constant below MUST match the C++ header
Claireon/Source/Claireon/Public/ClaireonProxyConstants.h.
A CI test parses both files and asserts equality.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import logging
import logging.handlers
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple

# ---------------------------------------------------------------------------
# Shared constants (MUST match the C++ side -- see ClaireonProxyConstants.h
# and ALWAYS_ON_MCP_PROXY_PROTOCOL.md).
# Stage 009 (D2): IDLE_AUTO_EXIT_HOURS removed. The singleton runs until
# SIGINT/SIGTERM; D3 port-as-lock is the source-of-truth for liveness.
# ---------------------------------------------------------------------------
PROXY_REG_PORT = 43017
HEARTBEAT_INTERVAL_SECONDS = 5
HEARTBEAT_STALENESS_SECONDS = 180
FORWARD_DEFAULT_TIMEOUT_SECONDS = 600

# How long after launch_editor was invoked the proxy will auto-wait on the first
# forwarded tool call before falling back to the "build and launch" error. The
# editor typically needs 60-120s to compile, load, and register. 120s is
# intentionally generous; the wait returns early the moment the session is ready.
_LAUNCH_PENDING_TIMEOUT_SECONDS = 120.0

# Stale-session evictor tick cadence. Replaces the idle-exit loop; a constant
# 10s tick is fine because the only work it does is per-worktree staleness
# eviction (D2/D3 -- proxy never auto-exits).
STALE_TICK_SECONDS = 10.0

# Windows SO_EXCLUSIVEADDRUSE -- prevents a second process from silently
# "binding" the same loopback port (today's bug). The Windows constant is
# 0xfffffffb (raw value); the Python `socket` module exposes it on most
# builds via socket.SO_EXCLUSIVEADDRUSE, but we fall back to the raw value
# if the attribute is missing. POSIX gets exclusive-bind by default and
# does NOT need any setsockopt call; we leave the socket alone there.
_SO_EXCLUSIVEADDRUSE_FALLBACK = 0xfffffffb

# Port derivation range (dynamic/private per IANA).
_EPHEMERAL_PORT_BASE = 49152
_EPHEMERAL_PORT_SPAN = 16384  # -> 49152 .. 65535
_MCP_PORT_BIND_ATTEMPTS = 32

# Logging.
_LOG_FORMAT = "%(asctime)s %(levelname)s %(name)s %(message)s"
_LOG_MAX_BYTES = 5 * 1024 * 1024
_LOG_BACKUP_COUNT = 1
_STDERR_MIRROR_SECONDS = 10

log = logging.getLogger("claireon_proxy")


# ---------------------------------------------------------------------------
# Singleton runtime directory (Stage 009 D9).
#
# Spawn copies of claireon_proxy.py and the proxy.log live under
# %LOCALAPPDATA%\Claireon\ on Windows and ~/.local/share/claireon/ on POSIX.
# Two engineers with separate Windows accounts each get their own
# singleton (their %LOCALAPPDATA% differ); two RDP sessions on the same
# account share the singleton (correct -- they share the loopback bind).
# ---------------------------------------------------------------------------


def claireon_runtime_dir() -> "Path":  # type: ignore[name-defined]
    """Return (creating if needed) the per-host singleton runtime dir.

    Windows -> %LOCALAPPDATA%\\Claireon\\
    POSIX   -> $XDG_DATA_HOME/claireon/   (default ~/.local/share/claireon/)
    """
    from pathlib import Path
    if sys.platform == "win32":
        local_appdata = os.environ.get("LOCALAPPDATA")
        if not local_appdata:
            local_appdata = os.path.expanduser(r"~\AppData\Local")
        base = Path(local_appdata) / "Claireon"
    else:
        base = Path(
            os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))
        ) / "claireon"
    base.mkdir(parents=True, exist_ok=True)
    return base


def claireon_runtime_script_path() -> str:
    """Path to the singleton's runtime copy of claireon_proxy.py (D9)."""
    runtime_dir = claireon_runtime_dir() / "runtime"
    runtime_dir.mkdir(parents=True, exist_ok=True)
    return str(runtime_dir / "claireon_proxy.py")


def claireon_runtime_log_path() -> str:
    """Path to the singleton's proxy.log under the per-host runtime dir (D9)."""
    return str(claireon_runtime_dir() / "proxy.log")


def _apply_exclusive_addr_use(sock: socket.socket) -> None:
    """Apply SO_EXCLUSIVEADDRUSE on Windows; no-op on POSIX (Stage 009 D3).

    Without this, two python.exe processes can both call bind() on the same
    loopback (addr, port) and "succeed". With it, the second bind fails
    with EADDRINUSE and the loser exits with code 2. POSIX rejects the
    second bind by default; we deliberately do NOT set SO_REUSEADDR.
    """
    if sys.platform == "win32":
        opt = getattr(socket, "SO_EXCLUSIVEADDRUSE", _SO_EXCLUSIVEADDRUSE_FALLBACK)
        try:
            sock.setsockopt(socket.SOL_SOCKET, opt, 1)
        except OSError as exc:
            log.warning("SO_EXCLUSIVEADDRUSE could not be applied: %r", exc)


class _ExclusiveBindHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer subclass that applies SO_EXCLUSIVEADDRUSE on
    Windows before bind. Used for both the registration listener (43017)
    and every per-worktree MCP listener (Stage 009 D3).
    """

    # Tell socketserver NOT to set SO_REUSEADDR before bind. We want the
    # kernel's exclusive-bind default on POSIX and SO_EXCLUSIVEADDRUSE on
    # Windows; SO_REUSEADDR breaks the bind-as-lock semantics.
    allow_reuse_address = False

    def server_bind(self) -> None:  # noqa: D401
        _apply_exclusive_addr_use(self.socket)
        ThreadingHTTPServer.server_bind(self)


# ---------------------------------------------------------------------------
# port -- derivation, collision advance, proxy.json persistence.
# ---------------------------------------------------------------------------


def canonicalize_worktree(worktree_root: str) -> str:
    """Canonicalize a worktree path for hashing. Windows-first, lowercase.

    Resolves junctions and symlinks via os.path.realpath so that a worktree
    reached via a junction (e.g. W:\\yara) and via its underlying realpath
    (e.g. D:\\git\\yara) hash to the SAME port. The PowerShell mirror in
    Initialize-WorktreeMCP.ps1::Get-ProxyDefaultMcpPort uses
    GetFinalPathNameByHandle (P/Invoke, Resolve-WorktreeFinalPath helper)
    for the same reason. Both sides MUST resolve links; do not switch this
    back to os.path.abspath, which would diverge from PowerShell on
    junctioned worktree paths.

    Caller contract: worktree_root is expected to be an absolute path. The
    proxy's CLI entrypoint runs os.path.abspath(args.worktree_root) before
    calling this helper (run() in this file at the cold-start hand-off,
    line ~1386), so relative-input behavior is defined by that call site.
    Both Python call sites in this module -- the cold-start path
    (RUNTIME["canonical_worktree"] = canonicalize_worktree(...)) and
    handle_register (canonical_req = canonicalize_worktree(...)) -- route
    through this single helper.
    """
    return os.path.realpath(worktree_root).lower()


def derive_default_mcp_port(worktree_root: str) -> int:
    """SHA-256(canonical path) -> port in [49152, 65535]."""
    canonical = canonicalize_worktree(worktree_root)
    digest = hashlib.sha256(canonical.encode("utf-8")).digest()
    offset = int.from_bytes(digest[0:2], "big") % _EPHEMERAL_PORT_SPAN
    return _EPHEMERAL_PORT_BASE + offset


def saved_dir(worktree_root: str) -> str:
    path = os.path.join(worktree_root, "Saved", "Claireon")
    os.makedirs(path, exist_ok=True)
    return path


def proxy_json_path(worktree_root: str) -> str:
    return os.path.join(saved_dir(worktree_root), "proxy.json")


def proxy_lock_path(worktree_root: str) -> str:
    return os.path.join(saved_dir(worktree_root), "proxy.lock")


def proxy_log_path(worktree_root: str) -> str:
    return os.path.join(saved_dir(worktree_root), "proxy.log")


def read_proxy_json(worktree_root: str) -> Optional[Dict[str, Any]]:
    path = proxy_json_path(worktree_root)
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError) as exc:
        log.warning("proxy.json unreadable at %s: %s", path, exc)
        return None


def write_proxy_json_atomic(worktree_root: str, payload: Dict[str, Any]) -> None:
    path = proxy_json_path(worktree_root)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


def bind_mcp_server(
    worktree_root: str, proxy_version_hash: str, handler_cls
) -> Tuple[ThreadingHTTPServer, int]:
    """Bind the Claude-facing MCP ThreadingHTTPServer.

    Tries the persisted port first (if any), then the hashed default, then
    advances +1 up to _MCP_PORT_BIND_ATTEMPTS times. Persists the accepted
    port back to proxy.json on success.
    """
    persisted = read_proxy_json(worktree_root)
    preferred: Optional[int] = None
    if persisted and isinstance(persisted.get("mcp_port"), int):
        preferred = int(persisted["mcp_port"])

    default_port = derive_default_mcp_port(worktree_root)
    candidates: list[int] = []
    if preferred is not None:
        candidates.append(preferred)
    if preferred != default_port:
        candidates.append(default_port)
    # Fill remaining attempts by advancing +1 from the default.
    seen = set(candidates)
    for offset in range(1, _MCP_PORT_BIND_ATTEMPTS):
        port = default_port + offset
        if port > _EPHEMERAL_PORT_BASE + _EPHEMERAL_PORT_SPAN - 1:
            break
        if port not in seen:
            candidates.append(port)
            seen.add(port)

    last_err: Optional[Exception] = None
    for port in candidates:
        try:
            server = _ExclusiveBindHTTPServer(("127.0.0.1", port), handler_cls)
        except OSError as exc:
            last_err = exc
            log.info("MCP port %d unavailable: %s", port, exc)
            continue
        log.info("MCP listener bound on 127.0.0.1:%d", port)
        write_proxy_json_atomic(
            worktree_root,
            {
                "mcp_port": port,
                "reg_port": PROXY_REG_PORT,
                "proxy_pid": os.getpid(),
                "proxy_start_time_iso8601": time.strftime(
                    "%Y-%m-%dT%H:%M:%SZ", time.gmtime()
                ),
                "proxy_version_hash": proxy_version_hash,
            },
        )
        return server, port

    raise RuntimeError(
        "could not bind MCP listener after %d attempts; last error: %r"
        % (_MCP_PORT_BIND_ATTEMPTS, last_err)
    )


def bind_registration_server(handler_cls) -> ThreadingHTTPServer:
    """Bind the editor-registration ThreadingHTTPServer on PROXY_REG_PORT.

    Stage 009 (D3): the bound state of 127.0.0.1:43017 is the source of
    truth for proxy liveness. SO_EXCLUSIVEADDRUSE on Windows / default
    exclusive bind on POSIX guarantees that exactly one python.exe owns
    the port. The loser logs the conflicting owner and exits with code 2.

    On failure, log an actionable message including the conflicting owner
    pid (best-effort via Get-NetTCPConnection / /proc/net/tcp) and exit 2.
    """
    try:
        server = _ExclusiveBindHTTPServer(("127.0.0.1", PROXY_REG_PORT), handler_cls)
    except OSError as exc:
        owner_pid = _probe_port_owner(PROXY_REG_PORT)
        log.critical(
            "Another process is holding PROXY_REG_PORT=%d (owner_pid=%s). "
            "Stop it and retry. Underlying error: %s",
            PROXY_REG_PORT,
            owner_pid or "unknown",
            exc,
        )
        sys.exit(2)
    log.info("Registration listener bound on 127.0.0.1:%d", PROXY_REG_PORT)
    return server


# ---------------------------------------------------------------------------
# lock -- proxy.lock with idempotent-startup reclamation.
# ---------------------------------------------------------------------------


def _process_start_time_ns(pid: int) -> Optional[int]:
    """Return Windows creation time (100ns ticks since 1601) for pid, or None."""
    if os.name != "nt":
        # Non-Windows: synthesize from /proc if available; None otherwise.
        proc_stat = "/proc/%d/stat" % pid
        try:
            with open(proc_stat, "r", encoding="utf-8") as f:
                fields = f.read().split()
            # Field 22 (0-indexed 21) is starttime in clock ticks since boot.
            return int(fields[21])
        except (OSError, IndexError, ValueError):
            return None

    import ctypes
    from ctypes import wintypes

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    OpenProcess = kernel32.OpenProcess
    OpenProcess.restype = wintypes.HANDLE
    OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    CloseHandle = kernel32.CloseHandle
    CloseHandle.restype = wintypes.BOOL
    CloseHandle.argtypes = [wintypes.HANDLE]
    GetProcessTimes = kernel32.GetProcessTimes
    GetProcessTimes.restype = wintypes.BOOL
    GetProcessTimes.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    ]

    handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        creation = wintypes.FILETIME()
        exit_t = wintypes.FILETIME()
        kernel_t = wintypes.FILETIME()
        user_t = wintypes.FILETIME()
        if not GetProcessTimes(
            handle, ctypes.byref(creation), ctypes.byref(exit_t),
            ctypes.byref(kernel_t), ctypes.byref(user_t),
        ):
            return None
        return (creation.dwHighDateTime << 32) | creation.dwLowDateTime
    finally:
        CloseHandle(handle)


def _process_image_name(pid: int) -> Optional[str]:
    """Return lowercase basename of the executable image for pid, or None."""
    if os.name != "nt":
        try:
            return os.path.basename(os.readlink("/proc/%d/exe" % pid)).lower()
        except OSError:
            return None

    import ctypes
    from ctypes import wintypes

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    OpenProcess = kernel32.OpenProcess
    OpenProcess.restype = wintypes.HANDLE
    OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    CloseHandle = kernel32.CloseHandle
    CloseHandle.restype = wintypes.BOOL
    CloseHandle.argtypes = [wintypes.HANDLE]
    QueryFullProcessImageNameW = kernel32.QueryFullProcessImageNameW
    QueryFullProcessImageNameW.restype = wintypes.BOOL
    QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]

    handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        buf_size = wintypes.DWORD(1024)
        buf = ctypes.create_unicode_buffer(buf_size.value)
        if not QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(buf_size)):
            return None
        return os.path.basename(buf.value).lower()
    finally:
        CloseHandle(handle)


def _current_process_start_time_ns() -> Optional[int]:
    return _process_start_time_ns(os.getpid())


def _write_lock_atomic(path: str, payload: Dict[str, Any]) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(payload, f)
    os.replace(tmp, path)


def acquire_lock_or_exit(worktree_root: str) -> str:
    """Create proxy.lock with O_CREAT|O_EXCL; reclaim if stale; exit 0 if live.

    Returns the lock-file path on successful acquisition. Exits the process
    with code 0 if an already-live proxy owns the lock (idempotent startup
    per D5).
    """
    path = proxy_lock_path(worktree_root)
    payload = {
        "pid": os.getpid(),
        "start_time_ns": _current_process_start_time_ns() or 0,
    }

    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY
    try:
        fd = os.open(path, flags)
    except FileExistsError:
        if _existing_lock_is_live(path):
            log.info("another proxy already owns %s; exiting cleanly", path)
            sys.exit(0)
        log.warning("stale lock at %s; reclaiming", path)
        _write_lock_atomic(path, payload)
        return path

    try:
        os.write(fd, json.dumps(payload).encode("utf-8"))
    finally:
        os.close(fd)
    return path


def _existing_lock_is_live(path: str) -> bool:
    """Read the lock and apply the three-check liveness test (per PYTHON.md)."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return False

    pid = data.get("pid")
    recorded_start = data.get("start_time_ns")
    if not isinstance(pid, int) or pid <= 0:
        return False

    image = _process_image_name(pid)
    if image is None:
        return False  # process gone
    if "python" not in image:
        return False  # pid reused by an unrelated process

    live_start = _process_start_time_ns(pid)
    if live_start is None:
        return False
    if recorded_start and live_start != recorded_start:
        return False  # pid reused by a different python process

    return True


def release_lock(path: str) -> None:
    try:
        os.remove(path)
    except FileNotFoundError:
        pass
    except OSError as exc:
        log.warning("could not remove lock file %s: %s", path, exc)


# ---------------------------------------------------------------------------
# registration -- /editor/register, /editor/heartbeat, /editor/deregister.
#
# Single-session state held under SESSION_LOCK. Semantics per PROTOCOL.md:
#   - register returns {accepted: true} or {accepted: false, reason: ...}
#     with reasons in {singleton_session, version_mismatch, malformed_request,
#     worktree_mismatch}.
#   - heartbeat returns {ok: true} or {ok: false, reason: "unknown_session"}.
#   - deregister clears session; proxy continues running.
# ---------------------------------------------------------------------------


# Required fields on the register request body. Stage 005 (multi-worktree
# proxy) replaced session_uuid with start_time_ns: a per-process tuple
# (pid, start_time_ns) is sufficient to identify a session, and start_time_ns
# is also what the editor and proxy use as the newest-wins discriminator (D8).
_REGISTER_REQUIRED_FIELDS = (
    "pid",
    "worktree_root",
    "start_time_ns",
    "build_id",
    "proxy_version",
    "editor_mcp_port",
    "editor_mcp_token",
)

# Registration state (guarded by SESSION_LOCK). None when no editor registered.
# After Stage 005, RUNTIME["worktrees"][canonical].session is authoritative;
# singleton_session is retained as a compatibility shim for the legacy /admin/...
# read paths (status, idle-exit) that still skim the singleton view. Both views
# are written under SESSION_LOCK so they stay coherent.
SESSION_LOCK = threading.Lock()
singleton_session: Optional[Dict[str, Any]] = None


# ---------------------------------------------------------------------------
# Multi-tenant routing skeleton (Stage 004 -- shape change, no behaviour change).
#
# Stages 005-007 wire handle_register / handle_heartbeat / handle_deregister to
# read AND write through these maps; this stage only mirrors writes alongside
# the legacy single-session state. Today the legacy globals (singleton_session +
# RUNTIME["singleton_worktree_root"] / RUNTIME["canonical_worktree"]) remain authoritative;
# nothing in the proxy reads from RUNTIME["worktrees"] yet.
#
# All mutations to these maps MUST be performed under SESSION_LOCK, the same
# lock that already guards singleton_session. A missed lock here will surface as
# flaky multi-tenant tests in Stage 005+.
# ---------------------------------------------------------------------------


@dataclass
class Session:
    """Per-editor registration record (one per WorktreeState).

    Fields populated from /editor/register today; start_time_ns is
    populated from _process_start_time_ns(editor_pid) and becomes the
    newest-wins discriminator in Stage 005+ once the wire request body
    grows the field. last_forward_status is reserved for the
    /admin/sessions endpoint (Stage 006) and is not populated yet.
    """

    editor_pid: int
    # Wire stores start_time_ns as a decimal string so values exceeding
    # JSON Number / IEEE-754 double precision (Windows FILETIME ~1.3e17)
    # round-trip without loss. The proxy never parses it back to an int;
    # the (pid, start_time_ns) session-identity tuple is compared as-is.
    start_time_ns: str
    editor_mcp_port: int
    editor_mcp_token: str
    build_id: str
    last_forward_status: int = 0


@dataclass
class WorktreeState:
    """Per-worktree slot in the singleton proxy.

    canonical_worktree is the routing key (lowercase os.path.realpath).
    mcp_port is the Claude-facing listener; today there is exactly one
    worktree so mcp_port mirrors the singleton-listener port. mcp_server
    starts None and is bound lazily on first register in Stage 006+;
    Stage 004 leaves it None on every entry.

    _evicted_by is a one-shot breadcrumb (D6 newest-wins): when a register
    request displaces an active session, we set _evicted_by to the
    incoming (pid, start_time_ns) tuple so the next /editor/heartbeat from
    the OLD session can populate evicted_by in its unknown_session
    response. Cleared as soon as the heartbeat fires (or on the next
    register, whichever comes first). The leading underscore discourages
    any external reader from depending on it.
    """

    canonical_worktree: str
    mcp_port: int
    mcp_server: Optional[ThreadingHTTPServer] = None
    session: Optional[Session] = None
    last_seen_ns: int = 0
    version_hash: str = ""
    _evicted_by: Optional[Tuple[int, int]] = None
    # I4 (#0000): True once the editor POSTs /editor/ready (tool catalog
    # populated + Python bridge initialized). False on fresh register so
    # the fallback shows "editor warming up" rather than "build and launch".
    ready: bool = False
    # Tool count reported at /editor/ready; 0 until ready.
    tool_count: int = 0
    # Monotonic timestamp set by the launch_editor proxy command after spawning
    # the editor process. The first forwarded tool call will auto-wait up to
    # _LAUNCH_PENDING_TIMEOUT_SECONDS instead of returning the "build and launch
    # the editor first" error. Cleared to 0.0 when a session registers.
    launch_pending_ts: float = 0.0


# Runtime context shared with handler classes. Populated by main() before
# the HTTP servers start accepting requests.
RUNTIME: Dict[str, Any] = {
    "worktree_root": None,          # canonicalized absolute path
    "canonical_worktree": None,     # lowercase form used for comparison
    "proxy_version_hash": None,     # sha1 of claireon_proxy.py (matches FSHA1 on the C++ side)
    "last_claude_activity_ts": 0.0, # monotonic clock; updated on any Claude-facing request
    "clock_offset_seconds": 0.0,    # test hook (see --clock-offset-seconds)
    # Multi-tenant routing maps (Stage 004 skeleton; populated by mirror
    # writes only -- no handler reads these in this stage).
    "worktrees": {},                # type: Dict[str, WorktreeState]   key=canonical_worktree
    "mcp_port_to_worktree": {},     # type: Dict[int, str]             reverse map for /mcp routing in Stage 006
}


def _mono_now() -> float:
    """Monotonic clock with test-injectable offset."""
    return time.monotonic() + float(RUNTIME.get("clock_offset_seconds", 0.0))


def evict_singleton_stale_session_locked() -> Optional[Dict[str, Any]]:
    """Drop the active session if it has gone silent beyond the staleness
    threshold. MUST be called with SESSION_LOCK held. Returns the evicted
    session dict (for logging) or None if nothing was evicted."""
    global singleton_session
    if singleton_session is None:
        return None
    age = _mono_now() - float(singleton_session.get("last_heartbeat_ts") or 0.0)
    if age <= HEARTBEAT_STALENESS_SECONDS:
        return None
    evicted = singleton_session
    forward_conn = evicted.get("forward_conn")
    if forward_conn is not None:
        try:
            forward_conn.close()
        except Exception:  # noqa: BLE001
            pass

    # Stage 004 mirror write: clear the matching WorktreeState.session
    # so the per-worktree map agrees with singleton_session=None. Caller
    # already holds SESSION_LOCK per the docstring.
    canonical = evicted.get("worktree_root")
    if canonical:
        canonical = os.path.realpath(canonical).lower()
        ws = RUNTIME["worktrees"].get(canonical)
        if ws is not None:
            ws.session = None

    singleton_session = None
    return evicted


def _is_valid_start_time_ns(value: Any) -> bool:
    """Wire format: a decimal string of digits, "0" allowed.

    The editor sends start_time_ns as a string because Windows FILETIME
    values (~1.3e17 today) exceed IEEE-754 double precision. The proxy
    stores and compares it verbatim; no parse round-trip happens.
    """
    return isinstance(value, str) and value.isdigit()


def _validate_register_body(body: Dict[str, Any]) -> Optional[str]:
    """Return an error reason string or None if the body is well-formed."""
    for field in _REGISTER_REQUIRED_FIELDS:
        if field not in body:
            return "malformed_request"
    if not isinstance(body["pid"], int) or body["pid"] <= 0:
        return "malformed_request"
    if not isinstance(body["worktree_root"], str) or not body["worktree_root"]:
        return "malformed_request"
    # start_time_ns is a decimal string of digits (Windows: FILETIME 100ns
    # ticks since 1601; Linux: /proc/<pid>/stat field 22 in clock-tick units).
    # "0" is permitted because the editor falls back to "0" if the OS query
    # fails; the (pid, start_time_ns) tuple still identifies the session
    # uniquely when start_time_ns is "0" because pid alone is unique on a
    # live host. Stored verbatim and compared as a string -- no parse round
    # trip, so values exceeding JSON Number precision are preserved.
    if not _is_valid_start_time_ns(body["start_time_ns"]):
        return "malformed_request"
    if not isinstance(body["build_id"], str):
        return "malformed_request"
    if not isinstance(body["proxy_version"], str) or not body["proxy_version"]:
        return "malformed_request"
    port = body["editor_mcp_port"]
    if not isinstance(port, int) or not (1 <= port <= 65535):
        return "malformed_request"
    token = body["editor_mcp_token"]
    if not isinstance(token, str) or len(token) < 32:
        return "malformed_request"
    return None


def handle_register(body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    """Multi-tenant register (Stage 005).

    Wire-shape changes vs. previous:
      - Required field set is now (pid, worktree_root, start_time_ns,
        build_id, proxy_version, editor_mcp_port, editor_mcp_token).
        session_uuid is gone; (pid, start_time_ns) is the per-session key.
      - 409 singleton_session is no longer returned. D6 newest-wins: the
        incoming registration evicts whatever session was previously
        bound to the worktree slot. The displaced session's next
        heartbeat sees evicted_by and walks to terminal Failed.
      - version_mismatch is advisory-only (D5). Drift is logged, not
        rejected.
      - worktree_mismatch is gone. The proxy is multi-tenant: any
        canonical worktree gets its own slot in RUNTIME["worktrees"].
      - singleton_session is still written for the legacy /admin/...
        observability + idle-exit views; the per-worktree map is now
        authoritative for routing/heartbeat/eviction.
    """
    reason = _validate_register_body(body)
    if reason is not None:
        log.warning("register rejected reason=%s", reason)
        return 400, {"accepted": False, "reason": reason}

    canonical_req = canonicalize_worktree(body["worktree_root"])

    # Version drift is advisory after Stage 005 (D5). Log dedup as before so
    # a fast-retrying editor doesn't flood proxy.log; never reject.
    expected_version = RUNTIME.get("proxy_version_hash") or ""
    if expected_version and body["proxy_version"] != expected_version:
        if note_version_mismatch_seen(body["proxy_version"]):
            log.info(
                "register: version drift (advisory) worktree=%s expected=%s got=%s",
                canonical_req,
                expected_version,
                body["proxy_version"],
            )

    global singleton_session
    with SESSION_LOCK:
        evict_singleton_stale_session_locked()

        editor_pid = int(body["pid"])
        editor_start = body["start_time_ns"]  # decimal string, never parsed

        # Look up (or create) the per-worktree slot. Each canonical
        # worktree gets its own WorktreeState; cross-worktree sessions
        # never collide.
        wt = RUNTIME["worktrees"].get(canonical_req)
        if wt is None:
            wt = WorktreeState(
                canonical_worktree=canonical_req,
                mcp_port=int(RUNTIME.get("mcp_port") or 0),
                mcp_server=None,  # Stage 006 binds per-worktree listeners lazily.
            )
            RUNTIME["worktrees"][canonical_req] = wt
            if wt.mcp_port:
                RUNTIME["mcp_port_to_worktree"][wt.mcp_port] = canonical_req

        # D6 newest-wins: if a session already holds this worktree, evict
        # it. Drop a one-shot breadcrumb so the displaced editor's next
        # heartbeat sees evicted_by and walks to Failed without staleness
        # ambiguity.
        if wt.session is not None:
            evicted = wt.session
            log.info(
                "[worktree=%s] evicted previous session "
                "evicted_pid=%d evicted_start_time_ns=%s "
                "incoming_pid=%d incoming_start_time_ns=%s",
                canonical_req,
                evicted.editor_pid,
                evicted.start_time_ns,
                editor_pid,
                editor_start,
            )
            wt._evicted_by = (editor_pid, editor_start)
        else:
            # No prior session. Clear any stale breadcrumb defensively;
            # in practice it is already None.
            wt._evicted_by = None

        wt.session = Session(
            editor_pid=editor_pid,
            start_time_ns=editor_start,
            editor_mcp_port=int(body["editor_mcp_port"]),
            editor_mcp_token=str(body["editor_mcp_token"]),
            build_id=str(body["build_id"]),
        )
        # I4 (#0000): new session is not ready until /editor/ready is received.
        wt.ready = False
        wt.tool_count = 0
        # Clear any pending-launch marker now that the editor has registered.
        wt.launch_pending_ts = 0.0
        wt.last_seen_ns = int(_mono_now() * 1_000_000_000)
        wt.version_hash = str(body["proxy_version"])

        # Compatibility shim: keep singleton_session populated so the legacy
        # /admin/status, idle-exit, and any other singleton-view readers
        # continue to work until they are migrated in Stage 006+. The
        # per-worktree map is authoritative; this mirror is read-only
        # outside of register/deregister.
        singleton_session = {
            "pid": editor_pid,
            "worktree_root": body["worktree_root"],
            "start_time_ns": editor_start,
            "build_id": body["build_id"],
            "proxy_version": body["proxy_version"],
            "editor_mcp_port": body["editor_mcp_port"],
            "editor_mcp_token": body["editor_mcp_token"],
            "last_heartbeat_ts": _mono_now(),
            "forward_conn": None,
        }

        log.info(
            "register accepted worktree=%s editor_pid=%d start_time_ns=%s "
            "build_id=%s editor_mcp_port=%d",
            canonical_req,
            editor_pid,
            editor_start,
            body["build_id"],
            body["editor_mcp_port"],
        )
    return 200, {"accepted": True}


def handle_heartbeat(body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    """Multi-tenant heartbeat (Stage 005).

    Body shape: {worktree_root, pid, start_time_ns}.

    Per WIRE_PROTOCOL.md:
      - Identity match -> 200 {"ok": true}.
      - No session at all -> 200 {"ok": false, "reason": "unknown_session"}.
      - Identity mismatch -> 200 {"ok": false, "reason": "unknown_session",
        evicted_by?: {pid, start_time_ns}}. The evicted_by sub-object is
        populated only when a register displaced this heartbeat's session
        (D6 newest-wins); otherwise the field is omitted and the editor
        treats it as proxy-side staleness.
    """
    worktree_root = body.get("worktree_root")
    pid = body.get("pid")
    start_time_ns = body.get("start_time_ns")
    if not isinstance(worktree_root, str) or not worktree_root:
        return 400, {"ok": False, "reason": "malformed_request"}
    if not isinstance(pid, int) or pid <= 0:
        return 400, {"ok": False, "reason": "malformed_request"}
    if not _is_valid_start_time_ns(start_time_ns):
        return 400, {"ok": False, "reason": "malformed_request"}

    canonical = os.path.realpath(worktree_root).lower()

    with SESSION_LOCK:
        evict_singleton_stale_session_locked()

        wt = RUNTIME["worktrees"].get(canonical)
        if wt is None or wt.session is None:
            return 200, {"ok": False, "reason": "unknown_session"}

        s = wt.session
        if (s.editor_pid, s.start_time_ns) != (pid, start_time_ns):
            response: Dict[str, Any] = {"ok": False, "reason": "unknown_session"}
            if wt._evicted_by is not None:
                evictor_pid, evictor_start = wt._evicted_by
                response["evicted_by"] = {
                    "pid": evictor_pid,
                    "start_time_ns": evictor_start,
                }
                # One-shot: clear after the displaced editor has been told.
                wt._evicted_by = None
            return 200, response

        wt.last_seen_ns = int(_mono_now() * 1_000_000_000)

        # Mirror to singleton_session for the legacy idle-exit watchdog and
        # /admin/status. Only meaningful when this is the same session
        # the singleton view believes is current.
        global singleton_session
        if singleton_session is not None \
                and singleton_session.get("pid") == pid \
                and singleton_session.get("start_time_ns") == start_time_ns:
            singleton_session["last_heartbeat_ts"] = _mono_now()
    return 200, {"ok": True}


def handle_deregister(body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    """Multi-tenant deregister (Stage 005).

    Body shape: {worktree_root, pid, start_time_ns}. Best-effort: clear
    the per-worktree slot only if the (pid, start_time_ns) tuple matches
    the current session (avoids racing a newest-wins eviction). Always
    returns 200 OK -- the editor doesn't act on the response.
    """
    worktree_root = body.get("worktree_root")
    pid = body.get("pid")
    start_time_ns = body.get("start_time_ns")
    if not isinstance(worktree_root, str) or not worktree_root:
        return 400, {"ok": False, "reason": "malformed_request"}
    if not isinstance(pid, int) or pid <= 0:
        return 400, {"ok": False, "reason": "malformed_request"}
    if not _is_valid_start_time_ns(start_time_ns):
        return 400, {"ok": False, "reason": "malformed_request"}

    canonical = os.path.realpath(worktree_root).lower()

    global singleton_session
    with SESSION_LOCK:
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is not None and wt.session is not None and \
                (wt.session.editor_pid, wt.session.start_time_ns) == (pid, start_time_ns):
            log.info(
                "deregister worktree=%s editor_pid=%d build_id=%s",
                canonical,
                pid,
                wt.session.build_id,
            )
            wt.session = None
            wt._evicted_by = None

            # Compatibility shim: clear singleton_session if it was tracking
            # this same session. The legacy /admin/status reader expects
            # the singleton view to agree with the per-worktree map.
            if singleton_session is not None \
                    and singleton_session.get("pid") == pid \
                    and singleton_session.get("start_time_ns") == start_time_ns:
                forward_conn = singleton_session.get("forward_conn")
                if forward_conn is not None:
                    try:
                        forward_conn.close()
                    except Exception:  # noqa: BLE001
                        pass
                singleton_session = None
    return 200, {"ok": True}


def handle_editor_ready(body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    """POST /editor/ready -- I4 (#0000).

    Editor sends this after FClaireonBridge::EnsureRegistered() completes
    (Python bridge initialized + tool catalog populated). Required fields:
      pid, start_time_ns, worktree_root  -- session identity
      tool_count (int)                   -- number of tools registered

    Sets WorktreeState.ready=True and records tool_count so the proxy can
    return a richer status for the I9 mcp_ready_status tool and produce
    "editor warming up" instead of "build and launch editor first" in the
    gap between register and ready.
    """
    worktree_root = body.get("worktree_root")
    pid = body.get("pid")
    start_time_ns = body.get("start_time_ns")
    tool_count = body.get("tool_count", 0)

    if not worktree_root or not isinstance(pid, int) or pid <= 0:
        return 400, {"ok": False, "reason": "malformed_request"}

    canonical = canonicalize_worktree(worktree_root)
    recorded_tool_count = 0
    with SESSION_LOCK:
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is None or wt.session is None:
            return 200, {"ok": False, "reason": "no_session"}
        if (wt.session.editor_pid, wt.session.start_time_ns) != (pid, start_time_ns):
            return 200, {"ok": False, "reason": "session_mismatch"}
        wt.ready = True
        wt.tool_count = int(tool_count) if isinstance(tool_count, (int, float)) else 0
        recorded_tool_count = wt.tool_count
    log.info(
        "editor_ready worktree=%s editor_pid=%d tool_count=%d",
        canonical, pid, recorded_tool_count,
    )
    return 200, {"ok": True}


def _read_json_body(handler: BaseHTTPRequestHandler) -> Optional[Dict[str, Any]]:
    """Read + parse a JSON request body; None if absent or malformed."""
    try:
        length = int(handler.headers.get("Content-Length") or 0)
    except ValueError:
        return None
    if length <= 0:
        return {}
    raw = handler.rfile.read(length)
    try:
        parsed = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(parsed, dict):
        return None
    return parsed


# ---------------------------------------------------------------------------
# Admin endpoints (Stage 006). Loopback-only by header validation; mirrors
# FClaireonServer::ValidateRequestHeaders semantics so non-loopback callers
# get the same forbidden response on the registration listener as on /mcp.
# ---------------------------------------------------------------------------


_LOOPBACK_HOSTS = frozenset({
    "127.0.0.1",
    "localhost",
    "[::1]",
    "::1",
})


def _is_loopback_origin(origin: str) -> bool:
    """Parse Origin (which carries a scheme) and check the hostname is loopback."""
    try:
        from urllib.parse import urlparse
        parsed = urlparse(origin)
    except ValueError:
        return False
    if parsed.scheme not in ("http", "https"):
        return False
    host = (parsed.hostname or "").lower()
    return host in _LOOPBACK_HOSTS


def _validate_admin_headers(handler: BaseHTTPRequestHandler) -> bool:
    """Mirror of FClaireonServer::ValidateRequestHeaders.

    Reject anything whose Origin/Host indicates a non-loopback caller.
    Returns True if the request should proceed.
    """
    host = handler.headers.get("Host", "")
    origin = handler.headers.get("Origin", "")

    if host:
        bare = host.split(":")[0].lower()
        if bare not in _LOOPBACK_HOSTS:
            return False

    if origin and not _is_loopback_origin(origin):
        return False

    return True


def handle_admin_health() -> Tuple[int, Dict[str, Any]]:
    """GET /admin/health -- spawn probe target.

    Caller (editor or Initialize-WorktreeMCP.ps1) hits this to distinguish
    'Claireon's proxy is up' from 'something else is on PROXY_REG_PORT'.
    """
    with SESSION_LOCK:
        worktrees_payload = []
        for wt in RUNTIME["worktrees"].values():
            session_payload: Optional[Dict[str, Any]] = None
            if wt.session is not None:
                session_payload = {
                    "editor_pid": wt.session.editor_pid,
                    "start_time_ns": wt.session.start_time_ns,
                }
            worktrees_payload.append({
                "canonical_worktree": wt.canonical_worktree,
                "mcp_port": wt.mcp_port,
                "session": session_payload,
            })
    return 200, {
        "version_hash": RUNTIME.get("proxy_version_hash"),
        "pid": os.getpid(),
        "worktrees": worktrees_payload,
    }


def handle_admin_sessions() -> Tuple[int, Dict[str, Any]]:
    """GET /admin/sessions -- read-only observability list."""
    with SESSION_LOCK:
        items = []
        for wt in RUNTIME["worktrees"].values():
            if wt.session is None:
                continue
            items.append({
                "canonical_worktree": wt.canonical_worktree,
                "mcp_port": wt.mcp_port,
                "editor_pid": wt.session.editor_pid,
                "start_time_ns": wt.session.start_time_ns,
                "build_id": wt.session.build_id,
                "version_hash": wt.version_hash,
                "last_heartbeat_at": wt.last_seen_ns,
                "last_forward_status": wt.session.last_forward_status,
            })
    return 200, {"sessions": items}


def _probe_port_owner(port: int) -> int:
    """Best-effort owner pid for a port held externally. 0 on failure."""
    if os.name == "nt":
        try:
            import subprocess as _sub
            ps = _resolve_powershell_exe()
            if not ps:
                return 0
            cmd = [
                ps, "-NoProfile", "-Command",
                ("$c = Get-NetTCPConnection -LocalPort {0} -State Listen "
                 "-ErrorAction SilentlyContinue | Select-Object -First 1; "
                 "if ($c) {{ $c.OwningProcess }}").format(port),
            ]
            out = _sub.run(  # noqa: S603 -- args are constructed from int + literal
                cmd,
                stdout=_sub.PIPE,
                stderr=_sub.DEVNULL,
                timeout=5,
                check=False,
            )
            text = out.stdout.decode("utf-8", errors="replace").strip()
            return int(text) if text.isdigit() else 0
        except Exception:  # noqa: BLE001
            return 0
    # Linux: parse /proc/net/tcp. Cheap fallback; returns 0 on failure.
    try:
        with open("/proc/net/tcp", "r", encoding="utf-8") as f:
            f.readline()  # header
            target_hex = f"{port:04X}"
            for line in f:
                fields = line.split()
                if len(fields) < 10:
                    continue
                local = fields[1]
                state = fields[3]
                if state != "0A":  # LISTEN
                    continue
                if local.endswith(":" + target_hex):
                    inode = fields[9]
                    # Best-effort: scan /proc/<pid>/fd for the inode link.
                    needle = f"socket:[{inode}]"
                    for entry in os.listdir("/proc"):
                        if not entry.isdigit():
                            continue
                        fd_dir = f"/proc/{entry}/fd"
                        try:
                            for fd in os.listdir(fd_dir):
                                try:
                                    if os.readlink(os.path.join(fd_dir, fd)) == needle:
                                        return int(entry)
                                except OSError:
                                    continue
                        except OSError:
                            continue
        return 0
    except OSError:
        return 0


def _probe_direct_connect_editor(port: int) -> Optional[Dict[str, Any]]:
    """If a Claireon editor in direct-connect mode owns `port`, return its
    identity tuple; otherwise return None.

    The probe sends a JSON-RPC initialize. A direct-connect editor's
    MCP server returns serverInfo.name == 'Claireon' (the editor side
    of the surface) and an empty session token on the wire. The probe
    is best-effort: any transport failure -> None.
    """
    try:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        try:
            payload = json.dumps({
                "jsonrpc": "2.0",
                "id": 0,
                "method": "initialize",
                "params": {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "clientInfo": {"name": "claireon-proxy-probe", "version": "0"},
                    "capabilities": {},
                },
            }).encode("utf-8")
            conn.request(
                "POST",
                "/mcp",
                body=payload,
                headers={
                    "Content-Type": "application/json",
                    "Content-Length": str(len(payload)),
                },
            )
            resp = conn.getresponse()
            body = resp.read()
        finally:
            conn.close()
    except (OSError, http.client.HTTPException):
        return None

    try:
        parsed = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(parsed, dict):
        return None
    result = parsed.get("result") or {}
    info = result.get("serverInfo") or {}
    name = info.get("name", "")
    if not isinstance(name, str) or "Claireon" not in name:
        return None
    # Best-effort identity. Stage 010 lands the editor side that exposes
    # pid/start_time_ns via initialize; for now we report port-only.
    return {
        "editor_pid": 0,
        "editor_start_time_ns": 0,
    }


def _try_bind_mcp_listener(canonical: str, port: int) -> Optional[ThreadingHTTPServer]:
    """Try to bind an MCP listener on (127.0.0.1, port) for the given
    canonical worktree. Three retries at 200ms before giving up; returns
    the bound server on success or None on failure.

    A successful bind also persists Saved/Claireon/proxy.json under the
    worktree so legacy consumers (today's Initialize-WorktreeMCP and
    .mcp.json readers) keep working.
    """
    # Defense against the Windows loopback-vs-wildcard non-exclusivity quirk:
    # if a Claireon editor in DirectConnect mode is already listening on this
    # port (with a 0.0.0.0 bind that doesn't set SO_EXCLUSIVEADDRUSE), then
    # our 127.0.0.1 bind would silently "succeed" and intercept loopback
    # traffic, causing the proxy to forward to itself. Probe first; if a
    # Claireon editor answers, refuse to bind so the caller (ensure_worktree)
    # returns 409 port_held_by_editor and the operator can resolve.
    pre_existing = _probe_direct_connect_editor(port)
    if pre_existing is not None:
        log.warning(
            "[worktree=%s] MCP listener bind refused: a Claireon editor is "
            "already serving port=%d (DirectConnect collision). The proxy "
            "will not bind on top of it; relaunch the editor with the "
            "ProxyAttached path to fix.",
            canonical, port,
        )
        return None

    last_err: Optional[Exception] = None
    for attempt in range(3):
        try:
            server = _ExclusiveBindHTTPServer(("127.0.0.1", port), McpHandler)
        except OSError as exc:
            last_err = exc
            time.sleep(0.2)
            continue
        log.info(
            "[worktree=%s] MCP listener bound port=%d (attempt=%d)",
            canonical, port, attempt + 1,
        )
        # Persist proxy.json for compatibility. Path uses the canonical
        # worktree directly (which is the realpath under Saved).
        try:
            write_proxy_json_atomic(canonical, {
                "mcp_port": port,
                "reg_port": PROXY_REG_PORT,
                "proxy_pid": os.getpid(),
                "proxy_start_time_iso8601": time.strftime(
                    "%Y-%m-%dT%H:%M:%SZ", time.gmtime()
                ),
                "proxy_version_hash": RUNTIME.get("proxy_version_hash"),
            })
        except OSError as exc:
            log.warning("could not persist proxy.json under %s: %r", canonical, exc)
        # Spawn a serve_forever thread so the listener is live before we
        # return to the caller.
        threading.Thread(
            target=server.serve_forever,
            name=f"mcp-listener-{port}",
            daemon=True,
        ).start()
        return server
    log.warning(
        "could not bind MCP listener worktree=%s port=%d: %r",
        canonical, port, last_err,
    )
    return None


def handle_admin_ensure_worktree(body: Dict[str, Any]) -> Tuple[int, Dict[str, Any]]:
    """POST /admin/ensure_worktree -- idempotent per-worktree bind.

    Body: {worktree_root, mcp_port?}. Three response shapes per
    WIRE_PROTOCOL.md:
      - 200 {mcp_port, bound: true}
      - 409 {reason: "port_held_by_editor", port, editor_pid, editor_start_time_ns}
      - 409 {reason: "port_held_externally", port, owner_pid}
      - 409 {reason: "port_collision_internal", port}  (defensive)
    """
    worktree_root = body.get("worktree_root")
    if not isinstance(worktree_root, str) or not worktree_root:
        return 400, {"reason": "malformed_request"}

    canonical = canonicalize_worktree(worktree_root)

    # Resolve desired port (caller hint > persisted > derived).
    requested_port: Optional[int] = None
    if "mcp_port" in body:
        port_val = body.get("mcp_port")
        if isinstance(port_val, int) and 1 <= port_val <= 65535:
            requested_port = port_val
        else:
            return 400, {"reason": "malformed_request"}

    if requested_port is not None:
        desired_port = requested_port
    else:
        persisted = read_proxy_json(worktree_root)
        if persisted and isinstance(persisted.get("mcp_port"), int):
            desired_port = int(persisted["mcp_port"])
        else:
            desired_port = derive_default_mcp_port(canonical)

    with SESSION_LOCK:
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is not None and wt.mcp_server is not None:
            # Already bound by us.
            if wt.mcp_port == desired_port:
                return 200, {"mcp_port": wt.mcp_port, "bound": True}
            # Different port. Defensively log; the wire spec calls this
            # port_collision_internal because each worktree should have a
            # stable SHA-derived port.
            log.warning(
                "ensure_worktree internal collision worktree=%s "
                "bound_port=%d requested_port=%d",
                canonical, wt.mcp_port, desired_port,
            )
            return 409, {
                "reason": "port_collision_internal",
                "port": desired_port,
            }

        # Defensive: if the reverse map says some OTHER worktree owns this
        # port, that is also a port_collision_internal.
        existing = RUNTIME["mcp_port_to_worktree"].get(desired_port)
        if existing is not None and existing != canonical:
            log.warning(
                "ensure_worktree port already mapped worktree=%s "
                "owner_worktree=%s port=%d",
                canonical, existing, desired_port,
            )
            return 409, {
                "reason": "port_collision_internal",
                "port": desired_port,
            }

        # Try to bind. On success, register the listener in our maps.
        server = _try_bind_mcp_listener(canonical, desired_port)
        if server is not None:
            if wt is None:
                wt = WorktreeState(
                    canonical_worktree=canonical,
                    mcp_port=desired_port,
                    mcp_server=server,
                )
                RUNTIME["worktrees"][canonical] = wt
            else:
                wt.mcp_port = desired_port
                wt.mcp_server = server
            RUNTIME["mcp_port_to_worktree"][desired_port] = canonical
            return 200, {"mcp_port": desired_port, "bound": True}

    # Bind failed -- something else is on the port. Probe to classify.
    direct = _probe_direct_connect_editor(desired_port)
    if direct is not None:
        return 409, {
            "reason": "port_held_by_editor",
            "port": desired_port,
            "editor_pid": direct["editor_pid"],
            "editor_start_time_ns": direct["editor_start_time_ns"],
        }
    owner_pid = _probe_port_owner(desired_port)
    return 409, {
        "reason": "port_held_externally",
        "port": desired_port,
        "owner_pid": owner_pid,
    }


class RegistrationHandler(BaseHTTPRequestHandler):
    """Editor-facing registration + admin endpoint handler (on PROXY_REG_PORT)."""

    server_version = "claireon-proxy/1"

    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: A003
        log.debug("reg %s - %s", self.address_string(), fmt % args)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            if not _validate_admin_headers(self):
                self._respond_json(403, {"reason": "forbidden"})
                return
            with SESSION_LOCK:
                evict_singleton_stale_session_locked()
                has_session = singleton_session is not None
                pid = singleton_session.get("pid") if has_session else None
                start_time_ns = (
                    singleton_session.get("start_time_ns") if has_session else None
                )
            self._respond_json(
                200,
                {
                    "ok": True,
                    "has_session": has_session,
                    # Stage 005: identify the session by (pid, start_time_ns)
                    # rather than the dropped session_uuid. has_session keeps
                    # its meaning for callers that only care if SOMETHING is
                    # registered.
                    "editor_pid": pid,
                    "editor_start_time_ns": start_time_ns,
                    "proxy_version": RUNTIME.get("proxy_version_hash"),
                },
            )
            return
        if self.path == "/admin/health":
            if not _validate_admin_headers(self):
                self._respond_json(403, {"reason": "forbidden"})
                return
            status, resp = handle_admin_health()
            self._respond_json(status, resp)
            return
        if self.path == "/admin/sessions":
            if not _validate_admin_headers(self):
                self._respond_json(403, {"reason": "forbidden"})
                return
            status, resp = handle_admin_sessions()
            self._respond_json(status, resp)
            return
        self._respond_json(404, {
            "error": "not_found",
            "reason": (
                "This proxy's HTTP surface is private. The only public paths are "
                "/health (GET), /admin/health (GET), /admin/sessions (GET), "
                "/admin/ensure_worktree (POST), and the /editor/* registration POSTs. "
                "If you're looking for proxy/session state, use the `mcp__claireon__proxy` "
                "MCP tool (commands: status, wait_for_editor, launch_editor, read_log, "
                "help) -- do not screen-scrape this HTTP surface."
            ),
            "requested_path": self.path,
        })

    def do_POST(self) -> None:  # noqa: N802
        # Loopback gate applies to every /editor/* and /admin/* route on
        # this listener. The validator is a no-op when Host/Origin are
        # absent (pure stdlib clients), which preserves today's contract.
        if not _validate_admin_headers(self):
            self._respond_json(403, {"reason": "forbidden"})
            return

        body = _read_json_body(self)
        if body is None:
            self._respond_json(400, {"accepted": False, "reason": "malformed_request"})
            return

        if self.path == "/editor/register":
            status, resp = handle_register(body)
        elif self.path == "/editor/heartbeat":
            status, resp = handle_heartbeat(body)
        elif self.path in ("/editor/deregister", "/editor/unregister"):
            status, resp = handle_deregister(body)
        elif self.path == "/editor/ready":
            # I4 (#0000): editor signals tool catalog populated + Python ready.
            status, resp = handle_editor_ready(body)
        elif self.path == "/admin/ensure_worktree":
            status, resp = handle_admin_ensure_worktree(body)
        else:
            self._respond_json(404, {
            "error": "not_found",
            "reason": (
                "This proxy's HTTP surface is private. The only public paths are "
                "/health (GET), /admin/health (GET), /admin/sessions (GET), "
                "/admin/ensure_worktree (POST), and the /editor/* registration POSTs. "
                "If you're looking for proxy/session state, use the `mcp__claireon__proxy` "
                "MCP tool (commands: status, wait_for_editor, launch_editor, read_log, "
                "help) -- do not screen-scrape this HTTP surface."
            ),
            "requested_path": self.path,
        })
            return
        self._respond_json(status, resp)

    def _respond_json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------------------
# mcp -- Claude-facing MCP surface + forwarder.
#
# Static tool list (search, execute) per PROTOCOL.md; local-handled initialize
# / ping / notifications/initialized. tools/call either forwards to the
# registered editor's MCP endpoint or returns the "build and launch the editor
# first" content result with isError: true.
# ---------------------------------------------------------------------------


MCP_PROTOCOL_VERSION = "2025-03-26"
MCP_SERVER_NAME = "claireon-proxy"

# Static tool catalogue exposed to Claude. The proxy and editor unify on this
# exact two-tool surface; the editor accepts only these two names in tools/call
# and forwards them through. Wire names match the editor's MCPVisibleTools
# bare names (no claireon.* prefix on the wire); the claireon.<tool>(...) Python
# attribute namespace lives strictly inside python_execute's runtime.
STATIC_TOOLS_LIST = [
    {
        "name": "tool_search",
        "description": (
            "Search Claireon's editor tool catalog by natural-language query. "
            "Returns a ranked list of tool names, descriptions, and their "
            "full input schemas so they can be invoked via `python_execute` "
            "running `claireon.<tool>(...)` Python."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "Natural-language description of the desired tool.",
                },
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of results to return (default 10).",
                    "minimum": 1,
                    "maximum": 50,
                },
            },
            "required": ["query"],
        },
    },
    {
        "name": "python_execute",
        "description": (
            "Execute a Python script inside the running Unreal Editor. "
            "The editor's embedded Python runtime has `unreal` available. "
            "Returns stdout, stderr, and the last-expression value."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {
                    "type": "string",
                    "description": "Python source to execute in the editor.",
                },
            },
            "required": ["code"],
        },
    },
    {
        "name": "mcp_ready_status",
        "description": (
            "Query the Claireon proxy's readiness state for this worktree. "
            "Returns {active_editor, ready, tool_count, last_heartbeat_ms_ago}. "
            "Use this to self-diagnose mid-run: if ready=false, the editor is "
            "still warming up (Python bridge not yet initialized). "
            "Handled locally by the proxy; never forwarded to the editor."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {},
        },
    },
    {
        "name": "proxy",
        "description": (
            "Manage the always-on Claireon proxy itself. Handled locally by the "
            "proxy; never forwarded to the editor. Call with no arguments (or "
            "command='help') to list available subcommands. Subcommands: help, "
            "restart, launch_editor, read_log, status."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": (
                        "Subcommand to run. Omit (or pass 'help') to list "
                        "available subcommands and their arguments."
                    ),
                },
                "args": {
                    "type": "object",
                    "description": "Subcommand-specific arguments (see help output).",
                },
            },
        },
    },
]

FALLBACK_TEXT = "build and launch the editor first"

# I4 (#0000): separate warming-up text from "no editor at all" text.
WARMING_UP_TEXT = (
    "Editor is registered but tool catalog is not yet ready (Python bridge still "
    "initializing). Retry in a few seconds, or call: proxy(command='wait_for_editor') "
    "to long-poll until the editor is fully ready."
)


# UPDATE_HERE_WHEN_ADDING_NEW_MCP_METHOD: keep ClaireonServer.cpp
# DispatchRequest and claireon_proxy.py FORWARDED_METHODS in sync.
FORWARDED_METHODS = frozenset({
    "prompts/list",
    "prompts/get",
    "resources/list",
    "resources/read",
    "resources/templates/list",
})


def _jsonrpc_result(request_id: Any, result: Dict[str, Any]) -> Dict[str, Any]:
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def _jsonrpc_error(request_id: Any, code: int, message: str) -> Dict[str, Any]:
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": code, "message": message},
    }


def _fallback_tool_result() -> Dict[str, Any]:
    return {
        "content": [{"type": "text", "text": FALLBACK_TEXT}],
        "isError": True,
    }


def _ensure_forward_conn_locked(session: Dict[str, Any]) -> http.client.HTTPConnection:
    """Return (creating if necessary) a persistent HTTPConnection to the editor.

    Caller MUST hold SESSION_LOCK.
    """
    conn = session.get("forward_conn")
    if conn is None:
        conn = http.client.HTTPConnection(
            "127.0.0.1",
            int(session["editor_mcp_port"]),
            timeout=FORWARD_DEFAULT_TIMEOUT_SECONDS,
        )
        session["forward_conn"] = conn
    return conn


def _forward_once(
    session_snapshot: Dict[str, Any], raw_body: bytes
) -> Tuple[int, bytes]:
    """Issue a single POST /mcp against the editor. Raises on transport error."""
    conn = http.client.HTTPConnection(
        "127.0.0.1",
        int(session_snapshot["editor_mcp_port"]),
        timeout=FORWARD_DEFAULT_TIMEOUT_SECONDS,
    )
    try:
        headers = {
            "Content-Type": "application/json",
            "Authorization": "Bearer %s" % session_snapshot["editor_mcp_token"],
            "Content-Length": str(len(raw_body)),
        }
        conn.request("POST", "/mcp", body=raw_body, headers=headers)
        resp = conn.getresponse()
        body = resp.read()
        return resp.status, body
    finally:
        try:
            conn.close()
        except Exception:  # noqa: BLE001
            pass


def _proxy_text_result(text: str, *, is_error: bool = False) -> Dict[str, Any]:
    """MCP content-envelope helper for proxy meta-tool responses."""
    return {
        "content": [{"type": "text", "text": text}],
        "isError": is_error,
    }


PROXY_HELP_TEXT = (
    "proxy meta-tool subcommands:\n"
    "  help                          List subcommands (this message). Default when 'command' is omitted.\n"
    "  launch_editor                 Build & launch the Unreal editor for this worktree.\n"
    "                                args: { skip_build: bool=false }\n"
    "  read_log                      Return the last N lines of proxy.log.\n"
    "                                args: { lines: int=200 }\n"
    "  status                        Show proxy runtime + registered editor session info.\n"
    "                                args: (none)\n"
    "  wait_for_editor               Long-poll until an editor is registered for this worktree with a\n"
    "                                fresh heartbeat. Returns the same payload as `status` on success;\n"
    "                                error on timeout.\n"
    "                                args: { timeout_seconds: float=120, max_heartbeat_age_seconds: float=10 }\n"
)
# (Stage 012) `restart` subcommand removed; the singleton runs continuously
# (Stage 009 D2/D3) and does not have a fresh-spawn shortcut. Use
# Invoke-MultiWorktreeProxyMigration.ps1 if you need to wipe leftover state.


def _spawn_detached(cmd: list) -> int:
    """Start a process detached from this proxy. Returns the new PID.

    On Windows, uses CREATE_NEW_PROCESS_GROUP so the child survives the
    proxy's exit and ignores Ctrl-C signals delivered to the proxy.
    """
    creationflags = 0
    start_new_session = False
    if sys.platform == "win32":
        # CREATE_NEW_PROCESS_GROUP = 0x00000200
        creationflags = 0x00000200
    else:
        start_new_session = True
    proc = subprocess.Popen(  # noqa: S603 -- args are constructed from worktree-local paths
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
        creationflags=creationflags,
        start_new_session=start_new_session,
    )
    return proc.pid


def _resolve_powershell_exe() -> Optional[str]:
    """Find a PowerShell executable. Preference: pwsh.exe (PS7), then powershell.exe."""
    if sys.platform != "win32":
        return None
    program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
    pwsh7 = os.path.join(program_files, "PowerShell", "7", "pwsh.exe")
    if os.path.isfile(pwsh7):
        return pwsh7
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    win_ps = os.path.join(system_root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
    if os.path.isfile(win_ps):
        return win_ps
    return None


def _scripts_utilities_dir(worktree_root: str) -> str:
    return os.path.join(worktree_root, "Scripts", "Utilities")


def _resolve_uproject(worktree_root: str) -> Optional[str]:
    """Return the absolute path to the worktree's .uproject file, if any.

    Discovers the project generically instead of assuming a fixed name, so
    the proxy works for any Unreal project that hosts the plugin. Returns
    None when no .uproject exists at the worktree root.
    """
    try:
        entries = sorted(os.listdir(worktree_root))
    except OSError:
        return None
    for name in entries:
        if name.lower().endswith(".uproject"):
            return os.path.join(worktree_root, name)
    return None


def _resolve_active_worktree_root(listener_port: Optional[int]) -> Optional[str]:
    """Stage 012: derive the meta-tool's worktree_root context.

    Singleton mode: prefer the canonical worktree the listener_port maps
    to (Claude reached us through that port). If listener_port is None or
    unmapped, fall back to RUNTIME["singleton_worktree_root"] (singleton
    fallback; still set by test fixtures). Returns None if nothing is
    resolvable.
    """
    if listener_port is not None:
        canonical = RUNTIME.get("mcp_port_to_worktree", {}).get(listener_port)
        if canonical:
            return canonical
    singleton = RUNTIME.get("singleton_worktree_root")
    if singleton:
        return singleton
    # Last resort: a single registered session's canonical worktree.
    with SESSION_LOCK:
        for canonical, wt in RUNTIME.get("worktrees", {}).items():
            if wt.session is not None:
                return canonical
    return None


def _handle_proxy_command(
    payload: Dict[str, Any], listener_port: Optional[int] = None
) -> Dict[str, Any]:
    """Local handler for the `proxy` meta-tool. Never forwarded to the editor."""
    request_id = payload.get("id")
    params = payload.get("params") or {}
    args = params.get("arguments") or {}
    command = (args.get("command") or "help").strip().lower()
    sub_args = args.get("args") or {}

    worktree_root = _resolve_active_worktree_root(listener_port)
    # help / status / read_log do not require a worktree (they read process-
    # level / per-host state). launch_editor needs one. We defer the check
    # until the subcommand demands it.

    if command in ("", "help"):
        return _jsonrpc_result(request_id, _proxy_text_result(PROXY_HELP_TEXT))

    if command == "status":
        with SESSION_LOCK:
            # Prefer the per-worktree session for this listener port so that
            # status reports the editor for THIS worktree, not whatever happened
            # to register most recently across all worktrees (singleton_session
            # is the global last-registered mirror and is misleading in
            # multi-worktree setups).
            session = None
            session_source = "none"
            if worktree_root:
                _wt = RUNTIME["worktrees"].get(worktree_root)
                if _wt is not None and _wt.session is not None:
                    _s = _wt.session
                    session = {
                        "pid": _s.editor_pid,
                        "worktree_root": worktree_root,
                        "start_time_ns": _s.start_time_ns,
                        "build_id": _s.build_id,
                        "proxy_version": _wt.version_hash,
                        "editor_mcp_port": _s.editor_mcp_port,
                        "editor_mcp_token": _s.editor_mcp_token,
                        "last_heartbeat_ts": _wt.last_seen_ns / 1_000_000_000,
                    }
                    session_source = "per_worktree"
            if session is None and singleton_session is not None:
                session = dict(singleton_session)
                session_source = "singleton_fallback"
        info = {
            # Stage 012: worktree_root is the resolved listener-mapped
            # worktree (or None in singleton mode without a session).
            "worktree_root": worktree_root,
            "proxy_pid": os.getpid(),
            "proxy_version_hash": RUNTIME.get("proxy_version_hash"),
            "active_editor": None,
        }
        if session is not None:
            now = _mono_now()
            info["active_editor"] = {
                # Stage 005: session_uuid replaced by (pid, start_time_ns).
                "editor_pid": session.get("pid"),
                "editor_start_time_ns": session.get("start_time_ns"),
                "build_id": session.get("build_id"),
                "editor_mcp_port": session.get("editor_mcp_port"),
                "proxy_version": session.get("proxy_version"),
                "last_heartbeat_age_seconds": (
                    round(now - float(session.get("last_heartbeat_ts") or 0.0), 2)
                ),
                # "per_worktree" means this is the session for the connected
                # worktree; "singleton_fallback" means the editor for THIS
                # worktree is not running and a different worktree's editor
                # is leaking through -- do not trust this session for tool calls.
                "session_source": session_source,
                "session_worktree": session.get("worktree_root"),
            }
        return _jsonrpc_result(request_id, _proxy_text_result(json.dumps(info, indent=2)))

    if command == "read_log":
        lines = int(sub_args.get("lines") or 200)
        if lines < 1:
            lines = 1
        if lines > 5000:
            lines = 5000
        # Stage 009 (D9): the singleton's log lives under the per-host
        # runtime dir, not under any worktree's Saved/. Fall back to the
        # legacy per-worktree location so a still-running pre-009 proxy
        # in some other worktree stays diagnosable.
        log_path = claireon_runtime_log_path()
        if not os.path.isfile(log_path):
            legacy = proxy_log_path(worktree_root) if worktree_root else None
            if legacy and os.path.isfile(legacy):
                log_path = legacy
            else:
                return _jsonrpc_result(request_id, _proxy_text_result(
                    f"proxy.log not found at {log_path}", is_error=True))
        try:
            with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
                buf = fh.readlines()
        except OSError as exc:
            return _jsonrpc_result(request_id, _proxy_text_result(
                f"failed to read proxy.log: {exc!r}", is_error=True))
        tail = "".join(buf[-lines:])
        return _jsonrpc_result(request_id, _proxy_text_result(tail))

    # (Stage 012) `restart` subcommand removed -- the singleton runs until
    # SIGINT/SIGTERM (D2/D3). Use Invoke-MultiWorktreeProxyMigration.ps1 if
    # you need to wipe leftover state.

    if command == "wait_for_editor":
        # Long-poll until an editor session is registered for this worktree
        # with a heartbeat newer than max_heartbeat_age_seconds. Returns the
        # same payload as `status` on success. Use this instead of bash poll
        # loops over MCP tool calls when waiting for the editor to bind after
        # Invoke-EditorBuildAndLaunch.ps1 -UseMCPProxy.
        try:
            timeout_seconds = float(sub_args.get("timeout_seconds", 120.0))
        except (TypeError, ValueError):
            timeout_seconds = 120.0
        try:
            max_age = float(sub_args.get("max_heartbeat_age_seconds", 10.0))
        except (TypeError, ValueError):
            max_age = 10.0
        # Clamp to sane ranges so a misconfigured caller can't hang the proxy
        # thread indefinitely or starve other MCP traffic with a tiny poll.
        timeout_seconds = max(1.0, min(timeout_seconds, 600.0))
        max_age = max(0.5, min(max_age, 60.0))
        deadline = _mono_now() + timeout_seconds
        poll_interval = 0.5
        while True:
            with SESSION_LOCK:
                session = dict(singleton_session) if singleton_session is not None else None
            if session is not None:
                age = _mono_now() - float(session.get("last_heartbeat_ts") or 0.0)
                if age <= max_age:
                    info = {
                        "worktree_root": worktree_root,
                        "proxy_pid": os.getpid(),
                        "proxy_version_hash": RUNTIME.get("proxy_version_hash"),
                        "active_editor": {
                            "editor_pid": session.get("pid"),
                            "editor_start_time_ns": session.get("start_time_ns"),
                            "build_id": session.get("build_id"),
                            "editor_mcp_port": session.get("editor_mcp_port"),
                            "proxy_version": session.get("proxy_version"),
                            "last_heartbeat_age_seconds": round(age, 2),
                        },
                    }
                    return _jsonrpc_result(
                        request_id, _proxy_text_result(json.dumps(info, indent=2)))
            now = _mono_now()
            if now >= deadline:
                return _jsonrpc_result(request_id, _proxy_text_result(
                    f"wait_for_editor: timed out after {timeout_seconds:.1f}s waiting for an editor"
                    f" with a heartbeat newer than {max_age:.1f}s. Launch the editor if it isn't running:"
                    f" call this tool with command='launch_editor' or run"
                    f" Scripts\\Utilities\\Invoke-EditorBuildAndLaunch.ps1 -UseMCPProxy.",
                    is_error=True))
            # Sleep until next tick or deadline, whichever is sooner. Short
            # interval keeps responsiveness high without burning the proxy
            # thread on a tight loop.
            time.sleep(min(poll_interval, deadline - now))

    if command == "launch_editor":
        if not worktree_root:
            return _jsonrpc_result(request_id, _proxy_text_result(
                "launch_editor requires a registered editor session for this listener port.",
                is_error=True))
        ps = _resolve_powershell_exe()
        if not ps:
            return _jsonrpc_result(request_id, _proxy_text_result(
                "PowerShell not found; cannot launch editor.", is_error=True))
        script = os.path.join(_scripts_utilities_dir(worktree_root), "Invoke-EditorBuildAndLaunch.ps1")
        if not os.path.isfile(script):
            return _jsonrpc_result(request_id, _proxy_text_result(
                f"Invoke-EditorBuildAndLaunch.ps1 not found at {script}", is_error=True))
        # Pass -ProjectPath explicitly so the launch script's project
        # auto-detection is never called with the proxy's inherited CWD
        # (which belongs to the worktree that first spawned the singleton,
        # not necessarily this one).
        project_file = _resolve_uproject(worktree_root)
        if not project_file:
            return _jsonrpc_result(request_id, _proxy_text_result(
                f"no .uproject found at worktree root {worktree_root}", is_error=True))
        cmd = [ps, "-NoProfile", "-File", script,
               "-ProjectPath", project_file, "-UseMCPProxy"]
        if bool(sub_args.get("skip_build")):
            cmd.append("-SkipBuild")
        try:
            new_pid = _spawn_detached(cmd)
        except OSError as exc:
            return _jsonrpc_result(request_id, _proxy_text_result(
                f"failed to spawn editor launch script: {exc!r}", is_error=True))
        log.info("proxy launch_editor: spawned pid=%d skip_build=%s", new_pid, bool(sub_args.get("skip_build")))
        # Record pending-launch timestamp so the first forwarded tool call
        # (python_execute, tool_search) auto-waits instead of erroring with
        # "build and launch the editor first".
        launch_canonical = canonicalize_worktree(worktree_root)
        with SESSION_LOCK:
            launch_wt = RUNTIME["worktrees"].get(launch_canonical)
            if launch_wt is not None:
                launch_wt.launch_pending_ts = _mono_now()
                log.info("launch_pending_ts set worktree=%s", launch_canonical)
        return _jsonrpc_result(request_id, _proxy_text_result(
            f"editor build/launch spawned (pid={new_pid}). "
            f"The proxy will auto-wait up to {_LAUNCH_PENDING_TIMEOUT_SECONDS:.0f}s on the next "
            f"python_execute or tool_search call; no explicit wait_for_editor needed."))

    return _jsonrpc_result(request_id, _proxy_text_result(
        f"Unknown subcommand: {command!r}. Call without 'command' (or with 'help') to list subcommands.",
        is_error=True))


def _resolve_session_for_listener(listener_port: Optional[int]) -> Optional[Dict[str, Any]]:
    """Stage 006: route Claude requests by listener port.

    The per-port McpHandler tells us which port it received on; we use
    RUNTIME["mcp_port_to_worktree"] to look up the canonical worktree
    and forward to that worktree's session. Caller MUST hold SESSION_LOCK.

    Returns a session-snapshot dict (the same shape _forward_once expects)
    or None if no session is bound to that worktree.

    When listener_port is None we fall back to singleton_session for the
    legacy single-tenant code path; this lets existing tests that drive
    forward_tool_call directly (without a real listener) keep working.
    """
    if listener_port is not None:
        canonical = RUNTIME["mcp_port_to_worktree"].get(listener_port)
        if canonical is None:
            return None
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is None or wt.session is None:
            return None
        return {
            "editor_mcp_port": wt.session.editor_mcp_port,
            "editor_mcp_token": wt.session.editor_mcp_token,
            "editor_pid": wt.session.editor_pid,
            "editor_start_time_ns": wt.session.start_time_ns,
            "canonical_worktree": canonical,
        }
    # Legacy / no-listener-port path.
    if singleton_session is None:
        return None
    return {
        "editor_mcp_port": singleton_session["editor_mcp_port"],
        "editor_mcp_token": singleton_session["editor_mcp_token"],
        "editor_pid": singleton_session.get("pid"),
        "editor_start_time_ns": singleton_session.get("start_time_ns"),
        "canonical_worktree": None,
    }


def _record_last_forward_status(
    session_snapshot: Dict[str, Any], status: int
) -> None:
    """Stage 007: stash the most recent forward status on the matching
    WorktreeState's Session so /admin/sessions can surface it. Best-
    effort -- a concurrent newest-wins eviction can null the session
    out from under us, in which case there is nothing to record."""
    canonical = session_snapshot.get("canonical_worktree")
    pid = session_snapshot.get("editor_pid")
    start = session_snapshot.get("editor_start_time_ns")
    if canonical is None:
        return
    with SESSION_LOCK:
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is None or wt.session is None:
            return
        if (wt.session.editor_pid, wt.session.start_time_ns) == (pid, start):
            wt.session.last_forward_status = int(status)


def _check_warming_up(listener_port: Optional[int]) -> Optional[str]:
    """I4/I10 (#0000): Return warming-up text if a session is registered but
    not yet ready. Caller MUST hold SESSION_LOCK. Returns None if no session
    (true fallback) so the caller can emit the standard FALLBACK_TEXT."""
    if listener_port is not None:
        canonical = RUNTIME["mcp_port_to_worktree"].get(listener_port)
        if canonical is None:
            # I10: no worktree bound to this port; likely a port-collision /
            # editor running in direct-connect mode on the proxy's port.
            # Produce a more actionable error than the bare FALLBACK_TEXT.
            return (
                "No editor session is registered for this proxy port. "
                "Likely causes:\n"
                "  1. Editor not yet started -- call proxy(command='launch_editor') or run "
                "Invoke-EditorBuildAndLaunch.ps1 -UseMCPProxy.\n"
                "  2. Editor is in direct-connect mode on the same port as the proxy "
                "(port collision). Relaunch with -UseMCPProxy to re-bind via the proxy.\n"
                "  3. Proxy lost track of the editor registration -- call "
                "proxy(command='wait_for_editor') to re-probe."
            )
        wt = RUNTIME["worktrees"].get(canonical)
        if wt is not None and wt.session is not None and not wt.ready:
            return WARMING_UP_TEXT
    else:
        # Legacy / no-listener-port path. Session exists in singleton_session;
        # check per-worktree state for the ready flag.
        if singleton_session is not None:
            ss_worktree = singleton_session.get("worktree_root") or ""
            if ss_worktree:
                try:
                    ss_canonical = canonicalize_worktree(ss_worktree)
                except Exception:  # noqa: BLE001
                    ss_canonical = ss_worktree.lower()
                wt = RUNTIME["worktrees"].get(ss_canonical)
                if wt is not None and not wt.ready:
                    return WARMING_UP_TEXT
    return None


def _has_pending_launch_locked(listener_port: Optional[int]) -> bool:
    """True if launch_editor was recently triggered for this listener port's worktree.

    MUST be called with SESSION_LOCK held. Returns False if the port is unmapped,
    no WorktreeState exists, or the pending timestamp has expired.
    """
    if listener_port is None:
        return False
    canonical = RUNTIME["mcp_port_to_worktree"].get(listener_port)
    if canonical is None:
        return False
    wt = RUNTIME["worktrees"].get(canonical)
    if wt is None:
        return False
    ts = wt.launch_pending_ts
    if ts == 0.0:
        return False
    return (_mono_now() - ts) <= _LAUNCH_PENDING_TIMEOUT_SECONDS


def _auto_wait_for_session(
    listener_port: Optional[int],
    timeout_seconds: float = _LAUNCH_PENDING_TIMEOUT_SECONDS,
) -> Optional[Dict[str, Any]]:
    """Block-poll until an editor session is registered and ready for listener_port.

    Called implicitly on the first forwarded tool call (python_execute, tool_search)
    when launch_pending_ts is set for the worktree -- so callers do not need to
    insert an explicit wait_for_editor call after launch_editor returns.

    Returns a session snapshot dict on success, or None on timeout. Blocks the
    calling handler thread (ThreadingHTTPServer spawns one thread per request,
    so blocking here does not stall other MCP traffic).
    """
    deadline = _mono_now() + timeout_seconds
    poll_interval = 1.0
    log.info(
        "auto_wait: launch pending for port=%s -- polling up to %.0fs for editor",
        listener_port, timeout_seconds,
    )
    while True:
        with SESSION_LOCK:
            snap = _resolve_session_for_listener(listener_port)
        if snap is not None:
            log.info("auto_wait: editor ready (port=%s)", listener_port)
            return snap
        now = _mono_now()
        if now >= deadline:
            log.warning(
                "auto_wait: timed out after %.1fs waiting for editor (port=%s)",
                timeout_seconds, listener_port,
            )
            return None
        time.sleep(min(poll_interval, deadline - now))


def _forward_payload_to_editor(
    payload: Dict[str, Any], listener_port: Optional[int] = None
) -> Dict[str, Any]:
    """Common transport for any forwarded MCP method.

    Returns a JSON-RPC response ready to send back to Claude.

    Behavior:
    - If no editor is registered for this worktree (or session is stale
      post-eviction), returns the fallback content result
      (`_fallback_tool_result`) via `_jsonrpc_result`.
    - On transport success, returns the parsed editor response, with
      `id` rewritten to the caller's request id when the editor echoed
      its own.
    - On two-attempt transport failure where every attempt was a
      `ConnectionRefusedError`, evicts the session and returns the
      fallback content result.
    - On other two-attempt transport failure, returns
      `-32000 "Editor connection failed"`.
    - On parse failure of the editor response body, returns
      `-32603 "Editor returned invalid JSON"` or
      `-32603 "Editor returned non-object"`.

    listener_port is the port on which the Claude request arrived. Stage
    006 uses it to pick the per-worktree session via
    RUNTIME["mcp_port_to_worktree"]. When omitted (legacy callers / tests),
    falls back to the singleton singleton_session.
    """
    global singleton_session
    request_id = payload.get("id")
    should_auto_wait = False
    with SESSION_LOCK:
        evict_singleton_stale_session_locked()
        session_snapshot = _resolve_session_for_listener(listener_port)
        if session_snapshot is None:
            # I10 (#0000): check if a session exists but ready=False (warming up)
            # vs. no session at all (build and launch / port collision).
            warming_up_text = _check_warming_up(listener_port)
            if warming_up_text:
                if warming_up_text == WARMING_UP_TEXT:
                    # Session registered but Python bridge not yet ready.
                    # Auto-wait instead of erroring -- both phases of the startup
                    # sequence (no-session and warming-up) deserve the same treatment.
                    should_auto_wait = True
                else:
                    # Port collision or other unresolvable condition (I10); auto-wait
                    # cannot fix this. Return the actionable diagnostic immediately.
                    return _jsonrpc_result(request_id, {
                        "content": [{"type": "text", "text": warming_up_text}],
                        "isError": True,
                    })
            elif _has_pending_launch_locked(listener_port):
                # No session yet but launch_editor was recently invoked.
                # Defer the fallback and block-poll outside the lock.
                should_auto_wait = True
            else:
                return _jsonrpc_result(request_id, _fallback_tool_result())

    # Auto-wait: covers two startup phases --
    #   1. launch_pending_ts set, no session yet (editor process starting)
    #   2. session registered but wt.ready=False (Python bridge initializing)
    # Returns early as soon as the session is fully ready.
    if should_auto_wait:
        session_snapshot = _auto_wait_for_session(listener_port)
        if session_snapshot is None:
            return _jsonrpc_result(request_id, _fallback_tool_result())

    raw = json.dumps(payload).encode("utf-8")
    last_exc: Optional[Exception] = None
    # Stage 007 (D7): Track clean transport-layer rejections separately
    # from other failures. ConnectionRefusedError fires when the editor's
    # MCP listener has gone (port stopped accepting); ConnectionResetError
    # fires when the listener accepted then immediately RST'd (editor mid-
    # crash). Either qualifies for synchronous eviction. socket.timeout
    # does NOT qualify -- a slow tool call may still be in-flight.
    transport_reject_count = 0
    for attempt in (1, 2):
        try:
            status, body = _forward_once(session_snapshot, raw)
        except (http.client.HTTPException, OSError) as exc:
            last_exc = exc
            if isinstance(exc, (ConnectionRefusedError, ConnectionResetError)):
                transport_reject_count += 1
            log.warning(
                "forward attempt %d failed editor_pid=%s start_time_ns=%s err=%r",
                attempt,
                session_snapshot.get("editor_pid"),
                session_snapshot.get("editor_start_time_ns"),
                exc,
            )
            if attempt == 1:
                time.sleep(0.1)
            continue

        if status >= 500:
            log.warning("forward attempt %d HTTP %d", attempt, status)
            # Stage 007 (D7): 5xx is NOT eviction-worthy. Record on the
            # session for observability and either retry once or surface.
            _record_last_forward_status(session_snapshot, status)
            if attempt == 1:
                time.sleep(0.1)
                continue

        try:
            parsed = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            log.error("forward response unparseable: %r", exc)
            _record_last_forward_status(session_snapshot, status)
            return _jsonrpc_error(request_id, -32603, "Editor returned invalid JSON")
        if not isinstance(parsed, dict):
            _record_last_forward_status(session_snapshot, status)
            return _jsonrpc_error(request_id, -32603, "Editor returned non-object")
        # Preserve Claude's request id (some servers echo their own).
        if "id" in parsed and parsed.get("id") != request_id:
            parsed["id"] = request_id
        _record_last_forward_status(session_snapshot, status)
        return parsed

    # Both attempts exhausted. If TCP was actively refused/reset on every
    # attempt, the editor's MCP listener is gone -- almost certainly the
    # editor died (Stage 007 D7). Evict the session immediately rather
    # than waiting for the heartbeat staleness watchdog (~60s), so Claude
    # gets the friendly content fallback within seconds instead of a
    # string of -32000 errors.
    if transport_reject_count == 2:
        with SESSION_LOCK:
            # Stage 006: prefer the per-worktree map. Only evict if the
            # session at the snapshotted canonical_worktree still matches
            # the (pid, start_time_ns) we saw fail (avoids racing a
            # concurrent newest-wins register).
            canonical = session_snapshot.get("canonical_worktree")
            evicted_pid = session_snapshot.get("editor_pid")
            evicted_start = session_snapshot.get("editor_start_time_ns")
            if canonical is not None:
                wt = RUNTIME["worktrees"].get(canonical)
                if wt is not None and wt.session is not None \
                        and (wt.session.editor_pid, wt.session.start_time_ns) \
                            == (evicted_pid, evicted_start):
                    wt.session = None
                    log.info(
                        "session evicted reason=connection_refused worktree=%s "
                        "editor_pid=%s start_time_ns=%s",
                        canonical, evicted_pid, evicted_start,
                    )
            # Legacy singleton_session shim eviction.
            if singleton_session is not None \
                    and singleton_session.get("pid") == evicted_pid \
                    and singleton_session.get("start_time_ns") == evicted_start:
                forward_conn = singleton_session.get("forward_conn")
                if forward_conn is not None:
                    try:
                        forward_conn.close()
                    except Exception:  # noqa: BLE001
                        pass
                if canonical is None:
                    log.info(
                        "session evicted reason=connection_refused editor_pid=%s "
                        "start_time_ns=%s",
                        evicted_pid, evicted_start,
                    )
                singleton_session = None
        return _jsonrpc_result(request_id, _fallback_tool_result())

    log.error(
        "forward failed after retries editor_pid=%s start_time_ns=%s err=%r",
        session_snapshot.get("editor_pid"),
        session_snapshot.get("editor_start_time_ns"),
        last_exc,
    )
    return _jsonrpc_error(request_id, -32000, "Editor connection failed")


def _handle_mcp_ready_status(
    payload: Dict[str, Any], listener_port: Optional[int] = None
) -> Dict[str, Any]:
    """I9 (#0000): Proxy-local handler for the `mcp_ready_status` tool.

    Returns a structured dict so agents can self-diagnose mid-run:
      active_editor  -- editor PID as string, or null if no session
      ready          -- bool: True once /editor/ready was received
      tool_count     -- int: tools registered at /editor/ready time (0 until ready)
      last_heartbeat_ms_ago -- ms since last heartbeat (null if no session)
    """
    request_id = payload.get("id")
    worktree_root = _resolve_active_worktree_root(listener_port)
    info: Dict[str, Any] = {
        "active_editor": None,
        "ready": False,
        "tool_count": 0,
        "last_heartbeat_ms_ago": None,
    }

    with SESSION_LOCK:
        # Prefer per-worktree state; fall back to singleton for legacy callers.
        wt = None
        if worktree_root:
            wt = RUNTIME["worktrees"].get(worktree_root)
        if wt is None and listener_port is not None:
            canonical = RUNTIME["mcp_port_to_worktree"].get(listener_port)
            if canonical:
                wt = RUNTIME["worktrees"].get(canonical)
        if wt is not None and wt.session is not None:
            info["active_editor"] = str(wt.session.editor_pid)
            info["ready"] = wt.ready
            info["tool_count"] = wt.tool_count
            now_ns = int(_mono_now() * 1_000_000_000)
            age_ns = now_ns - wt.last_seen_ns
            info["last_heartbeat_ms_ago"] = max(0, age_ns // 1_000_000)
        elif singleton_session is not None:
            info["active_editor"] = str(singleton_session.get("pid", ""))
            # For legacy singleton path, ready is unknown; report False conservatively.
            info["ready"] = False
            now = _mono_now()
            age = now - float(singleton_session.get("last_heartbeat_ts") or 0.0)
            info["last_heartbeat_ms_ago"] = int(max(0.0, age * 1000.0))

    result = {
        "content": [{"type": "text", "text": json.dumps(info, indent=2)}],
        "isError": False,
    }
    return _jsonrpc_result(request_id, result)


def forward_tool_call(
    payload: Dict[str, Any], listener_port: Optional[int] = None
) -> Dict[str, Any]:
    """Forward a JSON-RPC `tools/call` to the editor.

    Returns a JSON-RPC response ready to send back to Claude. If no editor
    is registered (or the session is stale), returns the fallback content
    result. Transport failures surface as a JSON-RPC error to Claude; the
    heartbeat watchdog is responsible for evicting the session if needed.
    The `proxy` meta-tool is handled locally and never forwarded.

    listener_port disambiguates which worktree the request came in on
    (Stage 006 multi-tenant routing).
    """
    request_id = payload.get("id")
    params = payload.get("params") or {}
    tool_name = params.get("name")
    if tool_name == "proxy":
        return _handle_proxy_command(payload, listener_port=listener_port)
    if tool_name == "mcp_ready_status":
        # I9 (#0000): proxy-local readiness query.
        return _handle_mcp_ready_status(payload, listener_port=listener_port)
    if tool_name not in {"tool_search", "python_execute"}:
        return _jsonrpc_error(request_id, -32601, "Method not found")
    return _forward_payload_to_editor(payload, listener_port=listener_port)


def _handle_mcp_payload(
    payload: Dict[str, Any], listener_port: Optional[int] = None
) -> Optional[Dict[str, Any]]:
    """Handle a single JSON-RPC object. Returns the response, or None for
    notifications that require no reply."""
    method = payload.get("method")
    request_id = payload.get("id")

    if method == "initialize":
        return _jsonrpc_result(
            request_id,
            {
                "protocolVersion": MCP_PROTOCOL_VERSION,
                "serverInfo": {
                    "name": MCP_SERVER_NAME,
                    "version": (RUNTIME.get("proxy_version_hash") or "unknown")[:12],
                },
                # Capability shape mirrors editor's HandleInitialize in
                # ClaireonServer.cpp lines 514-548. tools.listChanged differs: the
                # editor advertises true (dynamic catalog) but the proxy serves
                # tools/list from STATIC_TOOLS_LIST (line 649) and never emits
                # notifications/tools/list_changed, so advertising true would mislead.
                "capabilities": {
                    "tools":     {"listChanged": False},
                    "prompts":   {"listChanged": False},
                    "resources": {"subscribe": False, "listChanged": False},
                },
            },
        )
    if method == "notifications/initialized":
        # Notification: no id, no reply.
        return None
    if method == "ping":
        return _jsonrpc_result(request_id, {"pong": True})
    if method == "tools/list":
        return _jsonrpc_result(request_id, {"tools": STATIC_TOOLS_LIST})
    if method == "tools/call":
        return forward_tool_call(payload, listener_port=listener_port)
    if method in FORWARDED_METHODS:
        return _forward_payload_to_editor(payload, listener_port=listener_port)

    return _jsonrpc_error(request_id, -32601, "Method not found")


class McpHandler(BaseHTTPRequestHandler):
    """Claude-facing MCP handler (on MCP_PORT)."""

    server_version = "claireon-proxy/1"

    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: A003
        log.debug("mcp %s - %s", self.address_string(), fmt % args)

    def do_GET(self) -> None:  # noqa: N802
        RUNTIME["last_claude_activity_ts"] = time.monotonic()
        if self.path == "/health":
            self._respond_json(200, {"ok": True})
            return
        self._respond_json(404, {
            "error": "not_found",
            "reason": (
                "This proxy's HTTP surface is private. The only public paths are "
                "/health (GET), /admin/health (GET), /admin/sessions (GET), "
                "/admin/ensure_worktree (POST), and the /editor/* registration POSTs. "
                "If you're looking for proxy/session state, use the `mcp__claireon__proxy` "
                "MCP tool (commands: status, wait_for_editor, launch_editor, read_log, "
                "help) -- do not screen-scrape this HTTP surface."
            ),
            "requested_path": self.path,
        })

    def do_POST(self) -> None:  # noqa: N802
        RUNTIME["last_claude_activity_ts"] = time.monotonic()
        if self.path not in ("/mcp", "/"):
            self._respond_json(404, {
            "error": "not_found",
            "reason": (
                "This proxy's HTTP surface is private. The only public paths are "
                "/health (GET), /admin/health (GET), /admin/sessions (GET), "
                "/admin/ensure_worktree (POST), and the /editor/* registration POSTs. "
                "If you're looking for proxy/session state, use the `mcp__claireon__proxy` "
                "MCP tool (commands: status, wait_for_editor, launch_editor, read_log, "
                "help) -- do not screen-scrape this HTTP surface."
            ),
            "requested_path": self.path,
        })
            return

        body = _read_json_body(self)
        if body is None:
            self._respond_json(
                400,
                _jsonrpc_error(None, -32700, "Parse error"),
            )
            return

        # Stage 006: tell _handle_mcp_payload which listener port the
        # request arrived on so it can route to the right per-worktree
        # session via RUNTIME["mcp_port_to_worktree"]. server_address is
        # the (host, port) tuple BaseHTTPServer captured at bind time.
        listener_port: Optional[int] = None
        try:
            listener_port = int(self.server.server_address[1])
        except (AttributeError, IndexError, TypeError, ValueError):
            listener_port = None

        # Support single JSON-RPC request (dict). Arrays are not part of the
        # MCP streamable-HTTP profile we target.
        response = _handle_mcp_payload(body, listener_port=listener_port)
        if response is None:
            # Notification -- HTTP 204.
            self.send_response(204)
            self.end_headers()
            return
        self._respond_json(200, response)

    def _respond_json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------------------
# idle-exit + shutdown plumbing.
#
# The main thread runs a low-frequency loop that:
#   - Evicts stale editor sessions (> HEARTBEAT_STALENESS_SECONDS since the
#     last heartbeat).
#   - Exits the process cleanly once BOTH conditions hold for longer than
#     IDLE_AUTO_EXIT_HOURS: no registered editor AND no Claude-facing
#     activity (the McpHandler refreshes RUNTIME["last_claude_activity_ts"]
#     on every request).
# ---------------------------------------------------------------------------


SHUTDOWN_EVENT = threading.Event()
# Version-mismatch log dedup: editors can retry register quickly; rate-limit
# the warning so the proxy.log doesn't fill with duplicates.
_VERSION_MISMATCH_LOG_INTERVAL_SECONDS = 60.0
_version_mismatch_state: Dict[str, Any] = {"last_log_ts": 0.0, "last_seen": None}


# Stage 009 (D2): idle_seconds() / should_idle_exit() are deleted along
# with the IDLE_AUTO_EXIT_HOURS ceiling. Kept as compatibility shims so
# any external test that still imports them sees a deterministic "never
# idle-exit" contract. See run_stale_session_loop above.


def idle_seconds() -> float:
    """Stage 009 D2 shim: idle-exit is gone. Always returns 0.0 so any
    legacy reader sees "never idle"; do not call this from new code."""
    return 0.0


def should_idle_exit(idle_ceiling_seconds: float) -> bool:
    """Stage 009 D2 shim: idle-exit is gone; always returns False."""
    del idle_ceiling_seconds
    return False


def note_version_mismatch_seen(version: str) -> bool:
    """Return True iff the caller should log a warning for this mismatch."""
    now = _mono_now()
    last_ts = float(_version_mismatch_state.get("last_log_ts") or 0.0)
    last_seen = _version_mismatch_state.get("last_seen")
    if version == last_seen and (now - last_ts) < _VERSION_MISMATCH_LOG_INTERVAL_SECONDS:
        return False
    _version_mismatch_state["last_log_ts"] = now
    _version_mismatch_state["last_seen"] = version
    return True


def install_signal_handlers() -> None:
    """Install SIGINT / SIGTERM (and Windows SIGBREAK) handlers that set
    SHUTDOWN_EVENT. The main loop observes the event and exits cleanly."""

    def _handler(signum: int, _frame: Any) -> None:
        log.info("signal %d received; shutting down", signum)
        SHUTDOWN_EVENT.set()

    signals = [getattr(signal, "SIGINT", None), getattr(signal, "SIGTERM", None)]
    if os.name == "nt":
        signals.append(getattr(signal, "SIGBREAK", None))
    for sig in signals:
        if sig is None:
            continue
        try:
            signal.signal(sig, _handler)
        except (ValueError, OSError):
            # signal.signal can only run on the main thread on some platforms;
            # swallow if unavailable (e.g. under unittest runners).
            pass


def evict_stale_sessions_all_locked() -> int:
    """Stage 009 (D2): evict per-worktree stale sessions across every
    WorktreeState. Returns the count of sessions evicted. MUST be called
    with SESSION_LOCK held.

    The single-tenant evict_singleton_stale_session_locked() (which still walks
    singleton_session) is invoked first so the legacy /admin/status mirror
    stays consistent; then we iterate every worktree and clear any
    session whose last_seen_ns predates the staleness threshold.
    """
    # Drain the legacy view first; it walks singleton_session and
    # transitively clears the matching WorktreeState.session.
    legacy_evicted = evict_singleton_stale_session_locked()
    n = 1 if legacy_evicted is not None else 0
    if legacy_evicted is not None:
        log.info(
            "session evicted reason=heartbeat_timeout editor_pid=%s "
            "start_time_ns=%s build_id=%s",
            legacy_evicted.get("pid"),
            legacy_evicted.get("start_time_ns"),
            legacy_evicted.get("build_id"),
        )

    # Now sweep every per-worktree slot. Multi-tenant routing means a
    # worktree with no singleton_session mirror can still hold a stale
    # session record from a previous editor.
    threshold_ns = HEARTBEAT_STALENESS_SECONDS * 1_000_000_000
    now_ns = int(_mono_now() * 1_000_000_000)
    for canonical, wt in RUNTIME["worktrees"].items():
        if wt.session is None:
            continue
        if wt.last_seen_ns == 0:
            continue
        age_ns = now_ns - wt.last_seen_ns
        if age_ns <= threshold_ns:
            continue
        log.info(
            "[worktree=%s] session evicted reason=heartbeat_timeout "
            "editor_pid=%d start_time_ns=%d build_id=%s age_seconds=%.1f",
            canonical,
            wt.session.editor_pid,
            wt.session.start_time_ns,
            wt.session.build_id,
            age_ns / 1_000_000_000,
        )
        wt.session = None
        n += 1
    return n


def run_stale_session_loop(tick_seconds: float = STALE_TICK_SECONDS) -> None:
    """Block until SHUTDOWN_EVENT is set, evicting stale sessions every
    tick (Stage 009 D2). The proxy never idle-exits -- only SIGINT/SIGTERM
    or fatal startup failures end its lifetime.
    """
    while not SHUTDOWN_EVENT.wait(timeout=tick_seconds):
        with SESSION_LOCK:
            evict_stale_sessions_all_locked()


# Legacy alias retained for any external smoke that still imports
# run_idle_exit_loop. The signature now ignores idle_ceiling_seconds and
# treats it as 0 (D2 -- never auto-exit). claireon_proxy_test.py asserts
# the contract that a 0 ceiling does not set SHUTDOWN_EVENT.
def run_idle_exit_loop(
    idle_ceiling_seconds: float = 0.0,
    tick_seconds: float = STALE_TICK_SECONDS,
) -> None:
    """Stage 009 D2 compatibility shim: idle-exit is gone; the loop now
    runs solely as a stale-session evictor. idle_ceiling_seconds is
    accepted but ignored.
    """
    del idle_ceiling_seconds  # D2: idle-exit removed.
    run_stale_session_loop(tick_seconds=tick_seconds)


# ---------------------------------------------------------------------------
# main -- CLI, logging setup, process lifecycle.
# ---------------------------------------------------------------------------


def compute_proxy_version_hash() -> str:
    """SHA-1 of this file's bytes.

    Matches FClaireonProxyClient::ComputeProxyScriptHash() on the C++ side,
    which uses FSHA1::HashBuffer -- UE does not ship a SHA-256 implementation
    on Windows (FPlatformMisc::GetSHA256Signature asserts). SHA-1 is fine
    here; the hash is a version-tag match, not a security boundary.
    """
    try:
        source_path = os.path.abspath(__file__)
        with open(source_path, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()
    except OSError as exc:
        log.warning("could not hash claireon_proxy.py: %s", exc)
        return "unknown"


def configure_logging(level: str) -> None:
    """Configure the singleton's root logger.

    Stage 009 (D9): the log lives at the per-host singleton path, NOT
    inside any worktree. claireon_runtime_log_path() returns
    %LOCALAPPDATA%\\Claireon\\proxy.log on Windows and
    ~/.local/share/claireon/proxy.log on POSIX.
    """
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    root.setLevel(getattr(logging, level.upper(), logging.INFO))

    file_handler = logging.handlers.RotatingFileHandler(
        claireon_runtime_log_path(),
        maxBytes=_LOG_MAX_BYTES,
        backupCount=_LOG_BACKUP_COUNT,
        encoding="utf-8",
    )
    file_handler.setFormatter(logging.Formatter(_LOG_FORMAT))
    root.addHandler(file_handler)

    # Mirror ERROR+ to stderr for the first window so spawn failures surface.
    stderr_handler = logging.StreamHandler(stream=sys.stderr)
    stderr_handler.setLevel(logging.ERROR)
    stderr_handler.setFormatter(logging.Formatter(_LOG_FORMAT))
    root.addHandler(stderr_handler)

    def _downgrade_stderr() -> None:
        time.sleep(_STDERR_MIRROR_SECONDS)
        stderr_handler.setLevel(logging.CRITICAL + 1)

    threading.Thread(target=_downgrade_stderr, daemon=True).start()


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse CLI args for the singleton proxy.

    Stage 009 removals (per OVERVIEW.md D2 + D3):
      - --worktree-root: the singleton serves every worktree; the
        ensure_worktree admin endpoint is the only path that adds one.
      - --idle-exit-seconds / --idle-tick-seconds: D2 -- no idle-exit.
        The proxy runs until SIGINT/SIGTERM. Stale sessions are still
        evicted on the per-tick loop so an editor crash is observable
        within HEARTBEAT_STALENESS_SECONDS.
    """
    parser = argparse.ArgumentParser(description="Claireon MCP Proxy")
    parser.add_argument(
        "--extra-python-args",
        default="",
        help="Reserved for future extension; currently unused.",
    )
    parser.add_argument(
        "--clock-offset-seconds",
        type=float,
        default=0.0,
        help="Debug: inject a simulated clock advance (used by tests).",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        help="Logging threshold (DEBUG, INFO, WARNING, ERROR, CRITICAL).",
    )
    return parser.parse_args(argv)


def run(argv: list[str]) -> int:
    args = parse_args(argv)
    configure_logging(args.log_level)

    log.info("claireon_proxy starting (singleton, pid=%d)", os.getpid())

    version_hash = compute_proxy_version_hash()
    # Stage 009: the proxy is multi-tenant from the start. RUNTIME no
    # longer carries a singleton worktree_root / canonical_worktree;
    # /admin/ensure_worktree adds worktrees on demand. The legacy keys
    # are left as None so any defensive reader still finds the slot.
    RUNTIME["singleton_worktree_root"] = None
    RUNTIME["canonical_worktree"] = None
    RUNTIME["proxy_version_hash"] = version_hash
    RUNTIME["last_claude_activity_ts"] = time.monotonic()
    RUNTIME["clock_offset_seconds"] = float(args.clock_offset_seconds or 0.0)
    log.info("proxy_version_hash=%s", version_hash)

    reg_server: Optional[ThreadingHTTPServer] = None
    try:
        # Stage 009 (D3): the registration listener IS the lock. The bind
        # either succeeds (we are the singleton) or fails with EADDRINUSE
        # (another singleton already owns the port) -- bind_registration
        # _server logs the conflicting owner and exits 2.
        reg_server = bind_registration_server(RegistrationHandler)

        reg_thread = threading.Thread(
            target=reg_server.serve_forever, name="reg-listener", daemon=True
        )
        reg_thread.start()
        log.info(
            "proxy ready (reg_port=%d). Awaiting /admin/ensure_worktree.",
            PROXY_REG_PORT,
        )

        install_signal_handlers()
        run_stale_session_loop()
    except KeyboardInterrupt:
        log.info("claireon_proxy interrupted; shutting down")
        SHUTDOWN_EVENT.set()
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001
        log.critical("claireon_proxy crashed", exc_info=True)
        return 1
    finally:
        # Close any persistent forward connection to the editor.
        with SESSION_LOCK:
            if singleton_session is not None:
                fc = singleton_session.get("forward_conn")
                if fc is not None:
                    try:
                        fc.close()
                    except Exception:  # noqa: BLE001
                        pass
            # Tear down every per-worktree MCP listener. Stage 009:
            # listeners are bound lazily by /admin/ensure_worktree, never
            # GC'd while the singleton runs (D3), but we must release them
            # cleanly on shutdown so a follow-on singleton can take 43017.
            for wt in RUNTIME.get("worktrees", {}).values():
                if wt.mcp_server is not None:
                    try:
                        wt.mcp_server.shutdown()
                        wt.mcp_server.server_close()
                    except Exception:  # noqa: BLE001
                        pass
                    wt.mcp_server = None
        if reg_server is not None:
            try:
                reg_server.shutdown()
                reg_server.server_close()
            except Exception:  # noqa: BLE001
                pass
        log.info("claireon_proxy exited")

    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
