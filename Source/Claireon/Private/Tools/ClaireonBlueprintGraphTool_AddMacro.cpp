// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_AddMacro.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_AddMacro::GetOperation() const { return TEXT("add_macro"); }

TArray<FString> ClaireonBlueprintGraphTool_AddMacro::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("blueprint"), TEXT("macro"), TEXT("add"), TEXT("create"),
            TEXT("tunnel"), TEXT("graph"), TEXT("library")};
}

FString ClaireonBlueprintGraphTool_AddMacro::GetDescription() const
{
    return TEXT("Create a macro graph on the Blueprint, including its entry/exit tunnel pair, and add the "
                "requested input and output pins. The session's active graph becomes the new macro, so a "
                "following bp_add_node builds the macro body. Instantiate it elsewhere with node_type='MacroInstance'. "
                "Session-mode tool: pass session_id from bp_open, or asset_path to auto-open one.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddMacro::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("macro_name"), TEXT("Name for the new macro graph. May contain spaces."), true);
    Builder.AddArray(TEXT("inputs"), TEXT("Optional array of {name, type}. Each becomes an output pin on the macro's entry tunnel (values flow into the macro body). 'type' uses the same grammar as bp_add_variable: 'float', 'bool', 'int', 'string', 'Array<int>', a struct/class name, etc."));
    Builder.AddArray(TEXT("outputs"), TEXT("Optional array of {name, type}. Each becomes an input pin on the macro's exit tunnel (values flow out of the macro). Same 'type' grammar as inputs."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddMacro::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_macro"), Params, SessionId, Data, Error))
    {
        return Error;
    }

    UBlueprint* Blueprint = Data->Blueprint.Get();
    if (!IsValid(Blueprint))
    {
        return MakeErrorResult(TEXT("Blueprint is no longer valid"));
    }

    FString MacroName;
    if (!Params->TryGetStringField(TEXT("macro_name"), MacroName) || MacroName.IsEmpty())
    {
        return MakeErrorResult(TEXT("Missing required field: macro_name"));
    }

    // Name collision is a named error, never a silent rename -- a caller who thinks
    // they created 'Foo' must not end up with 'Foo_1'.
    for (UEdGraph* Existing : Blueprint->MacroGraphs)
    {
        if (IsValid(Existing) && Existing->GetName() == MacroName)
        {
            return MakeErrorResult(FString::Printf(
                TEXT("Macro '%s' already exists in this Blueprint. Pick a different macro_name."),
                *MacroName));
        }
    }

    // Parse every requested pin type BEFORE touching the graph, so a bad type never
    // leaves a half-built macro behind.
    struct FPendingPin
    {
        FName Name;
        FEdGraphPinType Type;
    };
    TArray<FPendingPin> PendingInputs;
    TArray<FPendingPin> PendingOutputs;

    auto ParsePinArray = [&Params](const TCHAR* FieldName, TArray<FPendingPin>& Out, FString& OutError) -> bool
    {
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (!Params->TryGetArrayField(FieldName, Array) || !Array)
        {
            return true;
        }
        int32 Index = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Array)
        {
            TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid())
            {
                OutError = FString::Printf(TEXT("%s[%d]: entry is not an object"), FieldName, Index);
                return false;
            }
            FString PinName, PinTypeString;
            if (!Entry->TryGetStringField(TEXT("name"), PinName) || PinName.IsEmpty())
            {
                OutError = FString::Printf(TEXT("%s[%d]: missing required field 'name'"), FieldName, Index);
                return false;
            }
            if (!Entry->TryGetStringField(TEXT("type"), PinTypeString) || PinTypeString.IsEmpty())
            {
                OutError = FString::Printf(TEXT("%s[%d]: missing required field 'type'"), FieldName, Index);
                return false;
            }
            ClaireonBlueprintHelpers::FParseVariableTypeResult ParseResult =
                ClaireonBlueprintHelpers::ParseVariableTypeChecked(PinTypeString);
            if (!ParseResult.bSucceeded)
            {
                OutError = FString::Printf(TEXT("%s[%d] '%s': %s"),
                    FieldName, Index, *PinName, *ParseResult.Error);
                return false;
            }
            Out.Add({FName(*PinName), ParseResult.PinType});
            ++Index;
        }
        return true;
    };

    FString ParseError;
    if (!ParsePinArray(TEXT("inputs"), PendingInputs, ParseError))
    {
        return MakeErrorResult(ParseError);
    }
    if (!ParsePinArray(TEXT("outputs"), PendingOutputs, ParseError))
    {
        return MakeErrorResult(ParseError);
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Macro")));
    Blueprint->Modify();

    UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!IsValid(MacroGraph))
    {
        return MakeErrorResult(FString::Printf(TEXT("Failed to create macro graph '%s'"), *MacroName));
    }

    // AddMacroGraph itself creates the entry/exit tunnel pair via
    // CreateMacroGraphTerminators -- do not construct UK2Node_Tunnel by hand.
    FBlueprintEditorUtils::AddMacroGraph(
        Blueprint, MacroGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

    // Entry vs exit: the entry tunnel can have outputs, the exit tunnel can have
    // inputs (same discriminator the Tunnel node_type lookup uses).
    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    for (UEdGraphNode* Node : MacroGraph->Nodes)
    {
        if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node); IsValid(Tunnel))
        {
            if (Tunnel->bCanHaveOutputs && !IsValid(EntryTunnel)) { EntryTunnel = Tunnel; }
            else if (Tunnel->bCanHaveInputs && !IsValid(ExitTunnel)) { ExitTunnel = Tunnel; }
        }
    }
    if (!IsValid(EntryTunnel) || !IsValid(ExitTunnel))
    {
        return MakeErrorResult(FString::Printf(
            TEXT("Macro graph '%s' was created but its entry/exit tunnel pair could not be located "
                 "(entry=%s, exit=%s)."),
            *MacroName,
            IsValid(EntryTunnel) ? TEXT("found") : TEXT("missing"),
            IsValid(ExitTunnel) ? TEXT("found") : TEXT("missing")));
    }

    // inputs[] become OUTPUT pins on the entry tunnel; outputs[] become INPUT pins
    // on the exit tunnel. That is the direction data actually flows through a macro.
    TArray<FString> Warnings;
    for (const FPendingPin& Pin : PendingInputs)
    {
        if (!EntryTunnel->CreateUserDefinedPin(Pin.Name, Pin.Type, EGPD_Output))
        {
            Warnings.Add(FString::Printf(TEXT("input pin '%s' could not be created on the entry tunnel"),
                *Pin.Name.ToString()));
        }
    }
    for (const FPendingPin& Pin : PendingOutputs)
    {
        if (!ExitTunnel->CreateUserDefinedPin(Pin.Name, Pin.Type, EGPD_Input))
        {
            Warnings.Add(FString::Printf(TEXT("output pin '%s' could not be created on the exit tunnel"),
                *Pin.Name.ToString()));
        }
    }

    EntryTunnel->ReconstructNode();
    ExitTunnel->ReconstructNode();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    // Land the session on the new macro so the caller can build its body immediately.
    // This is also what makes an asset_path auto-open against a graph-less
    // MacroLibrary usable without a second round trip.
    Data->Cursor.PushHistory(Data->Cursor.GraphName);
    Data->Graph = MacroGraph;
    Data->Cursor.GraphName = MacroGraph->GetName();
    Data->Cursor.FocusedNodeGuid = EntryTunnel->NodeGuid;
    Data->Cursor.FocusedPinName = NAME_None;
    for (UEdGraphPin* Pin : EntryTunnel->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            Data->Cursor.FocusedPinName = Pin->PinName;
            Data->Cursor.FocusedPinDirection = Pin->Direction;
            break;
        }
    }

    Data->LastOperationAffectedNodes.Add(EntryTunnel->NodeGuid);
    Data->LastOperationAffectedNodes.Add(ExitTunnel->NodeGuid);
    Data->Cursor.LastOperationStatus = FString::Printf(
        TEXT("Created macro '%s' (%d input(s), %d output(s)); session graph switched to '%s'."),
        *MacroName, PendingInputs.Num(), PendingOutputs.Num(), *MacroGraph->GetName());

    FToolResult Result = BuildStateResponse(SessionId, Data);
    if (Result.Data.IsValid())
    {
        Result.Data->SetStringField(TEXT("graph_name"), MacroGraph->GetName());
    }
    Result.Warnings.Append(Warnings);
    return Result;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_AddMacro::GetFullDescription() const
{
    return TEXT(
        "Creates a macro graph on the current Blueprint and returns its graph_name.\n\n"
        "The entry/exit tunnel pair is created for you (by the engine's own "
        "AddMacroGraph), and each inputs[] entry becomes an OUTPUT pin on the entry "
        "tunnel while each outputs[] entry becomes an INPUT pin on the exit tunnel -- "
        "that is the direction data flows through a macro.\n\n"
        "Pin types use the same string grammar as bp_add_variable / bp_add_function "
        "('float', 'bool', 'int', 'string', 'Array<int>', struct and class names). A "
        "type that fails to parse aborts before the graph is touched, so a rejected "
        "call leaves no half-built macro behind. A macro_name that already exists is "
        "an error rather than a silent rename.\n\n"
        "On success the session's active graph becomes the new macro, so the next "
        "bp_add_node / bp_connect_pins calls build the macro body with no bp_switch_graph "
        "in between. Use node_type='MacroInstance' with macro_name to instantiate it, and "
        "node_type='Tunnel' to find the terminators again later.\n\n"
        "Works on a MacroLibrary Blueprint (bp_create blueprint_type='MacroLibrary'), "
        "which has no EventGraph at all -- the session opens graph-less and this call "
        "gives it its first graph.");
}

FString ClaireonBlueprintGraphTool_AddMacro::GetExampleUsage() const
{
    return TEXT("bp_add_macro session_id=\"...\" macro_name=\"ApplyDamage\" "
                "inputs=[{\"name\":\"Amount\",\"type\":\"float\"}] "
                "outputs=[{\"name\":\"Applied\",\"type\":\"bool\"}]");
}

#undef LOCTEXT_NAMESPACE
