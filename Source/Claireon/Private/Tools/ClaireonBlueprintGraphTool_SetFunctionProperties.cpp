// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_SetFunctionProperties.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonBPFunctionRecipe.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

/** Merge optional properties with existing state and validate the complete proposal before mutation. */
namespace ClaireonSetFunctionPropertiesInternal
{


/** Shared transaction title for mutation and ownership reporting. */
FString SetFunctionProperties_TransactionTitle()
{
	return TEXT("[Claireon] Set Function Properties");
}

/** Access rank, ordered from most to least visible, so a REDUCTION is a rank increase. */
int32 AccessRank(int32 Flags)
{
	if ((Flags & FUNC_Private) != 0)   { return 2; }
	if ((Flags & FUNC_Protected) != 0) { return 1; }
	return 0;
}

const TCHAR* AccessName(int32 Flags)
{
	if ((Flags & FUNC_Private) != 0)   { return TEXT("Private"); }
	if ((Flags & FUNC_Protected) != 0) { return TEXT("Protected"); }
	if ((Flags & FUNC_Public) != 0)    { return TEXT("Public"); }
	return TEXT("Unspecified");
}

/** The mode the mutually exclusive net bits name. FUNC_NetReliable is deliberately not a mode. */
const TCHAR* NetModeName(int32 Flags)
{
	if ((Flags & FUNC_NetMulticast) != 0) { return TEXT("NetMulticast"); }
	if ((Flags & FUNC_NetServer) != 0)    { return TEXT("Server"); }
	if ((Flags & FUNC_NetClient) != 0)    { return TEXT("Client"); }
	return TEXT("None");
}

/** Network modes are reliable, matching bp_add_function creation semantics. */
bool NetFlagsForMode(const FString& Mode, int32& OutSetFlags)
{
	if (Mode == TEXT("None"))         { OutSetFlags = 0; return true; }
	if (Mode == TEXT("Server"))       { OutSetFlags = FUNC_Net | FUNC_NetServer    | FUNC_NetReliable; return true; }
	if (Mode == TEXT("Client"))       { OutSetFlags = FUNC_Net | FUNC_NetClient    | FUNC_NetReliable; return true; }
	if (Mode == TEXT("NetMulticast")) { OutSetFlags = FUNC_Net | FUNC_NetMulticast | FUNC_NetReliable; return true; }
	return false;
}

bool AccessFlagsForName(const FString& Name, int32& OutSetFlags)
{
	if (Name == TEXT("Public"))    { OutSetFlags = FUNC_Public;    return true; }
	if (Name == TEXT("Protected")) { OutSetFlags = FUNC_Protected; return true; }
	if (Name == TEXT("Private"))   { OutSetFlags = FUNC_Private;   return true; }
	return false;
}

FString HexFlags(int32 Flags)
{
	return FString::Printf(TEXT("0x%08X"), static_cast<uint32>(Flags));
}

/** The five bits of the clear mask, named so a caller reads the mask rather than decoding it. */
TArray<TSharedPtr<FJsonValue>> NetModeClearMaskNames()
{
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("FUNC_Net")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("FUNC_NetServer")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("FUNC_NetClient")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("FUNC_NetMulticast")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("FUNC_NetReliable")));
	return Names;
}


/** Keep the call-site scope summary within the inline scalar bound; longer citations remain in nested spill data. */
const TCHAR* const kCallSiteSearchScopeSummary =
	TEXT("Loaded, non-transient UK2Node_CallFunction nodes in this editor process only -- the ")
	TEXT("same population the engine's own call-site reconstruction reaches. NOT SEARCHED: ")
	TEXT("Blueprints that are not currently loaded, call sites reachable only through the ")
	TEXT("Find-in-Blueprints on-disk index, and native C++ callers. The enumeration is ")
	TEXT("therefore PARTIAL, and a partial enumeration cannot establish that an access ")
	TEXT("reduction or a net-mode change is safe.");

const TCHAR* const kCallSiteSearchScopeCitation =
	TEXT("Matched to this function by FunctionReference member name and owning class, mirroring ")
	TEXT("the engine's own test: HandleParameterDefaultValueChanged (EdGraphSchema_K2.cpp) ")
	TEXT("builds an FParamsChangedHelper and calls Broadcast, which resolves to the ")
	TEXT("non-virtual FBasePinChangeHelper::Broadcast, whose call-site loop is ")
	TEXT("TObjectIterator<UK2Node_CallFunction>(RF_Transient) gated on ")
	TEXT("FBasePinChangeHelper::NodeIsNotTransient (BlueprintEditorUtils.cpp:456-494). Call-site ")
	TEXT("reconstruction reaches exactly this set, so the set reported here and the set the ")
	TEXT("engine actually reshaped are the same set. Compiling this Blueprint cannot find ")
	TEXT("breakage in callers outside it.");

/** One enumerated call site, recorded by value. */
struct FCallSiteRecord
{
	FGuid NodeGuid;
	FString OwningBlueprint;
	FString GraphName;

	/** The Self pin's observable state, which is what a static retrofit rewrites. */
	bool bHasSelfPin = false;
	bool bSelfPinLinked = false;
	bool bSelfPinHidden = false;
	FString SelfPinDefaultObject;
};

void RecordSelfPin(const UK2Node_CallFunction* CallSite, FCallSiteRecord& Record)
{
	const UEdGraphPin* SelfPin = CallSite->FindPin(UEdGraphSchema_K2::PN_Self);
	if (!SelfPin)
	{
		return;
	}
	Record.bHasSelfPin = true;
	Record.bSelfPinLinked = SelfPin->LinkedTo.Num() > 0;
	Record.bSelfPinHidden = SelfPin->bHidden;
	Record.SelfPinDefaultObject = IsValid(SelfPin->DefaultObject)
		? SelfPin->DefaultObject->GetPathName()
		: FString();
}

/** Match the engine call-site reconstruction population when enumerating affected callers. */
void EnumerateCallSites(UBlueprint* Blueprint, FName FunctionName, TArray<FCallSiteRecord>& OutRecords)
{
	if (!IsValid(Blueprint))
	{
		return;
	}

	UFunction* Function = IsValid(Blueprint->SkeletonGeneratedClass)
		? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionName)
		: nullptr;
	const UClass* SignatureClass = IsValid(Function) ? Function->GetOwnerClass() : nullptr;

	for (TObjectIterator<UK2Node_CallFunction> It(RF_Transient); It; ++It)
	{
		UK2Node_CallFunction* CallSite = *It;
		if (!FBasePinChangeHelper::NodeIsNotTransient(CallSite))
		{
			continue;
		}
		UBlueprint* CallSiteBlueprint = FBlueprintEditorUtils::FindBlueprintForNode(CallSite);
		if (!IsValid(CallSiteBlueprint) || CallSite->GetSchema() == nullptr)
		{
			continue;
		}
		if (CallSite->FunctionReference.GetMemberName() != FunctionName)
		{
			continue;
		}

		const UClass* MemberParentClass =
			CallSite->FunctionReference.GetMemberParentClass(CallSite->GetBlueprintClassFromNode());
		const bool bClassMatchesEasy = (MemberParentClass != nullptr)
			&& ((SignatureClass != nullptr && MemberParentClass->IsChildOf(SignatureClass))
				|| (Blueprint->GeneratedClass != nullptr && MemberParentClass->IsChildOf(Blueprint->GeneratedClass)));
		const bool bClassMatchesHard = !bClassMatchesEasy
			&& CallSite->FunctionReference.IsSelfContext()
			&& (SignatureClass == nullptr)
			&& (CallSiteBlueprint == Blueprint
				|| (IsValid(CallSiteBlueprint->SkeletonGeneratedClass) && IsValid(Blueprint->SkeletonGeneratedClass)
					&& CallSiteBlueprint->SkeletonGeneratedClass->IsChildOf(Blueprint->SkeletonGeneratedClass)));

		if (!bClassMatchesEasy && !bClassMatchesHard)
		{
			continue;
		}

		FCallSiteRecord Record;
		Record.NodeGuid = CallSite->NodeGuid;
		Record.OwningBlueprint = CallSiteBlueprint->GetPathName();
		Record.GraphName = IsValid(CallSite->GetGraph()) ? CallSite->GetGraph()->GetName() : FString();
		RecordSelfPin(CallSite, Record);
		OutRecords.Add(MoveTemp(Record));
	}

	OutRecords.Sort([](const FCallSiteRecord& A, const FCallSiteRecord& B)
	{
		return A.NodeGuid < B.NodeGuid;
	});
}

TSharedPtr<FJsonValue> CallSiteToJson(const FCallSiteRecord& Record)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("node_guid"), Record.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Entry->SetStringField(TEXT("blueprint"), Record.OwningBlueprint);
	Entry->SetStringField(TEXT("graph"), Record.GraphName);
	Entry->SetBoolField(TEXT("has_self_pin"), Record.bHasSelfPin);
	if (Record.bHasSelfPin)
	{
		Entry->SetBoolField(TEXT("self_pin_linked"), Record.bSelfPinLinked);
		Entry->SetBoolField(TEXT("self_pin_hidden"), Record.bSelfPinHidden);
		Entry->SetStringField(TEXT("self_pin_default_object"), Record.SelfPinDefaultObject);
	}
	return MakeShared<FJsonValueObject>(Entry);
}

TArray<TSharedPtr<FJsonValue>> CallSitesToJson(const TArray<FCallSiteRecord>& Records)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FCallSiteRecord& Record : Records)
	{
		Values.Add(CallSiteToJson(Record));
	}
	return Values;
}


/** One reason a proposed state is refused, with the evidence a caller needs to repair. */
struct FRefusalFinding
{
	/** Machine token, e.g. "static_plus_const". */
	FString Reason;

	/** invalid means non-round-trippable; unverified means unsupported. */
	FString Class;

	/** Caller-facing explanation, including the engine evidence. */
	FString Detail;

	/** Optional node GUID or other identity the caller can act on. */
	FString Target;
};

TSharedPtr<FJsonValue> FindingToJson(const FRefusalFinding& Finding)
{
	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("reason"), Finding.Reason);
	Entry->SetStringField(TEXT("class"), Finding.Class);
	Entry->SetStringField(TEXT("detail"), Finding.Detail);
	if (!Finding.Target.IsEmpty())
	{
		Entry->SetStringField(TEXT("target"), Finding.Target);
	}
	return MakeShared<FJsonValueObject>(Entry);
}

/**
 * Detect self-dependent bodies before making them static.
 * Allow explicit receivers and existing static calls; sort evidence by GUID.
 */
void StaticBodyPreflight(UEdGraph* FunctionGraph, TArray<FRefusalFinding>& OutFindings)
{
	if (!IsValid(FunctionGraph))
	{
		return;
	}

	TArray<FRefusalFinding> Findings;
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}
		const FString NodeId = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

		if (Node->IsA<UK2Node_Self>())
		{
			Findings.Add({TEXT("static_body_uses_self"), TEXT("unverified"),
				TEXT("the body contains a Self node, which a static function has no value for"),
				NodeId});
			continue;
		}

		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node); IsValid(VariableNode))
		{
			if (!VariableNode->VariableReference.IsLocalScope())
			{
				// Member ownership does not imply self context; wired or defaulted targets are explicit receivers.
				const UEdGraphPin* SelfPin =
					VariableNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
				const bool bExplicitReceiver = SelfPin
					&& (SelfPin->LinkedTo.Num() > 0 || IsValid(SelfPin->DefaultObject));
				if (!bExplicitReceiver)
				{
					Findings.Add({TEXT("static_body_uses_self"), TEXT("unverified"),
						FString::Printf(
							TEXT("the body reads or writes the member variable '%s' through the ")
							TEXT("implicit self context, which has no self to resolve on once the ")
							TEXT("function is static. Member access through an explicitly wired ")
							TEXT("Target is allowed."),
							*VariableNode->VariableReference.GetMemberName().ToString()),
						NodeId});
				}
				continue;
			}
		}

		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node); IsValid(CallNode))
		{
			const UFunction* Target = CallNode->GetTargetFunction();
			if (IsValid(Target) && Target->HasAllFunctionFlags(FUNC_Static))
			{
				continue;
			}
		}

		if (const UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input))
		{
			if (SelfPin->LinkedTo.Num() == 0 && !IsValid(SelfPin->DefaultObject))
			{
				Findings.Add({TEXT("static_body_uses_self"), TEXT("unverified"),
					FString::Printf(
						TEXT("node '%s' has an unwired Self pin, so it calls through the implicit ")
						TEXT("self context a static function does not have"),
						*Node->GetName()),
					NodeId});
			}
		}
	}

	Findings.Sort([](const FRefusalFinding& A, const FRefusalFinding& B)
	{
		return A.Target < B.Target;
	});
	OutFindings.Append(MoveTemp(Findings));
}

/** True when the function declares at least one user-defined output parameter. */
bool FunctionHasOutputs(UEdGraph* FunctionGraph)
{
	if (!IsValid(FunctionGraph))
	{
		return false;
	}
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (const UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node); IsValid(ResultNode))
		{
			if (ResultNode->UserDefinedPins.Num() > 0)
			{
				return true;
			}
		}
	}
	return false;
}

/** Find a real exec body, excluding direct entry-to-return links that exist on pure functions too. */
UEdGraphNode* FindConnectedExecBody(UK2Node_FunctionEntry* EntryNode)
{
	if (!IsValid(EntryNode))
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output)
		{
			continue;
		}
		if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			continue;
		}
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			UEdGraphNode* Owner = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
			if (Owner == nullptr || Owner->IsA<UK2Node_FunctionResult>())
			{
				continue;
			}
			return Owner;
		}
	}
	return nullptr;
}


TArray<UEdGraph*> SnapshotGraphs(UBlueprint* Blueprint)
{
	TArray<UEdGraph*> Graphs;
	if (IsValid(Blueprint))
	{
		Blueprint->GetAllGraphs(Graphs);
	}
	return Graphs;
}

/** Refuse before transactions, cursor changes, and snapshots. */
FToolResult Refuse(
	UBlueprint* Blueprint,
	const FString& SessionId,
	const FString& FunctionName,
	const FString& Message,
	const TArray<FRefusalFinding>& Findings)
{
	FClaireonBPMutationResult Envelope;
	Envelope.MutationState = EClaireonMutationState::Refused;
	Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);
	Envelope.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::MergedStateValidation);
	Envelope.LastCompletedPhase = kClaireonBPPhaseNone;
	Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;
	Envelope.bRollbackGroupSafe = false;
	Envelope.bUndoRecordAvailable = false;
	Envelope.AssetPath = IsValid(Blueprint) ? Blueprint->GetPathName() : FString();
	Envelope.SessionId = SessionId;

	FToolResult Error = IClaireonTool::MakeErrorResult(Message);
	Error.Data = MakeShared<FJsonObject>();
	Envelope.WriteInlineScalars(*Error.Data);

	Error.Data->SetStringField(TEXT("function_name"), FunctionName);

	Error.Data->SetStringField(TEXT("quiescence_proof"), TEXT("by_construction"));

	// Any proven-invalid combination makes the overall refusal invalid.
	bool bAnyInvalid = false;
	TArray<TSharedPtr<FJsonValue>> FindingValues;
	for (const FRefusalFinding& Finding : Findings)
	{
		bAnyInvalid = bAnyInvalid || Finding.Class == TEXT("invalid");
		FindingValues.Add(FindingToJson(Finding));
	}
	Error.Data->SetStringField(TEXT("refusal_class"), bAnyInvalid ? TEXT("invalid") : TEXT("unverified"));
	Error.Data->SetArrayField(TEXT("refusal_findings"), FindingValues);

	return Error;
}

/** What a post-transaction return reports about itself. */
struct FSetterOutcome
{
	EClaireonMutationState State = EClaireonMutationState::AppliedOperationFailed;
	FString FailedPhase;
	FString LastCompletedPhase = kClaireonBPPhaseNone;
	TOptional<EClaireonEngineCompileStatus> CompileStatus;

	/** TOOL-ASSERTED, never derived from the transaction buffer. */
	bool bUndoRecordAvailable = true;
};

TSharedPtr<FJsonObject> RecoveryHint(const FClaireonBPMutationResult& Envelope)
{
	if (Envelope.bRollbackAvailable && !Envelope.bRollbackGroupSafe)
	{
		return IClaireonTool::MakeGuidanceHint(
			TEXT("transaction_end_group"),
			TEXT("The property change is retained and your transaction group is still open, so "
			     "every later edit is being swept into it. Rollback safety could not be "
			     "established, so transaction_end_group is the safe terminal action; do not "
			     "roll back."));
	}
	if (Envelope.bUndoRecordAvailable)
	{
		return IClaireonTool::MakeGuidanceHint(
			TEXT("transaction_undo"),
			TEXT("The property change is retained on this asset and an undo record for it "
			     "exists. Read operation_delta for what remains, then either repair forward or "
			     "undo once."));
	}
	return IClaireonTool::MakeGuidanceHint(
		TEXT("bp_get_graph"),
		TEXT("The property change is retained on this asset and NO undo record for it exists. "
		     "Do NOT undo -- that would revert an unrelated earlier transaction. Inspect the "
		     "graph, then repair forward from operation_delta."));
}

/** Build retained failures from observed post-operation snapshots rather than the phase journal. */
FToolResult RetainedFailure(
	UBlueprint* Blueprint,
	const FString& SessionId,
	const FString& FunctionName,
	const FSetterOutcome& Outcome,
	const FClaireonBPSnapshot& Before,
	const FClaireonBPPhaseJournal& Journal,
	const FString& Message)
{
	FClaireonBPSnapshot After;
	ClaireonBPSnapshot::Capture(Blueprint, SnapshotGraphs(Blueprint),
		EClaireonBPSnapshotFamily::Setter, After);
	const FClaireonBPSnapshotDelta Delta = ClaireonBPSnapshot::Diff(Before, After);

	FClaireonBPMutationResult Envelope;
	Envelope.MutationState = Outcome.State;
	Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Outcome.State);
	Envelope.FailedPhase = Outcome.FailedPhase;
	Envelope.LastCompletedPhase = Outcome.LastCompletedPhase;
	Envelope.EngineCompileStatus = Outcome.CompileStatus;
	Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;

	// Retain the unsettled-formatting rollback guard.
	Envelope.bRollbackGroupSafe = false;
	Envelope.bUndoRecordAvailable = Outcome.bUndoRecordAvailable;
	Envelope.AssetPath = IsValid(Blueprint) ? Blueprint->GetPathName() : FString();
	Envelope.SessionId = SessionId;

	FToolResult Result = IClaireonTool::MakeErrorResult(Message);
	Result.Data = MakeShared<FJsonObject>();
	Envelope.WriteInlineScalars(*Result.Data);
	Result.Data->SetStringField(TEXT("function_name"), FunctionName);

	Result.Data->SetObjectField(TEXT("claireon_transactions"),
		ClaireonBPMutation::MakeClaireonTransactionsReport(
			{SetFunctionProperties_TransactionTitle()},
			Envelope.bRollbackAvailable));

	if (TSharedPtr<FJsonObject> DeltaJson = Delta.ToJson(); DeltaJson.IsValid())
	{
		Result.Data->SetObjectField(TEXT("operation_delta"), DeltaJson);
	}

	if (!Outcome.FailedPhase.IsEmpty())
	{
		FClaireonBPPhaseJournal PhaseOnly;
		PhaseOnly.Entries = Journal.EntriesForPhase(Outcome.FailedPhase);
		if (TSharedPtr<FJsonObject> PhaseJson = PhaseOnly.ToJson(); PhaseJson.IsValid())
		{
			Result.Data->SetObjectField(TEXT("failed_phase_delta"), PhaseJson);
		}
	}
	if (TSharedPtr<FJsonObject> JournalJson = Journal.ToJson(); JournalJson.IsValid())
	{
		Result.Data->SetObjectField(TEXT("phase_journal"), JournalJson);
	}

	Result.AddHint(RecoveryHint(Envelope));
	return Result;
}


/** Accept only user-owned function graphs; inherited and interface signatures remain unsupported. */
UEdGraph* ResolveOwnFunctionGraph(
	UBlueprint* Blueprint,
	const FString& FunctionName,
	TArray<FRefusalFinding>& OutFindings,
	FString& OutRefusalMessage)
{
	const FName FuncFName(*FunctionName);

	UEdGraph* Found = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (IsValid(Graph) && Graph->GetFName() == FuncFName)
		{
			Found = Graph;
			break;
		}
	}

	if (!IsValid(Found))
	{
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				if (IsValid(Graph) && Graph->GetFName() == FuncFName)
				{
					OutFindings.Add({TEXT("interface_function"), TEXT("unverified"),
						FString::Printf(
							TEXT("'%s' is an implementation of interface '%s', not a function this ")
							TEXT("Blueprint owns. Interface and inherited functions are refused ")
							TEXT("until their behaviour under a property change is established ")
							TEXT("(DEC-23)."),
							*FunctionName, *GetNameSafe(InterfaceDesc.Interface.Get())),
						FString()});
					OutRefusalMessage = FString::Printf(
						TEXT("bp_set_function_properties refused: '%s' is an interface implementation."),
						*FunctionName);
					return nullptr;
				}
			}
		}

		OutFindings.Add({TEXT("function_not_owned"), TEXT("unverified"),
			FString::Printf(
				TEXT("no function graph named '%s' is owned by this Blueprint. Inherited, ")
				TEXT("interface and event-graph functions are refused until their behaviour ")
				TEXT("under a property change is established."),
				*FunctionName),
			FString()});
		OutRefusalMessage = FString::Printf(
			TEXT("bp_set_function_properties refused: this Blueprint owns no function graph named '%s'."),
			*FunctionName);
		return nullptr;
	}

	UK2Node_FunctionEntry* EntryNode = ClaireonBPFunctionRecipe::FindFunctionEntry(Found);
	if (!IsValid(EntryNode))
	{
		OutFindings.Add({TEXT("no_function_entry"), TEXT("unverified"),
			FString::Printf(TEXT("graph '%s' has no UK2Node_FunctionEntry to carry properties"), *FunctionName),
			FString()});
		OutRefusalMessage = FString::Printf(
			TEXT("bp_set_function_properties refused: graph '%s' has no function entry node."),
			*FunctionName);
		return nullptr;
	}

	if (EntryNode->FunctionReference.ResolveMember<UFunction>(EntryNode->GetBlueprintClassFromNode()) != nullptr)
	{
		OutFindings.Add({TEXT("inherited_function"), TEXT("unverified"),
			FString::Printf(
				TEXT("'%s' overrides an inherited function -- its entry node resolves a signature ")
				TEXT("UFunction through FunctionReference, so its flags are not this Blueprint's ")
				TEXT("to set alone. Refused until established."),
				*FunctionName),
			FString()});
		OutRefusalMessage = FString::Printf(
			TEXT("bp_set_function_properties refused: '%s' is an override of an inherited function."),
			*FunctionName);
		return nullptr;
	}

	return Found;
}

} // namespace ClaireonSetFunctionPropertiesInternal

using namespace ClaireonSetFunctionPropertiesInternal;


FString ClaireonBlueprintGraphTool_SetFunctionProperties::GetOperation() const { return TEXT("set_function_properties"); }

FString ClaireonBlueprintGraphTool_SetFunctionProperties::GetDescription() const
{
	return TEXT("Set properties on an existing Blueprint function in the open editing session: purity, const, static, category, tooltip, access specifier, network call mode. Every property is optional; an omitted one is left alone, never reset. Transactional. Refuses an invalid or unverified merged state BEFORE any transaction opens. Flag bits are verified; RPC routing is NOT.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetFunctionProperties::GetInputSchema() const
{
	// Use creation-time property vocabulary while validating the merged retrofit state.
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
	Builder.AddString(TEXT("function_name"), TEXT("Name of the function to modify."), true);
	Builder.AddBoolean(TEXT("is_pure"), TEXT("Optional. If true, the function is pure (no exec pins). Refused when the entry node's exec output is connected to a body."));
	Builder.AddBoolean(TEXT("is_const"), TEXT("Optional. If true, the function is marked const. Refused together with is_static: the engine silently clears const on load."));
	Builder.AddBoolean(TEXT("is_static"), TEXT("Optional. If true, the function is static. Refused when the body reads self (member variables, Self nodes, unwired Self pins)."));
	Builder.AddString(TEXT("category"), TEXT("Optional function category (appears in My Blueprint pane)."));
	Builder.AddString(TEXT("tooltip"), TEXT("Optional tooltip text."));
	Builder.AddString(TEXT("access_specifier"), TEXT("Optional: 'Public' | 'Protected' | 'Private'."));
	Builder.AddString(TEXT("is_network_call"), TEXT("Optional: 'None' | 'Server' | 'Client' | 'NetMulticast'. 'None' clears all five net bits including FUNC_NetReliable."));
	Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetFunctionProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("set_function_properties"), Params, SessionId, Data, Error))
	{
		return Error;
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field 'function_name' for set_function_properties"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Apply supplied properties and leave omitted ones alone.
	const bool bHasPure   = Params->HasField(TEXT("is_pure"));
	const bool bHasConst  = Params->HasField(TEXT("is_const"));
	const bool bHasStatic = Params->HasField(TEXT("is_static"));
	const bool bHasCategory = Params->HasField(TEXT("category"));
	const bool bHasTooltip  = Params->HasField(TEXT("tooltip"));
	const bool bHasAccess   = Params->HasField(TEXT("access_specifier"));
	const bool bHasNet      = Params->HasField(TEXT("is_network_call"));

	bool bWantPure = false;   Params->TryGetBoolField(TEXT("is_pure"), bWantPure);
	bool bWantConst = false;  Params->TryGetBoolField(TEXT("is_const"), bWantConst);
	bool bWantStatic = false; Params->TryGetBoolField(TEXT("is_static"), bWantStatic);

	FString Category; Params->TryGetStringField(TEXT("category"), Category);
	FString Tooltip;  Params->TryGetStringField(TEXT("tooltip"), Tooltip);

	int32 RequestedAccessFlags = 0;
	FString AccessSpec;
	if (bHasAccess)
	{
		Params->TryGetStringField(TEXT("access_specifier"), AccessSpec);
		if (!AccessFlagsForName(AccessSpec, RequestedAccessFlags))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid access_specifier '%s' (expected Public, Protected, Private)"), *AccessSpec));
		}
	}

	int32 RequestedNetFlags = 0;
	FString NetCall;
	if (bHasNet)
	{
		Params->TryGetStringField(TEXT("is_network_call"), NetCall);
		if (!NetFlagsForMode(NetCall, RequestedNetFlags))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid is_network_call value '%s' (expected None, Server, Client, NetMulticast)"), *NetCall));
		}
	}

	const bool bAnyFlagProperty = bHasPure || bHasConst || bHasStatic || bHasAccess || bHasNet;
	const bool bAnyMetadataProperty = bHasCategory || bHasTooltip;
	if (!bAnyFlagProperty && !bAnyMetadataProperty)
	{
		TArray<FRefusalFinding> Findings;
		Findings.Add({TEXT("no_property_supplied"), TEXT("invalid"),
			TEXT("no property was supplied, so there is no proposed state to validate or apply"),
			FString()});
		return Refuse(Blueprint, SessionId, FunctionName,
			TEXT("bp_set_function_properties refused: no property was supplied. No transaction was "
			     "opened and nothing was written."),
			Findings);
	}

	TArray<FRefusalFinding> Findings;
	FString RefusalMessage;
	UEdGraph* FunctionGraph = ResolveOwnFunctionGraph(Blueprint, FunctionName, Findings, RefusalMessage);
	if (!IsValid(FunctionGraph))
	{
		return Refuse(Blueprint, SessionId, FunctionName, RefusalMessage, Findings);
	}

	UK2Node_FunctionEntry* EntryNode = ClaireonBPFunctionRecipe::FindFunctionEntry(FunctionGraph);
	check(EntryNode != nullptr);

	// Validate the complete proposed state, including existing flags.
	const int32 CurrentFlags = EntryNode->GetExtraFlags();

	int32 ManagedMask = 0;
	if (bHasPure)   { ManagedMask |= FUNC_BlueprintPure; }
	if (bHasConst)  { ManagedMask |= FUNC_Const; }
	if (bHasStatic) { ManagedMask |= FUNC_Static; }
	if (bHasAccess) { ManagedMask |= ClaireonBPFunctionRecipe::kAccessClearMask; }
	if (bHasNet)    { ManagedMask |= ClaireonBPFunctionRecipe::kNetModeClearMask; }

	int32 ProposedFlags = CurrentFlags & ~ManagedMask;
	if (bHasPure && bWantPure)     { ProposedFlags |= FUNC_BlueprintPure; }
	if (bHasConst && bWantConst)   { ProposedFlags |= FUNC_Const; }
	if (bHasStatic && bWantStatic) { ProposedFlags |= FUNC_Static; }
	if (bHasAccess) { ProposedFlags |= RequestedAccessFlags; }
	if (bHasNet)    { ProposedFlags |= RequestedNetFlags; }

	const bool bProposedPure   = (ProposedFlags & FUNC_BlueprintPure) != 0;
	const bool bProposedConst  = (ProposedFlags & FUNC_Const) != 0;
	const bool bProposedStatic = (ProposedFlags & FUNC_Static) != 0;
	const bool bProposedNet    = (ProposedFlags & (FUNC_NetServer | FUNC_NetClient | FUNC_NetMulticast)) != 0;

	// PostLoad clears Const on Static functions, so accepting both would not round-trip.
	if (bProposedStatic && bProposedConst)
	{
		Findings.Add({TEXT("static_plus_const"), TEXT("invalid"),
			TEXT("a static function cannot be const: UFunction::PostLoad (Class.cpp:7256-7261) "
			     "silently clears FUNC_Const whenever FUNC_Const and FUNC_Static are both set, "
			     "so the asset would come back different after a reload"),
			FString()});
	}

	if (bProposedPure && bProposedNet)
	{
		Findings.Add({TEXT("pure_plus_rpc"), TEXT("unverified"),
			TEXT("a pure networked function has no established behaviour: a pure call is "
			     "evaluated wherever its result is needed, and an RPC is dispatched once. "
			     "Refused until established."),
			FString()});
	}
	if (bProposedStatic && bProposedNet)
	{
		Findings.Add({TEXT("static_plus_rpc"), TEXT("unverified"),
			TEXT("a static networked function has no established behaviour: RPC routing needs an "
			     "actor to resolve authority and an owning connection from, and a static call "
			     "has no such target. Refused until established."),
			FString()});
	}
	if (bProposedNet && FunctionHasOutputs(FunctionGraph))
	{
		Findings.Add({TEXT("rpc_with_outputs"), TEXT("unverified"),
			TEXT("this function declares output parameters, and an RPC does not return values to "
			     "its caller. What the caller reads from those outputs after a retrofit was not "
			     "established. Refused until it is."),
			FString()});
	}

	// Reject self-dependent bodies before making them static.
	const bool bStaticBeingSet = bProposedStatic && ((CurrentFlags & FUNC_Static) == 0);
	if (bStaticBeingSet)
	{
		StaticBodyPreflight(FunctionGraph, Findings);
	}

	// Reject real exec bodies before purity would prune their execution.
	if (bProposedPure && (CurrentFlags & FUNC_BlueprintPure) == 0)
	{
		if (UEdGraphNode* BodyNode = FindConnectedExecBody(EntryNode); IsValid(BodyNode))
		{
			Findings.Add({TEXT("pure_with_exec_body"), TEXT("invalid"),
				TEXT("the function entry's exec output is connected, so the function has an exec "
				     "body a pure function cannot run; the compiler would prune that chain rather "
				     "than fail, leaving a function that reports success and no longer does what "
				     "it did"),
				BodyNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)});
		}
	}

	if (Findings.Num() > 0)
	{
		TArray<FString> Reasons;
		for (const FRefusalFinding& Finding : Findings)
		{
			Reasons.AddUnique(Finding.Reason);
		}
		return Refuse(Blueprint, SessionId, FunctionName,
			FString::Printf(
				TEXT("bp_set_function_properties refused the merged post-change state for '%s': %s. ")
				TEXT("The complete proposed state was built and validated BEFORE any transaction ")
				TEXT("opened, so nothing was written and no cleanup is needed."),
				*FunctionName, *FString::Join(Reasons, TEXT(", "))),
			Findings);
	}

	const int32 ChangedBits = (ProposedFlags ^ CurrentFlags) & ManagedMask;
	const bool bPurityChanged = (ChangedBits & FUNC_BlueprintPure) != 0;
	const bool bStaticChanged = (ChangedBits & FUNC_Static) != 0;
	const bool bNetChanged    = (ChangedBits & ClaireonBPFunctionRecipe::kNetModeClearMask) != 0;
	const bool bAccessChanged = (ChangedBits & ClaireonBPFunctionRecipe::kAccessClearMask) != 0;

	const FString CategoryBefore = EntryNode->MetaData.Category.ToString();
	const FString TooltipBefore  = EntryNode->MetaData.ToolTip.ToString();

	TArray<FCallSiteRecord> CallSitesBefore;
	EnumerateCallSites(Blueprint, FunctionGraph->GetFName(), CallSitesBefore);

	// Capture after preflight and immediately before mutation.
	FClaireonBPSnapshot StartSnapshot;
	ClaireonBPSnapshot::Capture(Blueprint, SnapshotGraphs(Blueprint),
		EClaireonBPSnapshotFamily::Setter, StartSnapshot);

	FClaireonBPPhaseJournal Journal;
	Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::MergedStateValidation),
		TEXT("merged_state_accepted"), FunctionName,
		FString::Printf(TEXT("current=%s proposed=%s managed_mask=%s"),
			*HexFlags(CurrentFlags), *HexFlags(ProposedFlags), *HexFlags(ManagedMask))});

	FString LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::MergedStateValidation);

	FScopedTransaction Transaction(FText::FromString(SetFunctionProperties_TransactionTitle()));
	Blueprint->Modify();

	if (bAnyMetadataProperty)
	{
		EntryNode->Modify();
		if (bHasCategory) { EntryNode->MetaData.Category = FText::FromString(Category); }
		if (bHasTooltip)  { EntryNode->MetaData.ToolTip  = FText::FromString(Tooltip); }
		Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::MetadataModification),
			TEXT("entry_metadata_written"), FunctionName,
			FString::Printf(TEXT("category_supplied=%s tooltip_supplied=%s"),
				bHasCategory ? TEXT("true") : TEXT("false"),
				bHasTooltip ? TEXT("true") : TEXT("false"))});
		LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::MetadataModification);
	}

	// Replace the entire managed mask, including NetReliable when switching networking off.
	if (bAnyFlagProperty)
	{
		ClaireonBPFunctionRecipe::FFlagRecipeSteps Steps;
		Steps.FlagPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::FlagModification);
		Steps.EntryReconstructionPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::EntryReconstruction);
		Steps.CallsiteRefreshPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CallsiteRefresh);
		Steps.bReconstructEntry = bPurityChanged || bStaticChanged || bNetChanged;
		Steps.bRefreshCallSites = Steps.bReconstructEntry || bAccessChanged;

		FString RecipeError;
		if (!ClaireonBPFunctionRecipe::ApplyFunctionFlagChange(
				Blueprint, FunctionGraph,
				/*SetMask=*/ProposedFlags & ManagedMask,
				/*ClearMask=*/ManagedMask,
				Steps, Journal, RecipeError))
		{
			FSetterOutcome Outcome;
			Outcome.State = EClaireonMutationState::AppliedOperationFailed;
			Outcome.FailedPhase = Steps.FlagPhase;
			Outcome.LastCompletedPhase = LastCompletedPhase;
			Outcome.bUndoRecordAvailable = true;
			return RetainedFailure(Blueprint, SessionId, FunctionName, Outcome, StartSnapshot, Journal,
				FString::Printf(
					TEXT("The flag change on '%s' could not be applied: %s. Any metadata already ")
					TEXT("written is RETAINED. Inspect operation_delta, then repair forward or undo once."),
					*FunctionName, *RecipeError));
		}

		LastCompletedPhase = Steps.bRefreshCallSites
			? FString(Steps.CallsiteRefreshPhase)
			: (Steps.bReconstructEntry ? FString(Steps.EntryReconstructionPhase) : FString(Steps.FlagPhase));
	}

	FCompilerResultsLog CompilerLog;
	CompilerLog.SetSourcePath(Blueprint->GetPathName());
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);

	TArray<ClaireonBPFunctionRecipe::FCompilerDiagnostic> Diagnostics;
	int32 TokenlessDiagnostics = 0;
	ClaireonBPFunctionRecipe::CollectCompilerDiagnostics(CompilerLog, Diagnostics, TokenlessDiagnostics);

	const bool bCompileFailed = (Blueprint->Status == BS_Error);
	Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate),
		bCompileFailed ? TEXT("compile_failed") : TEXT("compile_succeeded"),
		Blueprint->GetPathName(),
		FString::Printf(TEXT("EBlueprintStatus=%d"), static_cast<int32>(Blueprint->Status))});

	if (bCompileFailed)
	{
		FSetterOutcome Outcome;

		Outcome.State = EClaireonMutationState::AppliedValidationFailed;
		Outcome.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate);
		Outcome.LastCompletedPhase = LastCompletedPhase;
		Outcome.CompileStatus = EClaireonEngineCompileStatus::Failed;
		Outcome.bUndoRecordAvailable = true;
		FToolResult Failure = RetainedFailure(Blueprint, SessionId, FunctionName, Outcome,
			StartSnapshot, Journal,
			FString::Printf(
				TEXT("The property change on '%s' was applied and RETAINED, and the Blueprint then ")
				TEXT("failed to compile. An undo record for the whole operation exists. Read ")
				TEXT("compiler_diagnostics and operation_delta, then repair forward or undo once."),
				*FunctionName));
		ClaireonBPFunctionRecipe::AttachDiagnostics(Failure, Diagnostics, TokenlessDiagnostics);
		return Failure;
	}

	// Re-find and compare both durable flag sites after compile, which may replace the skeleton function.
	UK2Node_FunctionEntry* EntryAfter = ClaireonBPFunctionRecipe::FindFunctionEntry(FunctionGraph);
	const int32 EntryFlagsAfter = IsValid(EntryAfter) ? EntryAfter->GetExtraFlags() : 0;
	UFunction* FunctionAfter = IsValid(Blueprint->SkeletonGeneratedClass)
		? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionGraph->GetFName())
		: nullptr;
	const int32 FunctionFlagsAfter = IsValid(FunctionAfter) ? static_cast<int32>(FunctionAfter->FunctionFlags) : 0;

	const int32 ExpectedManaged = ProposedFlags & ManagedMask;
	const bool bEntryMatches = EntryAfter != nullptr && ((EntryFlagsAfter & ManagedMask) == ExpectedManaged);
	const bool bFunctionMatches = FunctionAfter != nullptr && ((FunctionFlagsAfter & ManagedMask) == ExpectedManaged);

	// Inject a readback mismatch after successful compile to test the real validation-failure path.
	const bool bReadbackMismatch = bAnyFlagProperty && (!bEntryMatches || !bFunctionMatches);
	if (bReadbackMismatch
		|| CLAIREON_BP_SHOULD_INJECT_FAILURE(
			ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate)))
	{
		FSetterOutcome Outcome;
		Outcome.State = EClaireonMutationState::AppliedValidationFailed;
		Outcome.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate);
		Outcome.LastCompletedPhase = LastCompletedPhase;
		Outcome.CompileStatus = EClaireonEngineCompileStatus::Succeeded;
		Outcome.bUndoRecordAvailable = true;
		FToolResult Failure = RetainedFailure(Blueprint, SessionId, FunctionName, Outcome,
			StartSnapshot, Journal,
			FString::Printf(
				TEXT("The property change on '%s' was applied and the Blueprint compiled, but ")
				TEXT("reading the flags back did not match the requested state over the managed ")
				TEXT("mask %s: entry node extra flags %s, skeleton UFunction flags %s, expected %s ")
				TEXT("within the mask. The mutation is RETAINED."),
				*FunctionName, *HexFlags(ManagedMask), *HexFlags(EntryFlagsAfter),
				*HexFlags(FunctionFlagsAfter), *HexFlags(ExpectedManaged)));
		ClaireonBPFunctionRecipe::AttachDiagnostics(Failure, Diagnostics, TokenlessDiagnostics);
		return Failure;
	}

	// Preserve the session cursor rather than switching to the edited function.
	Data->LastOperationAffectedNodes.Add(IsValid(EntryAfter) ? EntryAfter->NodeGuid : EntryNode->NodeGuid);
	TArray<FCallSiteRecord> CallSitesAfter;
	EnumerateCallSites(Blueprint, FunctionGraph->GetFName(), CallSitesAfter);
	for (const FCallSiteRecord& Record : CallSitesAfter)
	{
		Data->LastOperationAffectedNodes.Add(Record.NodeGuid);
	}
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set properties on function '%s'. Session graph left unchanged."), *FunctionName);

	FToolResult Result = PublicBuildStateResponse(SessionId, Data);
	if (Result.bIsError)
	{
		// State-render failure still reports the completed mutation and compile.
		FSetterOutcome Outcome;
		Outcome.State = EClaireonMutationState::AppliedOperationFailed;
		Outcome.LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate);
		Outcome.CompileStatus = EClaireonEngineCompileStatus::Succeeded;
		Outcome.bUndoRecordAvailable = true;
		return RetainedFailure(Blueprint, SessionId, FunctionName, Outcome, StartSnapshot, Journal,
			FString::Printf(
				TEXT("The property change on '%s' was applied and compiled successfully, but the ")
				TEXT("session state response could not be built (%s). The mutation is RETAINED and ")
				TEXT("an undo record for it exists."),
				*FunctionName, *Result.ErrorMessage));
	}

	if (!Result.Data.IsValid())
	{
		Result.Data = MakeShared<FJsonObject>();
	}

	{
		FClaireonBPMutationResult Envelope;
		Envelope.MutationState = EClaireonMutationState::AppliedClean;
		Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);
		Envelope.LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::CompileValidate);
		Envelope.EngineCompileStatus = EClaireonEngineCompileStatus::Succeeded;
		Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;
		Envelope.bRollbackGroupSafe = false;
		Envelope.bUndoRecordAvailable = true;
		Envelope.AssetPath = Blueprint->GetPathName();
		Envelope.SessionId = SessionId;
		Envelope.WriteInlineScalars(*Result.Data);
	}

	Result.Data->SetStringField(TEXT("function_name"), FunctionName);
	Result.Data->SetStringField(TEXT("function_graph"), FunctionGraph->GetName());
	Result.Data->SetBoolField(TEXT("session_graph_preserved"), true);

	{
		TArray<TSharedPtr<FJsonValue>> Applied;
		TArray<TSharedPtr<FJsonValue>> LeftAlone;
		const TCHAR* const AllProperties[] = {
			TEXT("is_pure"), TEXT("is_const"), TEXT("is_static"),
			TEXT("category"), TEXT("tooltip"), TEXT("access_specifier"), TEXT("is_network_call")};
		const bool Supplied[] = {bHasPure, bHasConst, bHasStatic, bHasCategory, bHasTooltip, bHasAccess, bHasNet};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AllProperties); ++Index)
		{
			(Supplied[Index] ? Applied : LeftAlone).Add(MakeShared<FJsonValueString>(AllProperties[Index]));
		}
		Result.Data->SetArrayField(TEXT("properties_supplied"), Applied);
		Result.Data->SetArrayField(TEXT("properties_left_alone"), LeftAlone);
	}

	{
		TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetStringField(TEXT("entry_extra_flags"), HexFlags(CurrentFlags));
		Before->SetStringField(TEXT("access_specifier"), AccessName(CurrentFlags));
		Before->SetStringField(TEXT("is_network_call"), NetModeName(CurrentFlags));
		Before->SetBoolField(TEXT("is_pure"), (CurrentFlags & FUNC_BlueprintPure) != 0);
		Before->SetBoolField(TEXT("is_const"), (CurrentFlags & FUNC_Const) != 0);
		Before->SetBoolField(TEXT("is_static"), (CurrentFlags & FUNC_Static) != 0);
		Before->SetBoolField(TEXT("net_reliable"), (CurrentFlags & FUNC_NetReliable) != 0);
		Before->SetStringField(TEXT("category"), CategoryBefore);
		Before->SetStringField(TEXT("tooltip"), TooltipBefore);
		Result.Data->SetObjectField(TEXT("properties_before"), Before);

		TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetStringField(TEXT("entry_extra_flags"), HexFlags(EntryFlagsAfter));
		After->SetStringField(TEXT("skeleton_function_flags"), HexFlags(FunctionFlagsAfter));
		After->SetStringField(TEXT("access_specifier"), AccessName(EntryFlagsAfter));
		After->SetStringField(TEXT("is_network_call"), NetModeName(EntryFlagsAfter));
		After->SetBoolField(TEXT("is_pure"), (EntryFlagsAfter & FUNC_BlueprintPure) != 0);
		After->SetBoolField(TEXT("is_const"), (EntryFlagsAfter & FUNC_Const) != 0);
		After->SetBoolField(TEXT("is_static"), (EntryFlagsAfter & FUNC_Static) != 0);
		After->SetBoolField(TEXT("net_reliable"), (EntryFlagsAfter & FUNC_NetReliable) != 0);
		After->SetStringField(TEXT("category"), IsValid(EntryAfter) ? EntryAfter->MetaData.Category.ToString() : FString());
		After->SetStringField(TEXT("tooltip"), IsValid(EntryAfter) ? EntryAfter->MetaData.ToolTip.ToString() : FString());
		Result.Data->SetObjectField(TEXT("properties_after"), After);
	}

	// Report network-mode changes separately from the full flag word.
	if (bHasNet)
	{
		TSharedPtr<FJsonObject> Net = MakeShared<FJsonObject>();
		Net->SetStringField(TEXT("mode_before"), NetModeName(CurrentFlags));
		Net->SetStringField(TEXT("mode_after"), NetModeName(EntryFlagsAfter));
		Net->SetStringField(TEXT("entry_net_flags_before"),
			HexFlags(CurrentFlags & ClaireonBPFunctionRecipe::kNetModeClearMask));
		Net->SetStringField(TEXT("entry_net_flags_after"),
			HexFlags(EntryFlagsAfter & ClaireonBPFunctionRecipe::kNetModeClearMask));
		Net->SetStringField(TEXT("skeleton_net_flags_after"),
			HexFlags(FunctionFlagsAfter & ClaireonBPFunctionRecipe::kNetModeClearMask));
		Net->SetStringField(TEXT("clear_mask"), HexFlags(ClaireonBPFunctionRecipe::kNetModeClearMask));
		Net->SetArrayField(TEXT("clear_mask_bits"), NetModeClearMaskNames());
		Net->SetBoolField(TEXT("net_reliable_before"), (CurrentFlags & FUNC_NetReliable) != 0);
		Net->SetBoolField(TEXT("net_reliable_after_entry"), (EntryFlagsAfter & FUNC_NetReliable) != 0);
		Net->SetBoolField(TEXT("net_reliable_after_skeleton"), (FunctionFlagsAfter & FUNC_NetReliable) != 0);
		Net->SetStringField(TEXT("reliability_note"),
			TEXT("Server, Client and NetMulticast are reliable, matching what bp_add_function "
			     "sets at creation; there is no separate reliability option. Moving to None "
			     "clears all five bits including FUNC_NetReliable, which the engine's own "
			     "SetNetFlags (BlueprintDetailsCustomization.cpp:5094) does not."));
		Result.Data->SetObjectField(TEXT("network_call"), Net);

		if (bNetChanged)
		{
			Result.Data->SetStringField(TEXT("network_compatibility_warning"),
				TEXT("A net-flag change alters the generated class's net function set and therefore "
				     "RPC ordering. A client and a dedicated server built from different revisions "
				     "of this asset can disagree about which RPC an index names, which is a field "
				     "bug rather than a compile error: both ends must ship the same asset revision."));
			Result.Data->SetStringField(TEXT("network_behaviour_not_verified"),
				TEXT("VERIFIED HERE: the flag bits set and clear correctly on both durable sites, "
				     "including the five-bit clear mask; the call sites below; the compiler "
				     "diagnostics. NOT VERIFIED: runtime RPC behaviour across a real client/server "
				     "boundary -- that a retrofitted RPC executes on the intended side, and that a "
				     "loaded call site invokes the intended endpoint. Using is_network_call on a "
				     "non-synthetic asset without a manual network test is a recorded ship risk."));
		}
	}

	// Report the inspected call-site population and its limits, including unloaded external callers.
	Result.Data->SetArrayField(TEXT("call_sites_before"), CallSitesToJson(CallSitesBefore));
	Result.Data->SetArrayField(TEXT("call_sites_after"), CallSitesToJson(CallSitesAfter));
	Result.Data->SetNumberField(TEXT("call_sites_found"), CallSitesAfter.Num());
	Result.Data->SetStringField(TEXT("call_site_search_scope"), kCallSiteSearchScopeSummary);
	Result.Data->SetBoolField(TEXT("call_site_search_complete"), false);
	{
		TSharedPtr<FJsonObject> Search = MakeShared<FJsonObject>();
		Search->SetStringField(TEXT("scope"), kCallSiteSearchScopeSummary);
		Search->SetStringField(TEXT("engine_citation"), kCallSiteSearchScopeCitation);
		Search->SetBoolField(TEXT("complete"), false);
		Result.Data->SetObjectField(TEXT("call_site_search"), Search);
	}

	if (bStaticChanged)
	{
		Result.Data->SetStringField(TEXT("static_retrofit_note"),
			TEXT("is_static changed, so every LOADED call site was structurally reconstructed "
			     "rather than re-flagged: UK2Node_CallFunction::PostReconstructNode reads "
			     "FUNC_Static and rewrites the Self pin's default object "
			     "(K2Node_CallFunction.cpp:1294-1313). Compare call_sites_before with "
			     "call_sites_after for the Self-pin set. Unloaded call sites were not reached."));
	}

	if (bAccessChanged && AccessRank(ProposedFlags) > AccessRank(CurrentFlags))
	{
		Result.Data->SetStringField(TEXT("access_reduction_note"),
			FString::Printf(
				TEXT("Access was REDUCED from %s to %s. The call-site enumeration above is partial "
				     "by construction, and compiling this Blueprint cannot find breakage in "
				     "callers that are not loaded, so this result does NOT establish that the "
				     "reduction is safe. Check external callers before relying on it."),
				AccessName(CurrentFlags), AccessName(EntryFlagsAfter)));
	}

	ClaireonBPFunctionRecipe::AttachDiagnostics(Result, Diagnostics, TokenlessDiagnostics);
	return CheckMutationAffectedNodes(TEXT("set_function_properties"), Data, Result);
}

#undef LOCTEXT_NAMESPACE
