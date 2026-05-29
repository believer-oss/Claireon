// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Find external-actor descriptors whose AActor::GetActorLabel() matches a query.
 * Reads ActorLabel from the AssetRegistry's tag map; falls back to walking the
 * external-actor folder when the tag isn't indexed.
 */
class CLAIREON_API ClaireonTool_AssetFindActorsByLabel : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
