// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_ApplySpec.h"
#include "Tools/ClaireonSpecApplicator_Audio.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonMetaSoundTool_ApplySpec::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_ApplySpec::GetOperation() const { return TEXT("apply_spec"); }

FString FClaireonMetaSoundTool_ApplySpec::GetDescription() const
{
	return TEXT("Apply a MetaSound declarative spec in one transactional pass. Accepts either "
				"{kind=\"MetaSoundSource\", ...} or {kind=\"MetaSoundPatch\", ...} (both share "
				"IMetaSoundDocumentInterface). Spec body sections (all optional): "
				"interfaces:[name,...]; inputs:[{name,type,default?}]; outputs:[{name,type,default?}]; "
				"nodes:[{class_namespace,class_name,class_variant?,major_version?}]; "
				"input_defaults:[{name,type,value}]; connections:[{graph_input_name,graph_output_name}]. "
				"Non-session/stateless. Rollback-on-failure deletes the asset if it was created by "
				"this call. Use claireon.metasound_list_available_interfaces (D4) to discover interface "
				"names. D7 + B24-style rollback.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_ApplySpec::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddObject(TEXT("spec"), TEXT("MetaSoundSource spec object (kind=\"MetaSoundSource\")"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_ApplySpec::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	const TSharedPtr<FJsonObject>* SpecObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("spec"), SpecObj) || !SpecObj || !SpecObj->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: spec"));
	}

	FClaireonSpecApplicator_Audio Applicator;
	FString Summary, Error;
	if (!Applicator.ApplyMetaSoundSpec(*SpecObj, Summary, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("summary"), Summary);
	return MakeSuccessResult(Out, Summary);
}
