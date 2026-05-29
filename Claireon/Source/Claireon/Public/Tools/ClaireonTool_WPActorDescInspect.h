// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Diagnostic tool: inspects FWorldPartitionActorDesc entries from the in-memory
 * actor descriptor list of UWorldPartition.
 *
 * Surfaces fields that are not UPROPERTY-accessible (and therefore invisible to
 * uobject_inspect), specifically:
 *   - bIsUsingDataLayerAsset  -- if false when the actor has asset-path data
 *     layers, the descriptor was saved before the FortniteSeasonBranch version
 *     gate, causing ResolveDataLayerInstanceNames to mis-treat asset paths as
 *     legacy instance names and producing an empty resolution -> DataLayersID=0
 *     -> always-loaded cell.
 *   - DataLayers (raw FName array as stored in the desc / on disk)
 *   - HasResolvedDataLayerInstanceNames + resolved instance names
 *   - Computed DataLayersID hash (CRC32 of sorted resolved instance names;
 *     0 = no-layer cell = always loaded regardless of layer state)
 *
 * Accepts a substring filter on the actor's Blueprint class name (base_class)
 * or package name. Optionally also filters by exact GUID string.
 */
class CLAIREON_API ClaireonTool_WPActorDescInspect : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("wp"); }
	FString GetOperation() const override { return TEXT("actor_desc_inspect"); }
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
