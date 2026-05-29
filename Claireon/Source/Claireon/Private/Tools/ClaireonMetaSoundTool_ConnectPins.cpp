// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_ConnectPins.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

#undef CLAIREON_HAS_METASOUND_BUILDER_API
#define CLAIREON_HAS_METASOUND_BUILDER_API 0
#if __has_include("MetasoundDocumentBuilderRegistry.h") && __has_include("MetasoundBuilderSubsystem.h") && __has_include("MetasoundFrontendLiteral.h")
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#undef CLAIREON_HAS_METASOUND_BUILDER_API
#define CLAIREON_HAS_METASOUND_BUILDER_API 1
#endif

FString FClaireonMetaSoundTool_ConnectPins::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_ConnectPins::GetOperation() const { return TEXT("connect_pins"); }

FString FClaireonMetaSoundTool_ConnectPins::GetDescription() const
{
	return TEXT("Connect two pins on the MetaSound graph within the current session. Links a graph "
				"input output pin to a graph output input pin via graph_input_name and graph_output_name. "
				"Requires session_id from a metasound.open call; changes are applied immediately.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_ConnectPins::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	S.AddString(TEXT("graph_input_name"), TEXT("Graph input node name"), true);
	S.AddString(TEXT("graph_output_name"), TEXT("Graph output node name"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_ConnectPins::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !CLAIREON_HAS_METASOUND_BUILDER_API
	return MakeErrorResult(TEXT("MetaSound builder API not available on this engine branch"));
#else
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::MetaSound);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("MetaSound session not found: %s"), *SessionId));
	}

	FString GraphInputName, GraphOutputName;
	if (!Arguments->TryGetStringField(TEXT("graph_input_name"), GraphInputName) || GraphInputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("connect_pins requires graph_input_name and graph_output_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("graph_output_name"), GraphOutputName) || GraphOutputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("connect_pins requires graph_input_name and graph_output_name"));
	}

	using namespace Metasound::Engine;
	FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase* BuilderObj = Registry.FindBuilderObject(Data->Asset.Get());
	if (!BuilderObj) return MakeErrorResult(TEXT("Builder object not found for session asset"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect MetaSound Pins")));
	Data->Asset->Modify();

	EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
	FMetaSoundNodeHandle InputNode = BuilderObj->FindGraphInputNode(FName(*GraphInputName), FindResult);
	if (FindResult != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("Graph input '%s' not found"), *GraphInputName));
	}
	FindResult = EMetaSoundBuilderResult::Failed;
	FMetaSoundNodeHandle OutputNode = BuilderObj->FindGraphOutputNode(FName(*GraphOutputName), FindResult);
	if (FindResult != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("Graph output '%s' not found"), *GraphOutputName));
	}

	TArray<FMetaSoundBuilderNodeOutputHandle> InOutputs = BuilderObj->FindNodeOutputs(InputNode, FindResult);
	if (FindResult != EMetaSoundBuilderResult::Succeeded || InOutputs.Num() == 0)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("Input node '%s' has no output pin"), *GraphInputName));
	}
	TArray<FMetaSoundBuilderNodeInputHandle> OutInputs = BuilderObj->FindNodeInputs(OutputNode, FindResult);
	if (FindResult != EMetaSoundBuilderResult::Succeeded || OutInputs.Num() == 0)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("Output node '%s' has no input pin"), *GraphOutputName));
	}

	EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
	BuilderObj->ConnectNodes(InOutputs[0], OutInputs[0], ConnectResult);
	if (ConnectResult != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("ConnectNodes failed (input=%s, output=%s)"), *GraphInputName, *GraphOutputName));
	}

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Connected input '%s' -> output '%s'"), *GraphInputName, *GraphOutputName);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("graph_input_name"), GraphInputName);
	Out->SetStringField(TEXT("graph_output_name"), GraphOutputName);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
#endif
}
