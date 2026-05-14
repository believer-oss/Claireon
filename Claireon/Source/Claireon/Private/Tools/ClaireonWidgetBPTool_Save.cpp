// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "ClaireonSafeExec.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Save::GetName() const
{
    return TEXT("claireon.widgetbp_save");
}

FString ClaireonWidgetBPTool_Save::GetDescription() const
{
    return TEXT("Save the Widget Blueprint in the open editing session to disk. Requires open session_id from claireon.widgetbp_open. Immediate-write to the .uasset on disk. Compiles before saving and reports compile errors. Run save periodically during long edit sessions so changes survive editor crashes.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Save::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("save"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	UPackage* Package = WBP->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	FSavePackageArgs SaveArgs;
	const bool bSaved = UPackage::SavePackage(Package, WBP, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
	}

	Data->bModified = false;

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResultJson->SetBoolField(TEXT("success"), true);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeSuccessResult(ResultJson, ResultString);
}

