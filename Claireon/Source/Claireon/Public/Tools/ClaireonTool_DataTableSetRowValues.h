// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: set_datatable_row
 * Set one or more property values on an existing row
 */
class ClaireonTool_DataTableSetRowValues : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
