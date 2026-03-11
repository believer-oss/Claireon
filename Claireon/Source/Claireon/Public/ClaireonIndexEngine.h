// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * SQLite FTS5 + vector search engine for large output indexing.
 * Used by the output gate to store and retrieve large tool results.
 *
 * TODO: Phase 2 — SQLite FTS5 + vector search
 */
class FClaireonIndexEngine
{
public:
	FClaireonIndexEngine() = default;
	~FClaireonIndexEngine() = default;
};
