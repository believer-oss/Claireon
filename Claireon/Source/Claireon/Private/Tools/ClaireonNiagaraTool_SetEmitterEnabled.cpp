// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetEmitterEnabled.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetEmitterEnabled::GetOperation() const { return TEXT("set_emitter_enabled"); }

FString ClaireonNiagaraTool_SetEmitterEnabled::GetDescription() const
{
    return TEXT("Enable or disable an emitter in the Niagara System. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetEmitterEnabled::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Index of the emitter."), true);
	Builder.AddBoolean(TEXT("enabled"), TEXT("Whether the emitter is enabled."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetEmitterEnabled::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	bool bEnabled = true;
	if (!Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return MakeErrorResult(TEXT("Missing required parameter: enabled"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Emitter Enabled")));
	System->Modify();

	TArray<FNiagaraEmitterHandle>& MutableHandles = System->GetEmitterHandles();
	MutableHandles[EmitterIndex].SetIsEnabled(bEnabled, *System, true);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_emitter_enabled -> Set emitter %d (%s) enabled=%s"),
		EmitterIndex, *Handles[EmitterIndex].GetName().ToString(), bEnabled ? TEXT("true") : TEXT("false"));
	return BuildStateResponse(SessionId, Data);
}
