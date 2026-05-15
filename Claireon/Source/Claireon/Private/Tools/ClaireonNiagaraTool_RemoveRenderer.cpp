// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RemoveRenderer.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RemoveRenderer::GetName() const
{
	return TEXT("claireon.niagara_remove_renderer");
}

FString ClaireonNiagaraTool_RemoveRenderer::GetDescription() const
{
	return TEXT("Remove a renderer from an emitter by index.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_RemoveRenderer::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddInteger(TEXT("renderer_index"), TEXT("Renderer index to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_RemoveRenderer::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UNiagaraRendererProperties* RendererToRemove = Renderers[RendererIndex];
	FString RendererName = ClaireonNiagaraHelpers::GetRendererTypeName(RendererToRemove);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Renderer")));
	System->Modify();

	Handle.GetInstance().Emitter->RemoveRenderer(RendererToRemove, Handle.GetInstance().Version);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_renderer -> Removed %s (index %d) from emitter %d"),
		*RendererName, RendererIndex, EmitterIndex);
	return BuildStateResponse(SessionId, Data);
}
