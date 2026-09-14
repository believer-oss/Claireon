// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** Notify and rebuild cached PCG editor graphs on demand; component regeneration is opt-in. */
class CLAIREON_API ClaireonPCGGraphTool_Refresh : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual bool RequiresNoPIE() const override { return true; }

	/** Upper bound on components regenerated in one call; a trim is reported, never silent. */
	static constexpr int32 MaxComponentsPerCall = 32;
};
