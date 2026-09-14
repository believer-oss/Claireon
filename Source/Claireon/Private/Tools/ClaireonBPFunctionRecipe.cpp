// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBPFunctionRecipe.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphToken.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Class.h"

namespace ClaireonBPFunctionRecipeInternal
{
	const TCHAR* SeverityLabel(EMessageSeverity::Type Severity)
	{
		switch (Severity)
		{
		case EMessageSeverity::Error:              return TEXT("error");
		case EMessageSeverity::PerformanceWarning: return TEXT("warning");
		case EMessageSeverity::Warning:            return TEXT("warning");
		case EMessageSeverity::Info:               return TEXT("note");
		default:                                   return TEXT("info");
		}
	}

	constexpr int32 kMaxRenderedDiagnostics = 100;
}

namespace ClaireonBPFunctionRecipe
{

UK2Node_FunctionEntry* FindFunctionEntry(UEdGraph* FunctionGraph)
{
	if (!IsValid(FunctionGraph))
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : FunctionGraph->Nodes)
	{
		if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node); IsValid(Candidate))
		{
			return Candidate;
		}
	}
	return nullptr;
}

bool ApplyFunctionFlagChange(
	UBlueprint* Blueprint,
	UEdGraph* FunctionGraph,
	int32 SetMask,
	int32 ClearMask,
	const FFlagRecipeSteps& Steps,
	FClaireonBPPhaseJournal& Journal,
	FString& OutError)
{
	if (!IsValid(Blueprint) || !IsValid(FunctionGraph))
	{
		OutError = TEXT("no Blueprint or no function graph");
		return false;
	}
	if (Steps.FlagPhase == nullptr)
	{
		OutError = TEXT("the flag phase wire string is required");
		return false;
	}

	UK2Node_FunctionEntry* EntryNode = FindFunctionEntry(FunctionGraph);
	if (!IsValid(EntryNode))
	{
		OutError = FString::Printf(
			TEXT("graph '%s' has no UK2Node_FunctionEntry. UK2Node_FunctionEntry is a ")
			TEXT("UK2Node_FunctionTerminator, NOT a UK2Node_Tunnel, so a boundary walk that ")
			TEXT("checks only tunnels silently skips every function graph."),
			*FunctionGraph->GetName());
		return false;
	}

	UFunction* Function = IsValid(Blueprint->SkeletonGeneratedClass)
		? Blueprint->SkeletonGeneratedClass->FindFunctionByName(FunctionGraph->GetFName())
		: nullptr;
	if (!IsValid(Function))
	{
		OutError = FString::Printf(
			TEXT("no UFunction named '%s' on the skeleton class"), *FunctionGraph->GetName());
		return false;
	}

	EntryNode->Modify();
	Function->Modify();

	const int32 BeforeExtraFlags = EntryNode->GetExtraFlags();
	const uint32 BeforeFunctionFlags = static_cast<uint32>(Function->FunctionFlags);

	EntryNode->SetExtraFlags((BeforeExtraFlags & ~ClearMask) | SetMask);
	Function->FunctionFlags = static_cast<EFunctionFlags>(
		(BeforeFunctionFlags & ~static_cast<uint32>(ClearMask)) | static_cast<uint32>(SetMask));

	Journal.Append({Steps.FlagPhase, TEXT("function_flags_written"), FunctionGraph->GetName(),
		FString::Printf(
			TEXT("set=0x%08X clear=0x%08X applied to BOTH the entry node's extra flags ")
			TEXT("(0x%08X -> 0x%08X) and the skeleton UFunction's flags (0x%08X -> 0x%08X); ")
			TEXT("set and cleared, never XORed"),
			static_cast<uint32>(SetMask), static_cast<uint32>(ClearMask),
			static_cast<uint32>(BeforeExtraFlags), static_cast<uint32>(EntryNode->GetExtraFlags()),
			BeforeFunctionFlags, static_cast<uint32>(Function->FunctionFlags))});

	if (Steps.bReconstructEntry)
	{
		const bool bPreviousDisableOrphanPinSaving = EntryNode->bDisableOrphanPinSaving;
		EntryNode->bDisableOrphanPinSaving = true;
		EntryNode->ReconstructNode();
		EntryNode->bDisableOrphanPinSaving = bPreviousDisableOrphanPinSaving;

		Journal.Append({Steps.EntryReconstructionPhase != nullptr ? Steps.EntryReconstructionPhase : Steps.FlagPhase,
			TEXT("entry_node_reconstructed"),
			EntryNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
			TEXT("orphan-pin saving disabled for the reconstruct, matching the reference "
			     "toggle: a pin the new flags invalidate is dropped, not orphaned")});
	}

	// Refresh loaded call-site pins after changing the flags.
	if (Steps.bRefreshCallSites)
	{
		GetDefault<UEdGraphSchema_K2>()->HandleParameterDefaultValueChanged(EntryNode);
		Journal.Append({Steps.CallsiteRefreshPhase != nullptr ? Steps.CallsiteRefreshPhase : Steps.FlagPhase,
			TEXT("callsite_refresh_broadcast"), FunctionGraph->GetName(),
			TEXT("HandleParameterDefaultValueChanged marked the Blueprint structurally "
			     "modified and reconstructed every LOADED, NON-TRANSIENT call site of the "
			     "function; unloaded call sites are not reached")});
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Journal.Append({Steps.FlagPhase, TEXT("blueprint_marked_structurally_modified"),
			Blueprint->GetPathName(),
			TEXT("no call-site refresh was required by this change, so the Blueprint is marked "
			     "structurally modified directly rather than through the broadcast")});
	}

	return true;
}

void CollectCompilerDiagnostics(
	const FCompilerResultsLog& Log,
	TArray<FCompilerDiagnostic>& OutDiagnostics,
	int32& OutTokenless)
{
	OutTokenless = 0;
	for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
	{
		FCompilerDiagnostic Diagnostic;
		Diagnostic.Severity = static_cast<int32>(Message->GetSeverity());
		Diagnostic.SeverityLabel = ClaireonBPFunctionRecipeInternal::SeverityLabel(Message->GetSeverity());
		Diagnostic.Identifier = (Message->GetIdentifier() != NAME_None)
			? Message->GetIdentifier().ToString()
			: FString();
		Diagnostic.Message = Message->ToText().ToString();

		for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
		{
			if (Token->GetType() != EMessageToken::EdGraph)
			{
				continue;
			}
			const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(*Token);

			// Capture identity now; weak token references may not survive reconstruction.
			if (const UEdGraphPin* Pin = GraphToken.GetPin())
			{
				if (const UEdGraphNode* PinOwner = Pin->GetOwningNodeUnchecked(); IsValid(PinOwner))
				{
					Diagnostic.bHasNode = true;
					Diagnostic.NodeGuid = PinOwner->NodeGuid;
					Diagnostic.PinName = Pin->PinName.ToString();
					break;
				}
			}
			if (const UEdGraphNode* Node = Cast<const UEdGraphNode>(GraphToken.GetGraphObject()); IsValid(Node))
			{
				Diagnostic.bHasNode = true;
				Diagnostic.NodeGuid = Node->NodeGuid;
				break;
			}
		}

		if (!Diagnostic.bHasNode)
		{
			++OutTokenless;
		}
		OutDiagnostics.Add(MoveTemp(Diagnostic));
	}
}

TArray<TSharedPtr<FJsonValue>> DiagnosticsToJson(const TArray<FCompilerDiagnostic>& Diagnostics)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FCompilerDiagnostic& Diagnostic : Diagnostics)
	{
		if (Values.Num() >= ClaireonBPFunctionRecipeInternal::kMaxRenderedDiagnostics)
		{
			break;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("severity"), Diagnostic.SeverityLabel);
		if (!Diagnostic.Identifier.IsEmpty())
		{
			Entry->SetStringField(TEXT("identifier"), Diagnostic.Identifier);
		}
		Entry->SetStringField(TEXT("message"), Diagnostic.Message);
		if (Diagnostic.bHasNode)
		{
			Entry->SetStringField(TEXT("node_guid"),
				Diagnostic.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			if (!Diagnostic.PinName.IsEmpty())
			{
				Entry->SetStringField(TEXT("pin"), Diagnostic.PinName);
			}
		}
		else
		{
			Entry->SetBoolField(TEXT("tokenless"), true);
		}
		Values.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Values;
}

void AttachDiagnostics(
	IClaireonTool::FToolResult& Result,
	const TArray<FCompilerDiagnostic>& Diagnostics,
	int32 TokenlessDiagnostics)
{
	if (!Result.Data.IsValid())
	{
		Result.Data = MakeShared<FJsonObject>();
	}
	Result.Data->SetArrayField(TEXT("compiler_diagnostics"), DiagnosticsToJson(Diagnostics));
	Result.Data->SetNumberField(TEXT("compiler_diagnostics_total"), Diagnostics.Num());
	Result.Data->SetNumberField(TEXT("compiler_diagnostics_tokenless"), TokenlessDiagnostics);
	if (Diagnostics.Num() > ClaireonBPFunctionRecipeInternal::kMaxRenderedDiagnostics)
	{
		Result.Data->SetBoolField(TEXT("compiler_diagnostics_truncated"), true);
	}
}

} // namespace ClaireonBPFunctionRecipe
