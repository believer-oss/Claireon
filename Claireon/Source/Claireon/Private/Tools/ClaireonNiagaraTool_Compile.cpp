// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Compile.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraScript.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Compile::GetName() const
{
	return TEXT("claireon.niagara_compile");
}

FString ClaireonNiagaraTool_Compile::GetDescription() const
{
	return TEXT("Request a (synchronous-ish) compile of the Niagara System and report errors/warnings.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Compile::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UNiagaraSystem* System = Data->System.Get();

	System->RequestCompile(true);

	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutSeconds = 5.0;
	while (System->HasOutstandingCompilationRequests())
	{
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			Data->LastOperationStatus = TEXT("compile -> Timed out after 5s (compilation may still be in progress)");
			return MakeErrorResult(TEXT("Compilation timed out after 5 seconds. The system may still be compiling in the background."));
		}
		FPlatformProcess::Sleep(0.01f);
	}

	FString Output;
	Output += TEXT("=== Compilation Results ===\n");

	bool bAllSuccess = true;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		auto CheckScript = [&](UNiagaraScript* Script, const FString& ScriptLabel)
		{
			if (!Script)
			{
				return;
			}

			ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
			if (Status == ENiagaraScriptCompileStatus::NCS_Dirty)
			{
				Warnings.Add(FString::Printf(TEXT("Emitter %d (%s) %s: needs compile"), i, *Handle.GetName().ToString(), *ScriptLabel));
			}
			else if (Status == ENiagaraScriptCompileStatus::NCS_Error)
			{
				bAllSuccess = false;
				Errors.Add(FString::Printf(TEXT("Emitter %d (%s) %s: compile error"), i, *Handle.GetName().ToString(), *ScriptLabel));
			}
		};

		CheckScript(EmitterData->GetGPUComputeScript(), TEXT("GPU Compute"));
	}

	Output += FString::Printf(TEXT("Success: %s\n"), bAllSuccess ? TEXT("true") : TEXT("false"));
	Output += FString::Printf(TEXT("Errors: %d\n"), Errors.Num());
	for (const FString& Err : Errors)
	{
		Output += FString::Printf(TEXT("  - %s\n"), *Err);
	}
	Output += FString::Printf(TEXT("Warnings: %d\n"), Warnings.Num());
	for (const FString& Warn : Warnings)
	{
		Output += FString::Printf(TEXT("  - %s\n"), *Warn);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("compile -> %s (%d errors, %d warnings)"),
		bAllSuccess ? TEXT("Success") : TEXT("Failed"), Errors.Num(), Warnings.Num());

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetBoolField(TEXT("success"), bAllSuccess);
	RespData->SetNumberField(TEXT("error_count"), Errors.Num());
	RespData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	return MakeSuccessResult(RespData, Output);
}
