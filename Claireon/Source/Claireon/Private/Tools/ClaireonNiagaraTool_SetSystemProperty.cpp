// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetSystemProperty.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetSystemProperty::GetOperation() const { return TEXT("set_system_property"); }

FString ClaireonNiagaraTool_SetSystemProperty::GetDescription() const
{
    return TEXT("Set a property on the open Niagara System by name, coerced via reflection. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetSystemProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set."), true);
	Builder.AddString(TEXT("value"), TEXT("String-encoded property value."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetSystemProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara System Property")));
	System->Modify();

	if (!ClaireonNiagaraHelpers::SetObjectProperty(System, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_system_property -> Set '%s' = '%s'"),
		*PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}
