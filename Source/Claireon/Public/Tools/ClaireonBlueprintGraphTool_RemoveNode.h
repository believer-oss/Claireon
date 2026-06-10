// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class CLAIREON_API ClaireonBlueprintGraphTool_RemoveNode : public ClaireonBlueprintGraphEditToolBase
{
public:
    FString GetOperation() const override;
    FString GetDescription() const override;
    TSharedPtr<FJsonObject> GetInputSchema() const override;
    FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
    FToolResult RemoveNode_Impl(
        const FString& SessionId,
        FBlueprintEditToolData* Data,
        const TSharedPtr<FJsonObject>& Params);
    FToolResult RemoveNodeStateless_Impl(const TSharedPtr<FJsonObject>& Params);
};
