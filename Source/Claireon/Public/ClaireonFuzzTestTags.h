// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

// Fuzz fixtures use plugin-owned tags to remain independent of project configuration.
// Use an ini search path: editor modules cannot define static native tags, and
// StartupModule may run after native tag registration has closed.
namespace ClaireonFuzzTestTags
{
	/** Register Config/Tags at module startup, including commandlets. Safe to repeat. */
	CLAIREON_API void RegisterFuzzTestTags();
}
