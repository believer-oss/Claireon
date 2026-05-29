// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: datatable_get_row
 * Get a single DataTable row as a nested JSON tree mirroring the row struct's
 * property layout. BP user-defined struct GUID suffixes are stripped, FText is
 * exploded into { text, namespace, key }, TMap is emitted as an array of
 * { key, value } pairs, and soft references are emitted as their path string
 * without forcing a load. Optional include_schema flag adds a sibling schema
 * field for callers that want the struct shape alongside the values.
 */
class ClaireonTool_DataTableGetRowStructured : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
