// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

namespace Claireon
{
	/**
	 * SHA-256 of the canonicalised lowercase realpath of the worktree root,
	 * with the first two big-endian bytes folded into [49152, 65535]. This
	 * MUST agree byte-for-byte with claireon_proxy.py::derive_default_mcp_port
	 * for any given canonical path; a regression unit-test (see
	 * Tests/ClaireonPortDerivation.spec.cpp) is the canary against the
	 * editor and Python helpers drifting apart.
	 *
	 * Stage 010: the editor uses this port directly in DirectConnect mode
	 * (the proxy would otherwise own it). On EADDRINUSE the editor probes
	 * 43017 to decide whether to auto-promote into ProxyAttached mode.
	 */
	uint16 DeriveDefaultMcpPort(const FString& WorktreeRoot);
}
