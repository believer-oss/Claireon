// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_ListNodeTypes.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintNodeTypeRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_ListNodeTypes::GetOperation() const { return TEXT("list_node_types"); }

TArray<FString> ClaireonBlueprintGraphTool_ListNodeTypes::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("blueprint"), TEXT("list"), TEXT("node"), TEXT("types"), TEXT("node_type"),
            TEXT("aliases"), TEXT("introspect"), TEXT("discover"), TEXT("catalog")};
}

FString ClaireonBlueprintGraphTool_ListNodeTypes::GetDescription() const
{
    return TEXT("List every node_type bp_add_node accepts, as a structured array of "
                "{alias, kind, required_params, optional_params, description}. Read from the same registry "
                "bp_add_node dispatches on, so the list cannot drift from what actually works. Read-only; "
                "needs no session and takes no asset.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ListNodeTypes::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("filter"), TEXT("Optional case-insensitive substring; only aliases containing it are returned."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ListNodeTypes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    FString Filter;
    if (Arguments.IsValid())
    {
        Arguments->TryGetStringField(TEXT("filter"), Filter);
    }

    auto KindToString = [](ClaireonBlueprintNodeTypes::ENodeTypeKind Kind) -> const TCHAR*
    {
        switch (Kind)
        {
        case ClaireonBlueprintNodeTypes::ENodeTypeKind::Factory:   return TEXT("factory");
        case ClaireonBlueprintNodeTypes::ENodeTypeKind::Inline:    return TEXT("inline");
        case ClaireonBlueprintNodeTypes::ENodeTypeKind::Shorthand: return TEXT("shorthand");
        case ClaireonBlueprintNodeTypes::ENodeTypeKind::Generic:   return TEXT("generic");
        default:                                                   return TEXT("unknown");
        }
    };

    auto ToJsonStringArray = [](const TArray<FString>& In)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(In.Num());
        for (const FString& S : In)
        {
            Out.Add(MakeShared<FJsonValueString>(S));
        }
        return Out;
    };

    // DELIBERATE DIVERGENCE from the sibling *_list_node_types tools (BehaviorTree,
    // PCG, SoundCue, StateTree): those return one pre-formatted text blob (or only a
    // Summary), which is exactly the not-machine-enumerable problem this tool exists
    // to fix. node_types here is a real JSON array. Do not "align" it back to a string.
    TArray<TSharedPtr<FJsonValue>> NodeTypesArray;
    int32 TotalCount = 0;
    for (const ClaireonBlueprintNodeTypes::FNodeTypeInfo& Info : ClaireonBlueprintNodeTypes::GetRegistry())
    {
        ++TotalCount;
        const FString Alias = Info.Alias;
        if (!Filter.IsEmpty() && !Alias.Contains(Filter, ESearchCase::IgnoreCase))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("alias"), Alias);
        Entry->SetStringField(TEXT("kind"), KindToString(Info.Kind));
        Entry->SetArrayField(TEXT("required_params"), ToJsonStringArray(Info.RequiredParams));
        Entry->SetArrayField(TEXT("optional_params"), ToJsonStringArray(Info.OptionalParams));
        Entry->SetStringField(TEXT("description"), Info.Description ? Info.Description : TEXT(""));
        NodeTypesArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("node_types"), NodeTypesArray);
    Data->SetNumberField(TEXT("returned_count"), NodeTypesArray.Num());
    Data->SetNumberField(TEXT("total_count"), TotalCount);
    Data->SetStringField(TEXT("generic_escape_hatch"), TEXT(
        "Any node class not listed here is still reachable: pass node_type='Generic' with "
        "class_name='<UEdGraphNode subclass>' and, where the class needs them, node_properties. "
        "A node_type that looks like a class name (contains 'Node_' or starts with '/Script/') and "
        "resolves to a class with no alias is routed through Generic automatically, so passing "
        "'K2Node_MakeArray' or a project K2Node subclass name directly also works."));

    return MakeSuccessResult(Data, FString::Printf(
        TEXT("%d of %d bp_add_node node_type(s)."), NodeTypesArray.Num(), TotalCount));
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_ListNodeTypes::GetFullDescription() const
{
    return TEXT(
        "Enumerates bp_add_node's whole node_type space as structured data, so a caller "
        "can discover what is constructible instead of guessing from prose.\n\n"
        "Data.node_types is a JSON ARRAY (not a text blob) of objects:\n"
        "  alias           -- the string to pass as node_type\n"
        "  kind            -- 'factory' | 'inline' | 'shorthand' | 'generic'\n"
        "  required_params -- params the call is rejected without\n"
        "  optional_params -- params that change the result but may be omitted\n"
        "  description     -- one line on what the node is and any gotcha\n\n"
        "'shorthand' entries are rewritten to a MacroInstance of the engine's "
        "StandardMacros library. 'inline' entries are built by dedicated branches in "
        "bp_add_node; a few of them (Tunnel, FunctionEntry) are find-only and locate an "
        "existing node rather than creating one.\n\n"
        "Data.generic_escape_hatch documents how to reach node classes that have no "
        "alias, including the rule that a class-shaped node_type auto-routes through "
        "Generic.\n\n"
        "The table is the same one bp_add_node dispatches on, so this list cannot drift "
        "from the dispatchable surface. Optional 'filter' narrows by alias substring.");
}

FString ClaireonBlueprintGraphTool_ListNodeTypes::GetExampleUsage() const
{
    return TEXT("bp_list_node_types filter=\"Switch\"");
}

#undef LOCTEXT_NAMESPACE
