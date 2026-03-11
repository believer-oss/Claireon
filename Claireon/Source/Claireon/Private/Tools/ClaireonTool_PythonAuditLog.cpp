// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PythonAuditLog.h"
#include "ClaireonPythonAuditLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString ClaireonTool_PythonAuditLog::GetName() const
{
	return TEXT("editor.python.getAuditLog");
}

FString ClaireonTool_PythonAuditLog::GetDescription() const
{
	return TEXT("Retrieve the Python invocation audit log with optional filtering. Shows recent script executions, their success status, duration, and optionally the full script text.");
}

TSharedPtr<FJsonObject> ClaireonTool_PythonAuditLog::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// limit - optional
	TSharedPtr<FJsonObject> LimitProp = MakeShared<FJsonObject>();
	LimitProp->SetStringField(TEXT("type"), TEXT("integer"));
	LimitProp->SetStringField(TEXT("description"), TEXT("Number of recent entries to return (default: 50, max: 500)"));
	Properties->SetObjectField(TEXT("limit"), LimitProp);

	// filter_success - optional
	TSharedPtr<FJsonObject> FilterSuccessProp = MakeShared<FJsonObject>();
	FilterSuccessProp->SetStringField(TEXT("type"), TEXT("boolean"));
	FilterSuccessProp->SetStringField(TEXT("description"), TEXT("If set, filter to only successful (true) or failed (false) invocations"));
	Properties->SetObjectField(TEXT("filter_success"), FilterSuccessProp);

	// include_scripts - optional
	TSharedPtr<FJsonObject> IncludeScriptsProp = MakeShared<FJsonObject>();
	IncludeScriptsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeScriptsProp->SetStringField(TEXT("description"), TEXT("Include full script text inline for each entry (default: false)"));
	Properties->SetObjectField(TEXT("include_scripts"), IncludeScriptsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PythonAuditLog::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse parameters
	int32 Limit = 50;
	if (Arguments->HasField(TEXT("limit")))
	{
		Limit = static_cast<int32>(Arguments->GetNumberField(TEXT("limit")));
		Limit = FMath::Clamp(Limit, 1, 500);
	}

	TOptional<bool> FilterSuccess;
	if (Arguments->HasField(TEXT("filter_success")))
	{
		FilterSuccess = Arguments->GetBoolField(TEXT("filter_success"));
	}

	bool bIncludeScripts = false;
	if (Arguments->HasField(TEXT("include_scripts")))
	{
		bIncludeScripts = Arguments->GetBoolField(TEXT("include_scripts"));
	}

	// Get recent entries as JSON
	FString EntriesJson = FClaireonPythonAuditLog::Get().GetRecentEntries(Limit, FilterSuccess);

	// If include_scripts is not requested, return entries as-is
	if (!bIncludeScripts)
	{
		return MakeSuccessResult(nullptr, EntriesJson);
	}

	// Parse the entries JSON to augment with full script text
	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EntriesJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return MakeSuccessResult(nullptr, EntriesJson);
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), EntriesArray))
	{
		return MakeSuccessResult(nullptr, EntriesJson);
	}

	const FString AuditDir = FClaireonPythonAuditLog::Get().GetAuditLogDir();

	// For each entry, read the script file and add it to the JSON
	for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
	{
		TSharedPtr<FJsonObject> EntryObj = EntryValue->AsObject();
		if (!EntryObj.IsValid())
		{
			continue;
		}

		FString EntryId;
		if (!EntryObj->TryGetStringField(TEXT("id"), EntryId))
		{
			continue;
		}

		// Read the script file
		const FString ScriptPath = AuditDir / TEXT("scripts") / (EntryId + TEXT(".py"));
		FString ScriptText;
		if (FFileHelper::LoadFileToString(ScriptText, *ScriptPath))
		{
			EntryObj->SetStringField(TEXT("script"), ScriptText);
		}
	}

	// Re-serialize the augmented JSON
	FString OutputString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer);

	return MakeSuccessResult(nullptr, OutputString);
}
