// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * E10: stateless StateTree health check. Returns
 * {ready_to_run, last_compiled_editor_data_hash, cold_load_valid} so callers can verify a
 * compiled / saved tree before trying to use it at runtime without scraping editor logs.
 * Distinct from claireon.statetree_status which inspects an open session; this is asset-level.
 */
class CLAIREON_API FClaireonStateTreeTool_AssetStatus : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
