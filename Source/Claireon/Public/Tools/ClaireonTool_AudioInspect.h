// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the structure of audio assets (SoundCue, MetaSoundSource,
 * SoundClass, SoundMix, SoundAttenuation, SoundConcurrency). Stateless
 * read-only surface.
 */
class FClaireonTool_AudioInspect : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
