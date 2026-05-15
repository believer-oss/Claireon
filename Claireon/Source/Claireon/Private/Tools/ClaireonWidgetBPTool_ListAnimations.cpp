// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ListAnimations.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ListAnimations::GetName() const
{
    return TEXT("claireon.widgetbp_list_animations");
}

FString ClaireonWidgetBPTool_ListAnimations::GetDescription() const
{
    return TEXT("List UWidgetAnimation assets on the Widget Blueprint, with start/end times.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ListAnimations::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ListAnimations::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("list_animations"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ListAnimations(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ListAnimations(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TArray<TSharedPtr<FJsonValue>> AnimArray;
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (!Anim)
		{
			continue;
		}

		TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
		AnimObj->SetStringField(TEXT("name"), Anim->GetFName().ToString());
		AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
		AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
		AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("animations"), AnimArray);
	ResultObj->SetNumberField(TEXT("count"), AnimArray.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
