// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class CLAIREON_API ClaireonBlueprintGraphTool_AddPin : public ClaireonBlueprintGraphEditToolBase
{
public:
    FString GetName() const override;
    FString GetDescription() const override;
    TSharedPtr<FJsonObject> GetInputSchema() const override;
    FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
    FToolResult AddPin_Impl(
        const FString& SessionId,
        FBlueprintEditToolData* Data,
        const TSharedPtr<FJsonObject>& Params);
};
