// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_AddRenderer.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_AddRenderer::GetOperation() const { return TEXT("add_renderer"); }

FString ClaireonNiagaraTool_AddRenderer::GetDescription() const
{
    return TEXT("Add a renderer (e.g. Sprite, Mesh) to an emitter in the open Niagara session. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_AddRenderer::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index to add the renderer to."), true);
	Builder.AddString(TEXT("renderer_type"), TEXT("Renderer class short name (e.g. 'Sprite', 'Mesh', 'Ribbon')."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_AddRenderer::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString RendererType;
	if (!Arguments->TryGetStringField(TEXT("renderer_type"), RendererType) || RendererType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: renderer_type"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	UClass* RendererClass = ClaireonNiagaraHelpers::ResolveRendererClass(RendererType, Error);
	if (!RendererClass)
	{
		return MakeErrorResult(Error);
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter data for emitter %d"), EmitterIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Renderer")));
	System->Modify();

	UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
	UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass);
	Emitter->AddRenderer(NewRenderer, Handle.GetInstance().Version);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_renderer -> Added %s to emitter %d (%s)"),
		*RendererType, EmitterIndex, *Handle.GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}
