// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_CreateEventEndpoint.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "Engine/Blueprint.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_CreateEventEndpoint::GetOperation() const { return TEXT("sequence_create_event_endpoint"); }

FString ClaireonLevelSequenceTool_CreateEventEndpoint::GetDescription() const
{
	return TEXT("Create (or ensure) a Director Blueprint event endpoint function on the sequence. "
				"Signature is either NoParams or BoundObject (single UObject* parameter).");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_CreateEventEndpoint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("endpoint_name"), TEXT("Name of the endpoint function to create on the Director."), true);
	Builder.AddEnum(TEXT("signature"), TEXT("Endpoint signature: 'NoParams' or 'BoundObject'."),
		{ TEXT("NoParams"), TEXT("BoundObject") }, false);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_CreateEventEndpoint::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FSequenceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}
	FString EndpointName;
	if (!Arguments->TryGetStringField(TEXT("endpoint_name"), EndpointName) || EndpointName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: endpoint_name"));
	}
	FString SignatureStr = TEXT("NoParams");
	Arguments->TryGetStringField(TEXT("signature"), SignatureStr);
	ESequenceEventEndpointSignature Signature = ESequenceEventEndpointSignature::NoParams;
	if (SignatureStr.Equals(TEXT("NoParams"), ESearchCase::IgnoreCase))
	{
		Signature = ESequenceEventEndpointSignature::NoParams;
	}
	else if (SignatureStr.Equals(TEXT("BoundObject"), ESearchCase::IgnoreCase))
	{
		Signature = ESequenceEventEndpointSignature::BoundObject;
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown signature '%s' (expected 'NoParams' or 'BoundObject')"), *SignatureStr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Create Event Endpoint")));

	ULevelSequence* Sequence = Data->Sequence.Get();
	UBlueprint* DirectorBP = nullptr;
	UFunction* EndpointFunc = nullptr;
	if (!ApplyCreateEventEndpoint(Sequence, FName(*EndpointName), Signature, DirectorBP, EndpointFunc, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Sequence);

	Data->LastOperationStatus = FString::Printf(TEXT("Created event endpoint '%s' (%s) on %s"),
		*EndpointName,
		Signature == ESequenceEventEndpointSignature::BoundObject ? TEXT("BoundObject") : TEXT("NoParams"),
		DirectorBP ? *DirectorBP->GetName() : TEXT("(null)"));

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (!Result.bIsError && Result.Data.IsValid())
	{
		Result.Data->SetBoolField(TEXT("ok"), true);
		Result.Data->SetStringField(TEXT("endpoint_name"), EndpointName);
		Result.Data->SetStringField(TEXT("signature"),
			Signature == ESequenceEventEndpointSignature::BoundObject ? TEXT("BoundObject") : TEXT("NoParams"));
		if (DirectorBP && DirectorBP->GeneratedClass)
		{
			Result.Data->SetStringField(TEXT("function_class"), DirectorBP->GeneratedClass->GetPathName());
		}
	}
	return Result;
}
