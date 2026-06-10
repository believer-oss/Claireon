// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintGraphInterfaceImpl.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"

namespace ClaireonBlueprintGraphInterfaceImpl
{
    IClaireonTool::FToolResult ApplyAddInterface(
        ClaireonBlueprintGraphEditToolBase& Tool,
        const FString& SessionId,
        FBlueprintEditToolData* Data,
        const TSharedPtr<FJsonObject>& Params)
    {
        using FToolResult = IClaireonTool::FToolResult;

        FString InterfaceClassName;
        if (!Params->TryGetStringField(TEXT("interface_class"), InterfaceClassName) || InterfaceClassName.IsEmpty())
        {
            return IClaireonTool::MakeErrorResult(TEXT("Missing required field 'interface_class' for add_interface"));
        }

        UBlueprint* Blueprint = Data->Blueprint.Get();
        if (!Blueprint)
        {
            return IClaireonTool::MakeErrorResult(TEXT("Blueprint is no longer valid"));
        }

        ClaireonBPInterfaceAuthor::FInterfaceOpResult OpResult =
            ClaireonBPInterfaceAuthor::Implement(Blueprint, InterfaceClassName);

        if (!OpResult.bSucceeded)
        {
            return IClaireonTool::MakeErrorResult(OpResult.Error);
        }

        Data->Cursor.LastOperationStatus = FString::Printf(
            TEXT("Added %s; compiled (%d interfaces total)"),
            *OpResult.ResolvedClassName,
            OpResult.PostOpInterfaceNames.Num());

        if (!OpResult.ResolutionNote.IsEmpty())
        {
            Data->Cursor.LastOperationStatus += FString::Printf(TEXT(" [%s]"), *OpResult.ResolutionNote);
        }

        FToolResult Result = Tool.PublicBuildStateResponse(SessionId, Data);
        if (!OpResult.ResolutionNote.IsEmpty())
        {
            Result.Warnings.Add(OpResult.ResolutionNote);
        }
        return Result;
    }
}
