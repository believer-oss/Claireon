// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * D5: stateless graph dump for a UMetaSoundSource or UMetaSoundPatch. Walks the document via
 * IMetaSoundDocumentInterface so it works on both asset kinds. Returns
 * {nodes:[], edges:[], inputs:[], outputs:[], interfaces:[]}.
 */
class CLAIREON_API FClaireonMetaSoundTool_DumpGraph : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
