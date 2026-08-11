// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"

#include "BlueprintEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"

static bool bLastExecutionCrashed = false;

// ---------------------------------------------------------------------------
// Declared-vs-supplied argument validation (P1-2 / T1).
//
// This is the single funnel both transports pass through -- the HTTP server
// (ClaireonServer.cpp) and the Python bridge (ClaireonBridge.cpp) both reach
// tools only via ExecuteTool -- and it is the only place where the schema and
// the supplied arguments are both in hand. Doing it per-tool would mean editing
// ~700 Execute bodies that have no common params type.
// ---------------------------------------------------------------------------

namespace ClaireonSafeExecArgs
{
	// Arguments the TRANSPORT consumes, not the tool. Schemas do not declare
	// them (they are cross-cutting), so they must be accepted everywhere or the
	// gate would reject a flag the server itself documents.
	// File-local prefix to avoid anon-NS collisions under unity batching.
	static const TCHAR* const kCl612TransportArgs[] =
	{
		// Read by FClaireonServer::HandleToolCall and by several edit-tool bases.
		TEXT("suppress_output"),
	};

	// Case- and separator-insensitive key, so "actorId" and "actor_id" collapse
	// to the same string. Almost every misspelling reported in this catalog is
	// exactly that swap, and a suggestion is worth more than a bare rejection.
	static FString Cl612NormalizeParamName(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len());
		for (const TCHAR Ch : Name)
		{
			if (Ch != TEXT('_'))
			{
				Out.AppendChar(FChar::ToLower(Ch));
			}
		}
		return Out;
	}
}

bool ClaireonSafeExec::IsTransportLevelArgument(const FString& ArgumentName)
{
	for (const TCHAR* const Known : ClaireonSafeExecArgs::kCl612TransportArgs)
	{
		if (ArgumentName.Equals(Known, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

FString ClaireonSafeExec::ValidateArgumentsAgainstSchema(
	const IClaireonTool* Tool,
	const TSharedPtr<FJsonObject>& Arguments)
{
	if (Tool == nullptr || !Arguments.IsValid() || Arguments->Values.Num() == 0)
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
	if (!Schema.IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Schema->TryGetObjectField(TEXT("properties"), PropertiesPtr)
		|| PropertiesPtr == nullptr
		|| !(*PropertiesPtr).IsValid())
	{
		// No declared properties: there is nothing to validate against, so stay
		// permissive rather than rejecting every argument to such a tool.
		return FString();
	}

	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	TArray<FString> Unknown;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Supplied : Arguments->Values)
	{
		if (Properties->HasField(Supplied.Key) || IsTransportLevelArgument(Supplied.Key))
		{
			continue;
		}
		Unknown.Add(Supplied.Key);
	}

	if (Unknown.Num() == 0)
	{
		return FString();
	}

	Unknown.Sort();

	// An explicit loop, not TMap::GetKeys(TArray<FString>&): FJsonObject's key type is
	// UE::FSharedString on 5.8 (unless UE_JSONOBJECT_LEGACY_STRING_KEYS=1), so the
	// out-parameter has nowhere to convert and the call does not resolve (C2672).
	// `*Key` is `const TCHAR*` for both key types, so this builds on 5.5 through 5.8.
	TArray<FString> Declared;
	Declared.Reserve(Properties->Values.Num());
	for (const auto& Declaration : Properties->Values)
	{
		Declared.Add(FString(*Declaration.Key));
	}
	Declared.Sort();

	// Suggest the closest declared name for each unknown one.
	FString Detail;
	for (const FString& Bad : Unknown)
	{
		const FString BadKey = ClaireonSafeExecArgs::Cl612NormalizeParamName(Bad);
		const FString* Suggestion = Declared.FindByPredicate(
			[&BadKey](const FString& Candidate)
			{
				return ClaireonSafeExecArgs::Cl612NormalizeParamName(Candidate) == BadKey;
			});

		if (!Detail.IsEmpty())
		{
			Detail += TEXT("; ");
		}
		Detail += Suggestion != nullptr
			? FString::Printf(TEXT("'%s' (did you mean '%s'?)"), *Bad, **Suggestion)
			: FString::Printf(TEXT("'%s'"), *Bad);
	}

	return FString::Printf(
		TEXT("Tool '%s' does not accept %s: %s. Declared parameters: %s. "
			 "An argument the schema does not declare is never read, so the call would "
			 "have appeared to succeed while doing nothing."),
		*Tool->GetName(),
		Unknown.Num() == 1 ? TEXT("this argument") : TEXT("these arguments"),
		*Detail,
		Declared.Num() > 0 ? *FString::Join(Declared, TEXT(", ")) : TEXT("(none)"));
}

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"

// EXCEPTION_EXECUTE_HANDLER may be undefined after HideWindowsPlatformTypes
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

// UE's check() raises SEH exception via RaiseException() with this code.
// Defined in WindowsPlatformCrashContext.cpp (verified against UE 5.5 source).
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

namespace ClaireonSafeExecInternal
{

// Trampoline for tool execution: calls Tool->Execute() and writes result.
// This function has C++ temporaries (FToolResult return value) on its stack,
// but it does NOT contain __try/__except. Under /EHsc, if an SEH exception
// fires inside Execute(), the temporaries leak -- accepted to prevent crash.
void ExecuteToolTrampoline(void* Context)
{
	FExecuteToolContext* Ctx = static_cast<FExecuteToolContext*>(Context);
	*Ctx->OutResult = Ctx->Tool->Execute(*Ctx->Args);
}

// Trampoline: casts void* back to TFunctionRef and invokes it
void ActionTrampoline(void* Context)
{
	(*static_cast<TFunctionRef<void()>*>(Context))();
}

}  // namespace ClaireonSafeExecInternal

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
// exposes to the Python reflection layer.  See
// Docs/llm/archive/bp-authoring-gaps-closure/GAP6_REPRO_ARTIFACT.md.
namespace ClaireonSafeExecInternal
{

void GeneratedClassLookupTrampoline(void* Context)
{
	FGeneratedClassLookupContext* Ctx =
		static_cast<FGeneratedClassLookupContext*>(Context);
	*Ctx->OutClass = UBlueprintEditorLibrary::GeneratedClass(Ctx->Blueprint);
}

}  // namespace ClaireonSafeExecInternal

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

	// Reject undeclared arguments BEFORE executing: the tool would ignore them,
	// and a partial write done under a misread argument is worse than no write.
	{
		const FString ArgError = ValidateArgumentsAgainstSchema(Tool, Arguments);
		if (!ArgError.IsEmpty())
		{
			Result.ToolResult = IClaireonTool::MakeErrorResult(ArgError);
			return Result;
		}
	}

	FExecuteToolContext Ctx;
	Ctx.Tool = Tool;
	Ctx.Args = &Arguments;
	Ctx.OutResult = &Result.ToolResult;

	uint32 ExceptionCode = GuardedCallSEH(
		&ClaireonSafeExecInternal::ExecuteToolTrampoline, &Ctx, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

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
		&ClaireonSafeExecInternal::ActionTrampoline, &Action, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

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

// SEH-guarded wrapper around UBlueprintEditorLibrary::GeneratedClass.
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
		&ClaireonSafeExecInternal::GeneratedClassLookupTrampoline, &Ctx, ExceptionMsg, UE_ARRAY_COUNT(ExceptionMsg));

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

	// Same pre-execution gate as the Windows path. See the comment there.
	{
		const FString ArgError = ValidateArgumentsAgainstSchema(Tool, Arguments);
		if (!ArgError.IsEmpty())
		{
			Result.ToolResult = IClaireonTool::MakeErrorResult(ArgError);
			return Result;
		}
	}

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
