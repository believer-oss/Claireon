// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class ClaireonBlueprintGraphEditToolBase;
struct FBlueprintEditToolData;

/**
 * Shared body for the AddInterface + ImplementInterface tools. ImplementInterface
 * is a UI-label alias of AddInterface preserved for editor command-pattern parity;
 * both tools' Execute() ends in a call into this namespace's ApplyAddInterface so
 * the implementation lives in exactly one translation unit.
 */
namespace ClaireonBlueprintGraphInterfaceImpl
{
    IClaireonTool::FToolResult ApplyAddInterface(
        ClaireonBlueprintGraphEditToolBase& Tool,
        const FString& SessionId,
        FBlueprintEditToolData* Data,
        const TSharedPtr<FJsonObject>& Params);
}
