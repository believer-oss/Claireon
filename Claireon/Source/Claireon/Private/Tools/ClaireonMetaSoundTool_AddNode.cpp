// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_AddNode.h"
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

FString FClaireonMetaSoundTool_AddNode::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_AddNode::GetOperation() const { return TEXT("add_node"); }

FString FClaireonMetaSoundTool_AddNode::GetDescription() const
{
	return TEXT("Add a node to the MetaSound graph by class name (e.g. class_namespace='Math', class_name='Mul').");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	S.AddString(TEXT("class_name"), TEXT("MetaSound node class name (e.g. 'Mul')"), true);
	S.AddString(TEXT("class_namespace"), TEXT("MetaSound node class namespace (e.g. 'Math')"));
	S.AddString(TEXT("class_variant"), TEXT("Optional class variant"));
	S.AddInteger(TEXT("major_version"), TEXT("Optional major version (defaults to 1)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Namespace, Name, Variant;
	Arguments->TryGetStringField(TEXT("class_namespace"), Namespace);
	if (!Arguments->TryGetStringField(TEXT("class_name"), Name) || Name.IsEmpty()) return MakeErrorResult(TEXT("Missing class_name"));
	Arguments->TryGetStringField(TEXT("class_variant"), Variant);
	int32 MajorVersion = 1;
	{
		int32 MV = 0;
		if (Arguments->TryGetNumberField(TEXT("major_version"), MV)) MajorVersion = MV;
	}

	using namespace Metasound::Engine;
	FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
	UMetaSoundBuilderBase* BuilderObj = Registry.FindBuilderObject(Data->Asset.Get());
	if (!BuilderObj) return MakeErrorResult(TEXT("Builder object not found for session asset"));

	const FName NsName(*Namespace);
	const FName ClsName(*Name);
	const FName VarName(*Variant);
	FMetasoundFrontendClassName ClassName(NsName, ClsName, VarName);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add MetaSound Node")));
	Data->Asset->Modify();

	EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
	BuilderObj->AddNodeByClassName(ClassName, Result, MajorVersion);
	if (Result != EMetaSoundBuilderResult::Succeeded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("AddNodeByClassName failed for %s.%s (v%d)"),
			*Namespace, *Name, MajorVersion));
	}

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Added MetaSound node %s.%s"), *Namespace, *Name);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("class_namespace"), Namespace);
	Out->SetStringField(TEXT("class_name"), Name);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
#endif
}
