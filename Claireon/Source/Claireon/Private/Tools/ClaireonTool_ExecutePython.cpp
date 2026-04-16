// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_MapOpen.h"
#include "Tools/ClaireonTool_PIEStart.h"
#include "Tools/ClaireonTool_PIEStop.h"
#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "Tools/ClaireonTool_MapDuplicate.h"
#include "ClaireonBridge.h"
#include "ClaireonAutoSave.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "ClaireonPythonAuditLog.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "Async/Async.h"
#include "HAL/ThreadHeartBeat.h"
#include "ClaireonLogCapture.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// CPython C API headers for timeout enforcement
THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

TAtomic<int32> ClaireonTool_ExecutePython::TempFileCounter(0);

FString ClaireonTool_ExecutePython::GetName() const
{
	return TEXT("claireon.python_execute");
}

FString ClaireonTool_ExecutePython::GetDescription() const
{
	return TEXT("Execute Python code in Code Mode with access to the claireon.* bridge. "
				"The code runs in the Unreal Editor's Python environment with the 'unreal' module "
				"and the 'claireon' module available for calling other MCP tools. "
				"Use 'import claireon' to access tool functions. Call tools as claireon.<tool_name>(...). "
				"Run dir(claireon) to list available tools, or help(claireon.<tool_name>) for usage details.");
}

TSharedPtr<FJsonObject> ClaireonTool_ExecutePython::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// code - required
	TSharedPtr<FJsonObject> CodeProp = MakeShared<FJsonObject>();
	CodeProp->SetStringField(TEXT("type"), TEXT("string"));
	CodeProp->SetStringField(TEXT("description"), TEXT("Python code to execute. Has access to 'unreal' module and claireon.* bridge functions."));
	Properties->SetObjectField(TEXT("code"), CodeProp);

	// timeout_ms - optional
	TSharedPtr<FJsonObject> TimeoutProp = MakeShared<FJsonObject>();
	TimeoutProp->SetStringField(TEXT("type"), TEXT("integer"));
	TimeoutProp->SetStringField(TEXT("description"), TEXT("Execution timeout in milliseconds."));
	TimeoutProp->SetNumberField(TEXT("default"), DefaultTimeoutMs);
	Properties->SetObjectField(TEXT("timeout_ms"), TimeoutProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("code")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FString ClaireonTool_ExecutePython::GetPythonPrefix()
{
	return TEXT(
		"import warnings as _warnings\n"
		"_warnings.filterwarnings('ignore', category=DeprecationWarning)\n"
		"import unreal\n"
		"import claireon\n"
		"\n"
		"# --- user code begins here ---\n");
}

FString ClaireonTool_ExecutePython::GetPythonSuffix()
{
	return TEXT(
		"\n"
		"# --- user code ends here ---\n"
	);
}

IClaireonTool::FToolResult ClaireonTool_ExecutePython::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Step 1: Validate input
	FString Code;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("code"), Code) || Code.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing or empty 'code' parameter. Provide Python code in the 'code' parameter."));
	}

	// Enforce 64KB size limit
	if (Code.Len() > MaxScriptSizeBytes)
	{
		return MakeErrorResult(
			FString::Printf(TEXT("Script size %d bytes exceeds maximum of %d bytes. Break the code into smaller scripts."),
				Code.Len(), MaxScriptSizeBytes));
	}

	// Read timeout_ms parameter
	int32 TimeoutMs = DefaultTimeoutMs;
	if (Arguments->HasField(TEXT("timeout_ms")))
	{
		double TimeoutVal = DefaultTimeoutMs;
		Arguments->TryGetNumberField(TEXT("timeout_ms"), TimeoutVal);
		TimeoutMs = static_cast<int32>(TimeoutVal);
		if (TimeoutMs <= 0)
		{
			TimeoutMs = DefaultTimeoutMs;
		}
	}

	// Step 2: Ensure bridge is registered
	FClaireonBridge::EnsureRegistered();

	// Step 2b: Rebuild claireon module if tools changed since last execution
	if (FClaireonBridge::bClaireonModuleStale.exchange(false))
	{
		FClaireonBridge::RebuildClaireonModule();
	}

	// Step 3: Reset tool call counter
	FClaireonBridge::ResetToolCallCount();

	// Step 4: Generate temp .py file
	const FString PythonDir = FPaths::ProjectSavedDir() / TEXT("MCP") / TEXT("Python");
	IFileManager::Get().MakeDirectory(*PythonDir, true);

	const int32 FileIndex = ++TempFileCounter;
	const FString TempFileName = FString::Printf(TEXT("mcp_exec_%d_%d.py"),
		FPlatformProcess::GetCurrentProcessId(), FileIndex);
	const FString TempFilePath = PythonDir / TempFileName;

	// Indent user code by 4 spaces so it can be wrapped in try-except.
	// This lets us capture Python exceptions and surface them to the LLM
	// instead of the opaque "Python execution failed with no error details."
	FString IndentedCode;
	{
		TArray<FString> Lines;
		Code.ParseIntoArray(Lines, TEXT("\n"), false);
		for (const FString& Line : Lines)
		{
			IndentedCode += TEXT("    ") + Line + TEXT("\n");
		}
	}

	// Build the full script: prefix + try-wrapped user code + except + suffix.
	// The except block prints the traceback so it appears in PythonCommand.LogOutput.
	FString FullScript = GetPythonPrefix()
		+ TEXT("_mcp_error_handled = False\n")
		+ TEXT("try:\n")
		+ IndentedCode
		+ TEXT("except Exception as _mcp_user_error:\n")
		+ TEXT("    import traceback as _tb\n")
		+ TEXT("    _mcp_error_msg = _tb.format_exc()\n")
		+ TEXT("    print(_mcp_error_msg)\n")
		+ TEXT("    _mcp_error_handled = True\n")
		+ GetPythonSuffix();

	if (!FFileHelper::SaveStringToFile(FullScript, *TempFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return MakeErrorResult(
			FString::Printf(TEXT("Failed to write temp Python file: %s. Check disk space and file permissions."), *TempFilePath));
	}

	// Step 4b: Auto-save dirty packages before crash-risk Python execution
	FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/true);

	// Step 5: Construct FPythonCommandEx — use ExecuteFile (matches original editor.python.execute)
	FPythonCommandEx PythonCommand;
	PythonCommand.Command = TempFilePath;
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;
	PythonCommand.Flags = EPythonCommandFlags::Unattended;

	// Step 6: Suspend UE's hang detection while Python executes.
	// Python execution can block the game thread for >5s (imports, heavy tool calls),
	// which triggers FThreadHeartBeat's abort and FGameThreadHitchHeartBeat's abort.
	FSlowHeartBeatScope SuspendHeartBeat;
	FDisableHitchDetectorScope SuspendHitchDetector;

	// Step 7: Execute with engine log capture at the top level.
	// All claireon.* tool calls within Python flow through ClaireonBridge::MCPCallTool,
	// so capturing here covers the entire execution scope.
	FClaireonLogCapture EngineLogCapture;
	const double StartTimeSeconds = FPlatformTime::Seconds();
	const bool bPythonSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
	const double DurationMs = (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;
	const bool bTimedOut = false; // Watchdog timer not yet implemented — timeout_seconds is advisory only

	// Step 7b: Dispatch any deferred world-transition actions.
	// The barrier runs inside each deferred action's lambda (right before the
	// world transition), not here -- no reason to purge Python state early.
	if (FClaireonBridge::HasDeferredActions())
	{
		// Auto-save before world-transition actions (map load, PIE, etc.)
		FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/false);

		TArray<FClaireonDeferredAction> Actions = FClaireonBridge::DrainDeferredActions();
		for (const FClaireonDeferredAction& Action : Actions)
		{
			FClaireonSafeActionResult ActionResult = ClaireonSafeExec::ExecuteAction([&Action]()
			{
				switch (Action.Type)
				{
					case EClaireonDeferredActionType::LoadMap:
						ClaireonTool_MapOpen::ExecuteDeferredLoadMap(Action.Payload);
						break;
					case EClaireonDeferredActionType::PIEStart:
						ClaireonTool_PIEStart::ExecuteDeferredPIEStart(Action.Payload);
						break;
					case EClaireonDeferredActionType::PIEStop:
						ClaireonTool_PIEStop::ExecuteDeferredPIEStop();
						break;
					case EClaireonDeferredActionType::LiveCodingReload:
						ClaireonTool_LiveCodingReload::ExecuteDeferredLiveCodingReload(Action.Payload);
						break;
					case EClaireonDeferredActionType::DuplicateAndOpenMap:
						ClaireonTool_MapDuplicate::ExecuteDeferredDuplicateAndOpenMap(Action.Payload);
						break;
				}
			});

			if (ActionResult.bCaughtFatalException)
			{
				UE_LOG(LogClaireon, Error, TEXT("Deferred action %d crashed: %s"),
					(int32)Action.Type, *ActionResult.ExceptionDescription);
				break;
			}
		}
	}

	// Step 8: Capture logs (Python stdout/stderr and engine UE_LOG)
	FString Logs = FToolResult::BuildLogString(PythonCommand.LogOutput);
	FString EngineOutput = EngineLogCapture.GetCapturedOutput();

	// Step 9: Read tool call count
	int32 ToolCallCount = FClaireonBridge::GetToolCallCount();

	// Step 9b: Update crash flag based on execution outcome.
	if (!bPythonSuccess)
	{
		FClaireonAutoSave::SetCrashFlag();
	}
	else
	{
		FClaireonAutoSave::ClearCrashFlag();
	}

	// Clean up temp file
	IFileManager::Get().Delete(*TempFilePath);

	// Step 10: Build structured FToolResult
	FToolResult FinalResult;
	FinalResult.Logs = Logs;
	FinalResult.UELog = EngineOutput;

	if (bTimedOut)
	{
		// Timeout — plain text error
		FinalResult.bIsError = true;
		FinalResult.ErrorMessage = FString::Printf(TEXT("Execution timed out after %dms. Break the operation into smaller steps, or increase timeout_ms."), TimeoutMs);
	}
	else if (!bPythonSuccess)
	{
		FinalResult.bIsError = true;

		// Extract error message from logs
		FString ErrorMsg;
		for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
		{
			if (Entry.Type == EPythonLogOutputType::Error)
			{
				if (!ErrorMsg.IsEmpty())
					ErrorMsg += TEXT("\n");
				ErrorMsg += Entry.Output;
			}
		}

		if (ErrorMsg.IsEmpty())
		{
			if (!Logs.IsEmpty())
			{
				ErrorMsg = TEXT("Python execution failed. Output:\n") + Logs;
			}
			else
			{
				// Enhanced diagnostics for bridge-level failures (before user code ran)
				IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
				FString DiagInfo = PythonPlugin
					? TEXT("Bridge status: registered.")
					: TEXT("Bridge status: Python plugin not available.");

				ErrorMsg = FString::Printf(
					TEXT("Python execution failed at bridge level (before user code executed). "
						 "Possible causes: Python bridge not initialized, syntax error in script prefix, or module import failure. %s"),
					*DiagInfo);
			}
		}
		FinalResult.ErrorMessage = ErrorMsg;
	}
	else
	{
		// Success -- logs are the primary output
		FinalResult.Summary = TEXT("Execution completed.");
		if (ToolCallCount > 0)
		{
			FinalResult.Summary += FString::Printf(TEXT(" (%d tool call(s) made)"), ToolCallCount);
		}
	}

	// Step 11: Record to audit log
	{
		bool bSuccess = !FinalResult.bIsError;
		FString ResultSummary;

		FClaireonPythonAuditLog::Get().RecordInvocation(
			Code,
			Logs,
			bSuccess,
			DurationMs,
			ToolCallCount,
			ResultSummary);
	}

	UE_LOG(LogClaireon, Log, TEXT("[MCP Execute] Completed in %.1fms (success=%s, timeout=%s, tool_calls=%d)"),
		DurationMs,
		bPythonSuccess ? TEXT("true") : TEXT("false"),
		bTimedOut ? TEXT("true") : TEXT("false"),
		ToolCallCount);

	return FinalResult;
}
