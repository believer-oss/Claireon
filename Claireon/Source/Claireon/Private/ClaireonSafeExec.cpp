// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"

#include "BlueprintEditorLibrary.h"
#include "Engine/Blueprint.h"

static bool bLastExecutionCrashed = false;

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// EXCEPTION_EXECUTE_HANDLER may be undefined after HideWindowsPlatformTypes
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

// UE's check() raises SEH exception via RaiseException() with this code.
// Defined in WindowsPlatformCrashContext.cpp.
// Used for log messages only, not control flow.
static constexpr uint32 CLAIREON_UE_ASSERT_EXCEPTION_CODE = 0x4000;

// Context struct for the tool execution trampoline. All fields are
// pointers/references owned by the caller -- no C++ objects with destructors.
struct FExecuteToolContext
{
	IClaireonTool* Tool;
	const TSharedPtr<FJsonObject>* Args;
	IClaireonTool::FToolResult* OutResult;
};

// Trampoline for tool execution: calls Tool->Execute() and writes result.
// This function has C++ temporaries (FToolResult return value) on its stack,
// but it does NOT contain __try/__except. Under /EHsc, if an SEH exception
// fires inside Execute(), the temporaries leak -- accepted to prevent crash.
static void ExecuteToolTrampoline(void* Context)
{
	FExecuteToolContext* Ctx = static_cast<FExecuteToolContext*>(Context);
	*Ctx->OutResult = Ctx->Tool->Execute(*Ctx->Args);
}

// Trampoline: casts void* back to TFunctionRef and invokes it
static void ActionTrampoline(void* Context)
{
	(*static_cast<TFunctionRef<void()>*>(Context))();
}

// Context for the generated-class lookup trampoline.  Mirrors the
// ExecuteToolTrampoline shape: raw pointer fields only, no destructors.
struct FGeneratedClassLookupContext
{
	UBlueprint* Blueprint;
	UClass** OutClass;
};

// Trampoline invoked inside GuardedCallSEH: calls
// UBlueprintEditorLibrary::GeneratedClass(bp) and writes the result
// pointer.  The call is a single static UFUNCTION dispatch, which
// matches what `unreal.BlueprintEditorLibrary.generated_class(bp)`
// exposes to the Python reflection layer.  See GAP6_REPRO_ARTIFACT.md.
static void GeneratedClassLookupTrampoline(void* Context)
{
	FGeneratedClassLookupContext* Ctx =
		static_cast<FGeneratedClassLookupContext*>(Context);
	*Ctx->OutClass = UBlueprintEditorLibrary::GeneratedClass(Ctx->Blueprint);
}

// Pure-SEH inner function. No C++ objects with destructors on this frame.
// Calls through a raw function pointer to avoid C2712.
__declspec(noinline) static uint32 GuardedCallSEH(
	void (*Fn)(void*),
	void* FnContext,
	TCHAR* OutExceptionMsg,
	int32 ExceptionMsgLen)
{
	__try
	{
		Fn(FnContext);
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		uint32 Code = GetExceptionCode();
		FCString::Strncpy(OutExceptionMsg, GErrorHist, ExceptionMsgLen);
		return Code;
	}
}

// Outer function -- C++ objects are safe here (outside SEH frame)
FClaireonSafeExecResult ClaireonSafeExec::ExecuteTool(
	IClaireonTool* Tool,
	const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonSafeExecResult Result;
	TCHAR ExceptionMsg[2048] = {};

	FExecuteToolContext Ctx;
	Ctx.Tool = Tool;
	Ctx.Args = &Arguments;
	Ctx.OutResult = &Result.ToolResult;

	uint32 ExceptionCode = GuardedCallSEH(
		&ExecuteToolTrampoline, &Ctx, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

	if (ExceptionCode != 0)
	{
		Result.bCaughtFatalException = true;
		Result.ExceptionCode = ExceptionCode;
		Result.ExceptionDescription = FString(ExceptionMsg);

		// Overwrite ToolResult with a well-formed error (safe -- outside SEH)
		Result.ToolResult.bIsError = true;
		Result.ToolResult.ErrorMessage = FString::Printf(
			TEXT("FATAL: Caught SEH exception 0x%08X during tool execution. ")
				TEXT("Editor state may be corrupted -- restart recommended. %s"),
			ExceptionCode, ExceptionMsg);
		Result.ToolResult.Data.Reset();
		Result.ToolResult.Warnings.Empty();

		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: Caught SEH exception 0x%08X: %s"),
			ExceptionCode, ExceptionMsg);
	}
	else
	{
		bLastExecutionCrashed = false;
	}

	return Result;
}

FClaireonSafeActionResult ClaireonSafeExec::ExecuteAction(TFunctionRef<void()> Action)
{
	FClaireonSafeActionResult Result;
	TCHAR ExceptionMsg[2048] = {};

	uint32 ExceptionCode = GuardedCallSEH(
		&ActionTrampoline, &Action, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

	if (ExceptionCode != 0)
	{
		Result.bSuccess = false;
		Result.bCaughtFatalException = true;
		Result.ExceptionDescription = FString::Printf(
			TEXT("SEH exception 0x%08X: %s"), ExceptionCode, ExceptionMsg);
		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: Caught SEH exception 0x%08X: %s"),
			ExceptionCode, ExceptionMsg);
	}
	// Note: ExecuteAction does NOT clear bLastExecutionCrashed on success.
	// Only ExecuteTool clears the flag (deliberate -- a successful deferred action
	// does not indicate the editor has recovered from a tool execution crash).

	return Result;
}

// Gap 6 of #0000: SEH-guarded wrapper around UBlueprintEditorLibrary::GeneratedClass.
// Callstack analysis is recorded in BP_AUTHORING_GAPS_CLOSURE/GAP6_REPRO_ARTIFACT.md.
// Rationale for the helper-based guard (over REPL interception): the crash
// originates in engine code reached via a well-defined UFUNCTION entry
// point, so a single __try/__except around the same entry point matches
// the existing ExecuteTool / ExecuteAction shape and keeps the guard out
// of the Python dispatch path.
FClaireonGeneratedClassLookupResult ClaireonSafeExec::ExecuteGeneratedClassLookup(UBlueprint* Blueprint)
{
	FClaireonGeneratedClassLookupResult Result;
	TCHAR ExceptionMsg[2048] = {};

	UClass* OutClass = nullptr;
	FGeneratedClassLookupContext Ctx;
	Ctx.Blueprint = Blueprint;
	Ctx.OutClass = &OutClass;

	uint32 ExceptionCode = GuardedCallSEH(
		&GeneratedClassLookupTrampoline, &Ctx, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

	if (ExceptionCode != 0)
	{
		Result.bCaughtFatalException = true;
		Result.ExceptionCode = ExceptionCode;
		Result.ExceptionDescription = FString::Printf(
			TEXT("SEH exception 0x%08X during BlueprintEditorLibrary::GeneratedClass: %s"),
			ExceptionCode, ExceptionMsg);
		Result.OutClass = nullptr;
		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: %s"), *Result.ExceptionDescription);
	}
	else
	{
		Result.OutClass = OutClass;
	}

	return Result;
}

#else // !PLATFORM_WINDOWS

FClaireonSafeExecResult ClaireonSafeExec::ExecuteTool(
	IClaireonTool* Tool,
	const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonSafeExecResult Result;
	try
	{
		Result.ToolResult = Tool->Execute(Arguments);
		bLastExecutionCrashed = false;
	}
	catch (...)
	{
		Result.bCaughtFatalException = true;
		Result.ExceptionDescription = TEXT("Caught unknown C++ exception during tool execution.");
		Result.ToolResult.bIsError = true;
		Result.ToolResult.ErrorMessage = Result.ExceptionDescription;
		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: %s"), *Result.ExceptionDescription);
	}
	return Result;
}

FClaireonSafeActionResult ClaireonSafeExec::ExecuteAction(TFunctionRef<void()> Action)
{
	FClaireonSafeActionResult Result;
	try
	{
		Action();
	}
	catch (...)
	{
		Result.bSuccess = false;
		Result.bCaughtFatalException = true;
		Result.ExceptionDescription = TEXT("Caught unknown C++ exception during action execution.");
		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: %s"), *Result.ExceptionDescription);
	}
	// Note: ExecuteAction does NOT clear bLastExecutionCrashed on success.
	return Result;
}

FClaireonGeneratedClassLookupResult ClaireonSafeExec::ExecuteGeneratedClassLookup(UBlueprint* Blueprint)
{
	FClaireonGeneratedClassLookupResult Result;
	try
	{
		Result.OutClass = UBlueprintEditorLibrary::GeneratedClass(Blueprint);
	}
	catch (...)
	{
		Result.bCaughtFatalException = true;
		Result.ExceptionDescription = TEXT("Caught unknown C++ exception in BlueprintEditorLibrary::GeneratedClass.");
		Result.OutClass = nullptr;
		bLastExecutionCrashed = true;

		UE_LOG(LogClaireon, Error, TEXT("ClaireonSafeExec: %s"), *Result.ExceptionDescription);
	}
	return Result;
}

#endif // PLATFORM_WINDOWS

bool ClaireonSafeExec::DidLastExecutionCrash()
{
	return bLastExecutionCrashed;
}

void ClaireonSafeExec::SetCrashFlag()
{
	bLastExecutionCrashed = true;
}

void ClaireonSafeExec::ClearCrashFlag()
{
	bLastExecutionCrashed = false;
}
