// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonExecTopology.h"
#include "ClaireonGraphIslands.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Editor.h"                       // GEditor, for the invariant rollback
#include "Editor/TransBuffer.h"           // UTransBuffer, to identify the pending undo
#include "Framework/Application/SlateApplication.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#if WITH_BLUEPRINT_ASSIST
#include "BlueprintAssistTabHandler.h"
#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistUtils.h"
#include "BlueprintAssistInputProcessor.h"
#include "BlueprintAssistActions/BlueprintAssistNodeActions.h"
#include "ClaireonBlueprintAssistCompat.h"
#endif
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


FString ClaireonBlueprintGraphTool_Format::GetOperation() const { return TEXT("format"); }

TArray<FString> ClaireonBlueprintGraphTool_Format::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("format"), TEXT("layout"), TEXT("arrange"), TEXT("pretty"), TEXT("graph")};
}

bool ClaireonBlueprintGraphTool_Format::GetSummaryAggregationSpec(TArray<FClaireonFieldAggregation>& OutSpec) const
{
    // Aggregate the last observation per target, then sum across distinct targets.
    OutSpec = {
        { FName(TEXT("nodes_moved")), EClaireonAggregationKind::Sum },
        { FName(TEXT("nodes_added")), EClaireonAggregationKind::Sum },
        { FName(TEXT("islands_formatted")), EClaireonAggregationKind::Sum },
        { FName(TEXT("islands_total")), EClaireonAggregationKind::Invariant },
        { FName(TEXT("format_status")), EClaireonAggregationKind::Histogram },
        { FName(TEXT("target")), EClaireonAggregationKind::DistinctCount },
    };
    return true;
}

FString ClaireonBlueprintGraphTool_Format::GetDescription() const
{
    return TEXT("Format a Blueprint graph ONE ISLAND AT A TIME, so laying out one connected component "
                "never moves or rewires another. island_guids picks which; omit for all. Reports coverage "
                "(format_status) and mutation (mutation_state) as two independent axes, plus a per-island "
                "row. Singletons and comment-spanning islands are skipped by policy. Needs the live editor "
                "window this session is bound to.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Format::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("graph_name"), TEXT("Graph to format. Omit to use the session cursor's current graph."), false);
    Builder.AddArray(TEXT("island_guids"), TEXT("Islands to format, each named by its representative GUID or by ANY member GUID (full GUID or >=8-hex prefix). Omit to request every island in the graph. Coverage is always reported over this requested set."), false);
    Builder.AddBoolean(TEXT("report_delta"), TEXT("Return the per-island report alone instead of the session state. A post-format report, not a dry run -- nothing is rolled back."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

// Per-island formatting.

#if WITH_BLUEPRINT_ASSIST
namespace ClaireonBPFormatInternal
{
    // Prefix helpers to avoid unity-build collisions.

    // Coverage is measured over requested islands, including policy exclusions.
    const TCHAR* FMT_CoverageComplete()      { return TEXT("complete"); }
    const TCHAR* FMT_CoveragePartial()       { return TEXT("partial"); }
    const TCHAR* FMT_CoverageNoneCompleted() { return TEXT("none_completed"); }
    const TCHAR* FMT_CoverageAllExcluded()   { return TEXT("all_targets_excluded"); }

    // formatted reports coverage; invariant failures use the mutation axis.
    const TCHAR* FMT_OutcomeFormatted()    { return TEXT("formatted"); }
    const TCHAR* FMT_OutcomeSkipped()      { return TEXT("skipped_by_policy"); }
    const TCHAR* FMT_OutcomeFailed()       { return TEXT("failed"); }
    const TCHAR* FMT_OutcomeNotAttempted() { return TEXT("not_attempted"); }

    // Why policy excluded an island, decided before it was mutated.
    const TCHAR* FMT_PolicySingleton()       { return TEXT("singleton"); }
    const TCHAR* FMT_PolicySpanningComment() { return TEXT("spanning_comment"); }
    const TCHAR* FMT_PolicyNoEligible()      { return TEXT("no_eligible_member"); }

    // Use the shared selection-refusal vocabulary.
    const TCHAR* FMT_ReasonNoWindow()    { return TEXT("no_editor_window"); }
    const TCHAR* FMT_ReasonBadArgument() { return TEXT("bad_argument"); }
    const TCHAR* FMT_ReasonNoIslands()   { return TEXT("no_islands"); }

    // Selection restoration is presentation and never changes the primary result.
    const TCHAR* FMT_RestoreOk()      { return TEXT("restored"); }
    const TCHAR* FMT_RestorePartial() { return TEXT("restored_partial"); }
    const TCHAR* FMT_RestoreFailed()  { return TEXT("restore_failed"); }

    FString FMT_Guid(const FGuid& Value)
    {
        return Value.ToString(EGuidFormats::DigitsWithHyphens);
    }

    FToolResult FMT_Refuse(const FString& Message, const TCHAR* Reason,
        TSharedPtr<FJsonObject> Evidence = nullptr)
    {
        FToolResult Result = IClaireonTool::MakeErrorResult(Message);
        Result.Data = Evidence.IsValid() ? Evidence : MakeShared<FJsonObject>();
        Result.Data->SetStringField(TEXT("refusal_reason"), Reason);
        return Result;
    }

    /**
     * Match BA eligibility: valid reachable graph nodes, excluding comments and knots.
     * An island that enqueues nothing must not be reported as formatted.
     */
    bool FMT_IsDispatchable(UEdGraphNode* Node)
    {
        return IsValid(Node)
            && FBAUtils::IsGraphNode(Node)
            && !FBAUtils::IsCommentNode(Node)
            && !FBAUtils::IsKnotNode(Node);
    }

    /** One island as the plan saw it, plus what execution then did to it. */
    struct FFmtIsland
    {
        /** Keep plan-time identity because BA can replace knots and change the lowest member GUID. */
        FString Representative;

        TArray<FString> MemberGuids;

        /** Members BA will actually enqueue, held weakly: BA deletes knots as it runs. */
        TArray<TWeakObjectPtr<UEdGraphNode>> Dispatchable;

        /** Non-knot members, the restriction set both post-mutation invariants use. */
        TSet<FGuid> NonKnotMembers;

        /** Every plan-time member, for the change-data intersection check. */
        TSet<FGuid> AllMembers;

        bool bRequested = false;

        const TCHAR* Outcome = nullptr;

        FString PolicyReason;
        FString SpanningCommentGuid;

        /** Set when this island's invariant failed AND its transaction was reversed. */
        bool bRolledBack = false;

        /** Always populated once a rollback was attempted -- including when it declined, and why. */
        FString RollbackDetail;
        TArray<FString> SpannedIslands;

        FString FailureDetail;
        FString FailedPhase;

        bool bInvariantViolated = false;
        FString InvariantDetail;
    };

    /** Compare edges within the non-knot member set through knot-transparent semantic keys. */
    TArray<FString> FMT_SemanticKeys(const UEdGraph* Graph, const TSet<FGuid>& Members)
    {
        TArray<FString> Keys;
        for (const FClaireonCollapsedEdge& Edge : ClaireonExecTopology::CollapseEdges(Graph, /*bIncludeDirect=*/true))
        {
            if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode) || !Edge.FromPin || !Edge.ToPin)
            {
                continue;
            }
            if (!Members.Contains(Edge.FromNode->NodeGuid) || !Members.Contains(Edge.ToNode->NodeGuid))
            {
                continue;
            }
            Keys.Add(FString::Printf(TEXT("%s.%s->%s.%s"),
                *FMT_Guid(Edge.FromNode->NodeGuid), *Edge.FromPin->PinName.ToString(),
                *FMT_Guid(Edge.ToNode->NodeGuid), *Edge.ToPin->PinName.ToString()));
        }
        Keys.Sort();
        return Keys;
    }

    /**
     * Title of the transaction bp_format opens around each island. BlueprintAssist's own
     * 'Format Only Selected Nodes' (and its 'Break Pin Links') transactions nest inside it, so
     * one undo reverses the whole island. The outer transaction exists because BlueprintAssist
     * rewires a reroute's neighbours (FBAUtils::DisconnectKnotNode) BEFORE it calls Modify() on
     * them; recorded on its own, an undo restores the two ends of a link from different moments
     * and leaves one-directional LinkedTo entries. Modify()-ing every member up front snapshots
     * the true pre-format state.
     */
    const TCHAR* FMT_IslandTransactionTitle() { return TEXT("Claireon bp_format island"); }

    /** LinkedTo entries whose partner does not link back. Non-zero means the graph is corrupt. */
    int32 FMT_AsymmetricLinkCount(const UEdGraph* Graph)
    {
        int32 Count = 0;
        if (!IsValid(Graph))
        {
            return Count;
        }
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!IsValid(Node))
            {
                continue;
            }
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                for (const UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    if (!Linked || !Linked->LinkedTo.Contains(Pin))
                    {
                        ++Count;
                    }
                }
            }
        }
        return Count;
    }

    /** Title and id of the transaction the NEXT undo would reverse, or false if there is none. */
    bool FMT_PeekPendingUndo(FString& OutTitle, FString& OutId)
    {
        OutTitle.Reset();
        OutId.Reset();

        const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
        if (!IsValid(TransBuffer))
        {
            return false;
        }

        // Undo targets the entry immediately before the undone tail.
        const int32 PendingIndex = TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount() - 1;
        if (PendingIndex < 0 || PendingIndex >= TransBuffer->GetQueueLength())
        {
            return false;
        }
        if (const FTransaction* Transaction = TransBuffer->GetTransaction(PendingIndex))
        {
            OutTitle = Transaction->GetTitle().ToString();
            OutId = Transaction->GetId().ToString(EGuidFormats::DigitsWithHyphens);
            return true;
        }
        return false;
    }

    /** Transaction id at the current undo head, or invalid when nothing is undoable. */
    FGuid FMT_UndoHeadTransactionId()
    {
        const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
        if (!IsValid(TransBuffer))
        {
            return FGuid();
        }
        const int32 HeadIndex = TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount() - 1;
        if (HeadIndex < 0 || HeadIndex >= TransBuffer->GetQueueLength())
        {
            return FGuid();
        }
        const FTransaction* Transaction = TransBuffer->GetTransaction(HeadIndex);
        return Transaction ? Transaction->GetId() : FGuid();
    }

    /**
     * Count standing transactions above the pre-call head, including BA link transactions.
     * If that head is lost, return a best-effort ceiling with bOutExact=false.
     */
    int32 FMT_CountStandingTransactionsAbove(const FGuid& PreCallHeadId, bool& bOutExact)
    {
        bOutExact = false;
        const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
        if (!IsValid(TransBuffer))
        {
            return 0;
        }
        const int32 StandingEnd = TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount();
        if (!PreCallHeadId.IsValid())
        {
            bOutExact = true;
            return FMath::Max(0, StandingEnd);
        }
        for (int32 Index = 0; Index < TransBuffer->GetQueueLength(); ++Index)
        {
            const FTransaction* Transaction = TransBuffer->GetTransaction(Index);
            if (Transaction && Transaction->GetId() == PreCallHeadId)
            {
                bOutExact = true;
                return FMath::Max(0, StandingEnd - (Index + 1));
            }
        }
        return FMath::Max(0, StandingEnd);
    }

    /** Undo one island only after validating the top transaction. Return whether undo ran and explain every outcome. */
    bool FMT_RollBackIslandTransaction(const UEdGraph* Graph, FString& OutDetail)
    {
        FString Title;
        FString Id;
        if (!FMT_PeekPendingUndo(Title, Id))
        {
            OutDetail = TEXT("no undoable transaction was on the stack, so there was nothing to reverse");
            return false;
        }
        if (!Title.Equals(FMT_IslandTransactionTitle(), ESearchCase::CaseSensitive))
        {
            OutDetail = FString::Printf(
                TEXT("declined: the pending undo is '%s', not this call's '%s'. Reversing it "
                     "would have undone something this call did not do."),
                *Title, FMT_IslandTransactionTitle());
            return false;
        }

        FString BlockedReason;
        FText Why;
        if (!IsValid(GEditor) || !IsValid(GEditor->Trans) || !GEditor->Trans->CanUndo(&Why))
        {
            OutDetail = FString::Printf(TEXT("declined: the transactor refused an undo (%s)"),
                Why.IsEmpty() ? TEXT("no reason given") : *Why.ToString());
            return false;
        }

        if (!GEditor->UndoTransaction())
        {
            OutDetail = FString::Printf(
                TEXT("attempted and FAILED: UndoTransaction() returned false for '%s' (id %s). The "
                     "mutation is still in place."),
                *Title, *Id);
            return false;
        }

        // A half-recorded undo is worse than a retained format: it leaves links the compiler
        // cannot resolve. Return to the consistent formatted state and report the retention.
        const int32 Asymmetric = FMT_AsymmetricLinkCount(Graph);
        if (Asymmetric > 0)
        {
            const bool bRedone = GEditor->RedoTransaction();
            OutDetail = FString::Printf(
                TEXT("undid '%s' (id %s) but %d pin link(s) came back one-directional, so the undo "
                     "was %s and the formatted mutation is retained. A one-directional link is a "
                     "corrupt graph (internal compiler error); do not save until the undo stack "
                     "is inspected."),
                *Title, *Id, Asymmetric,
                bRedone ? TEXT("redone") : TEXT("NOT redone (RedoTransaction() failed)"));
            return false;
        }

        OutDetail = FString::Printf(TEXT("rolled back '%s' (id %s)"), *Title, *Id);
        return true;
    }

    /** Track comment containment separately from topology, using non-knot members. */
    TArray<FString> FMT_ContainmentKeys(const UEdGraph* Graph, const TSet<FGuid>& Members)
    {
        TArray<FString> Keys;
        if (!IsValid(Graph))
        {
            return Keys;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
            if (!IsValid(Comment))
            {
                continue;
            }

            // Compare declared containment and live anchor geometry; widget-maintained membership can be stale.
            const FIntRect Bounds(
                Comment->NodePosX,
                Comment->NodePosY,
                Comment->NodePosX + Comment->NodeWidth,
                Comment->NodePosY + Comment->NodeHeight);

            TArray<FString> Declared;
            for (const UObject* Under : Comment->GetNodesUnderComment())
            {
                const UEdGraphNode* UnderNode = Cast<UEdGraphNode>(Under);
                if (IsValid(UnderNode) && Members.Contains(UnderNode->NodeGuid))
                {
                    Declared.Add(FMT_Guid(UnderNode->NodeGuid));
                }
            }

            TArray<FString> Geometric;
            for (const UEdGraphNode* Candidate : Graph->Nodes)
            {
                if (!IsValid(Candidate) || !Members.Contains(Candidate->NodeGuid))
                {
                    continue;
                }
                if (Bounds.Contains(FIntPoint(Candidate->NodePosX, Candidate->NodePosY)))
                {
                    Geometric.Add(FMT_Guid(Candidate->NodeGuid));
                }
            }

            // Key each comment/member pair so added containment is allowed while losses are detected.
            for (const FString& Member : Declared)
            {
                Keys.Add(FString::Printf(TEXT("decl:%s|%s"), *FMT_Guid(Comment->NodeGuid), *Member));
            }
            for (const FString& Member : Geometric)
            {
                Keys.Add(FString::Printf(TEXT("geom:%s|%s"), *FMT_Guid(Comment->NodeGuid), *Member));
            }
        }
        Keys.Sort();
        return Keys;
    }

    /** Every containment that held before, still holds. Additions are allowed. */
    bool FMT_ContainmentPreserved(const TArray<FString>& Before, const TArray<FString>& After,
        FString& OutFirstLost)
    {
        const TSet<FString> AfterSet(After);
        for (const FString& Key : Before)
        {
            if (!AfterSet.Contains(Key))
            {
                OutFirstLost = Key;
                return false;
            }
        }
        return true;
    }

    /** Unbind on every return because the handler outlives stack captures. */
    struct FFmtPostFormattingSubscription
    {
        TSharedPtr<FBAGraphHandler> Handler;
        FDelegateHandle Handle;

        ~FFmtPostFormattingSubscription()
        {
            if (Handler.IsValid() && Handle.IsValid())
            {
                ClaireonBA::OnPostFormatting(*Handler).Remove(Handle);
            }
        }
    };

    /** The selection as node GUIDs, read through the widget the dispatch will use. */
    TArray<FGuid> FMT_ReadSelection(const TSharedPtr<SGraphEditor>& Widget, const UEdGraph* Graph)
    {
        TArray<FGuid> Out;
        if (!Widget.IsValid())
        {
            return Out;
        }
        for (UObject* Selected : Widget->GetSelectedNodes())
        {
            const UEdGraphNode* Node = Cast<UEdGraphNode>(Selected);
            if (IsValid(Node) && Node->GetGraph() == Graph)
            {
                Out.Add(Node->NodeGuid);
            }
        }
        Out.Sort([](const FGuid& A, const FGuid& B) { return FMT_Guid(A) < FMT_Guid(B); });
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> FMT_StringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }
}

using namespace ClaireonBPFormatInternal;
#endif // WITH_BLUEPRINT_ASSIST

FToolResult ClaireonBlueprintGraphTool_Format::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("format"), Params, SessionId, Data, Error))
    {
        return Error;
    }

    UBlueprint* Blueprint = Data->Blueprint.Get();
    UEdGraph* Graph = Data->Graph.Get();

    if (!IsValid(Blueprint) || !IsValid(Graph))
    {
        return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
    }

#if !WITH_BLUEPRINT_ASSIST
    // Without BA, refuse before compiling or executing BA-specific code.
    return MakeErrorResult(TEXT(
        "bp_format requires the BlueprintAssist plugin, which is not present in this build. "
        "There is deliberately no fallback layout: the previous built-in layout positioned exec "
        "nodes only and left data nodes untouched, producing worse graphs than doing nothing."));
#else
    FString RequestedGraphName;
    if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_name"), RequestedGraphName)
        && !RequestedGraphName.IsEmpty()
        && !Graph->GetName().Equals(RequestedGraphName, ESearchCase::IgnoreCase))
    {
        TArray<UEdGraph*> AllGraphs;
        Blueprint->GetAllGraphs(AllGraphs);
        UEdGraph* Found = nullptr;
        for (UEdGraph* Candidate : AllGraphs)
        {
            if (IsValid(Candidate) && Candidate->GetName().Equals(RequestedGraphName, ESearchCase::IgnoreCase))
            {
                Found = Candidate;
                break;
            }
        }
        if (!IsValid(Found))
        {
            return MakeErrorResult(FString::Printf(
                TEXT("No graph named '%s' on this Blueprint."), *RequestedGraphName));
        }
        Graph = Found;
    }

    bool bReportDelta = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("report_delta"), bReportDelta);
    }

    // Write result state as top-level scalars for spill survival. Formatting never sets compiler status.
    FClaireonBPMutationResult Envelope;
    Envelope.AssetPath = Blueprint->GetPathName();
    Envelope.SessionId = SessionId;
    Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;
    Envelope.bRollbackGroupSafe = false;
    Envelope.bUndoRecordAvailable = false;

    // No dispatch has occurred, so early refusals require no snapshot.
    auto RefusedByConstruction = [&Envelope](const FString& Message, const TCHAR* Reason,
        TSharedPtr<FJsonObject> Evidence = nullptr) -> FToolResult
    {
        Envelope.MutationState = EClaireonMutationState::Refused;
        Envelope.bMutationRetained = false;
        FToolResult Result = FMT_Refuse(Message, Reason, MoveTemp(Evidence));
        Envelope.WriteInlineScalars(*Result.Data);
        return Result;
    };

    // Validate island_guids presence and type before Slate checks; invalid input must not widen to all islands.
    TArray<FString> RequestedIslandArgs;
    if (Params.IsValid())
    {
        const TSharedPtr<FJsonValue>* Field = Params->Values.Find(TEXT("island_guids"));
        if (Field && Field->IsValid() && (*Field)->Type != EJson::Null)
        {
            if ((*Field)->Type != EJson::Array)
            {
                return RefusedByConstruction(
                    TEXT("island_guids must be an array of GUID strings. It was supplied with "
                         "another type, and an ignored island_guids would have formatted every "
                         "island in the graph. Nothing was formatted."),
                    FMT_ReasonBadArgument());
            }

            for (const TSharedPtr<FJsonValue>& Value : (*Field)->AsArray())
            {
                // Check JSON type directly because TryGetString coerces numbers.
                if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
                {
                    return RefusedByConstruction(
                        TEXT("island_guids must contain only non-empty GUID strings. Nothing was formatted."),
                        FMT_ReasonBadArgument());
                }
                RequestedIslandArgs.Add(Value->AsString());
            }

            // An explicit empty array is invalid; omit the field to request every island.
            if (RequestedIslandArgs.Num() == 0)
            {
                return RefusedByConstruction(
                    TEXT("island_guids was supplied as an empty array, which names no island. "
                         "Omit the field to format every island. Nothing was formatted."),
                    FMT_ReasonBadArgument());
            }
        }
    }

    if (!FSlateApplication::IsInitialized())
    {
        return MakeErrorResult(TEXT(
            "bp_format requires an interactive editor. BlueprintAssist's formatter needs live Slate "
            "widgets to measure node sizes, so it cannot run headless or in a commandlet."));
    }

    // Use one bound widget for borrowing selection, dispatch, and restoration.
    EClaireonEditorBindingStatus BindingStatus = EClaireonEditorBindingStatus::NotBound;
    TSharedPtr<SGraphEditor> BoundWidget = Data->EditorBinding.ResolveGraphEditor(Graph, BindingStatus);
    if (!BoundWidget.IsValid())
    {
        TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
        Evidence->SetStringField(TEXT("binding"), ClaireonEditorBindingStatusToWireString(BindingStatus));

        const bool bNeverBound = BindingStatus == EClaireonEditorBindingStatus::NotBound;
        TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
        if (bNeverBound)
        {
            HintArgs->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        }
        else
        {
            HintArgs->SetStringField(TEXT("session_id"), SessionId);
        }

        FToolResult Result = RefusedByConstruction(FString::Printf(
            TEXT("bp_format needs the Blueprint editor window this session is bound to, and cannot "
                 "reach it (%s). Per-island formatting drives the editor's own selection, so it is "
                 "Slate state all the way down -- there is no headless path, and reporting a format "
                 "here would claim an effect that did not happen."),
            ClaireonEditorBindingStatusToWireString(BindingStatus)),
            FMT_ReasonNoWindow(), Evidence);
        Result.AddHint(bNeverBound
            ? MakeGuidanceHint(TEXT("bp_open"),
                TEXT("bp_open on this asset opens its editor window and binds the session to it."),
                HintArgs)
            : MakeGuidanceHint(TEXT("bp_close"),
                TEXT("close this session, then bp_open the asset again -- a session bound to a dead "
                     "editor cannot be repaired by opening a window."),
                HintArgs));
        return Result;
    }

    // Capture selection before tab activation, which can clear it. Slate selection is not restored by undo.
    const TArray<FGuid> PreCallSelection = FMT_ReadSelection(BoundWidget, Graph);

    TMap<FGuid, FIntPoint> PositionsBefore;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (IsValid(Node))
        {
            PositionsBefore.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
        }
    }

    // Focus the requested graph; handler acquisition below determines whether the editor is ready.
    FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Graph);

    // Activate the graph tab through Slate so BA notices it without OS window focus.
    FString TabDiagnostics = TEXT("target tab: not searched");
    bool bTabActivated = false;

    // Retain the tab for explicit re-enqueue: SetActiveTab broadcasts only on change,
    // and BA can stop a batch after another tab acquires a handler.
    TSharedPtr<SDockTab> ActivatedTargetTab;

    // Retry tab discovery while Slate finishes building the UI.
    auto TryActivateTargetTab = [&]()
    {
        if (bTabActivated)
        {
            return;
        }

        // Search background as well as interactive windows using BA-supported widget types.
        TSharedPtr<SDockTab> TargetTab;
        int32 WindowsVisited = 0;
        int32 DockTabsSeen = 0;
        int32 GraphEditorsSeen = 0;
        TArray<FString> OtherGraphs;

        TFunction<void(TSharedPtr<SWidget>)> Visit;
        Visit = [&](TSharedPtr<SWidget> Widget)
        {
            if (TargetTab.IsValid() || !Widget.IsValid())
            {
                return;
            }

            if (TSharedPtr<SDockTab> AsTab = CAST_SLATE_WIDGET(Widget, SDockTab))
            {
                ++DockTabsSeen;
                TSharedPtr<SGraphEditor> TabGraphEditor = FBAUtils::GetChildWidgetByTypesCasted<SGraphEditor>(
                    AsTab->GetContent(), UBASettings::Get().SupportedGraphEditors);
                if (TabGraphEditor.IsValid())
                {
                    ++GraphEditorsSeen;
                    UEdGraph* Current = TabGraphEditor->GetCurrentGraph();
                    if (Current == Graph)
                    {
                        TargetTab = AsTab;
                        return;
                    }
                    OtherGraphs.AddUnique(IsValid(Current) ? Current->GetName() : TEXT("<null>"));
                }
            }

            if (FChildren* Children = Widget->GetChildren())
            {
                for (int32 I = 0; I < Children->Num() && !TargetTab.IsValid(); ++I)
                {
                    Visit(Children->GetChildAt(I));
                }
            }
        };

        for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetTopLevelWindows())
        {
            ++WindowsVisited;
            Visit(Window);
            if (TargetTab.IsValid())
            {
                break;
            }
        }

        if (TargetTab.IsValid())
        {
            // Foreground the tab within its own tab well.
            FGlobalTabmanager::Get()->DrawAttention(TargetTab.ToSharedRef());
            // Notify BA through active-tab change.
            FGlobalTabmanager::Get()->SetActiveTab(TargetTab);
            ActivatedTargetTab = TargetTab;
            bTabActivated = true;
            TabDiagnostics = FString::Printf(
                TEXT("target tab found and activated (%d window(s), %d dock tab(s), %d graph editor(s)); foreground=%s"),
                WindowsVisited, DockTabsSeen, GraphEditorsSeen,
                TargetTab->IsForeground() ? TEXT("true") : TEXT("false"));
        }
        else
        {
            // Distinguish absent graph UI from a handler focused on another graph.
            TabDiagnostics = FString::Printf(
                TEXT("target tab NOT found (%d window(s), %d dock tab(s), %d graph editor(s); graphs seen: %s)"),
                WindowsVisited, DockTabsSeen, GraphEditorsSeen,
                OtherGraphs.Num() > 0 ? *FString::Join(OtherGraphs, TEXT(", ")) : TEXT("none"));
        }
    };

    TryActivateTargetTab();

    // Pump Slate, never the core ticker that dispatches MCP requests.
    const double PumpDeltaSeconds = 1.0 / 60.0;
    auto PumpOnce = [PumpDeltaSeconds]()
    {
        FSlateApplication::Get().Tick();

        // Ordinary timer ticks cannot repeat within one frame; ForceEditorTimerTick supplies a bounded frame-counter advance.
        if (IsValid(GEditor))
        {
            GEditor->GetTimerManager()->Tick(static_cast<float>(PumpDeltaSeconds));
        }

        FBATabHandler::Get().Tick(static_cast<float>(PumpDeltaSeconds));
    };

    // BA queues handler creation for an editor-timer next tick.
    // Bounded GFrameCounter increments allow those timers to run without pumping the core ticker.
    int32 ForcedTimerTicks = 0;
    constexpr int32 MaxForcedTimerTicks = 16;
    auto ForceEditorTimerTick = [&ForcedTimerTicks, PumpDeltaSeconds]()
    {
        if (!IsValid(GEditor) || ForcedTimerTicks >= MaxForcedTimerTicks)
        {
            return;
        }
        ++ForcedTimerTicks;
        ++GFrameCounter;
        GEditor->GetTimerManager()->Tick(static_cast<float>(PumpDeltaSeconds));
    };

    TSharedPtr<FBAGraphHandler> GraphHandler;
    const double HandlerDeadline = FPlatformTime::Seconds() + 5.0;
    while (FPlatformTime::Seconds() < HandlerDeadline)
    {
        PumpOnce();
        TryActivateTargetTab();

        // Re-enqueue the retained tab within the forced-tick budget; activation alone is edge-triggered.
        if (ActivatedTargetTab.IsValid() && ForcedTimerTicks < MaxForcedTimerTicks)
        {
            FGlobalTabmanager::Get()->DrawAttention(ActivatedTargetTab.ToSharedRef());
            FBATabHandler::Get().ProcessTab(ActivatedTargetTab);
        }

        ForceEditorTimerTick();

        GraphHandler = FBATabHandler::Get().GetActiveGraphHandler();
        if (GraphHandler.IsValid() && GraphHandler->GetFocusedEdGraph() == Graph)
        {
            break;
        }

        // Stop when the existing tab exhausts its budget; otherwise keep discovering UI until the deadline.
        if (ActivatedTargetTab.IsValid() && ForcedTimerTicks >= MaxForcedTimerTicks)
        {
            break;
        }
    }

    if (!GraphHandler.IsValid())
    {
        return RefusedByConstruction(FString::Printf(TEXT(
            "No BlueprintAssist graph handler appeared within 5s for '%s'. Diagnostics: %s. "
            "THE FIX IS ALMOST ALWAYS: make one more MCP call, then call bp_format again. "
            "MEASURED: an asset editor's Slate UI is built by work serviced on the CORE TICKER "
            "(UAssetEditorSubsystem registers its ticker there), and this tool deliberately pumps only Slate "
            "and the editor timer manager -- pumping the core ticker would let the MCP listener dispatch "
            "another tool call re-entrantly inside this one. So a graph tab that does not exist yet cannot "
            "come into existence DURING this call, no matter how long it waits; it needs a real frame, which "
            "means a call boundary. '0 graph editor(s)' above is exactly that case, and a retry after any "
            "intervening call succeeds. "
            "This call does activate the tab once it exists (DrawAttention + SetActiveTab), and needs no OS "
            "window focus to do it -- a background, unfocused editor formats fine. "
            "DO NOT create or save the Blueprint and open its editor in the same call: that trips a "
            "reentrant-load assertion (UObjectGlobals.cpp:3537) inside LoadLibrariesFromAssetRegistry and "
            "takes the editor down. Leave a call boundary between them."),
            *Blueprint->GetName(), *TabDiagnostics),
            TEXT("no_graph_handler"));
    }

    // Require the active handler to target the requested graph.
    if (GraphHandler->GetFocusedEdGraph() != Graph)
    {
        const UEdGraph* Focused = GraphHandler->GetFocusedEdGraph();
        FToolResult Result = RefusedByConstruction(FString::Printf(
            TEXT("BlueprintAssist is focused on '%s' but '%s' was requested, after this call opened "
                 "the target document, activated its tab, re-enqueued it with BlueprintAssist and "
                 "forced %d editor timer tick(s) to drive BA's deferred tab processing. Refusing to "
                 "format the wrong graph. THIS IS A HARD FAILURE, not a retry hint -- a batch of "
                 "bp_format calls should abort here rather than carry on formatting other graphs, "
                 "because a graph BA will not adopt on this call will not adopt on the next one "
                 "either. Most likely cause: other asset editor windows are competing for the "
                 "foreground. Diagnostics: %s"),
            IsValid(Focused) ? *Focused->GetName() : TEXT("<none>"), *Graph->GetName(),
            ForcedTimerTicks, *TabDiagnostics),
            TEXT("wrong_graph_focused"));

        // Offer unforced editor closure so bound sessions are identified before windows close.
        TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
        HintArgs->SetBoolField(TEXT("all"), true);
        Result.AddHint(MakeGuidanceHint(TEXT("editor_close_asset"),
            TEXT("close the competing asset editor windows, then bp_open this asset again and retry "
                 "bp_format -- BlueprintAssist follows the foreground tab, so a single open editor "
                 "removes the contention entirely."),
            HintArgs));
        return Result;
    }

    // Require BA's handler and the session binding to use the same widget.
    if (GraphHandler->GetGraphEditor() != BoundWidget)
    {
        FToolResult Result = RefusedByConstruction(FString::Printf(
            TEXT("BlueprintAssist's active graph handler is not the editor widget this session is "
                 "bound to, though both report graph '%s'. The asset is open in more than one place. "
                 "bp_format refuses rather than formatting a selection it set in a different window."),
            *Graph->GetName()),
            TEXT("editor_instance_mismatch"));

        TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
        HintArgs->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        Result.AddHint(MakeGuidanceHint(TEXT("editor_close_asset"),
            TEXT("close every window on this asset, then bp_open it once and retry bp_format -- with a "
                 "single editor instance the session binding and BlueprintAssist's handler cannot "
                 "disagree."),
            HintArgs));
        return Result;
    }

    // Wait for node-size readiness before formatting.
    const double SizeDeadline = FPlatformTime::Seconds() + 10.0;
    while (ClaireonBA::IsCalculatingNodeSize(*GraphHandler) && FPlatformTime::Seconds() < SizeDeadline)
    {
        PumpOnce();
    }
    if (ClaireonBA::IsCalculatingNodeSize(*GraphHandler))
    {
        return RefusedByConstruction(TEXT(
            "BlueprintAssist was still measuring node sizes after 10s. Formatting now would lay out "
            "against incomplete sizes, so nothing was changed. Retry once the editor is idle."),
            TEXT("node_sizes_not_ready"));
    }

    // Build the plan once before mutation; BA replaces knots, so later topology cannot reconstruct its identities.
    TArray<ClaireonGraphIslands::FIsland> RawIslands;
    ClaireonGraphIslands::Build(Graph, RawIslands);

    TArray<FFmtIsland> Plan;
    Plan.Reserve(RawIslands.Num());
    TMap<FGuid, int32> NodeToIsland;

    for (const ClaireonGraphIslands::FIsland& Raw : RawIslands)
    {
        FFmtIsland Island;
        Island.Representative = Raw.Representative;
        for (UEdGraphNode* Node : Raw.Nodes)
        {
            if (!IsValid(Node))
            {
                continue;
            }
            Island.MemberGuids.Add(FMT_Guid(Node->NodeGuid));
            Island.AllMembers.Add(Node->NodeGuid);
            NodeToIsland.Add(Node->NodeGuid, Plan.Num());
            if (!FBAUtils::IsKnotNode(Node))
            {
                Island.NonKnotMembers.Add(Node->NodeGuid);
            }
            if (FMT_IsDispatchable(Node))
            {
                Island.Dispatchable.Add(Node);
            }
        }
        Plan.Add(MoveTemp(Island));
    }

    if (RequestedIslandArgs.Num() == 0)
    {
        for (FFmtIsland& Island : Plan)
        {
            Island.bRequested = true;
        }
    }
    else
    {
        for (const FString& Arg : RequestedIslandArgs)
        {
            UEdGraphNode* Node = nullptr;
            FString ResolveError;
            ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, Arg, Node, ResolveError,
                TEXT("island_guids"));
            const int32* Index = IsValid(Node) ? NodeToIsland.Find(Node->NodeGuid) : nullptr;
            if (!Index)
            {
                return RefusedByConstruction(FString::Printf(
                    TEXT("island_guids entry '%s' names no island in '%s': %s. An island is named by "
                         "its representative GUID or by any member GUID. Nothing was formatted."),
                    *Arg, *Graph->GetName(),
                    IsValid(Node) ? TEXT("the node resolved but belongs to no island") : *ResolveError),
                    FMT_ReasonBadArgument());
            }
            Plan[*Index].bRequested = true;
        }
    }

    // Skip singletons because there is no intra-island layout to arrange.
    for (FFmtIsland& Island : Plan)
    {
        if (Island.bRequested && Island.MemberGuids.Num() == 1)
        {
            Island.Outcome = FMT_OutcomeSkipped();
            Island.PolicyReason = FMT_PolicySingleton();
        }
    }

    // Skip islands under shared comments before mutation; BA may move nodes without moving a spanning comment.
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
        if (!IsValid(Comment))
        {
            continue;
        }

        // Use the union of declared and geometric containment, matching post-format validation.
        const FIntRect CommentBounds(
            Comment->NodePosX,
            Comment->NodePosY,
            Comment->NodePosX + Comment->NodeWidth,
            Comment->NodePosY + Comment->NodeHeight);

        TSet<int32> Spanned;
        for (const UObject* Under : Comment->GetNodesUnderComment())
        {
            const UEdGraphNode* UnderNode = Cast<UEdGraphNode>(Under);
            if (!IsValid(UnderNode))
            {
                continue;
            }
            if (const int32* Index = NodeToIsland.Find(UnderNode->NodeGuid))
            {
                Spanned.Add(*Index);
            }
        }
        for (const UEdGraphNode* Candidate : Graph->Nodes)
        {
            if (!IsValid(Candidate) || Candidate->IsA<UEdGraphNode_Comment>())
            {
                continue;
            }
            if (!CommentBounds.Contains(FIntPoint(Candidate->NodePosX, Candidate->NodePosY)))
            {
                continue;
            }
            if (const int32* Index = NodeToIsland.Find(Candidate->NodeGuid))
            {
                Spanned.Add(*Index);
            }
        }
        if (Spanned.Num() < 2)
        {
            continue;
        }

        TArray<int32> SpannedSorted = Spanned.Array();
        SpannedSorted.Sort();
        TArray<FString> SpannedReps;
        for (int32 Index : SpannedSorted)
        {
            SpannedReps.Add(Plan[Index].Representative);
        }

        // Skip only the affected islands.
        for (int32 Index : SpannedSorted)
        {
            FFmtIsland& Island = Plan[Index];
            if (!Island.bRequested || Island.Outcome != nullptr)
            {
                continue;
            }
            Island.Outcome = FMT_OutcomeSkipped();
            Island.PolicyReason = FMT_PolicySpanningComment();
            Island.SpanningCommentGuid = FMT_Guid(Comment->NodeGuid);
            Island.SpannedIslands = SpannedReps;
        }
    }

    // Skip islands with no BA-eligible nodes so quiet is not mistaken for completed formatting.
    for (FFmtIsland& Island : Plan)
    {
        if (Island.bRequested && Island.Outcome == nullptr && Island.Dispatchable.Num() == 0)
        {
            Island.Outcome = FMT_OutcomeSkipped();
            Island.PolicyReason = FMT_PolicyNoEligible();
        }
    }

    // Keep ascending plan-time representative order for reproducible not_attempted results.
    TArray<int32> Eligible;
    int32 RequestedCount = 0;
    for (int32 Index = 0; Index < Plan.Num(); ++Index)
    {
        if (!Plan[Index].bRequested)
        {
            continue;
        }
        ++RequestedCount;
        if (Plan[Index].Outcome == nullptr)
        {
            Plan[Index].Outcome = FMT_OutcomeNotAttempted();
            Eligible.Add(Index);
        }
    }

    if (RequestedCount == 0)
    {
        return RefusedByConstruction(FString::Printf(
            TEXT("'%s' decomposes into no islands, so bp_format has nothing to format. An island is a "
                 "connected component over links of any category; comments are excluded because they "
                 "have no pins."),
            *Graph->GetName()),
            FMT_ReasonNoIslands());
    }

    TArray<TArray<FString>> BeforeSemantic;
    TArray<TArray<FString>> BeforeContainment;
    BeforeSemantic.SetNum(Plan.Num());
    BeforeContainment.SetNum(Plan.Num());
    for (int32 Index : Eligible)
    {
        BeforeSemantic[Index] = FMT_SemanticKeys(Graph, Plan[Index].NonKnotMembers);
        BeforeContainment[Index] = FMT_ContainmentKeys(Graph, Plan[Index].NonKnotMembers);
    }

    FClaireonBPSnapshot Before;
    ClaireonBPSnapshot::Capture(Blueprint, {Graph}, EClaireonBPSnapshotFamily::Format, Before);

    FString LastCompletedPhase = kClaireonBPPhaseNone;

    if (Eligible.Num() == 0)
    {
        LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::Preflight);
        Envelope.MutationState = EClaireonMutationState::Refused;
        Envelope.bMutationRetained = false;
        Envelope.LastCompletedPhase = LastCompletedPhase;
    }

    // Execute in plan order.

    bool bBorrowedSelection = false;

    int32 FormattedCount = 0;

    // Count observed formatter completions separately from successful settlement; failed settlement can retain transactions.
    int32 DispatchedTransactionCount = 0;

    // A dispatch without completion may still land a later transaction, making current recovery counts a floor.
    bool bUnobservedDispatchOutcome = false;

    // Track successful rollback separately from retained invariant failures.
    int32 RolledBackCount = 0;
    int32 RetainedViolationCount = 0;
    int32 FirstViolatingIsland = INDEX_NONE;
    int32 FailedIsland = INDEX_NONE;

    // Capture the pre-call undo head as the boundary for standing transactions.
    const FGuid PreCallUndoHeadId = FMT_UndoHeadTransactionId();

    for (int32 Position = 0; Position < Eligible.Num(); ++Position)
    {
        const int32 Index = Eligible[Position];
        FFmtIsland& Island = Plan[Index];

        auto FailIsland = [&](const TCHAR* Phase, const FString& Detail)
        {
            Island.Outcome = FMT_OutcomeFailed();
            Island.FailedPhase = Phase;
            Island.FailureDetail = Detail;
            FailedIsland = Index;
        };

        // Resolve surviving plan members because earlier formatting can remove knots.
        TArray<UEdGraphNode*> Live;
        for (const TWeakObjectPtr<UEdGraphNode>& Weak : Island.Dispatchable)
        {
            UEdGraphNode* Node = Weak.Get();
            if (FMT_IsDispatchable(Node))
            {
                Live.Add(Node);
            }
        }

        // Check each silent-return guard before dispatch.
        if (Live.Num() == 0)
        {
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch),
                TEXT("every member BlueprintAssist would enqueue has been destroyed since the plan "
                     "was computed, so this island cannot be dispatched."));
        }
        else if (!GraphHandler->GetGraphPanel().IsValid())
        {
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch),
                TEXT("the graph panel is no longer valid, and FBAGraphHandler::FormatNodes returns "
                     "silently in that case."));
        }
        else if (GraphHandler->GetFocusedEdGraph() != Graph)
        {
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch),
                TEXT("BlueprintAssist stopped being focused on this graph mid-run."));
        }
        else if (FBlueprintEditorUtils::IsGraphReadOnly(Graph))
        {
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch),
                TEXT("the graph is read-only, and FBAGraphHandler::FormatNodes returns silently in "
                     "that case."));
        }

        if (FailedIsland == Index)
        {
            break;
        }

        // Open the island transaction and snapshot every member before BlueprintAssist rewires
        // anything (see FMT_IslandTransactionTitle). BA's own transactions nest inside this one.
        TUniquePtr<FScopedTransaction> IslandTransaction =
            MakeUnique<FScopedTransaction>(FText::FromString(FMT_IslandTransactionTitle()));
        Graph->Modify();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (IsValid(Node) && Island.AllMembers.Contains(Node->NodeGuid))
            {
                Node->Modify();
            }
        }

        // Require completion evidence; quiet settlement alone cannot establish that formatting ran.
        GraphHandler->ClearFormatters();

        bool bPostFormattingFired = false;
        FFmtPostFormattingSubscription Subscription;
        Subscription.Handler = GraphHandler;
        Subscription.Handle = ClaireonBA::OnPostFormatting(*GraphHandler).AddLambda(
            [&bPostFormattingFired]() { bPostFormattingFired = true; });

        BoundWidget->ClearSelectionSet();
        for (UEdGraphNode* Node : Live)
        {
            BoundWidget->SetNodeSelection(Node, /*bSelect=*/true);
        }
        bBorrowedSelection = true;

        // Selective formatting reads the bound widget's selection through the verified active handler.
        ClaireonBA::FormatNodesSelectively();

        // Require completion change data to intersect plan-time membership.
        // BA populates it before broadcasting; freshly generated knots cannot satisfy the intersection.
        bool bChangeDataIntersects = false;
        const double CompletionDeadline = FPlatformTime::Seconds() + 20.0;
        while (FPlatformTime::Seconds() < CompletionDeadline)
        {
            PumpOnce();
            if (!bPostFormattingFired)
            {
                continue;
            }
            for (const TPair<FGuid, FBAFormattingChangeData>& Pair : GraphHandler->GetFormattingChangeData())
            {
                if (Island.AllMembers.Contains(Pair.Key))
                {
                    bChangeDataIntersects = true;
                    break;
                }
            }
            if (bChangeDataIntersects)
            {
                break;
            }
        }

        const bool bInjectDispatchFailure =
            CLAIREON_BP_SHOULD_INJECT_FAILURE(*ClaireonBPFormatFaultSeam::Dispatch(Position));

        // Count completion even when later change-data validation or settlement fails.
        if (bPostFormattingFired)
        {
            ++DispatchedTransactionCount;
        }
        else
        {
            bUnobservedDispatchOutcome = true;
        }

        if (!bChangeDataIntersects || bInjectDispatchFailure)
        {
            IslandTransaction.Reset();
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch),
                bInjectDispatchFailure
                    ? TEXT("dispatch failure injected by the test fault seam.")
                    : bPostFormattingFired
                        ? TEXT("BlueprintAssist finished formatting but its change data named no node of "
                               "this island, so the dispatch did not reach it.")
                        : TEXT("BlueprintAssist never signalled OnPostFormatting within 20s, so no "
                               "formatter ran for this island."));
            break;
        }
        LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::SelectiveDispatch);

        // Poll whole-graph positions because newly created reroutes are absent from the plan member set.
        const int32 RequiredStableRounds = 4;
        int32 StableRounds = 0;
        TMap<FGuid, FIntPoint> LastSeen;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (IsValid(Node))
            {
                LastSeen.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
            }
        }
        const double SettleDeadline = FPlatformTime::Seconds() + 20.0;
        while (FPlatformTime::Seconds() < SettleDeadline)
        {
            PumpOnce();

            if (ClaireonBA::IsCalculatingNodeSize(*GraphHandler))
            {
                StableRounds = 0;
                continue;
            }

            bool bChanged = false;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (!IsValid(Node))
                {
                    continue;
                }
                const FIntPoint Current(Node->NodePosX, Node->NodePosY);
                const FIntPoint* Previous = LastSeen.Find(Node->NodeGuid);
                if (!Previous || *Previous != Current)
                {
                    bChanged = true;
                    LastSeen.Add(Node->NodeGuid, Current);
                }
            }

            StableRounds = bChanged ? 0 : StableRounds + 1;
            if (StableRounds >= RequiredStableRounds)
            {
                break;
            }
        }

        // Close the island transaction before anything below may undo it.
        IslandTransaction.Reset();

        const bool bInjectSettleFailure =
            CLAIREON_BP_SHOULD_INJECT_FAILURE(*ClaireonBPFormatFaultSeam::Settle(Position));

        if (StableRounds < RequiredStableRounds || bInjectSettleFailure)
        {
            FailIsland(ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::Settle),
                bInjectSettleFailure
                    ? TEXT("settle failure injected by the test fault seam. This island IS formatted; "
                           "the injection asserts the retention contract, not a real timeout.")
                    : TEXT("node positions were still moving after 20s, so this call cannot confirm the "
                           "layout finished. This island is partially moved."));
            break;
        }
        LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::Settle);

        Island.Outcome = FMT_OutcomeFormatted();
        ++FormattedCount;

        // Operation failure stops the plan; completed formatting with an invariant failure permits later islands.
        const TArray<FString> AfterSemantic = FMT_SemanticKeys(Graph, Island.NonKnotMembers);
        const TArray<FString> AfterContainment = FMT_ContainmentKeys(Graph, Island.NonKnotMembers);

        if (AfterSemantic != BeforeSemantic[Index])
        {
            Island.bInvariantViolated = true;
            Island.InvariantDetail = FString::Printf(
                TEXT("semantic topology changed: %d knot-transparent edge(s) before, %d after"),
                BeforeSemantic[Index].Num(), AfterSemantic.Num());
        }
        else
        {
            FString LostContainment;
            if (!FMT_ContainmentPreserved(BeforeContainment[Index], AfterContainment, LostContainment))
            {
                Island.bInvariantViolated = true;
                Island.InvariantDetail = FString::Printf(
                    TEXT("comment containment was lost: '%s' held before the format and does not "
                         "hold after -- the island moved out from under a comment that held it. "
                         "The 'decl:' form is the comment's own NodesUnderComment array, the "
                         "'geom:' form is anchor-space containment against its rectangle."),
                    *LostContainment);
            }
        }

        if (!Island.bInvariantViolated
            && CLAIREON_BP_SHOULD_INJECT_FAILURE(*ClaireonBPFormatFaultSeam::InvariantValidation(Position)))
        {
            Island.bInvariantViolated = true;
            Island.InvariantDetail =
                TEXT("invariant failure injected by the test fault seam. Both real invariants held; "
                     "the injection asserts that a validation failure keeps the mutation and does "
                     "NOT stop the plan.");
        }

        // Attempt immediate invariant rollback while the transaction can be checked; retain and report work if it declines.
        if (Island.bInvariantViolated)
        {
            FString RollbackDetail;
            Island.bRolledBack = FMT_RollBackIslandTransaction(Graph, RollbackDetail);
            Island.RollbackDetail = RollbackDetail;
            if (Island.bRolledBack)
            {
                ++RolledBackCount;
            }
            else
            {
                ++RetainedViolationCount;
            }
        }

        if (Island.bInvariantViolated && FirstViolatingIsland == INDEX_NONE)
        {
            FirstViolatingIsland = Index;
        }
        LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::InvariantValidation);
    }


    // Restore surviving selection GUIDs without changing the primary result; replaced knots may be missing.
    FString SelectionRestoreStatus;
    TArray<FString> SelectionRestoreMissing;
    if (bBorrowedSelection)
    {
        if (!BoundWidget.IsValid())
        {
            SelectionRestoreStatus = FMT_RestoreFailed();
        }
        else
        {
            TMap<FGuid, UEdGraphNode*> Survivors;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (IsValid(Node))
                {
                    Survivors.Add(Node->NodeGuid, Node);
                }
            }
            BoundWidget->ClearSelectionSet();
            for (const FGuid& Guid : PreCallSelection)
            {
                if (UEdGraphNode** Found = Survivors.Find(Guid))
                {
                    BoundWidget->SetNodeSelection(*Found, /*bSelect=*/true);
                }
                else
                {
                    SelectionRestoreMissing.Add(FMT_Guid(Guid));
                }
            }
            SelectionRestoreStatus = SelectionRestoreMissing.Num() == 0
                ? FMT_RestoreOk() : FMT_RestorePartial();
        }
    }

    // Coverage and mutation outcomes.

    const TCHAR* Coverage = nullptr;
    if (Eligible.Num() == 0)
    {
        Coverage = FMT_CoverageAllExcluded();
    }
    else if (FormattedCount == RequestedCount)
    {
        Coverage = FMT_CoverageComplete();
    }
    else if (FormattedCount > 0)
    {
        Coverage = FMT_CoveragePartial();
    }
    else
    {
        Coverage = FMT_CoverageNoneCompleted();
    }

    // Derive retained transaction counts from the buffer above the pre-call head, excluding undone entries.
    bool bStandingCountExact = false;
    const int32 RetainedTransactionCount =
        FMT_CountStandingTransactionsAbove(PreCallUndoHeadId, bStandingCountExact);

    // Measure graph changes before reporting retention; untransacted position residue can survive rollback.
    int32 MovedNodes = 0;
    int32 AddedNodes = 0;
    for (const UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node))
        {
            continue;
        }
        const FIntPoint* PositionBefore = PositionsBefore.Find(Node->NodeGuid);
        if (!PositionBefore)
        {
            ++AddedNodes;
        }
        else if (*PositionBefore != FIntPoint(Node->NodePosX, Node->NodePosY))
        {
            ++MovedNodes;
        }
    }

    if (Eligible.Num() > 0)
    {
        Envelope.LastCompletedPhase = LastCompletedPhase;
        if (FailedIsland != INDEX_NONE)
        {
            Envelope.MutationState = EClaireonMutationState::AppliedOperationFailed;
            Envelope.FailedPhase = Plan[FailedIsland].FailedPhase;
            Envelope.FailedIslandGuid = Plan[FailedIsland].Representative;
        }
        else if (FirstViolatingIsland != INDEX_NONE)
        {
            Envelope.MutationState = EClaireonMutationState::AppliedValidationFailed;
            Envelope.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::InvariantValidation);
            Envelope.FailedIslandGuid = Plan[FirstViolatingIsland].Representative;
        }
        else
        {
            Envelope.MutationState = EClaireonMutationState::AppliedClean;
        }
        Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);

        // No retained mutation requires both no standing transactions and no observed graph changes.
        if (Envelope.MutationState == EClaireonMutationState::AppliedValidationFailed
            && RolledBackCount > 0 && RetainedViolationCount == 0
            && RetainedTransactionCount == 0
            && MovedNodes == 0 && AddedNodes == 0)
        {
            Envelope.bMutationRetained = false;
        }

        // Offer undo only for standing transactions, never for records already consumed by rollback.
        Envelope.bUndoRecordAvailable = RetainedTransactionCount > 0;
    }

    // Report plan-time representatives with their member GUIDs.
    TArray<TSharedPtr<FJsonValue>> IslandRows;
    for (const FFmtIsland& Island : Plan)
    {
        if (!Island.bRequested)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("representative"), Island.Representative);
        Row->SetStringField(TEXT("outcome"), Island.Outcome);
        Row->SetNumberField(TEXT("member_count"), Island.MemberGuids.Num());
        Row->SetArrayField(TEXT("member_guids"), FMT_StringArray(Island.MemberGuids));
        if (!Island.PolicyReason.IsEmpty())
        {
            Row->SetStringField(TEXT("policy_reason"), Island.PolicyReason);
        }
        if (!Island.SpanningCommentGuid.IsEmpty())
        {
            Row->SetStringField(TEXT("comment_guid"), Island.SpanningCommentGuid);
            Row->SetArrayField(TEXT("spans_islands"), FMT_StringArray(Island.SpannedIslands));
        }
        if (!Island.FailureDetail.IsEmpty())
        {
            Row->SetStringField(TEXT("failure_detail"), Island.FailureDetail);
            Row->SetStringField(TEXT("failed_phase"), Island.FailedPhase);
        }
        if (Island.bInvariantViolated)
        {
            Row->SetBoolField(TEXT("invariant_violated"), true);
            Row->SetStringField(TEXT("invariant_detail"), Island.InvariantDetail);
        }
        IslandRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TArray<FString> RequestedReps;
    TArray<FString> EligibleReps;
    for (const FFmtIsland& Island : Plan)
    {
        if (Island.bRequested)
        {
            RequestedReps.Add(Island.Representative);
        }
    }
    for (int32 Index : Eligible)
    {
        EligibleReps.Add(Plan[Index].Representative);
    }

    Data->Cursor.LastOperationStatus = FString::Printf(
        TEXT("bp_format '%s': %s / %s -- %d of %d requested island(s) formatted, %d node(s) moved, "
             "%d added (reroutes)."),
        *Graph->GetName(), Coverage,
        ClaireonBPMutation::ToWireString(Envelope.MutationState),
        FormattedCount, RequestedCount, MovedNodes, AddedNodes);

    auto Decorate = [&](TSharedPtr<FJsonObject> Target)
    {
        if (!Target.IsValid())
        {
            return;
        }
        Envelope.WriteInlineScalars(*Target);
        Target->SetStringField(TEXT("format_status"), Coverage);
        Target->SetStringField(TEXT("graph_name"), Graph->GetName());
        Target->SetStringField(TEXT("session_id"), SessionId);
        Target->SetNumberField(TEXT("islands_total"), Plan.Num());
        Target->SetNumberField(TEXT("islands_requested"), RequestedCount);
        Target->SetNumberField(TEXT("islands_formatted"), FormattedCount);
        Target->SetArrayField(TEXT("requested_islands"), FMT_StringArray(RequestedReps));
        Target->SetArrayField(TEXT("eligible_islands"), FMT_StringArray(EligibleReps));
        Target->SetArrayField(TEXT("island_outcomes"), IslandRows);
        Target->SetNumberField(TEXT("nodes_moved"), MovedNodes);
        Target->SetNumberField(TEXT("nodes_added"), AddedNodes);
        Target->SetNumberField(TEXT("nodes_total"), Graph->Nodes.Num());
        // Retain the existing report_delta field for callers.
        Target->SetNumberField(TEXT("nodes_before"), PositionsBefore.Num());
        if (!SelectionRestoreStatus.IsEmpty())
        {
            Target->SetStringField(TEXT("selection_restore_status"), SelectionRestoreStatus);
            Target->SetArrayField(TEXT("selection_restore_missing"),
                FMT_StringArray(SelectionRestoreMissing));
        }
        if (Envelope.FailedIslandGuid.Len() > 0)
        {
            const int32 FailedIndex = FailedIsland != INDEX_NONE ? FailedIsland : FirstViolatingIsland;
            if (Plan.IsValidIndex(FailedIndex))
            {
                Target->SetArrayField(TEXT("failed_island_member_guids"),
                    FMT_StringArray(Plan[FailedIndex].MemberGuids));
            }
        }

        // Claireon opens no transaction here; BA owns its formatting and link transactions.
        Target->SetObjectField(TEXT("claireon_transactions"),
            ClaireonBPMutation::MakeClaireonTransactionsReport({}, Envelope.bRollbackAvailable));

        if (RolledBackCount > 0 || RetainedViolationCount > 0)
        {
            TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
            Rollback->SetNumberField(TEXT("islands_rolled_back"), RolledBackCount);
            Rollback->SetNumberField(TEXT("islands_retained"), RetainedViolationCount);

            TArray<TSharedPtr<FJsonValue>> Details;
            for (const FFmtIsland& Row : Plan)
            {
                if (Row.RollbackDetail.IsEmpty())
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("island"), Row.Representative);
                Obj->SetBoolField(TEXT("rolled_back"), Row.bRolledBack);
                Obj->SetStringField(TEXT("detail"), Row.RollbackDetail);
                Details.Add(MakeShared<FJsonValueObject>(Obj));
            }
            Rollback->SetArrayField(TEXT("islands"), Details);
            Target->SetObjectField(TEXT("invariant_rollback"), Rollback);
        }

        // Report recovery for retained BA work separately from Claireon group rollback availability.
        if (Envelope.bMutationRetained && RetainedTransactionCount > 0)
        {
            TSharedPtr<FJsonObject> Recovery = MakeShared<FJsonObject>();
            Recovery->SetNumberField(TEXT("blueprintassist_transactions_opened"), DispatchedTransactionCount);
            Recovery->SetNumberField(TEXT("blueprintassist_transactions_remaining"), RetainedTransactionCount);
            Recovery->SetStringField(TEXT("undo_tool"), TEXT("transaction_undo"));
            // Count only transactions still standing after island rollbacks.
            Recovery->SetNumberField(TEXT("undo_count"), RetainedTransactionCount);
            // Counts are exact only if the pre-call head is found and every dispatch completed.
            // Otherwise verify transaction history: the estimate may be a floor or ceiling.
            const bool bCountExact = bStandingCountExact && !bUnobservedDispatchOutcome;
            Recovery->SetStringField(TEXT("count_confidence"),
                bCountExact ? TEXT("exact") : TEXT("verify_with_transaction_history"));
            Recovery->SetStringField(TEXT("note"),
                FString::Printf(
                    TEXT("%d transaction(s) from this call still stand on the undo stack, measured "
                         "from the buffer above the pre-call head -- one 'Claireon bp_format island' "
                         "entry per retained island, BlueprintAssist's own transactions nested "
                         "inside it (%d formatter completion(s) observed, %d auto-rolled-back). "
                         "transaction_undo(count=%d) reverses what remains, a settle-failed "
                         "island's work included -- reverses the TRANSACTED work, exactly: node "
                         "moves BlueprintAssist makes on later ticks while node sizes settle land "
                         "outside any transaction, so small position residue can survive every "
                         "undo. Confirm against transaction_history first: any edit made after "
                         "this call sits above these entries and would be undone too%s. "
                         "rollback_available is false because CLAIREON opened no transaction "
                         "group, not because the work is unrecoverable."),
                    RetainedTransactionCount, DispatchedTransactionCount, RolledBackCount,
                    RetainedTransactionCount,
                    bCountExact
                        ? TEXT("")
                        : TEXT(" -- and this count could NOT be proven exact (a dispatch never "
                               "signalled completion, or the pre-call head was lost), so verify "
                               "it there rather than undoing by it blindly")));
            Target->SetObjectField(TEXT("recovery"), Recovery);
        }

        // Report untransacted settle residue explicitly because no undo can restore it.
        if (Envelope.bMutationRetained && RetainedTransactionCount == 0)
        {
            Target->SetBoolField(TEXT("retained_is_untransacted_residue"), true);
            Target->SetStringField(TEXT("retained_residue_note"),
                FString::Printf(
                    TEXT("%d node(s) sit at positions differing from the pre-call graph while "
                         "no transaction of this call remains on the undo stack: the residue "
                         "is settle-tick position adjustment BlueprintAssist makes outside "
                         "any transaction, so transaction_undo cannot reverse it. Re-running "
                         "bp_format converges it (formatting is a fixed point), or reposition "
                         "manually with bp_move_node."),
                    MovedNodes));
        }

        FClaireonBPSnapshot After;
        ClaireonBPSnapshot::Capture(Blueprint, {Graph}, EClaireonBPSnapshotFamily::Format, After);
        const FClaireonBPSnapshotDelta Delta = ClaireonBPSnapshot::Diff(Before, After);
        if (TSharedPtr<FJsonObject> DeltaJson = Delta.ToJson(); DeltaJson.IsValid())
        {
            Target->SetObjectField(TEXT("operation_delta"), DeltaJson);
        }
    };

    const bool bIsError = ClaireonBPMutation::IsErrorState(Envelope.MutationState);

    if (bIsError || bReportDelta)
    {
        TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
        Decorate(ResponseData);

        if (!bIsError)
        {
            return MakeSuccessResult(ResponseData, Data->Cursor.LastOperationStatus);
        }

        FToolResult Result = MakeErrorResult(Data->Cursor.LastOperationStatus);
        Result.Data = ResponseData;
        return Result;
    }

    FToolResult Result = BuildStateResponse(SessionId, Data);
    Decorate(Result.Data);
    return Result;
#endif // WITH_BLUEPRINT_ASSIST
}



FString ClaireonBlueprintGraphTool_Format::GetFullDescription() const
{
    return TEXT(
        "Lays out a Blueprint graph using BlueprintAssist, which places data nodes and "
        "reroute lanes as well as exec flow. REQUIRES an interactive editor with the "
        "BlueprintAssist plugin: the formatter measures node sizes from live Slate widgets, "
        "so it cannot run headless or in a commandlet, and there is deliberately no fallback "
        "layout -- the previous built-in one positioned exec nodes only and produced worse "
        "graphs than doing nothing. Accepts session_id or asset_path, plus optional "
        "graph_name to target a graph other than the session cursor's. asset_path auto-opens a "
        "transient edit session (and closes it), so no prior bp_open is needed. REQUIRES the asset "
        "editor to already be open -- call editor_open_asset first; bp_format cannot open it, "
        "because opening defers to the engine loop and this tool pumps only Slate so it cannot "
        "dispatch other MCP calls re-entrantly. Honours the user's "
        "BlueprintAssist settings and never modifies them, so output legitimately varies with "
        "FormatAllStyle, DPI and plugin version. Waits for formatting to settle and reports a "
        "timeout rather than claiming success early. report_delta=true returns the per-island "
        "report alone instead of the session state; it is a post-format report, not a dry run -- "
        "nothing is rolled back."
        "\n\n"
        "PER-ISLAND. An island is a connected component over links of ANY category -- exec and "
        "data alike, so two exec chains joined only by a shared variable read are one island. "
        "Reroute knots count as members; comments do not, because they have no pins. Name an "
        "island in island_guids by its representative GUID or by any member GUID; omit the field "
        "to request every island. Islands are formatted in ascending representative-GUID order, "
        "and that order is part of the result: after a failure, every island not yet reached is "
        "reported not_attempted."
        "\n\n"
        "TWO INDEPENDENT AXES. format_status is COVERAGE over the islands you REQUESTED -- "
        "complete, partial, none_completed, or all_targets_excluded. mutation_state is whether "
        "the change applied -- refused, applied_clean, applied_operation_failed, or "
        "applied_validation_failed. They cross: a policy skip yields partial + applied_clean "
        "with no error, while an invariant failure yields complete + applied_validation_failed "
        "with the mutation RETAINED. island_outcomes carries one row per requested island "
        "(formatted, skipped_by_policy, failed, not_attempted) with its member GUIDs -- keep "
        "those, because BlueprintAssist mints reroutes with fresh GUIDs and a new one can sort "
        "below the old representative, so a representative is a PLAN-TIME name that may not be "
        "re-derivable from the formatted graph."
        "\n\n"
        "SKIPPED BY POLICY, BEFORE ANYTHING MOVES. A one-node island is skipped: it has nothing "
        "to be arranged relative to, so formatting it could only move it. An island held by a "
        "comment that also holds another island is skipped, naming the comment -- "
        "BlueprintAssist will not move such a comment, so formatting under it would leave the "
        "island outside its own comment. Both decisions are made in a preflight pass, so a skip "
        "is always a clean skip and never a half-formatted graph."
        "\n\n"
        "IT BORROWS YOUR SELECTION AND GIVES IT BACK. Islands are dispatched by selecting them "
        "in the editor, so the visible selection changes during the call. The pre-call selection "
        "is restored on every return and selection_restore_status says how it went -- "
        "restored_partial is normal when a reroute was selected, because BlueprintAssist deletes "
        "existing knots and mints new ones. Restoration never changes the primary result. "
        "RELATEDLY, selecting a SINGLE node and formatting is a BlueprintAssist behavior worth "
        "knowing: it expands the selection to the whole connected tree reachable from that node, "
        "in a direction that depends on whether the node is pure. That is intended, and it is why "
        "a genuine one-node island is skipped rather than formatted."
        "\n\n"
        "EXPECT REROUTE AND WIRE COUNTS TO RISE, and do not read that as a regression. Formatting "
        "a hand-placed graph for the first time typically RAISES bp_lint's reroute-chain, "
        "long-reroute and long-wire counts: the formatter is making pre-existing structural debt "
        "visible, not adding it. Those findings are the input to the next round -- fix "
        "entry-param-wire and extract pure data islands, and the knots fall out with them. Do not "
        "revert a format pass on account of the delta. What should fall is the count at the END of "
        "the round sequence. Read the judgement rules before restructuring on the strength of a "
        "lint delta: instructions_read(\"blueprint-authoring\") or "
        "claireon://instructions/blueprint-authoring.");
}

FString ClaireonBlueprintGraphTool_Format::GetExampleUsage() const
{
    return TEXT("bp_format session_id=\"...\"  |  bp_format asset_path=\"/Game/BP/MyActor\"  |  "
                "bp_format session_id=\"...\" island_guids=[\"A1B2C3D4\"] report_delta=true");
}

#undef LOCTEXT_NAMESPACE
