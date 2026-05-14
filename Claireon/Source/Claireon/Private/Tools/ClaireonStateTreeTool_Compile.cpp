// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Compile.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLog.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Compile::GetOperation() const { return TEXT("compile"); }

FString ClaireonStateTreeTool_Compile::GetDescription() const
{
	return TEXT("Compile the State Tree in the open editing session and report success/failure with diagnostics. Requires open session_id from statetree_open. Read-only with respect to authoring data (compilation only writes derived runtime data). Run after structural edits to verify the asset is valid.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Compile::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UStateTree* StateTree = Data->StateTree.Get();

	FStateTreeCompilerLog CompilerLog;
	bool bSuccess = UStateTreeEditingSubsystem::CompileStateTree(StateTree, CompilerLog);

	FString Output;
	if (bSuccess)
	{
		Output = TEXT("=== Compilation Succeeded ===\n");
	}
	else
	{
		Output = TEXT("=== Compilation Failed ===\n");
	}

	CompilerLog.DumpToLog(LogClaireon);

	if (!bSuccess)
	{
		Output += TEXT("Check editor log (LogClaireon) for detailed compilation errors.\n");
	}

	Data->LastOperationStatus = bSuccess ? TEXT("compile -> Succeeded") : TEXT("compile -> Failed");

	if (bSuccess)
	{
		TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
		RespData->SetBoolField(TEXT("success"), true);
		return MakeSuccessResult(RespData, Output);
	}
	return MakeErrorResult(Output);
}
