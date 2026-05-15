// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetEmitterProperty.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetEmitterProperty::GetName() const
{
	return TEXT("claireon.niagara_set_emitter_property");
}

FString ClaireonNiagaraTool_SetEmitterProperty::GetDescription() const
{
	return TEXT("Set a property on an emitter instance.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetEmitterProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set."), true);
	Builder.AddString(TEXT("value"), TEXT("String-encoded property value."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetEmitterProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	int32 EmitterIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
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
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
	if (!Emitter)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter instance for emitter %d"), EmitterIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Emitter Property")));
	System->Modify();

	if (!ClaireonNiagaraHelpers::SetObjectProperty(Emitter, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_emitter_property -> Set '%s' = '%s' on emitter %d (%s)"),
		*PropertyName, *Value, EmitterIndex, *Handle.GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}
