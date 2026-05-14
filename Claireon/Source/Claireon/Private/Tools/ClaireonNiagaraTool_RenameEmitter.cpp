// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RenameEmitter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RenameEmitter::GetOperation() const { return TEXT("rename_emitter"); }

FString ClaireonNiagaraTool_RenameEmitter::GetDescription() const
{
	return TEXT("Rename an emitter in the Niagara System.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_RenameEmitter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Index of the emitter to rename."), true);
	Builder.AddString(TEXT("new_name"), TEXT("New name for the emitter."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_RenameEmitter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NewName;
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FString OldName = Handles[EmitterIndex].GetName().ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Niagara Emitter")));
	System->Modify();

	TArray<FNiagaraEmitterHandle>& MutableHandles = System->GetEmitterHandles();
	MutableHandles[EmitterIndex].SetName(FName(*NewName), *System);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("rename_emitter -> Renamed emitter %d from '%s' to '%s'"),
		EmitterIndex, *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}
