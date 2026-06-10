// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_AddEmitter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonNiagaraEditInternal.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEditorUtilities.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_AddEmitter::GetOperation() const { return TEXT("add_emitter"); }

FString ClaireonNiagaraTool_AddEmitter::GetDescription() const
{
    return TEXT("Add a new emitter to the Niagara System using the default empty emitter template. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_AddEmitter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("emitter_name"), TEXT("Name for the new emitter."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_AddEmitter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString EmitterName;
	if (!Arguments->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_name"));
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Emitter")));
	System->Modify();

	UNiagaraEmitter* TemplateEmitter = nullptr;
	FSoftObjectPath DefaultEmptyPath = GetDefault<UNiagaraEditorSettings>()->DefaultEmptyEmitter;
	if (DefaultEmptyPath.IsValid())
	{
		TemplateEmitter = Cast<UNiagaraEmitter>(DefaultEmptyPath.TryLoad());
	}
	if (!TemplateEmitter)
	{
		return MakeErrorResult(TEXT("Could not load default empty emitter template. Check NiagaraEditorSettings.DefaultEmptyEmitter"));
	}

	FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *TemplateEmitter, TemplateEmitter->GetExposedVersion().VersionGuid);

	int32 NewEmitterIndex = System->GetEmitterHandles().Num() - 1;
	ClaireonNiagaraEditInternal::EnsureEmitterGraphChains(System, NewEmitterIndex);

	if (NewEmitterIndex >= 0)
	{
		System->GetEmitterHandles()[NewEmitterIndex].SetName(FName(*EmitterName), *System);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_emitter -> Added emitter '%s' (index %d)"),
		*EmitterName, System->GetEmitterHandles().Num() - 1);
	return BuildStateResponse(SessionId, Data);
}
