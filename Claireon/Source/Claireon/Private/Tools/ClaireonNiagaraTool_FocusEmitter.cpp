// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_FocusEmitter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_FocusEmitter::GetName() const
{
	return TEXT("claireon.niagara_focus_emitter");
}

FString ClaireonNiagaraTool_FocusEmitter::GetDescription() const
{
	return TEXT("Focus on a specific emitter in the Niagara edit session. Use -1 for system level.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_FocusEmitter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index (-1 for system level)."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_FocusEmitter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index (-1 for system level)"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex != -1 && (EmitterIndex < 0 || EmitterIndex >= Handles.Num()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d, or -1 for system level)"),
			EmitterIndex, Handles.Num() - 1));
	}

	Data->FocusedEmitterIndex = EmitterIndex;

	Data->LastOperationStatus = EmitterIndex < 0
		? TEXT("focus_emitter -> Focused on system level")
		: FString::Printf(TEXT("focus_emitter -> Focused on emitter %d (%s)"),
			  EmitterIndex, *Handles[EmitterIndex].GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}
