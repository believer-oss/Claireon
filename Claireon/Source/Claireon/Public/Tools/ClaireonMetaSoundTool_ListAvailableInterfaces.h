// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * D4: stateless enumeration of registered MetaSound frontend interfaces (e.g. UE.Spatialization,
 * UE.Source.Stereo). Lets an agent discover the correct name before calling add_interface.
 */
class CLAIREON_API FClaireonMetaSoundTool_ListAvailableInterfaces : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
