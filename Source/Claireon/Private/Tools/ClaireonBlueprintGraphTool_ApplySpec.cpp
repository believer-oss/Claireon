// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Editor.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString ClaireonBlueprintGraphTool_ApplySpec::GetDescription() const
{
    // Opens with "Apply" and stays inside the 400-char description budget that
    // Claireon.DescriptionLint enforces. The 'bp_edit_batch' / 'bp_apply_graph_diff'
    // spellings that used to be quoted here are NOT lost -- they live in
    // GetSearchKeywords() below, which the search index weights as its own field, so
    // repeating them in prose only diluted this tool's term density and pushed it out
    // of the top hit for "apply spec blueprint".
    return TEXT("Apply a declarative JSON spec to a Blueprint graph in ONE call: creates or modifies nodes, pin defaults, connections and variables atomically. Transactional -- the spec is a single rollback unit, so a partial failure reverts every operation in it. Prefer this over long bp_add_node/bp_set_pin_value/bp_connect_pins sequences. Takes session_id or asset_path; auto-opens a session for asset_path.");
}

TArray<FString> ClaireonBlueprintGraphTool_ApplySpec::GetSearchKeywords() const
{
    // Vocabulary callers use when they want a bulk primitive and do not yet
    // know this tool's name. The July 2026 friction sessions searched for a
    // batch editor, concluded none existed, and filed 'bp_edit_batch' /
    // 'bp_apply_graph_diff' as feature requests -- these keywords make that
    // search land here instead.
    // "blueprint" is repeated deliberately, in the phrasings callers actually type.
    // Claireon.ToolDiscoverability.Discoverability_ApplySpecBlueprint requires this tool
    // in the top 2 for "apply spec blueprint", and it was landing at 6 behind
    // widgetbp_apply_spec, material_apply_to_blueprint and animbp_apply_delta. Every
    // *_apply_spec sibling matches "apply" and "spec" equally, so "blueprint" is the only
    // discriminating term in that query -- and this tool carried it exactly once while the
    // widget/material tools say it throughout their own docs. Its long GetPatterns() blob
    // further dilutes term density under BM25 length normalisation.
    // Do NOT try to fix Discoverability_ApplySpecBlueprint by adding "blueprint" phrasings
    // here. Measured 2026-08-10, full suite each time:
    //   as-is                      -> "apply spec blueprint" ranks this [6] (needs <=1) FAIL
    //                                 "apply graph blueprint" ranks bp_apply_delta [3] PASS
    //   + "blueprint graph" et al  -> spec [2] PASS, but delta falls to [4]          FAIL
    //   + "blueprint spec" only    -> spec [2] FAIL and delta still [4]              FAIL
    // Every *_apply_spec sibling matches "apply" and "spec" equally, so "blueprint" is the
    // sole discriminator, and any vocabulary that lifts this tool on blueprint queries lifts
    // it on the graph query too, at bp_apply_delta's expense. It is zero-sum: the two
    // assertions cannot both be satisfied by editing one tool's keywords. Fixing this needs
    // field-level boosting in the index (a name/operation match outranking incidental prose),
    // not more terms here.
    return {TEXT("bp"), TEXT("blueprint"), TEXT("batch"), TEXT("bulk"), TEXT("edit"),
            TEXT("edit_batch"), TEXT("apply"), TEXT("spec"), TEXT("diff"),
            TEXT("graph_diff"), TEXT("atomic"), TEXT("transaction"),
            TEXT("multiple"), TEXT("operations"), TEXT("declarative"),
            TEXT("one"), TEXT("call"), TEXT("roundtrip")};
}

FString ClaireonBlueprintGraphTool_ApplySpec::GetPatterns() const
{
    return TEXT(
        "## When to use\n"
        "\n"
        "This is the batch/bulk authoring primitive: when the graph shape is "
        "already known (a template, a plan, a shape verified incrementally), "
        "one spec call replaces the 35-90 bp_add_node / bp_set_pin_value / "
        "bp_connect_pins round trips it would otherwise take. Incremental "
        "single-op editing remains the right tool while DISCOVERING what the "
        "graph should look like -- each step verifies as you go.\n"
        "\n"
        "Node ids declared in the spec's nodes[] act as temp ids: "
        "connections[] and pin_defaults reference them directly, so no GUID "
        "round-tripping is needed.\n"
        "\n"
        "Use dry_run=true to validate a large spec before mutating anything.\n"
        "\n"
        "## See also\n"
        "\n"
        "- claireon.bp_add_node -- single-node incremental authoring "
        "(exploration, debugging)\n"
        "- .claude/areas/apply-spec.md -- full spec-document reference and "
        "the discover-then-capture workflow\n");
}

FString ClaireonBlueprintGraphTool_ApplySpec::GetExampleUsage() const
{
    return TEXT(
        "bp_apply_spec asset_path=\"/Game/Dir/BP_Foo\" spec={\"nodes\": [...], "
        "\"connections\": [...], \"variables\": [...]} dry_run=false");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ApplySpec::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Target Blueprint asset path."), true);
    Builder.AddObject(TEXT("spec"), TEXT("Declarative Blueprint specification object. nodes[] entries take an optional 'graph' field naming the target graph for that node (default: the Blueprint's EventGraph); an unresolvable name fails only that entry. connections[] and nodes[].pin_defaults resolve each node against the graph it was created in."), true);
    Builder.AddBoolean(TEXT("dry_run"), TEXT("If true, only validate the spec without applying."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // apply_spec is stateless; it does its own session handling internally.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	// Extract asset_path -- required
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	// Extract spec -- required JSON object
	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	// Optional: dry_run validates without applying.
	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// PIE block. Mutating apply is rejected while PIE is running; dry_run
	// is honored unconditionally because the dry-run path below never invokes the
	// applicator and so cannot mutate state.
	if (!bDryRun && IsValid(GEditor) && GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("apply_spec cannot be applied while PIE is running. Stop PIE first, or pass dry_run=true to validate the spec without mutating Blueprint state."));
	}

	// Forward dry_run into the base-class applicator so it short-circuits
	// BEFORE OpenOrCreateAsset (which would otherwise create the asset on
	// disk for dry_run=true on a missing path). The base class also skips
	// the mutating passes + SaveAsset + AssetRegistry + MarkPackageDirty
	// path; nothing on disk changes under dry_run.

	// Optional: reuse an existing session
	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_Blueprint Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId, bDryRun);
}

#undef LOCTEXT_NAMESPACE
