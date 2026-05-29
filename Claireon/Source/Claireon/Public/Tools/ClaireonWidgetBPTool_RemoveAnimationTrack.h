// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonWidgetBPEditToolBase.h"

/**
 * Remove an animation track on a UWidgetAnimation in the open widget BP
 * session. Pairs with widgetbp_remove_animation_keyframe so callers can both peel
 * keys off a track and delete the track itself.
 */
class CLAIREON_API ClaireonWidgetBPTool_RemoveAnimationTrack : public ClaireonWidgetBPEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	TArray<FString> GetSearchKeywords() const override;
};
