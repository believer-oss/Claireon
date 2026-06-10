// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: datatable_get_row_json
 * Get a single DataTable row as JSON in the same format used by datatable_export_json.
 * Uses FJsonObjectConverter so output is round-trippable with datatable_import_json.
 */
class ClaireonTool_DataTableGetRowJson : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
