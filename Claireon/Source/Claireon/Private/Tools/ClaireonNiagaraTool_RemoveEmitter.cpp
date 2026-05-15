// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RemoveEmitter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RemoveEmitter::GetName() const
{
	return TEXT("claireon.niagara_remove_emitter");
}

FString ClaireonNiagaraTool_RemoveEmitter::GetDescription() const
{
	return TEXT("Remove an emitter from the Niagara System by index.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_RemoveEmitter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Index of the emitter to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_RemoveEmitter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FString RemovedName = Handles[EmitterIndex].GetName().ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Emitter")));
	System->Modify();

	System->RemoveEmitterHandle(Handles[EmitterIndex]);
	System->MarkPackageDirty();

	if (Data->FocusedEmitterIndex == EmitterIndex)
	{
		Data->FocusedEmitterIndex = -1;
	}
	else if (Data->FocusedEmitterIndex > EmitterIndex)
	{
		Data->FocusedEmitterIndex--;
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_emitter -> Removed emitter %d (%s)"),
		EmitterIndex, *RemovedName);
	return BuildStateResponse(SessionId, Data);
}
