// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class FClaireonCameraAssetTool_SetNodeProperty : public IClaireonTool
{
public:
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FString GetCategory() const override { return TEXT("camera_asset"); }
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
