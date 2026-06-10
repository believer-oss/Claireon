// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * D3: stateless enumeration of currently-active MetaSound editing sessions.
 * Lets an agent self-recover from a stuck session id after a mid-session failure
 * without remembering ids across tool calls.
 */
class CLAIREON_API FClaireonMetaSoundTool_ListActiveSessions : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
