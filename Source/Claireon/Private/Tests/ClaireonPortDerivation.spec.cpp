// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Regression spec for Claireon::DeriveDefaultMcpPort.
// The C++ helper MUST agree byte-for-byte with claireon_proxy.py's
// derive_default_mcp_port for any given canonical worktree path; this
// spec hard-codes vectors computed independently (via .NET SHA-256)
// so any drift surfaces as a failed assertion.
//
// Vectors:
//   "C:\\test\\worktree"  -> digest[0]=152, digest[1]=243 -> port 55539
//   "W:\\project"         -> digest[0]=8,   digest[1]=112 -> port 51312
//   "/home/foo/bar"       -> digest[0]=221, digest[1]=71  -> port 56647
//
// Category: Claireon.PortDerivation.* (run with the automation test
// filter "Claireon.PortDerivation.").

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonPortDerivation.h"

#include "CoreMinimal.h"
#include "Misc/Paths.h"

// Forward-declare the test seam defined in ClaireonModule.cpp.
extern uint32 Claireon_Test_ResolveLivePortFallback();

// Removed helper: PortDerivationSpec_IsInPrivateRange(Port), asserted after each
// vector below. DeriveDefaultMcpPort returns 49152u + (Offset % 16384u), so
// [49152, 65535] holds by construction for every possible input -- the check
// could not fail. Each vector's exact port is already pinned by the
// UNTEST_ASSERT_EQ immediately preceding it, which is strictly stronger.

// ---------------------------------------------------------------------------
// Vector 1: lowercase Windows-style path "c:\\test\\worktree".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, WindowsPathVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("C:\\test\\worktree"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 55539);
	co_return;
}

// ---------------------------------------------------------------------------
// Vector 2: short single-segment worktree path "W:\\project".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, RealWorktreeVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("W:\\project"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 51312);
	co_return;
}

// ---------------------------------------------------------------------------
// Vector 3: POSIX-style path "/home/foo/bar".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, PosixPathVector, UNTEST_TIMEOUTMS(2000))
{
	const uint16 Port = Claireon::DeriveDefaultMcpPort(TEXT("/home/foo/bar"));
	UNTEST_ASSERT_EQ(static_cast<int32>(Port), 56647);
	co_return;
}

// ---------------------------------------------------------------------------
// Case-insensitivity: the canonical form is lowercased before hashing, so
// uppercase and mixed-case inputs MUST produce identical ports.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, CaseInsensitive, UNTEST_TIMEOUTMS(2000))
{
	const uint16 PortLower = Claireon::DeriveDefaultMcpPort(TEXT("w:\\project"));
	const uint16 PortUpper = Claireon::DeriveDefaultMcpPort(TEXT("W:\\PROJECT"));
	const uint16 PortMixed = Claireon::DeriveDefaultMcpPort(TEXT("W:\\Project"));
	UNTEST_ASSERT_EQ(static_cast<int32>(PortLower), static_cast<int32>(PortUpper));
	UNTEST_ASSERT_EQ(static_cast<int32>(PortLower), static_cast<int32>(PortMixed));
	co_return;
}

// ---------------------------------------------------------------------------
// DELETED: Claireon.PortDerivation.Deterministic.
//
// It called the pure, stateless DeriveDefaultMcpPort three times on the same
// string literal and asserted the three results were equal. There is no state
// for a second call to observe, so the assertions could not fail. The exact
// output for that same input ("W:\\project" -> 51312) is already pinned by
// RealWorktreeVector above, which is strictly stronger.
// Coverage lost: none.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ResolveLivePort fallback: when no server is running and no port file
// exists on disk, must return DeriveDefaultMcpPort(ProjectDir), not 8017.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PortDerivation, ResolveLivePortFallback, UNTEST_TIMEOUTMS(2000))
{
	const uint32 Resolved = Claireon_Test_ResolveLivePortFallback();
	const FString WorktreeRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const uint32 Expected = static_cast<uint32>(Claireon::DeriveDefaultMcpPort(WorktreeRoot));
	// Must equal the SHA-derived port, not the old hardcoded 8017.
	UNTEST_ASSERT_NE(static_cast<int32>(Resolved), 8017);
	UNTEST_ASSERT_EQ(static_cast<int32>(Resolved), static_cast<int32>(Expected));
	co_return;
}

#endif // WITH_UNTESTED
