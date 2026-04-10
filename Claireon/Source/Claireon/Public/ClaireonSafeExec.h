// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

struct FClaireonSafeExecResult
{
	IClaireonTool::FToolResult ToolResult;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
	uint32 ExceptionCode = 0;
};

struct FClaireonSafeActionResult
{
	bool bSuccess = true;
	bool bCaughtFatalException = false;
	FString ExceptionDescription;
};

namespace ClaireonSafeExec
{
	FClaireonSafeExecResult ExecuteTool(IClaireonTool* Tool, const TSharedPtr<FJsonObject>& Arguments);
	FClaireonSafeActionResult ExecuteAction(TFunctionRef<void()> Action);
	bool DidLastExecutionCrash();
	void SetCrashFlag();
	void ClearCrashFlag();
} // namespace ClaireonSafeExec
