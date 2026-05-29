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
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSafeExec.h"
#include "ClaireonPythonAuditLog.h"
#include "ClaireonToolCatalogMatcher.h"
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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// CPython C API headers for timeout enforcement
THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

TAtomic<int32> ClaireonTool_ExecutePython::TempFileCounter(0);

// Forward declaration of internal helper used by the public BuildHintFromLogs
// method.  Defined in the Cl622PyHintInternal namespace below.
namespace Cl622PyHintInternal
{
	static TSharedPtr<FJsonObject> Cl622Py_BuildHintFromLogs(const FString& Logs);
}

TSharedPtr<FJsonObject> ClaireonTool_ExecutePython::BuildHintFromLogs(const FString& Logs)
{
	return Cl622PyHintInternal::Cl622Py_BuildHintFromLogs(Logs);
}

// ---------------------------------------------------------------------------
// Hint-emission helpers (Stage 002 / Part A of #0000).
//
// Parses the Python traceback emitted by the user-script template at :332-343
// for four signature-class error patterns and constructs an
// FToolResult::Hint payload that nudges the agent toward tool_search:
//
//   - NameError: name '<X>' is not defined        (with claireon.<X> source ctx)
//   - AttributeError: module 'claireon' has no attribute '<X>'
//   - TypeError: <tool>() got an unexpected keyword argument '<K>'
//   - TypeError: <tool>() missing N required positional argument
//
// SyntaxError is intentionally not nudged (operator answer D4 + design notes).
//
// File-local discriminator: helpers are prefixed `Cl622Py_` to avoid
// anonymous-namespace collisions under Module.Claireon.<N>.cpp unity batching
// (project memory: unique anon-NS naming required).
// ---------------------------------------------------------------------------
namespace Cl622PyHintInternal
{
	static bool Cl622Py_LineMatchesPrefix(const FString& Line, const FString& Prefix)
	{
		return Line.StartsWith(Prefix, ESearchCase::CaseSensitive);
	}

	/** True if the traceback (the lines preceding `ExceptionLineIdx`) shows a
	 *  `File "...", line ...,` block whose echoed source line references
	 *  `claireon.<Name>`. The user-script template wraps user code with a fixed
	 *  prefix, so the echoed source for the failing line in the traceback
	 *  contains the call expression -- a substring match for "claireon.<Name>"
	 *  is sufficient.
	 *
	 *  We scan a small backward window of up to 6 lines from the exception
	 *  line so that nested call sites (where the failing frame is several
	 *  source lines up from the exception line) still match. */
	static bool Cl622Py_TracebackCallsClaireonOnName(
		const TArray<FString>& Lines,
		int32 ExceptionLineIdx,
		const FString& Name)
	{
		const FString Needle = TEXT("claireon.") + Name;
		const int32 Start = FMath::Max(0, ExceptionLineIdx - 6);
		for (int32 I = Start; I < ExceptionLineIdx; ++I)
		{
			if (Lines[I].Contains(Needle, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** Look up a tool by name from the live FClaireonServer.  Returns true if
	 *  the name corresponds to a registered tool (so we don't hint on
	 *  non-claireon library functions whose names happen to appear in a
	 *  TypeError signature line). */
	static bool Cl622Py_IsRegisteredTool(const FString& Name)
	{
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (!Server)
		{
			return false;
		}
		const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
		return Tools.Contains(Name);
	}

	/** Extract the substring between two delimiters (exclusive). Returns true if both
	 *  delimiters are found and the start is before the end. */
	static bool Cl622Py_BetweenDelimiters(
		const FString& Source,
		const FString& Open,
		const FString& Close,
		FString& OutValue)
	{
		const int32 OpenAt = Source.Find(Open, ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (OpenAt < 0)
		{
			return false;
		}
		const int32 CloseAt = Source.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenAt + Open.Len());
		if (CloseAt < 0 || CloseAt <= OpenAt + Open.Len())
		{
			return false;
		}
		OutValue = Source.Mid(OpenAt + Open.Len(), CloseAt - (OpenAt + Open.Len()));
		return true;
	}

	/** Parse `<tool>(` out of a TypeError line such as
	 *  `TypeError: bp_compile() got an unexpected keyword argument 'foo'`.
	 *  Returns the bare tool name, or empty when the pattern is not present. */
	static FString Cl622Py_ExtractTypeErrorToolName(const FString& Line)
	{
		// Strip optional traceback prefix that Python sometimes prefixes
		// (e.g. "  File ..." vs. "TypeError: ..."). Find "TypeError:" first.
		const int32 At = Line.Find(TEXT("TypeError:"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (At < 0)
		{
			return FString();
		}
		// Substring from "TypeError:" forward.
		const FString Tail = Line.Mid(At + FString(TEXT("TypeError:")).Len()).TrimStartAndEnd();
		const int32 ParenAt = Tail.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (ParenAt <= 0)
		{
			return FString();
		}
		FString Name = Tail.Left(ParenAt).TrimStartAndEnd();
		// Reject anything that contains a space (e.g. "missing 1 required positional")
		// -- the bare tool basename is the token immediately preceding '('.
		int32 SpaceAt;
		if (Name.FindLastChar(TEXT(' '), SpaceAt))
		{
			Name = Name.Mid(SpaceAt + 1);
		}
		return Name;
	}

	/** Inspect the Python log lines for one of the four nudge patterns and,
	 *  on match, build a hint object.  Returns null when no pattern matches
	 *  or when the match is explicitly suppressed (SyntaxError, or TypeError
	 *  on a non-registered tool name). */
	static TSharedPtr<FJsonObject> Cl622Py_BuildHintFromLogs(const FString& Logs)
	{
		if (Logs.IsEmpty())
		{
			return nullptr;
		}

		TArray<FString> Lines;
		Logs.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);

		for (int32 I = 0; I < Lines.Num(); ++I)
		{
			const FString& Line = Lines[I];

			// Highest priority: SyntaxError -- suppress hint regardless of any
			// later patterns.  The user-script template's bare-except branch
			// only catches Exception, so SyntaxErrors at parse time surface
			// here as a top-level traceback line.
			if (Line.Contains(TEXT("SyntaxError:"), ESearchCase::CaseSensitive))
			{
				return nullptr;
			}

			// TypeError signature-mismatch nudges. Two sub-shapes share the
			// same hint payload (name + detail=full + reason).
			const bool bUnexpectedKwarg = Line.Contains(
				TEXT("got an unexpected keyword argument"), ESearchCase::CaseSensitive);
			const bool bMissingPositional = Line.Contains(
				TEXT("missing"), ESearchCase::CaseSensitive)
				&& Line.Contains(TEXT("required positional"), ESearchCase::CaseSensitive);
			if ((bUnexpectedKwarg || bMissingPositional)
				&& Line.Contains(TEXT("TypeError:"), ESearchCase::CaseSensitive))
			{
				const FString ToolName = Cl622Py_ExtractTypeErrorToolName(Line);
				if (!ToolName.IsEmpty() && Cl622Py_IsRegisteredTool(ToolName))
				{
					TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
					Hint->SetStringField(TEXT("tool"), TEXT("tool_search"));
					TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
					Args->SetStringField(TEXT("name"), ToolName);
					Args->SetStringField(TEXT("detail"), TEXT("full"));
					Hint->SetObjectField(TEXT("args"), Args);
					Hint->SetStringField(TEXT("reason"),
						FString::Printf(TEXT("signature mismatch on %s"), *ToolName));
					return Hint;
				}
			}

			// NameError: name '<X>' is not defined -- only when traceback
			// source context shows `claireon.<X>`.
			if (Line.Contains(TEXT("NameError:"), ESearchCase::CaseSensitive))
			{
				FString MissingName;
				if (Cl622Py_BetweenDelimiters(Line, TEXT("name '"), TEXT("'"), MissingName)
					&& !MissingName.IsEmpty()
					&& Cl622Py_TracebackCallsClaireonOnName(Lines, I, MissingName))
				{
					TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
					Hint->SetStringField(TEXT("tool"), TEXT("tool_search"));
					TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
					Args->SetStringField(TEXT("query"), MissingName);
					Hint->SetObjectField(TEXT("args"), Args);

					FString Reason = FString::Printf(
						TEXT("unknown tool '%s'"), *MissingName);

					FClaireonToolCatalogMatcher::EnsureBuilt();
					TArray<FClaireonToolCatalogMatch> Matches =
						FClaireonToolCatalogMatcher::FindNearest(MissingName, 1);
					if (Matches.Num() > 0)
					{
						Reason += FString::Printf(
							TEXT("; best match: %s"), *Matches[0].Name);
					}
					Hint->SetStringField(TEXT("reason"), Reason);
					return Hint;
				}
			}

			// AttributeError: module 'claireon' has no attribute '<X>'.
			if (Line.Contains(TEXT("AttributeError:"), ESearchCase::CaseSensitive)
				&& Line.Contains(TEXT("module 'claireon'"), ESearchCase::CaseSensitive))
			{
				FString MissingName;
				// Two attribute shapes: "has no attribute 'X'" or "has no attribute \"X\""
				if (!Cl622Py_BetweenDelimiters(
						Line, TEXT("has no attribute '"), TEXT("'"), MissingName))
				{
					Cl622Py_BetweenDelimiters(
						Line, TEXT("has no attribute \""), TEXT("\""), MissingName);
				}
				if (!MissingName.IsEmpty())
				{
					TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
					Hint->SetStringField(TEXT("tool"), TEXT("tool_search"));
					TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
					Args->SetStringField(TEXT("query"), MissingName);
					Hint->SetObjectField(TEXT("args"), Args);

					FString Reason = FString::Printf(
						TEXT("unknown tool '%s'"), *MissingName);

					FClaireonToolCatalogMatcher::EnsureBuilt();
					TArray<FClaireonToolCatalogMatch> Matches =
						FClaireonToolCatalogMatcher::FindNearest(MissingName, 1);
					if (Matches.Num() > 0)
					{
						Reason += FString::Printf(
							TEXT("; best match: %s"), *Matches[0].Name);
					}
					Hint->SetStringField(TEXT("reason"), Reason);
					return Hint;
				}
			}
		}

		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Tool-catalog nearest-string bindings.
//
// Exposed to Python as claireon._tool_catalog_build(entries_json_str) /
// claireon._tool_catalog_nearest(query_str, max_results_int). Registered into the
// 'unreal' module dict alongside _mcp_call_tool (see
// FClaireonBridge::RegisterBridgeFunctions), then aliased onto the claireon proxy
// in the script prefix below.
// ---------------------------------------------------------------------------

namespace ClaireonToolCatalogBindings
{
	static PyObject* BuildCatalog(PyObject* /*Self*/, PyObject* Args)
	{
		const char* EntriesJsonUtf8 = nullptr;
		if (!PyArg_ParseTuple(Args, "s", &EntriesJsonUtf8))
		{
			return nullptr;
		}
		if (!EntriesJsonUtf8)
		{
			PyErr_SetString(PyExc_ValueError, "claireon._tool_catalog_build: entries_json is null");
			return nullptr;
		}

		const FString EntriesJson = UTF8_TO_TCHAR(EntriesJsonUtf8);

		TArray<TSharedPtr<FJsonValue>> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EntriesJson);
		if (!FJsonSerializer::Deserialize(Reader, Root))
		{
			PyErr_SetString(PyExc_ValueError, "claireon._tool_catalog_build: failed to parse entries JSON array");
			return nullptr;
		}

		TArray<FClaireonToolCatalogEntry> Entries;
		Entries.Reserve(Root.Num());
		for (const TSharedPtr<FJsonValue>& Val : Root)
		{
			if (!Val.IsValid() || Val->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FClaireonToolCatalogEntry Entry;
			Obj->TryGetStringField(TEXT("name"), Entry.Name);
			Obj->TryGetStringField(TEXT("description"), Entry.Description);
			Obj->TryGetStringField(TEXT("category"), Entry.Category);
			Obj->TryGetStringField(TEXT("operation"), Entry.Operation);
			const TArray<TSharedPtr<FJsonValue>>* KeywordsArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("keywords"), KeywordsArr) && KeywordsArr)
			{
				Entry.Keywords.Reserve(KeywordsArr->Num());
				for (const TSharedPtr<FJsonValue>& KV : *KeywordsArr)
				{
					FString K;
					if (KV.IsValid() && KV->TryGetString(K) && !K.IsEmpty())
					{
						Entry.Keywords.Add(MoveTemp(K));
					}
				}
			}
			if (Entry.Name.IsEmpty() && Entry.Category.IsEmpty() && Entry.Operation.IsEmpty()
				&& Entry.Description.IsEmpty() && Entry.Keywords.Num() == 0)
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("[ToolCatalogBindings] Entry '%s' has no indexable fields; nothing will be indexed for it."),
					*Entry.Name);
			}
			Entries.Add(MoveTemp(Entry));
		}

		FClaireonToolCatalogMatcher::BuildCatalog(Entries);

		Py_RETURN_NONE;
	}

	static PyObject* FindNearest(PyObject* /*Self*/, PyObject* Args)
	{
		const char* QueryUtf8 = nullptr;
		int MaxResults = 0;
		if (!PyArg_ParseTuple(Args, "si", &QueryUtf8, &MaxResults))
		{
			return nullptr;
		}
		const FString Query = QueryUtf8 ? FString(UTF8_TO_TCHAR(QueryUtf8)) : FString();
		const int32 K = FMath::Max(0, static_cast<int32>(MaxResults));

		TArray<FClaireonToolCatalogMatch> Matches = FClaireonToolCatalogMatcher::FindNearest(Query, K);

		// Serialise to JSON array of { name, category, score, tokens_matched }.
		// `tokens_matched` is the count of distinct query tokens that produced
		// any exact/prefix/fuzzy hit; consumed by ClaireonTool_SearchTools::Execute
		// to surface `query_tokens_matched` per result and to detect pathological
		// fuzzy responses that warrant a substring-fallback merge.
		TArray<TSharedPtr<FJsonValue>> OutArr;
		OutArr.Reserve(Matches.Num());
		for (const FClaireonToolCatalogMatch& M : Matches)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), M.Name);
			Obj->SetStringField(TEXT("category"), M.Category);
			Obj->SetNumberField(TEXT("score"), M.Score);
			Obj->SetNumberField(TEXT("tokens_matched"), M.TokensMatched);
			OutArr.Add(MakeShared<FJsonValueObject>(Obj));
		}

		FString OutJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
		FJsonSerializer::Serialize(OutArr, Writer);
		Writer->Close();

		return PyUnicode_FromString(TCHAR_TO_UTF8(*OutJson));
	}

	static PyMethodDef BuildDef = {
		"_tool_catalog_build",
		&BuildCatalog,
		METH_VARARGS,
		"Rebuild the tool catalog from JSON. Args: (entries_json: str) -> None"
	};

	static PyMethodDef NearestDef = {
		"_tool_catalog_nearest",
		&FindNearest,
		METH_VARARGS,
		"Rank catalog entries against a query. Args: (query: str, max_results: int) -> str (JSON array)"
	};
}

void ClaireonTool_ExecutePython::RegisterToolCatalogBindings(void* UnrealModuleDict)
{
	PyObject* ModuleDict = static_cast<PyObject*>(UnrealModuleDict);
	if (!ModuleDict)
	{
		return;
	}

	PyObject* BuildFn = PyCFunction_New(&ClaireonToolCatalogBindings::BuildDef, nullptr);
	PyObject* NearestFn = PyCFunction_New(&ClaireonToolCatalogBindings::NearestDef, nullptr);
	if (!BuildFn || !NearestFn)
	{
		Py_XDECREF(BuildFn);
		Py_XDECREF(NearestFn);
		UE_LOG(LogClaireon, Error, TEXT("[MCP Bridge] Failed to create tool-catalog PyCFunctions"));
		return;
	}

	PyDict_SetItemString(ModuleDict, "_tool_catalog_build", BuildFn);
	PyDict_SetItemString(ModuleDict, "_tool_catalog_nearest", NearestFn);

	Py_DECREF(BuildFn);
	Py_DECREF(NearestFn);

	UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Registered _tool_catalog_build / _tool_catalog_nearest in unreal module"));
}

FString ClaireonTool_ExecutePython::GetCategory() const { return TEXT("python"); }
FString ClaireonTool_ExecutePython::GetOperation() const { return TEXT("execute"); }

TArray<FString> ClaireonTool_ExecutePython::GetSearchKeywords() const
{
	return {TEXT("python"), TEXT("execute"), TEXT("run"), TEXT("script"), TEXT("code"), TEXT("eval")};
}

FString ClaireonTool_ExecutePython::GetDescription() const
{
	return TEXT("Run Python code in the Unreal Editor. The `claireon.*` Python module exposes hundreds of tools "
				"across many categories -- write Python like `import claireon; claireon.<tool_name>(arg=...)`. "
				"Before calling a non-trivial tool, use `tool_search(tool_name=\"<name>\")` to fetch its exact "
				"input schema and example usage so you don't pass the wrong arguments. "
				"Run `dir(claireon)` to list available tools or `help(claireon.<tool_name>)` for usage details. "
				"Bypass-mode tool: the bridge will refuse this call if any other Claireon session "
				"(per-asset or editor-wide) is currently held. Call session_release first if needed.");
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

	// Step 7c: Drain any deferred-action aborts (e.g. leaked-World
	// guard refused a map transition).
	TArray<FString> DeferredAborts = FClaireonBridge::DrainDeferredActionAborts();

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

	// Stage 002 / Part A: scan the traceback (regardless of bPythonSuccess --
	// the user-script template's bare-except branch can absorb the exception
	// while still printing the traceback to stdout, so the hint signal must
	// be available on both result paths). The bridge from Stage 001 surfaces
	// Result.Hint on the wire envelope only when valid; null leaves the wire
	// shape byte-identical.
	FinalResult.Hint = Cl622PyHintInternal::Cl622Py_BuildHintFromLogs(Logs);

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
	else if (DeferredAborts.Num() > 0)
	{
		// A deferred world-transition action was refused by the leaked-World
		// guard. python_execute MUST NOT report success for this case.
		FinalResult.bIsError = true;
		const FString Joined = FString::Join(DeferredAborts, TEXT("\n"));
		FinalResult.ErrorMessage = TEXT("Deferred action aborted: ") + Joined;

		// Mirror the same string into UELog so log-tailers and
		// engine-log audit readers also surface the abort, not just
		// JSON consumers.
		if (!FinalResult.UELog.IsEmpty()
			&& !FinalResult.UELog.EndsWith(TEXT("\n")))
		{
			FinalResult.UELog += TEXT("\n");
		}
		FinalResult.UELog += Joined;
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

	// Route stdout ("Logs") and uelog streams through the disk-spill gate (per D6).
	// python_execute has no structured "data" stream.  The conversation id comes
	// from FClaireonBridge::GetCurrentConversationId(), set by the REPL before dispatch.
	return FClaireonOutputGate::RouteResult(
		MoveTemp(FinalResult),
		TEXT("python_execute"),
		FClaireonBridge::GetCurrentConversationId(),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);
}
