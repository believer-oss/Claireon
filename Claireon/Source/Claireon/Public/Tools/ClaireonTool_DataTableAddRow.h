// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: add_datatable_row
 * Add a new row to a data table
 */
class ClaireonTool_DataTableAddRow : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetCategory() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
