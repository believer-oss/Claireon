// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_AddInput.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

#if CLAIREON_HAS_METASOUND_BUILDER_API
namespace
{
	// File-local discriminator prefix (MSAddInput_) avoids unity-batch name collisions with
	// the same helper names in other ClaireonMetaSoundTool_*.cpp files.
	bool MSAddInput_BuildLiteralFromJson(FName DataType, const TSharedPtr<FJsonValue>& Value, FMetasoundFrontendLiteral& Out)
	{
		const FString TypeStr = DataType.ToString();
		if (TypeStr == TEXT("Bool"))
		{
			bool B = false;
			if (Value.IsValid() && Value->TryGetBool(B)) { Out.Set(B); return true; }
			return false;
		}
		if (TypeStr == TEXT("Float") || TypeStr == TEXT("Time"))
		{
			double N = 0;
			if (Value.IsValid() && Value->TryGetNumber(N)) { Out.Set(static_cast<float>(N)); return true; }
			return false;
		}
		if (TypeStr == TEXT("Int32"))
		{
			double N = 0;
			if (Value.IsValid() && Value->TryGetNumber(N)) { Out.Set(static_cast<int32>(N)); return true; }
			return false;
		}
		if (TypeStr == TEXT("String"))
		{
			FString S;
			if (Value.IsValid() && Value->TryGetString(S)) { Out.Set(S); return true; }
			return false;
		}
		if (Value.IsValid())
		{
			bool B; double N; FString S;
			if (Value->TryGetBool(B)) { Out.Set(B); return true; }
			if (Value->TryGetNumber(N)) { Out.Set(static_cast<float>(N)); return true; }
			if (Value->TryGetString(S)) { Out.Set(S); return true; }
		}
		return false;
	}
}
#endif

FString FClaireonMetaSoundTool_AddInput::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_AddInput::GetOperation() const { return TEXT("add_input"); }

FString FClaireonMetaSoundTool_AddInput::GetDescription() const
{
	return TEXT("Add a graph input to the MetaSound within the current session (e.g. name='Volume', "
				"data_type='Float', default=0.5). Registered with the FMetaSoundFrontendDocumentBuilder "
				"and immediately visible in the graph. Requires session_id from metasound.open.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_AddInput::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	S.AddString(TEXT("name"), TEXT("Input name"), true);
	S.AddString(TEXT("data_type"), TEXT("Input data type (Bool, Float, Int32, String, Audio, Trigger, Time, etc.)"), true);
	S.AddString(TEXT("default"), TEXT("Optional default literal value"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_AddInput::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Name, TypeStr;
	if (!Arguments->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty()) return MakeErrorResult(TEXT("Missing name"));
	if (!Arguments->TryGetStringField(TEXT("data_type"), TypeStr) || TypeStr.IsEmpty()) return MakeErrorResult(TEXT("Missing data_type"));

	using namespace Metasound::Engine;
	FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase* BuilderObj = Registry.FindBuilderObject(Data->Asset.Get());
	if (!BuilderObj) return MakeErrorResult(TEXT("Builder object not found for session asset"));

	FMetasoundFrontendLiteral Default;
	const TSharedPtr<FJsonValue> DefaultJson = Arguments->TryGetField(TEXT("default"));
	if (DefaultJson.IsValid())
	{
		MSAddInput_BuildLiteralFromJson(FName(*TypeStr), DefaultJson, Default);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add MetaSound Input")));
	Data->Asset->Modify();

	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	BuilderObj->AddGraphInputNode(FName(*Name), FName(*TypeStr), Default, Result, /*bIsConstructorInput=*/false);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("AddGraphInputNode failed for '%s' (type %s)"), *Name, *TypeStr));
	}

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Added MetaSound input '%s' (%s)"), *Name, *TypeStr);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("input_name"), Name);
	Out->SetStringField(TEXT("data_type"), TypeStr);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
#endif
}
