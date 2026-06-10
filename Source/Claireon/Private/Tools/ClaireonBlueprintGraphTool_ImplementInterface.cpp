// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_ImplementInterface.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintGraphInterfaceImpl.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_ImplementInterface::GetOperation() const { return TEXT("implement_interface"); }

FString ClaireonBlueprintGraphTool_ImplementInterface::GetDescription() const
{
    return TEXT("Alias of add_interface matching the editor's 'Implement Interface' UI label. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Adds the interface to the Blueprint's ImplementedInterfaces and compiles. Prefer bp_add_interface as the canonical name. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ImplementInterface::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("interface_class"), TEXT("Interface class path (e.g. /Script/Engine.Interactable_Interface)."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ImplementInterface::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // implement_interface is an alias of add_interface; share the same underlying handler.
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("implement_interface"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return ClaireonBlueprintGraphInterfaceImpl::ApplyAddInterface(*this, SessionId, Data, Params);
}
