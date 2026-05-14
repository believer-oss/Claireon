// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Regression spec for Claireon::DeriveDefaultMcpPort (Stage 010).
// The C++ helper MUST agree byte-for-byte with claireon_proxy.py's
// derive_default_mcp_port for any given canonical worktree path; this
// spec hard-codes vectors computed independently (via .NET SHA-256)
// so any drift surfaces as a failed assertion.
//
// Vectors:
//   "C:\\test\\worktree"  -> digest[0]=152, digest[1]=243 -> port 55539
//   "W:\\yara"            -> digest[0]=181, digest[1]=130 -> port 62850
//   "/home/foo/bar"       -> digest[0]=221, digest[1]=71  -> port 56647
//
// Category: Claireon.PortDerivation.* (run via
// `Scripts\Testing\Invoke-UntestTests.ps1 -TestFilter "Claireon.PortDerivation."`).

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonPortDerivation.h"

#include "CoreMinimal.h"

namespace
{
	// File-local prefix on every helper to avoid colliding with other anon-NS
	// helpers under unity batching (MEMORY:
	// feedback_anon_namespace_unity_collision.md).
	bool PortDerivationSpec_IsInPrivateRange(uint16 Port)
	{
		return Port >= 49152 && Port <= 65535;
	}
} // namespace

// ---------------------------------------------------------------------------
// Vector 1: lowercase Windows-style path "c:\\test\\worktree".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, WindowsPathVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("C:\\test\\worktree"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 55539);
	UNTEST_ASSERT_TRUE(PortDerivationSpec_IsInPrivateRange(Port));
	co_return;
}

// ---------------------------------------------------------------------------
// Vector 2: realistic worktree path "W:\\yara".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, RealWorktreeVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("W:\\yara"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 62850);
	UNTEST_ASSERT_TRUE(PortDerivationSpec_IsInPrivateRange(Port));
	co_return;
}

// ---------------------------------------------------------------------------
// Vector 3: POSIX-style path "/home/foo/bar".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, PosixPathVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("/home/foo/bar"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 56647);
	UNTEST_ASSERT_TRUE(PortDerivationSpec_IsInPrivateRange(Port));
	co_return;
}

// ---------------------------------------------------------------------------
// Case-insensitivity: the canonical form is lowercased before hashing, so
// uppercase and mixed-case inputs MUST produce identical ports.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, CaseInsensitive, UNTEST_TIMEOUTMS(2000))
{
	const uint16 PortLower = Claireon::DeriveDefaultMcpPort(TEXT("w:\\worktree"));
	const uint16 PortUpper = Claireon::DeriveDefaultMcpPort(TEXT("W:\\WORKTREE"));
	const uint16 PortMixed = Claireon::DeriveDefaultMcpPort(TEXT("W:\\Worktree"));
	UNTEST_ASSERT_EQ(static_cast<int32>(PortLower), static_cast<int32>(PortUpper));
	UNTEST_ASSERT_EQ(static_cast<int32>(PortLower), static_cast<int32>(PortMixed));
	co_return;
}

// ---------------------------------------------------------------------------
// Determinism: repeated calls with the same input return the same port.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, Deterministic, UNTEST_TIMEOUTMS(2000))
{
	const uint16 First = Claireon::DeriveDefaultMcpPort(TEXT("W:\\yara"));
	const uint16 Second = Claireon::DeriveDefaultMcpPort(TEXT("W:\\yara"));
	const uint16 Third = Claireon::DeriveDefaultMcpPort(TEXT("W:\\yara"));
	UNTEST_ASSERT_EQ(static_cast<int32>(First), static_cast<int32>(Second));
	UNTEST_ASSERT_EQ(static_cast<int32>(First), static_cast<int32>(Third));
	co_return;
}

#endif // WITH_UNTESTED
