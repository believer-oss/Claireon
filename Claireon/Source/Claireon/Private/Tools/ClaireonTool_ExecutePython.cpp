// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_MapOpen.h"
#include "Tools/ClaireonTool_PIEStart.h"
#include "Tools/ClaireonTool_PIEStop.h"
#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "Tools/ClaireonTool_MapDuplicate.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"
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
		"and all claireon.* wrapper functions available for calling other MCP tools.");
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
	// Constant string — the claireon.* namespace proxy
	// Uses _json and _unreal prefixed imports to avoid colliding with user code
	return TEXT(
		"import json as _json\n"
		"import unreal as _unreal\n"
		"import warnings as _warnings\n"
		"_warnings.filterwarnings('ignore', category=DeprecationWarning)\n"
		"\n"
		"# --- sleep short-circuit ---\n"
		"# Monkey-patch time.sleep so it cannot block the editor thread.\n"
		"import time as _time\n"
		"_time._original_sleep = _time.sleep\n"
		"def _noop_sleep(seconds=0):\n"
		"    _warnings.warn(\n"
		"        f'time.sleep({seconds}) intercepted and skipped — sleeping is disabled in Claireon MCP to prevent editor hangs.',\n"
		"        RuntimeWarning, stacklevel=2)\n"
		"_time.sleep = _noop_sleep\n"
		"try:\n"
		"    import asyncio as _asyncio\n"
		"    async def _noop_async_sleep(seconds=0, result=None):\n"
		"        _warnings.warn(\n"
		"            f'asyncio.sleep({seconds}) intercepted and skipped.',\n"
		"            RuntimeWarning, stacklevel=2)\n"
		"        return result\n"
		"    _asyncio.sleep = _noop_async_sleep\n"
		"except ImportError:\n"
		"    pass\n"
		"\n"
		"class _MCPToolProxy:\n"
		"    \"\"\"Callable proxy that supports dotted access for tool dispatch.\n"
		"    claireon.asset_search returns _MCPToolProxy('claireon.asset_search').\n"
		"    claireon.asset_search() dispatches _mcp_call_tool('claireon.asset_search', ...).\"\"\"\n"
		"    _schema_cache = {}\n"
		"    def __init__(self, full_name):\n"
		"        object.__setattr__(self, '_full_name', full_name)\n"
		"    def __call__(self, *args, **kwargs):\n"
		"        fn = self._full_name\n"
		"        if args:\n"
		"            if fn not in _MCPToolProxy._schema_cache:\n"
		"                schema_json = _unreal._mcp_call_tool('__get_schema__', fn)\n"
		"                _MCPToolProxy._schema_cache[fn] = _json.loads(schema_json) if schema_json else {}\n"
		"            schema = _MCPToolProxy._schema_cache[fn]\n"
		"            required = schema.get('required', [])\n"
		"            props = list(schema.get('properties', {}).keys())\n"
		"            param_names = list(required) + [p for p in props if p not in required]\n"
		"            for i, val in enumerate(args):\n"
		"                if i < len(param_names):\n"
		"                    kwargs[param_names[i]] = val\n"
		"        result_json = _unreal._mcp_call_tool(fn, _json.dumps(kwargs))\n"
		"        return _json.loads(result_json)\n"
		"    def __getattr__(self, name):\n"
		"        return _MCPToolProxy(self._full_name + '.' + name)\n"
		"    def __repr__(self):\n"
		"        return f'<tool {self._full_name}>'\n"
		"\n"
		"def _index_search(index_id, query='', max_results=5, method='hybrid'):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    _engine = _get_engine()\n"
		"    _hits = _engine.search(index_id, query, max_results=max_results, method=method)\n"
		"    return {'index_id': index_id, 'query': query, 'results': _hits}\n"
		"\n"
		"def _index_info(index_id):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().get_index_info(index_id)\n"
		"\n"
		"def _index_list():\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return {'indexes': _get_engine().list_indexes()}\n"
		"\n"
		"def _index_stats(index_id):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().stats(index_id)\n"
		"\n"
		"def _index_search_all(query='', max_results=10):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    _hits = _get_engine().search_all(query, max_results=max_results)\n"
		"    return {'query': query, 'results': _hits}\n"
		"\n"
		"def _index_dump(index_id=None, name=None):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().dump(index_id=index_id, name=name)\n"
		"\n"
		"def _index_load(name):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().load(name)\n"
		"\n"
		"def _index_clear(index_id=None):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().clear(index_id=index_id)\n"
		"\n"
		"def _index_expire(max_age_seconds=600.0):\n"
		"    from mcp_index_engine import get_engine as _get_engine\n"
		"    return _get_engine().expire(max_age_seconds=max_age_seconds)\n"
		"\n"
		"import unreal\n"
		"claireon = _MCPToolProxy('claireon')\n"
		"claireon.index_search = _index_search\n"
		"claireon.index_info = _index_info\n"
		"claireon.index_list = _index_list\n"
		"claireon.index_stats = _index_stats\n"
		"claireon.index_search_all = _index_search_all\n"
		"claireon.index_dump = _index_dump\n"
		"claireon.index_load = _index_load\n"
		"claireon.index_clear = _index_clear\n"
		"claireon.index_expire = _index_expire\n"
		"result = None\n"
		"\n"
		"# --- user code begins here ---\n"
	);
}

FString ClaireonTool_ExecutePython::GetPythonSuffix()
{
	// Suffix routes result through the Output Gate, then passes the (possibly
	// rewritten) result back to C++ via the bridge.  When the result exceeds
	// the threshold, the raw data is stored in the IndexEngine and a compact
	// IndexedResult envelope is returned instead so the LLM can issue follow-up
	// claireon.index_search() calls without blowing its context window.
	return TEXT(
		"\n"
		"# --- user code ends here ---\n"
		"import json as _json\n"
		"import unreal as _unreal\n"
		"if not _mcp_error_handled:\n"
		"    try:\n"
		"        # Bypass the Output Gate for index operation results — re-indexing\n"
		"        # search results creates an infinite nesting loop.\n"
		"        _bypass_gate = (\n"
		"            isinstance(result, dict)\n"
		"            and 'index_id' in result\n"
		"            and ('results' in result or 'indexes' in result or 'chunks_inserted' in result)\n"
		"        )\n"
		"        if _bypass_gate:\n"
		"            _final_json = _json.dumps(result)\n"
		"        else:\n"
		"            from mcp_output_gate import get_gate as _get_gate\n"
		"            _gate = _get_gate()\n"
		"            _routed = _gate.route(result, stream_type='result', source_tool='execute')\n"
		"            _routed_type = type(_routed).__name__\n"
		"            if _routed_type == 'DirectResult':\n"
		"                # Small result — pass through unchanged\n"
		"                _final_json = _routed.content if _routed.content else _json.dumps(result)\n"
		"            else:\n"
		"                # Large result — emit a compact indexed_result envelope\n"
		"                _final_json = _json.dumps({\n"
		"                    '__mcp_indexed__': True,\n"
		"                    'index_id': _routed.index_id,\n"
		"                    'chunk_count': _routed.chunk_count,\n"
		"                    'summary': _routed.summary,\n"
		"                    'excerpts': _routed.excerpts,\n"
		"                    'hint': 'Use claireon.index_search(index_id, query) to retrieve specific content.',\n"
		"                })\n"
		"    except Exception as _gate_err:\n"
		"        # Output gate failure must never silently swallow the result\n"
		"        _final_json = _json.dumps(result)\n"
		"    _unreal._mcp_set_result(_final_json)\n"
	);
}

FString ClaireonTool_ExecutePython::BuildLogString(const TArray<FPythonLogOutputEntry>& LogOutput)
{
	return FToolResult::BuildLogString(LogOutput);
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

	// Step 3: Reset result state
	FClaireonBridge::ResetLastExecuteResult();
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
	// The except block sets _mcp_error_handled so the suffix skips the output
	// gate and doesn't overwrite the error result with a routed None.
	FString FullScript = GetPythonPrefix()
		+ TEXT("_mcp_error_handled = False\n")
		+ TEXT("try:\n")
		+ IndentedCode
		+ TEXT("except Exception as _mcp_user_error:\n")
		+ TEXT("    import traceback as _tb\n")
		+ TEXT("    _mcp_error_msg = _tb.format_exc()\n")
		+ TEXT("    print(_mcp_error_msg)\n")
		+ TEXT("    _unreal._mcp_set_result(_json.dumps({'__mcp_error__': True, 'error': str(_mcp_user_error), 'traceback': _mcp_error_msg}))\n")
		+ TEXT("    _mcp_error_handled = True\n")
		+ GetPythonSuffix();

	if (!FFileHelper::SaveStringToFile(FullScript, *TempFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return MakeErrorResult(
			FString::Printf(TEXT("Failed to write temp Python file: %s. Check disk space and file permissions."), *TempFilePath));
	}

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

	// Step 7: Execute (no watchdog timer for now — matches original editor.python.execute behavior)
	const double StartTimeSeconds = FPlatformTime::Seconds();
	const bool bPythonSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
	const double DurationMs = (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;
	const bool bTimedOut = false; // Watchdog timer not yet implemented — timeout_seconds is advisory only

	// Step 7b: Dispatch any deferred world-transition actions.
	// The barrier runs inside each deferred action's lambda (right before the
	// world transition), not here — no reason to purge Python state early.
	if (FClaireonBridge::HasDeferredActions())
	{
		TArray<FClaireonDeferredAction> Actions = FClaireonBridge::DrainDeferredActions();
		for (const FClaireonDeferredAction& Action : Actions)
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
		}
	}

	// Step 8: Capture logs
	FString Logs = FToolResult::BuildLogString(PythonCommand.LogOutput);

	// Step 9: Read result
	FString ResultJson = FClaireonBridge::GetLastExecuteResult();
	int32 ToolCallCount = FClaireonBridge::GetToolCallCount();

	// Clean up temp file
	IFileManager::Get().Delete(*TempFilePath);

	// Step 10: Build structured FToolResult
	FToolResult FinalResult;
	FinalResult.Logs = Logs;

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
				if (!ErrorMsg.IsEmpty()) ErrorMsg += TEXT("\n");
				ErrorMsg += Entry.Output;
			}
		}

		if (!ResultJson.IsEmpty())
		{
			// Partial: _mcp_set_result was called before the error
			if (ErrorMsg.IsEmpty())
			{
				ErrorMsg = TEXT("Python execution failed.");
			}
			FinalResult.ErrorMessage = ErrorMsg;

			// Parse partial result into Data
			TSharedPtr<FJsonValue> ParsedPartial;
			TSharedRef<TJsonReader<>> PartialReader = TJsonReaderFactory<>::Create(ResultJson);
			if (FJsonSerializer::Deserialize(PartialReader, ParsedPartial) && ParsedPartial.IsValid()
				&& ParsedPartial->Type == EJson::Object)
			{
				FinalResult.Data = ParsedPartial->AsObject();
			}
		}
		else
		{
			// Full error
			if (ErrorMsg.IsEmpty())
			{
				if (!Logs.IsEmpty())
				{
					ErrorMsg = TEXT("Python execution failed. Output:\n") + Logs;
				}
				else
				{
					// Enhanced diagnostics for bridge-level failures (before user code ran)
					FString DiagInfo;

					// Check Python bridge availability
					IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
					if (!PythonPlugin)
					{
						DiagInfo = TEXT("Bridge status: Python plugin not available.");
					}
					else
					{
						DiagInfo = TEXT("Bridge status: registered.");
					}

					ErrorMsg = FString::Printf(
						TEXT("Python execution failed at bridge level (before user code executed). "
							 "Possible causes: Python bridge not initialized, syntax error in script prefix, or module import failure. %s"),
						*DiagInfo);
				}
			}
			FinalResult.ErrorMessage = ErrorMsg;
		}
	}
	else
	{
		// Check if the user code raised an exception caught by our try-except wrapper
		bool bHandled = false;
		if (!ResultJson.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrorCheck;
			TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(ResultJson);
			if (FJsonSerializer::Deserialize(ErrReader, ErrorCheck) && ErrorCheck.IsValid()
				&& ErrorCheck->HasField(TEXT("__mcp_error__")))
			{
				FString ErrorMsg;
				ErrorCheck->TryGetStringField(TEXT("traceback"), ErrorMsg);
				if (ErrorMsg.IsEmpty())
				{
					ErrorCheck->TryGetStringField(TEXT("error"), ErrorMsg);
				}
				FinalResult.bIsError = true;
				FinalResult.ErrorMessage = ErrorMsg;
				bHandled = true;
			}
		}

		// Success — check whether the Output Gate produced an indexed result envelope
		if (!bHandled && !ResultJson.IsEmpty() && ResultJson != TEXT("null"))
		{
			TSharedPtr<FJsonValue> ParsedValue;
			TSharedRef<TJsonReader<>> IndexCheckReader = TJsonReaderFactory<>::Create(ResultJson);
			if (FJsonSerializer::Deserialize(IndexCheckReader, ParsedValue) && ParsedValue.IsValid()
				&& ParsedValue->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> ParsedObj = ParsedValue->AsObject();
				bool bMCPIndexed = false;
				if (ParsedObj->TryGetBoolField(TEXT("__mcp_indexed__"), bMCPIndexed) && bMCPIndexed)
				{
					// Indexed result — store the envelope in Data
					FinalResult.Data = ParsedObj;
					FString IndexId;
					ParsedObj->TryGetStringField(TEXT("index_id"), IndexId);
					double ChunkCountDouble = 0.0;
					ParsedObj->TryGetNumberField(TEXT("chunk_count"), ChunkCountDouble);
					FinalResult.Summary = FString::Printf(
						TEXT("Result indexed (%d chunks). Use claireon.index_search(\"%s\", query) to retrieve content."),
						static_cast<int32>(ChunkCountDouble), *IndexId);
					bHandled = true;
				}
			}
		}

		// Normal (small) result — populate structured fields
		if (!bHandled)
		{
			if (!ResultJson.IsEmpty() && ResultJson != TEXT("null"))
			{
				TSharedPtr<FJsonValue> ParsedValue;
				TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResultJson);
				if (FJsonSerializer::Deserialize(JsonReader, ParsedValue) && ParsedValue.IsValid())
				{
					if (ParsedValue->Type == EJson::Object)
					{
						TSharedPtr<FJsonObject> ParsedObj = ParsedValue->AsObject();

						// The Python bridge wraps sub-tool results as {"data": ..., "summary": ..., "warnings": [...]}.
						// Unwrap: use the inner "data" for Data and "summary" for Summary.
						const TSharedPtr<FJsonObject>* InnerData = nullptr;
						FString InnerSummary;
						if (ParsedObj->TryGetObjectField(TEXT("data"), InnerData) && InnerData
							&& ParsedObj->TryGetStringField(TEXT("summary"), InnerSummary))
						{
							FinalResult.Data = *InnerData;
							FinalResult.Summary = InnerSummary;

							// Propagate warnings from the bridge envelope
							const TArray<TSharedPtr<FJsonValue>>* WarningsArr = nullptr;
							if (ParsedObj->TryGetArrayField(TEXT("warnings"), WarningsArr) && WarningsArr)
							{
								for (const TSharedPtr<FJsonValue>& WarnVal : *WarningsArr)
								{
									FString WarnStr;
									if (WarnVal->TryGetString(WarnStr))
									{
										FinalResult.Warnings.Add(WarnStr);
									}
								}
							}
						}
						else
						{
							// Not a bridge envelope — use the parsed object directly
							FinalResult.Data = ParsedObj;
							FinalResult.Summary = TEXT("Execution completed.");
						}
					}
					else
					{
						// Non-object result: wrap in {"value": ...}
						TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
						Wrapper->SetField(TEXT("value"), ParsedValue);
						FinalResult.Data = Wrapper;
						FinalResult.Summary = TEXT("Execution completed.");
					}
				}
				else
				{
					// Raw string result — wrap it
					TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
					Wrapper->SetStringField(TEXT("value"), ResultJson);
					FinalResult.Data = Wrapper;
					FinalResult.Summary = TEXT("Execution completed.");
				}
			}
			else
			{
				FinalResult.Summary = TEXT("Execution completed (no result).");
			}

			if (ToolCallCount > 0)
			{
				FinalResult.Summary += FString::Printf(TEXT(" (%d tool call(s) made)"), ToolCallCount);
			}
		}
	}

	// Step 11: Record to audit log
	{
		bool bSuccess = !FinalResult.bIsError;
		FString ResultSummary = ResultJson.Left(500);

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
