// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetRendererProperty.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetRendererProperty::GetOperation() const { return TEXT("set_renderer_property"); }

FString ClaireonNiagaraTool_SetRendererProperty::GetDescription() const
{
	return TEXT("Set a property on a renderer attached to an emitter.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetRendererProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddInteger(TEXT("renderer_index"), TEXT("Renderer index."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set."), true);
	Builder.AddString(TEXT("value"), TEXT("String-encoded property value."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetRendererProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 RendererIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("renderer_index"), RendererIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: renderer_index"));
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
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter data for emitter %d"), EmitterIndex));
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Renderer index %d out of range (0-%d)"),
			RendererIndex, Renderers.Num() - 1));
	}

	UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Renderer Property")));
	System->Modify();

	if (!ClaireonNiagaraHelpers::SetObjectProperty(Renderer, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_renderer_property -> Set '%s' = '%s' on renderer %d of emitter %d"),
		*PropertyName, *Value, RendererIndex, EmitterIndex);
	return BuildStateResponse(SessionId, Data);
}
