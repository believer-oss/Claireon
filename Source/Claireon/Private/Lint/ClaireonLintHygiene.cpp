// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLintTypes.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonExecTopology.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"

namespace ClaireonLint
{

namespace ClaireonLintHygieneInternal
{
	FString ClaireonLintHygiene_NodeId(const UEdGraphNode* Node)
	{
		return IsValid(Node) ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>");
	}
}
using namespace ClaireonLintHygieneInternal;

void RunHygieneRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings)
{
	if (!IsValid(Context.Graph))
	{
		return;
	}

	for (UEdGraphNode* Node : Context.Graph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}

		if (const UK2Node_ExecutionSequence* Sequence = Cast<UK2Node_ExecutionSequence>(Node); IsValid(Sequence))
		{
			int32 ConnectedOutputs = 0;
			for (const UEdGraphPin* Pin : Sequence->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& Pin->LinkedTo.Num() > 0)
				{
					++ConnectedOutputs;
				}
			}

			if (ConnectedOutputs <= 1)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("no-op-sequence");
				Finding.Scope = EClaireonLintScope::Hygiene;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = ClaireonLintHygiene_NodeId(Node);
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetNumberField(TEXT("connected_outputs"), ConnectedOutputs);
				Finding.Message = FString::Printf(
					TEXT("Sequence node has %d connected output(s). A Sequence earns its place only when it fans execution out to two or more lanes."),
					ConnectedOutputs);
				OutFindings.Add(MoveTemp(Finding));
			}
		}

		// Unused cast results can coexist with useful exec-pin type tests.
		if (const UK2Node_DynamicCast* Cast = ::Cast<UK2Node_DynamicCast>(Node); IsValid(Cast))
		{
			const UEdGraphPin* ResultPin = Cast->GetCastResultPin();
			if (ResultPin && ResultPin->LinkedTo.Num() == 0)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("dead-cast");
				Finding.Scope = EClaireonLintScope::Hygiene;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = ClaireonLintHygiene_NodeId(Node);
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetStringField(TEXT("result_pin"), ResultPin->PinName.ToString());
				Finding.Message = TEXT("Cast result pin is unused. If the cast is only being used as a type test, "
					"the boolean success output says so more cheaply and more clearly.");
				OutFindings.Add(MoveTemp(Finding));
			}
		}

		// Detect conflicts in existing content as well as refusing new ones during authoring.
		if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node); IsValid(CustomEvent))
		{
			const FString EventName = CustomEvent->CustomFunctionName.ToString();
			const ClaireonBlueprintHelpers::FCustomEventNameConflict Conflict =
				ClaireonBlueprintHelpers::FindCustomEventNameConflict(
					Context.Blueprint, EventName, Node);

			if (Conflict.IsConflict())
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("custom-event-shadows-function");
				Finding.Scope = EClaireonLintScope::Hygiene;
				Finding.Severity = EClaireonLintSeverity::Warning;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = ClaireonLintHygiene_NodeId(Node);
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetStringField(TEXT("event_name"), EventName);
				Finding.Evidence->SetStringField(TEXT("resolved_function"),
					Conflict.ResolvedFunctionName.ToString());
				Finding.Evidence->SetStringField(TEXT("owner"), Conflict.OwnerName);
				Finding.Evidence->SetStringField(TEXT("conflict"), ClaireonBlueprintHelpers::ToString(Conflict.Kind));
				Finding.Evidence->SetBoolField(TEXT("native_event"), Conflict.bNativeEvent);
				if (!Conflict.ExistingNodeGuid.IsEmpty())
				{
					Finding.Evidence->SetStringField(TEXT("existing_node"), Conflict.ExistingNodeGuid);
					Finding.Evidence->SetStringField(TEXT("existing_graph"), Conflict.ExistingGraphName);
				}
				if (!Conflict.ResolutionNote.IsEmpty())
				{
					Finding.Evidence->SetStringField(TEXT("name_resolution"), Conflict.ResolutionNote);
				}
				Finding.Evidence->SetStringField(TEXT("remedy"), Conflict.Remedy);

				Finding.Message = FString::Printf(TEXT("%s %s"), *Conflict.Explanation, *Conflict.Remedy);
				OutFindings.Add(MoveTemp(Finding));
			}
		}
	}
}

}
