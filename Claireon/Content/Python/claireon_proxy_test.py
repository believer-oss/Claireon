"""Unit tests for claireon_proxy.py.

Runnable by UE's vendored Python 3 with stdlib unittest:
  <EngineDir>/Binaries/ThirdParty/Python3/Win64/python.exe claireon_proxy_test.py

Coverage tracks ALWAYS_ON_MCP_PROXY_TESTS.md:
  1. Port derivation (determinism, distribution, case normalization).
  2. Lock reclamation (live / dead / pid-reuse / image-mismatch).
  3. Register / heartbeat / unregister protocol + staleness eviction.
  4. Idempotent double-spawn.
  5. Forwarded tool-call round-trip against a stdlib fake editor.
  6. 24h idle auto-exit (with and without a registered editor).
  7. "build and launch the editor first" fallback + isError.
  8. PROXY_REG_PORT constant sync with ClaireonProxyConstants.h.

The suite runs in-process; it never spawns a real subprocess. For
integration coverage that does spawn claireon_proxy, see the C++ smoke
test FClaireonProxyClientSmokeTest.
"""

from __future__ import annotations

import contextlib
import http.client
import io
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Dict, List, Optional, Tuple
from unittest import mock

# Ensure this file can be executed from its own directory even if the CWD
# differs -- needed when operator double-clicks or CI chdir's elsewhere.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

import claireon_proxy  # noqa: E402  (path insertion above is intentional)


# ---------------------------------------------------------------------------
# Test helpers.
# ---------------------------------------------------------------------------


def _reset_proxy_runtime(worktree_root: str) -> None:
    """Clear module-level state so each test starts from a clean slate."""
    claireon_proxy.active_session = None
    claireon_proxy._version_mismatch_state["last_log_ts"] = 0.0
    claireon_proxy._version_mismatch_state["last_seen"] = None
    claireon_proxy.RUNTIME["worktree_root"] = worktree_root
    claireon_proxy.RUNTIME["canonical_worktree"] = claireon_proxy.canonicalize_worktree(worktree_root)
    claireon_proxy.RUNTIME["proxy_version_hash"] = "TESTHASH"
    claireon_proxy.RUNTIME["last_claude_activity_ts"] = time.monotonic()
    claireon_proxy.RUNTIME["clock_offset_seconds"] = 0.0
    # Stage 005: per-worktree maps are authoritative; reset them between
    # tests so cross-test state from a previous register cannot leak.
    claireon_proxy.RUNTIME["worktrees"] = {}
    claireon_proxy.RUNTIME["mcp_port_to_worktree"] = {}


def _well_formed_register_body(
    worktree_root: str,
    *,
    proxy_version: str = "TESTHASH",
    pid: Optional[int] = None,
    start_time_ns: Any = None,
    editor_mcp_port: int = 60000,
    token: str = "a" * 32,
) -> Dict[str, Any]:
    """Stage 005 wire shape: identifies the session by (pid, start_time_ns)
    rather than session_uuid. Tests pass an explicit start_time_ns to
    simulate distinct sessions; default uses int(uuid4()) hashed into the
    int range so that two unspecified callers get different identities.

    start_time_ns is a decimal-digit string on the wire (Windows FILETIME
    values exceed JSON Number precision); int values are coerced for
    test-author convenience."""
    if pid is None:
        pid = os.getpid()
    if start_time_ns is None:
        start_time_ns = str(uuid.uuid4().int & 0x7FFFFFFFFFFFFFFF)
    elif isinstance(start_time_ns, int):
        start_time_ns = str(start_time_ns)
    return {
        "pid": pid,
        "worktree_root": worktree_root,
        "start_time_ns": start_time_ns,
        "build_id": "deadbeef",
        "proxy_version": proxy_version,
        "editor_mcp_port": editor_mcp_port,
        "editor_mcp_token": token,
    }


class _FakeEditor:
    """Minimal stdlib HTTP server standing in for the editor's MCP listener."""

    def __init__(self, expected_token: str):
        self.expected_token = expected_token
        self.received: List[Dict[str, Any]] = []
        self.response_payload: Dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {"content": [{"type": "text", "text": "ok-from-editor"}]},
        }
        self._server: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None
        self.port: int = 0

    def start(self) -> None:
        editor = self

        class _Handler(BaseHTTPRequestHandler):
            server_version = "fake-editor/1"

            def log_message(self, fmt: str, *args: Any) -> None:  # noqa: A003
                pass

            def do_POST(self) -> None:  # noqa: N802
                auth = self.headers.get("Authorization", "")
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length) if length > 0 else b""
                try:
                    parsed = json.loads(raw.decode("utf-8")) if raw else {}
                except Exception:  # noqa: BLE001
                    parsed = {}
                editor.received.append({"auth": auth, "body": parsed, "path": self.path})
                if auth != f"Bearer {editor.expected_token}":
                    self.send_response(401)
                    self.end_headers()
                    return
                payload = dict(editor.response_payload)
                # Echo the Claude request id so the forwarder's id-rewriting
                # path is validated.
                if "id" in parsed:
                    payload["id"] = parsed["id"]
                body = json.dumps(payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.port = self._server.server_address[1]
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True, name="fake-editor"
        )
        self._thread.start()

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None


# ---------------------------------------------------------------------------
# Port derivation.
# ---------------------------------------------------------------------------


class TestPortDerivation(unittest.TestCase):
    def test_same_root_same_port(self) -> None:
        root = r"C:\path\to\project"
        a = claireon_proxy.derive_default_mcp_port(root)
        b = claireon_proxy.derive_default_mcp_port(root)
        self.assertEqual(a, b)
        self.assertGreaterEqual(a, 49152)
        self.assertLessEqual(a, 65535)

    def test_case_normalization(self) -> None:
        a = claireon_proxy.derive_default_mcp_port(r"C:\Git\MyGame")
        b = claireon_proxy.derive_default_mcp_port(r"c:\path\to\project")
        self.assertEqual(a, b)

    def test_distribution_across_samples(self) -> None:
        # Generate 100 distinct synthetic worktree-root strings and confirm
        # the derived ports are overwhelmingly unique. We allow up to 1
        # collision to absorb SHA-256 "bad luck" without flaking CI.
        ports: List[int] = []
        for i in range(100):
            path = rf"D:\wt\{i:03d}-sample-worktree"
            ports.append(claireon_proxy.derive_default_mcp_port(path))
        duplicates = len(ports) - len(set(ports))
        self.assertLessEqual(duplicates, 1, f"too many collisions: {duplicates}")


# ---------------------------------------------------------------------------
# Lock reclamation.
# ---------------------------------------------------------------------------


class TestLockReclamation(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-lock-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _lock_path(self) -> str:
        return claireon_proxy.proxy_lock_path(self.tmp)

    def test_dead_pid_is_reclaimed(self) -> None:
        os.makedirs(os.path.dirname(self._lock_path()), exist_ok=True)
        with open(self._lock_path(), "w", encoding="utf-8") as f:
            json.dump({"pid": 1, "start_time_ns": 0}, f)
        # PID 1 on Windows is the "System Idle Process" and does not pass our
        # python-image check, so the lock should be reclaimed without the
        # helper exiting the process.
        self.assertFalse(claireon_proxy._existing_lock_is_live(self._lock_path()))

    def test_wrong_image_is_reclaimed(self) -> None:
        os.makedirs(os.path.dirname(self._lock_path()), exist_ok=True)
        with open(self._lock_path(), "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "start_time_ns": 1}, f)
        # Live pid + matching start time, but stub image name to something
        # non-python to simulate PID reuse by an unrelated process.
        with mock.patch.object(
            claireon_proxy, "_process_image_name", return_value="notepad.exe"
        ):
            self.assertFalse(claireon_proxy._existing_lock_is_live(self._lock_path()))

    def test_mismatched_start_time_is_reclaimed(self) -> None:
        os.makedirs(os.path.dirname(self._lock_path()), exist_ok=True)
        with open(self._lock_path(), "w", encoding="utf-8") as f:
            # Recorded start_time_ns is deliberately different from the real
            # one -- simulates PID reuse by a new python process.
            json.dump({"pid": os.getpid(), "start_time_ns": 1}, f)
        with mock.patch.object(
            claireon_proxy, "_process_image_name", return_value="python.exe"
        ), mock.patch.object(
            claireon_proxy, "_process_start_time_ns", return_value=9_999_999
        ):
            self.assertFalse(claireon_proxy._existing_lock_is_live(self._lock_path()))

    def test_live_matching_lock_is_detected(self) -> None:
        os.makedirs(os.path.dirname(self._lock_path()), exist_ok=True)
        # Fabricate a lock that looks exactly like us.
        real_start = claireon_proxy._current_process_start_time_ns() or 1
        with open(self._lock_path(), "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "start_time_ns": real_start}, f)
        with mock.patch.object(
            claireon_proxy, "_process_image_name", return_value="python.exe"
        ), mock.patch.object(
            claireon_proxy, "_process_start_time_ns", return_value=real_start
        ):
            self.assertTrue(claireon_proxy._existing_lock_is_live(self._lock_path()))

    def test_acquire_lock_exits_when_live_owner_present(self) -> None:
        os.makedirs(os.path.dirname(self._lock_path()), exist_ok=True)
        with open(self._lock_path(), "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "start_time_ns": 1}, f)
        with mock.patch.object(
            claireon_proxy, "_existing_lock_is_live", return_value=True
        ):
            with self.assertRaises(SystemExit) as cm:
                claireon_proxy.acquire_lock_or_exit(self.tmp)
            self.assertEqual(cm.exception.code, 0)


# ---------------------------------------------------------------------------
# Registration / heartbeat / unregister protocol.
# ---------------------------------------------------------------------------


class TestRegistrationProtocol(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-reg-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

    def test_register_success(self) -> None:
        body = _well_formed_register_body(self.tmp)
        status, resp = claireon_proxy.handle_register(body)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])
        self.assertIsNotNone(claireon_proxy.active_session)

    def test_register_json_roundtrip_with_filetime_value(self) -> None:
        """Regression: Windows FILETIME values (~1.3e17) exceed JSON Number
        precision. The C++ editor sends start_time_ns as a decimal string;
        the proxy stores it verbatim. This test mirrors the live wire by
        json.dumps + json.loads-ing the body before handing it to the
        validator -- catching any future drift back to a numeric field."""
        body = _well_formed_register_body(
            self.tmp,
            pid=12345,
            start_time_ns=134228224836658432,  # > 2^53, beyond double precision
        )
        wire = json.loads(json.dumps(body))
        self.assertIsInstance(wire["start_time_ns"], str)
        self.assertEqual(wire["start_time_ns"], "134228224836658432")
        status, resp = claireon_proxy.handle_register(wire)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])
        canonical = claireon_proxy.canonicalize_worktree(self.tmp)
        wt = claireon_proxy.RUNTIME["worktrees"][canonical]
        # Stored verbatim -- never parsed to int, so no precision loss.
        self.assertEqual(wt.session.start_time_ns, "134228224836658432")
        # Heartbeat round-trips the same string and matches identity.
        hb = json.loads(json.dumps({
            "worktree_root": self.tmp,
            "pid": 12345,
            "start_time_ns": "134228224836658432",
        }))
        hb_status, hb_resp = claireon_proxy.handle_heartbeat(hb)
        self.assertEqual(hb_status, 200)
        self.assertTrue(hb_resp["ok"])

    def test_register_rejects_numeric_start_time_ns(self) -> None:
        """The wire contract is a digit string; a JSON Number must be
        rejected as malformed. This guards against silent regressions where
        a future caller sends a float (UE TJsonWriter for large doubles)
        and the proxy accidentally accepts it."""
        body = _well_formed_register_body(self.tmp)
        body["start_time_ns"] = 1.342282248366584e17  # what the bug looked like
        status, resp = claireon_proxy.handle_register(body)
        self.assertEqual(status, 400)
        self.assertFalse(resp["accepted"])
        self.assertEqual(resp["reason"], "malformed_request")

    def test_register_second_evicts_with_newest_wins(self) -> None:
        """Stage 005 (D6 newest-wins): a second register on the same
        worktree displaces the previous session and drops a one-shot
        evicted_by breadcrumb so the displaced editor's next heartbeat
        sees evicted_by and walks to terminal Failed."""
        first = _well_formed_register_body(self.tmp, pid=1111, start_time_ns=100)
        self.assertEqual(claireon_proxy.handle_register(first)[0], 200)

        second = _well_formed_register_body(self.tmp, pid=2222, start_time_ns=200)
        status, resp = claireon_proxy.handle_register(second)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])
        # The evicted_by breadcrumb is set on the worktree state for the
        # displaced editor's next heartbeat to consume.
        canonical = claireon_proxy.canonicalize_worktree(self.tmp)
        wt = claireon_proxy.RUNTIME["worktrees"][canonical]
        self.assertEqual(wt._evicted_by, (2222, "200"))
        # The displaced session's heartbeat surfaces evicted_by once.
        hb_status, hb_resp = claireon_proxy.handle_heartbeat(
            {"worktree_root": self.tmp, "pid": 1111, "start_time_ns": "100"}
        )
        self.assertEqual(hb_status, 200)
        self.assertFalse(hb_resp["ok"])
        self.assertEqual(hb_resp["reason"], "unknown_session")
        self.assertEqual(hb_resp["evicted_by"], {"pid": 2222, "start_time_ns": "200"})
        # One-shot: the breadcrumb is cleared after the first heartbeat.
        self.assertIsNone(wt._evicted_by)

    def test_register_version_mismatch_is_advisory(self) -> None:
        """Stage 005 (D5): version drift is logged but not rejected."""
        body = _well_formed_register_body(self.tmp, proxy_version="WRONGHASH")
        status, resp = claireon_proxy.handle_register(body)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])
        self.assertNotIn("reason", resp)

    def test_heartbeat_updates_timestamp(self) -> None:
        body = _well_formed_register_body(self.tmp)
        claireon_proxy.handle_register(body)
        canonical = claireon_proxy.canonicalize_worktree(self.tmp)
        before = claireon_proxy.RUNTIME["worktrees"][canonical].last_seen_ns
        # Deterministically advance the clock rather than real-sleeping; on
        # Windows the default timer resolution (~15.6 ms) sometimes makes
        # `time.sleep(0.01)` a no-op and the test would flake.
        claireon_proxy.RUNTIME["clock_offset_seconds"] = 1.0
        try:
            status, resp = claireon_proxy.handle_heartbeat({
                "worktree_root": self.tmp,
                "pid": body["pid"],
                "start_time_ns": body["start_time_ns"],
            })
        finally:
            claireon_proxy.RUNTIME["clock_offset_seconds"] = 0.0
        self.assertEqual(status, 200)
        self.assertTrue(resp["ok"])
        self.assertGreater(
            claireon_proxy.RUNTIME["worktrees"][canonical].last_seen_ns, before
        )

    def test_heartbeat_unknown_session_no_breadcrumb(self) -> None:
        """Heartbeat for a worktree that has no session at all -> stale
        unknown_session; no evicted_by because no displacement happened."""
        status, resp = claireon_proxy.handle_heartbeat({
            "worktree_root": self.tmp,
            "pid": 9999,
            "start_time_ns": "12345",
        })
        self.assertEqual(status, 200)
        self.assertFalse(resp["ok"])
        self.assertEqual(resp["reason"], "unknown_session")
        self.assertNotIn("evicted_by", resp)

    def test_clock_injection_evicts_stale_session(self) -> None:
        body = _well_formed_register_body(self.tmp)
        claireon_proxy.handle_register(body)
        # Jump forward past the staleness threshold.
        claireon_proxy.RUNTIME["clock_offset_seconds"] = (
            float(claireon_proxy.HEARTBEAT_STALENESS_SECONDS) + 1.0
        )
        with claireon_proxy.SESSION_LOCK:
            evicted = claireon_proxy.evict_stale_session_locked()
        self.assertIsNotNone(evicted)
        self.assertIsNone(claireon_proxy.active_session)

    def test_unregister_clears_session(self) -> None:
        body = _well_formed_register_body(self.tmp)
        claireon_proxy.handle_register(body)
        status, resp = claireon_proxy.handle_deregister({
            "worktree_root": self.tmp,
            "pid": body["pid"],
            "start_time_ns": body["start_time_ns"],
        })
        self.assertEqual(status, 200)
        self.assertTrue(resp["ok"])
        self.assertIsNone(claireon_proxy.active_session)
        canonical = claireon_proxy.canonicalize_worktree(self.tmp)
        self.assertIsNone(claireon_proxy.RUNTIME["worktrees"][canonical].session)

    def test_tool_call_fallback_after_unregister(self) -> None:
        body = _well_formed_register_body(self.tmp)
        claireon_proxy.handle_register(body)
        claireon_proxy.handle_deregister({
            "worktree_root": self.tmp,
            "pid": body["pid"],
            "start_time_ns": body["start_time_ns"],
        })
        resp = claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 7,
            "method": "tools/call",
            "params": {"name": "tool_search", "arguments": {"query": "x"}},
        })
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"], claireon_proxy.FALLBACK_TEXT
        )


# ---------------------------------------------------------------------------
# Idempotent double-spawn (SystemExit path).
# ---------------------------------------------------------------------------


class TestIdempotentDoubleSpawn(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-dup-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_second_invocation_exits_zero(self) -> None:
        os.makedirs(claireon_proxy.saved_dir(self.tmp), exist_ok=True)
        lock = claireon_proxy.proxy_lock_path(self.tmp)
        with open(lock, "w", encoding="utf-8") as f:
            json.dump({"pid": os.getpid(), "start_time_ns": 0}, f)
        with mock.patch.object(
            claireon_proxy, "_existing_lock_is_live", return_value=True
        ):
            with self.assertRaises(SystemExit) as cm:
                claireon_proxy.acquire_lock_or_exit(self.tmp)
            self.assertEqual(cm.exception.code, 0)


# ---------------------------------------------------------------------------
# Forwarded tool-call round-trip (live fake editor).
# ---------------------------------------------------------------------------


class TestForwardRoundTrip(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-fwd-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

        self.token = "t" * 40
        self.editor = _FakeEditor(expected_token=self.token)
        self.editor.start()
        self.addCleanup(self.editor.stop)

        body = _well_formed_register_body(
            self.tmp, editor_mcp_port=self.editor.port, token=self.token
        )
        status, resp = claireon_proxy.handle_register(body)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])

    def test_tool_call_forwarded_verbatim(self) -> None:
        payload = {
            "jsonrpc": "2.0",
            "id": 42,
            "method": "tools/call",
            "params": {"name": "tool_search", "arguments": {"query": "spawner"}},
        }
        resp = claireon_proxy.forward_tool_call(payload)
        self.assertEqual(resp.get("id"), 42)
        self.assertIn("result", resp)
        self.assertEqual(
            resp["result"]["content"][0]["text"], "ok-from-editor"
        )
        # Fake editor saw the Authorization header verbatim.
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(
            self.editor.received[0]["auth"], f"Bearer {self.token}"
        )
        self.assertEqual(
            self.editor.received[0]["body"]["params"]["name"], "tool_search"
        )

    def test_unknown_tool_returns_method_not_found(self) -> None:
        resp = claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": "does_not_exist"},
        })
        self.assertIn("error", resp)
        self.assertEqual(resp["error"]["code"], -32601)

    def test_connection_refused_evicts_session_and_returns_fallback(self) -> None:
        # Stop the fake editor so the registered editor_mcp_port is no longer
        # listening. Both forward attempts should hit ConnectionRefusedError;
        # the proxy must evict the session immediately and return the
        # friendly content fallback rather than a -32000 transport error.
        self.editor.stop()
        resp = claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 99,
            "method": "tools/call",
            "params": {"name": "python_execute", "arguments": {"code": "1"}},
        })
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"], claireon_proxy.FALLBACK_TEXT
        )
        # Active session has been cleared so the next call hits the no-editor
        # branch directly (no further wait or transport error).
        self.assertIsNone(claireon_proxy.active_session)

    def test_socket_timeout_does_not_evict(self) -> None:
        """Stage 007 (D7): only clean transport-layer rejection
        (ConnectionRefused/ConnectionReset) triggers fast eviction. A
        socket.timeout is treated as a slow tool, not session death."""
        with mock.patch.object(claireon_proxy, "_forward_once",
                               side_effect=socket.timeout("slow tool")):
            resp = claireon_proxy.forward_tool_call({
                "jsonrpc": "2.0",
                "id": 100,
                "method": "tools/call",
                "params": {"name": "python_execute", "arguments": {"code": "1"}},
            })
        # -32000 transport error rather than the eviction fallback.
        self.assertIn("error", resp)
        self.assertEqual(resp["error"]["code"], -32000)
        # Session intact -- the heartbeat watchdog will decide on staleness.
        self.assertIsNotNone(claireon_proxy.active_session)

    def test_connection_reset_evicts_session(self) -> None:
        """Stage 007 (D7): ConnectionResetError qualifies as a clean
        transport-layer rejection and triggers synchronous eviction."""
        with mock.patch.object(claireon_proxy, "_forward_once",
                               side_effect=ConnectionResetError("editor RST")):
            resp = claireon_proxy.forward_tool_call({
                "jsonrpc": "2.0",
                "id": 101,
                "method": "tools/call",
                "params": {"name": "python_execute", "arguments": {"code": "1"}},
            })
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"], claireon_proxy.FALLBACK_TEXT
        )
        self.assertIsNone(claireon_proxy.active_session)


# ---------------------------------------------------------------------------
# Stage 009 D2 -- never idle-exit. Replaces the previous 24h auto-exit suite.
# ---------------------------------------------------------------------------


class TestNeverIdleExit(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-noidle-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

    def test_should_idle_exit_always_false(self) -> None:
        # D2: idle-exit is gone. Even with a huge offset and no editor,
        # should_idle_exit MUST return False.
        claireon_proxy.RUNTIME["clock_offset_seconds"] = 1e9
        self.assertFalse(claireon_proxy.should_idle_exit(0.0))
        self.assertFalse(claireon_proxy.should_idle_exit(1e6))

    def test_run_stale_session_loop_only_evicts(self) -> None:
        # The loop sleeps tick_seconds, evicts stale sessions, and never
        # sets SHUTDOWN_EVENT. The test sets the event after one tick so
        # the loop returns; if the loop had auto-exit logic, it would set
        # the event itself before our timer fires.
        previous_event = claireon_proxy.SHUTDOWN_EVENT
        replacement = threading.Event()
        with mock.patch.object(claireon_proxy, "SHUTDOWN_EVENT", replacement):
            def _trigger() -> None:
                time.sleep(0.15)
                replacement.set()

            t = threading.Thread(target=_trigger, daemon=True)
            t.start()
            claireon_proxy.run_stale_session_loop(tick_seconds=0.05)
            t.join(timeout=1.0)
        self.assertTrue(replacement.is_set())
        self.assertIs(claireon_proxy.SHUTDOWN_EVENT, previous_event)

    def test_legacy_run_idle_exit_loop_is_compat_shim(self) -> None:
        # The legacy alias must accept idle_ceiling_seconds (any value)
        # without auto-exiting. Schedule the event externally so the loop
        # returns naturally.
        replacement = threading.Event()
        with mock.patch.object(claireon_proxy, "SHUTDOWN_EVENT", replacement):
            def _trigger() -> None:
                time.sleep(0.15)
                replacement.set()

            t = threading.Thread(target=_trigger, daemon=True)
            t.start()
            claireon_proxy.run_idle_exit_loop(
                idle_ceiling_seconds=1.0, tick_seconds=0.05
            )
            t.join(timeout=1.0)
        # Event was set by the test thread; the loop did not set it.
        self.assertTrue(replacement.is_set())


# ---------------------------------------------------------------------------
# "build and launch the editor first" fallback + isError.
# ---------------------------------------------------------------------------


class TestFallbackContentResult(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-fallback-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

    def test_no_editor_registered_returns_fallback(self) -> None:
        resp = claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "python_execute", "arguments": {"code": "1"}},
        })
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"], claireon_proxy.FALLBACK_TEXT
        )

    def test_stale_session_returns_fallback(self) -> None:
        body = _well_formed_register_body(self.tmp)
        claireon_proxy.handle_register(body)
        claireon_proxy.RUNTIME["clock_offset_seconds"] = (
            float(claireon_proxy.HEARTBEAT_STALENESS_SECONDS) + 1.0
        )
        resp = claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 9,
            "method": "tools/call",
            "params": {"name": "tool_search", "arguments": {"query": "x"}},
        })
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"], claireon_proxy.FALLBACK_TEXT
        )


# ---------------------------------------------------------------------------
# Proxy meta-tool (`proxy` MCP tool) -- handled locally, never forwarded.
# ---------------------------------------------------------------------------


class TestProxyMetaTool(unittest.TestCase):
    def setUp(self) -> None:
        # Set a worktree_root so _handle_proxy_command does not error out.
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-meta-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self._prev_runtime = dict(claireon_proxy.RUNTIME)
        claireon_proxy.RUNTIME["worktree_root"] = self.tmp
        claireon_proxy.RUNTIME["proxy_version_hash"] = "DEADBEEF"
        # Ensure no leftover session from earlier tests.
        with claireon_proxy.SESSION_LOCK:
            claireon_proxy.active_session = None

    def tearDown(self) -> None:
        claireon_proxy.RUNTIME.clear()
        claireon_proxy.RUNTIME.update(self._prev_runtime)

    def _call(self, command: Optional[str] = None, sub_args: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        arguments: Dict[str, Any] = {}
        if command is not None:
            arguments["command"] = command
        if sub_args is not None:
            arguments["args"] = sub_args
        return claireon_proxy.forward_tool_call({
            "jsonrpc": "2.0",
            "id": 99,
            "method": "tools/call",
            "params": {"name": "proxy", "arguments": arguments},
        })

    def test_static_tools_list_includes_proxy(self) -> None:
        names = [t["name"] for t in claireon_proxy.STATIC_TOOLS_LIST]
        self.assertIn("proxy", names)
        self.assertIn("tool_search", names)
        self.assertIn("python_execute", names)

    def test_help_default(self) -> None:
        resp = self._call()  # no command -> help
        self.assertIn("result", resp)
        text = resp["result"]["content"][0]["text"]
        self.assertIn("subcommands", text)
        # Stage 012: `restart` removed from the help text.
        for cmd in ("help", "launch_editor", "read_log", "status"):
            self.assertIn(cmd, text)
        self.assertNotIn("Restart-ClaireonProxy", text)
        self.assertFalse(resp["result"]["isError"])

    def test_help_explicit(self) -> None:
        resp = self._call(command="help")
        self.assertIn("result", resp)
        self.assertIn("subcommands", resp["result"]["content"][0]["text"])

    def test_unknown_subcommand(self) -> None:
        resp = self._call(command="bogus")
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertIn("Unknown subcommand", resp["result"]["content"][0]["text"])

    def test_status_no_editor(self) -> None:
        resp = self._call(command="status")
        self.assertIn("result", resp)
        body = json.loads(resp["result"]["content"][0]["text"])
        self.assertEqual(body["worktree_root"], self.tmp)
        self.assertEqual(body["proxy_version_hash"], "DEADBEEF")
        self.assertIsNone(body["active_editor"])

    def test_status_with_editor(self) -> None:
        with claireon_proxy.SESSION_LOCK:
            claireon_proxy.active_session = {
                "pid": 12345,
                "worktree_root": self.tmp,
                "start_time_ns": "9876543210",
                "build_id": "UE5-CL-0-Development",
                "proxy_version": "DEADBEEF",
                "editor_mcp_port": 8281,
                "editor_mcp_token": "x" * 32,
                "last_heartbeat_ts": claireon_proxy._mono_now(),
                "forward_conn": None,
            }
        try:
            resp = self._call(command="status")
            body = json.loads(resp["result"]["content"][0]["text"])
            self.assertEqual(body["active_editor"]["editor_pid"], 12345)
            self.assertEqual(body["active_editor"]["editor_start_time_ns"], "9876543210")
            self.assertEqual(body["active_editor"]["editor_mcp_port"], 8281)
            self.assertGreaterEqual(body["active_editor"]["last_heartbeat_age_seconds"], 0.0)
        finally:
            with claireon_proxy.SESSION_LOCK:
                claireon_proxy.active_session = None

    def test_read_log_missing_file(self) -> None:
        resp = self._call(command="read_log")
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertIn("not found", resp["result"]["content"][0]["text"])

    def test_read_log_returns_tail(self) -> None:
        saved = os.path.join(self.tmp, "Saved", "Claireon")
        os.makedirs(saved, exist_ok=True)
        log_path = os.path.join(saved, "proxy.log")
        with open(log_path, "w", encoding="utf-8") as fh:
            for i in range(500):
                fh.write(f"line-{i:03d}\n")
        resp = self._call(command="read_log", sub_args={"lines": 5})
        self.assertIn("result", resp)
        self.assertFalse(resp["result"]["isError"])
        text = resp["result"]["content"][0]["text"]
        self.assertIn("line-499\n", text)
        self.assertIn("line-495\n", text)
        self.assertNotIn("line-494", text)

    def test_proxy_not_forwarded_to_editor(self) -> None:
        # Even with an active session, `proxy` must be handled locally.
        with claireon_proxy.SESSION_LOCK:
            claireon_proxy.active_session = {
                "pid": 1,
                "worktree_root": self.tmp,
                "start_time_ns": "1",
                "build_id": "b",
                "proxy_version": "v",
                "editor_mcp_port": 9999,  # unbound; would fail if forwarded
                "editor_mcp_token": "x" * 32,
                "last_heartbeat_ts": claireon_proxy._mono_now(),
                "forward_conn": None,
            }
        try:
            resp = self._call(command="help")
            self.assertIn("result", resp)
            self.assertIn("subcommands", resp["result"]["content"][0]["text"])
        finally:
            with claireon_proxy.SESSION_LOCK:
                claireon_proxy.active_session = None


# ---------------------------------------------------------------------------
# Forwarded MCP methods (prompts/* and resources/*) round-trip + fallback.
# ---------------------------------------------------------------------------


class TestForwardedMcpMethods(unittest.TestCase):
    """Round-trip tests for prompts/* and resources/* methods that the
    proxy forwards to the editor. Mirrors TestForwardRoundTrip's setup
    pattern (fake editor + register) so the same fixture covers all
    five methods plus the unknown-method and no-editor fallbacks."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-fwdmcp-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

        self.token = "t" * 40
        self.editor = _FakeEditor(expected_token=self.token)
        self.editor.start()
        self.addCleanup(self.editor.stop)

        body = _well_formed_register_body(
            self.tmp, editor_mcp_port=self.editor.port, token=self.token
        )
        status, resp = claireon_proxy.handle_register(body)
        self.assertEqual(status, 200)
        self.assertTrue(resp["accepted"])

    # round-trip tests below dispatch via _handle_mcp_payload (NOT
    # forward_tool_call) because the new methods come in as their own
    # JSON-RPC method strings, not as tools/call params.

    def test_prompts_list_forwarded(self) -> None:
        self.editor.response_payload = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {"prompts": [{"name": "example", "description": "..."}]},
        }

        payload = {
            "jsonrpc": "2.0",
            "id": 42,
            "method": "prompts/list",
            "params": {},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 42)
        self.assertEqual(
            resp["result"],
            {"prompts": [{"name": "example", "description": "..."}]},
        )
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(self.editor.received[0]["body"]["method"], "prompts/list")
        self.assertEqual(self.editor.received[0]["auth"], f"Bearer {self.token}")

    def test_prompts_get_forwarded(self) -> None:
        self.editor.response_payload = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {
                "messages": [
                    {"role": "user", "content": {"type": "text", "text": "hello"}}
                ]
            },
        }

        payload = {
            "jsonrpc": "2.0",
            "id": 43,
            "method": "prompts/get",
            "params": {"name": "example"},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 43)
        self.assertIn("result", resp)
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(self.editor.received[0]["body"]["method"], "prompts/get")
        self.assertEqual(self.editor.received[0]["auth"], f"Bearer {self.token}")
        self.assertEqual(
            self.editor.received[0]["body"]["params"]["name"], "example"
        )

    def test_resources_list_forwarded(self) -> None:
        self.editor.response_payload = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {"resources": []},
        }

        payload = {
            "jsonrpc": "2.0",
            "id": 44,
            "method": "resources/list",
            "params": {},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 44)
        self.assertEqual(resp["result"], {"resources": []})
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(self.editor.received[0]["body"]["method"], "resources/list")
        self.assertEqual(self.editor.received[0]["auth"], f"Bearer {self.token}")

    def test_resources_read_forwarded(self) -> None:
        self.editor.response_payload = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {
                "contents": [
                    {
                        "uri": "claireon://prompts/example",
                        "mimeType": "text/plain",
                        "text": "ok",
                    }
                ]
            },
        }

        payload = {
            "jsonrpc": "2.0",
            "id": 45,
            "method": "resources/read",
            "params": {"uri": "claireon://prompts/example"},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 45)
        self.assertIn("result", resp)
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(self.editor.received[0]["body"]["method"], "resources/read")
        self.assertEqual(self.editor.received[0]["auth"], f"Bearer {self.token}")
        self.assertEqual(
            self.editor.received[0]["body"]["params"]["uri"],
            "claireon://prompts/example",
        )

    def test_resource_templates_list_forwarded(self) -> None:
        self.editor.response_payload = {
            "jsonrpc": "2.0",
            "id": 0,
            "result": {"resourceTemplates": []},
        }

        payload = {
            "jsonrpc": "2.0",
            "id": 46,
            "method": "resources/templates/list",
            "params": {},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 46)
        self.assertEqual(resp["result"], {"resourceTemplates": []})
        self.assertEqual(len(self.editor.received), 1)
        self.assertEqual(
            self.editor.received[0]["body"]["method"], "resources/templates/list"
        )
        self.assertEqual(self.editor.received[0]["auth"], f"Bearer {self.token}")

    def test_forwarded_method_no_editor_returns_fallback(self) -> None:
        # Explicitly clear the registered session so _forward_payload_to_editor
        # takes the "active_session is None" branch.
        with claireon_proxy.SESSION_LOCK:
            claireon_proxy.active_session = None

        payload = {
            "jsonrpc": "2.0",
            "id": 99,
            "method": "prompts/list",
            "params": {},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        # Same fallback shape that tools/call uses when no editor is registered.
        self.assertEqual(resp["id"], 99)
        self.assertIn("result", resp)
        self.assertTrue(resp["result"]["isError"])
        self.assertEqual(
            resp["result"]["content"][0]["text"],
            claireon_proxy.FALLBACK_TEXT,
        )

    def test_unknown_mcp_method_still_returns_method_not_found(self) -> None:
        payload = {
            "jsonrpc": "2.0",
            "id": 7,
            "method": "prompts/foo",   # NOT in FORWARDED_METHODS
            "params": {},
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp["id"], 7)
        self.assertIn("error", resp)
        self.assertEqual(resp["error"]["code"], -32601)
        self.assertIn("Method not found", resp["error"].get("message", ""))
        # Crucially: no fallback content, no -32000, no editor reach.
        self.assertNotIn("result", resp)
        self.assertEqual(len(self.editor.received), 0)


# ---------------------------------------------------------------------------
# Initialize-capability advertisement (static, unconditional).
# ---------------------------------------------------------------------------


class TestInitializeCapabilities(unittest.TestCase):
    """Static-advertisement contract for the proxy's initialize
    response. No fake editor is required because initialize is
    handled locally by the proxy."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-init-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

    def test_initialize_advertises_prompts_and_resources(self) -> None:
        payload = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": claireon_proxy.MCP_PROTOCOL_VERSION,
                "clientInfo": {"name": "test", "version": "0.0.0"},
            },
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        self.assertEqual(resp.get("id"), 1)
        self.assertIn("result", resp)
        caps = resp["result"]["capabilities"]
        self.assertEqual(caps.get("tools"),     {"listChanged": False})
        self.assertEqual(caps.get("prompts"),   {"listChanged": False})
        self.assertEqual(caps.get("resources"), {"subscribe": False, "listChanged": False})

    def test_initialize_caps_present_with_no_editor_registered(self) -> None:
        # Clear the session so capability advertisement is exercised in
        # the no-editor state. _reset_proxy_runtime in setUp has already
        # done this; the explicit lock block here documents the intent.
        with claireon_proxy.SESSION_LOCK:
            claireon_proxy.active_session = None

        payload = {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "initialize",
            "params": {
                "protocolVersion": claireon_proxy.MCP_PROTOCOL_VERSION,
                "clientInfo": {"name": "test", "version": "0.0.0"},
            },
        }
        resp = claireon_proxy._handle_mcp_payload(payload)

        # Same capability shape regardless of session state -- D1 contract.
        self.assertEqual(resp.get("id"), 2)
        caps = resp["result"]["capabilities"]
        self.assertIn("prompts",   caps)
        self.assertIn("resources", caps)
        self.assertEqual(caps["prompts"],   {"listChanged": False})
        self.assertEqual(caps["resources"], {"subscribe": False, "listChanged": False})


# ---------------------------------------------------------------------------
# PROXY_REG_PORT constant sync with ClaireonProxyConstants.h.
# ---------------------------------------------------------------------------


class TestProxyRegPortSync(unittest.TestCase):
    def _repo_root(self) -> str:
        # Content/Python -> Content -> Claireon -> Plugins -> <repo>
        return os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", ".."))

    def _constants_header_path(self) -> str:
        return os.path.join(
            self._repo_root(),
            "Plugins",
            "Claireon",
            "Source",
            "Claireon",
            "Public",
            "ClaireonProxyConstants.h",
        )

    def test_sentinel_comment_present_in_proxy_py(self) -> None:
        with open(claireon_proxy.__file__, "r", encoding="utf-8") as f:
            text = f.read()
        self.assertIn(
            "PROXY_REG_PORT_SOURCE_OF_TRUTH: ClaireonProxyConstants.h",
            text,
            "claireon_proxy.py is missing the source-of-truth sentinel comment.",
        )

    def test_reg_port_matches_header(self) -> None:
        header_path = self._constants_header_path()
        self.assertTrue(
            os.path.isfile(header_path),
            f"Constants header not found at {header_path}",
        )
        with open(header_path, "r", encoding="utf-8") as f:
            header_text = f.read()
        m = re.search(
            r"PROXY_REG_PORT\s*=\s*(\d+)", header_text
        )
        self.assertIsNotNone(m, "PROXY_REG_PORT not found in header")
        header_value = int(m.group(1))
        self.assertEqual(
            header_value,
            claireon_proxy.PROXY_REG_PORT,
            f"PROXY_REG_PORT mismatch: header={header_value} py={claireon_proxy.PROXY_REG_PORT}",
        )


# ---------------------------------------------------------------------------
# Admin endpoints (Stage 006): /admin/health, /admin/sessions, and the
# loopback header validator. Exercised in-process; ensure_worktree's bind
# path is intentionally not driven here (it would race a real port bind);
# the smoke test in Stage 008 covers it end-to-end.
# ---------------------------------------------------------------------------


class TestAdminEndpoints(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="claireon-proxy-admin-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        _reset_proxy_runtime(self.tmp)

    def test_admin_health_lists_worktrees(self) -> None:
        # Empty state.
        status, body = claireon_proxy.handle_admin_health()
        self.assertEqual(status, 200)
        self.assertEqual(body["worktrees"], [])
        self.assertEqual(body["pid"], os.getpid())
        self.assertEqual(body["version_hash"], "TESTHASH")
        # Register an editor and confirm the worktree shows up.
        reg = _well_formed_register_body(self.tmp, pid=4321, start_time_ns=42)
        claireon_proxy.handle_register(reg)
        status, body = claireon_proxy.handle_admin_health()
        self.assertEqual(status, 200)
        self.assertEqual(len(body["worktrees"]), 1)
        wt = body["worktrees"][0]
        self.assertEqual(wt["canonical_worktree"], claireon_proxy.canonicalize_worktree(self.tmp))
        self.assertEqual(wt["session"], {"editor_pid": 4321, "start_time_ns": "42"})

    def test_admin_sessions_skips_unbound_worktrees(self) -> None:
        # No sessions.
        status, body = claireon_proxy.handle_admin_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(body, {"sessions": []})
        # Register and verify.
        reg = _well_formed_register_body(self.tmp, pid=5555, start_time_ns=99)
        claireon_proxy.handle_register(reg)
        status, body = claireon_proxy.handle_admin_sessions()
        self.assertEqual(status, 200)
        self.assertEqual(len(body["sessions"]), 1)
        s = body["sessions"][0]
        self.assertEqual(s["editor_pid"], 5555)
        self.assertEqual(s["start_time_ns"], "99")
        self.assertEqual(s["build_id"], "deadbeef")

    def test_loopback_validator_rejects_remote_host(self) -> None:
        class _StubHandler:
            def __init__(self, headers: Dict[str, str]) -> None:
                self.headers = headers

        # Remote-looking Host -> reject.
        self.assertFalse(
            claireon_proxy._validate_admin_headers(_StubHandler({"Host": "example.com"}))
        )
        # Loopback -> allow.
        self.assertTrue(
            claireon_proxy._validate_admin_headers(_StubHandler({"Host": "127.0.0.1:43017"}))
        )
        # Empty headers -> allow (preserves stdlib clients).
        self.assertTrue(claireon_proxy._validate_admin_headers(_StubHandler({})))
        # Remote Origin -> reject.
        self.assertFalse(
            claireon_proxy._validate_admin_headers(
                _StubHandler({"Origin": "http://evil.example.com"})
            )
        )
        # Loopback Origin -> allow.
        self.assertTrue(
            claireon_proxy._validate_admin_headers(
                _StubHandler({"Origin": "http://localhost:43017"})
            )
        )


# ---------------------------------------------------------------------------
# Smoke entrypoint for CI.
# ---------------------------------------------------------------------------


def main() -> int:
    # Surface ERRORS from claireon_proxy's module-level logger on stderr so a
    # failing handler doesn't hide behind unittest's default capture.
    import logging

    handler = logging.StreamHandler(stream=sys.stderr)
    handler.setLevel(logging.ERROR)
    logging.getLogger("claireon_proxy").addHandler(handler)

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
