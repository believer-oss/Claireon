// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_SetDefault.h"
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
	bool MSSetDefault_BuildLiteralFromJson(FName DataType, const TSharedPtr<FJsonValue>& Value, FMetasoundFrontendLiteral& Out)
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

FString FClaireonMetaSoundTool_SetDefault::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_SetDefault::GetOperation() const { return TEXT("set_default"); }

FString FClaireonMetaSoundTool_SetDefault::GetDescription() const
{
	return TEXT("Set the default value of a MetaSound graph input by name.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_SetDefault::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	S.AddString(TEXT("input_name"), TEXT("Graph input name"), true);
	S.AddString(TEXT("data_type"), TEXT("Input data type (used to coerce value into a typed literal)"), true);
	S.AddString(TEXT("value"), TEXT("Default value"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_SetDefault::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString InputName, TypeStr;
	if (!Arguments->TryGetStringField(TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing input_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("data_type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing data_type"));
	}
	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid()) return MakeErrorResult(TEXT("Missing value"));

	FMetasoundFrontendLiteral Literal;
	if (!MSSetDefault_BuildLiteralFromJson(FName(*TypeStr), ValueJson, Literal))
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not build literal of type %s from value"), *TypeStr));
	}

	using namespace Metasound::Engine;
	FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase* BuilderObj = Registry.FindBuilderObject(Data->Asset.Get());
	if (!BuilderObj) return MakeErrorResult(TEXT("Builder object not found for session asset"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set MetaSound Default")));
	Data->Asset->Modify();

	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	BuilderObj->SetGraphInputDefault(FName(*InputName), Literal, Result);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("SetGraphInputDefault failed for '%s'"), *InputName));
	}

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Set MetaSound input '%s' default"), *InputName);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("input_name"), InputName);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
#endif
}
