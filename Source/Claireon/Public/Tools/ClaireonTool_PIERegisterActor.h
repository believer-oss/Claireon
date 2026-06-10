// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Resolve an arbitrary PIE UObject path to a stable actor_id usable with
 * statetree_runtime_inspect and the rest of the pie_* tool family.
 *
 * Today only pie_get_player_pawn auto-mints an actor_id via FClaireonPIEManager.
 * Callers that need to address NPC pawns, AI controllers, or any actor not
 * reachable through a player controller have no way to obtain an id without
 * patching the manager. This tool fills that gap.
 *
 * Stateless / read-only / non-mutating: looks the actor up by path in the
 * active PIE world and registers it with FClaireonPIEManager::GetActorId.
 */
class ClaireonTool_PIERegisterActor : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
