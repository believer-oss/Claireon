// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Read-only MCP tool that returns the schema of a State Tree asset:
 * the schema's UClass, its UPROPERTY values, and the Context-source
 * FStateTreeBindableStructDesc records emitted by VisitGlobalNodes.
 *
 * Stateless: never mutates and requires no open session.
 */
class ClaireonTool_StateTreeGetSchema : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
