// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Compile.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Compile::GetName() const
{
    return TEXT("claireon.widgetbp_compile");
}

FString ClaireonWidgetBPTool_Compile::GetDescription() const
{
    return TEXT("Compile the Widget Blueprint associated with a session. Returns compilation diagnostics.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Compile::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("compile"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_Compile(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_Compile(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::BatchCompile);

	const bool bCompileSuccess = (WBP->Status == BS_UpToDate || WBP->Status == BS_UpToDateWithWarnings);
	FString CompileStatus = bCompileSuccess ? TEXT("Success") : TEXT("Failed");

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("compile_status"), CompileStatus);
	ResultJson->SetBoolField(TEXT("success"), bCompileSuccess);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
