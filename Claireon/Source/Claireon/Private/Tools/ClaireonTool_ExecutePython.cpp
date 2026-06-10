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
#include "ClaireonSettings.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSafeExec.h"
#include "ClaireonPythonAuditLog.h"
#include "ClaireonToolSearchIndex.h"
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

// CPython C API headers for watchdog timeout enforcement
THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

TAtomic<int32> ClaireonTool_ExecutePython::TempFileCounter(0);

// ---------------------------------------------------------------------------
// Python execution watchdog.
//
// The watchdog runs on a thread-pool thread while ExecPythonCommandEx blocks
// the game thread.  On timeout it calls Py_AddPendingCall to inject a
// TimeoutError into the Python eval loop at the next bytecode boundary.
//
// Thread-safety notes:
//   - Py_AddPendingCall is explicitly documented as callable without the GIL
//     from any thread (including threads with no Python thread state).  It
//     uses CPython's internal eval-breaker atomic rather than the GIL.
//   - PyExc_TimeoutError is a module-level singleton initialized at startup;
//     reading it from any thread is safe.
//   - The callback runs on the game thread with the GIL held (CPython
//     guarantees this) so calling PyErr_SetString there is safe.
//   - SPythonExecNonce is a static atomic bumped once per Execute() call.
//     The callback receives a heap-allocated copy of the nonce at fire time;
//     it frees the allocation and is a no-op if the nonce no longer matches
//     the current execution (stale pending-call from a previous timeout).
//   - Because Execute() calls WatchdogFuture.Wait() before returning, any
//     stack-locals captured by reference in the watchdog lambda remain valid
//     for the watchdog's entire lifetime.
// ---------------------------------------------------------------------------
static std::atomic<uint64_t> SPythonExecNonce{0};

static int ClaireonPy_WatchdogRaiseTimeout(void* Arg)
{
	// Called on the game thread with the GIL held.
	// Arg is a heap-allocated uint64_t* we own; always free it.
	const uint64_t* pExpectedNonce = static_cast<uint64_t*>(Arg);
	const uint64_t Expected = pExpectedNonce ? *pExpectedNonce : 0;
	delete pExpectedNonce;

	if (Expected == 0 || Expected != SPythonExecNonce.load(std::memory_order_acquire))
	{
		// Stale: either queue overflow gave us a null arg, or a new execution
		// has already started.  Return 0 so no exception is raised.
		return 0;
	}

	PyErr_SetString(PyExc_TimeoutError,
		"claireon python_execute timed out -- increase PythonExecutionTimeoutSeconds "
		"in Editor Preferences > Plugins > Claireon, or break the script into smaller steps");
	return -1; // -1 tells CPython to raise the current exception
}

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
// Hint-emission helpers.
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

					FClaireonToolSearchIndex::EnsureBuilt();
					TArray<FClaireonToolCatalogMatch> Matches =
						FClaireonToolSearchIndex::FindNearest(MissingName, 1);
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

					FClaireonToolSearchIndex::EnsureBuilt();
					TArray<FClaireonToolCatalogMatch> Matches =
						FClaireonToolSearchIndex::FindNearest(MissingName, 1);
					if (Matches.Num() > 0)
					{
						Reason += FString::Printf(
							TEXT("; best match: %s"), *Matches[0].Name);
					}
					Hint->SetStringField(TEXT("reason"), Reason);
					return Hint;
				}
			}

			// SoftObjectPath -> Object nativize hint. The Python error text is
			// "Cannot nativize SoftObjectPath as Object" (originates from the unreal
			// Python bridge when set_editor_property receives a soft path for a UPROPERTY
			// of UObject* type). The fix is to load the asset first via SoftObjectPath.try_load()
			// (or LoadAsset). Emit a hint pointing the caller at the correct sequence.
			if (Line.Contains(TEXT("Cannot nativize SoftObjectPath as Object"), ESearchCase::CaseSensitive)
				|| Line.Contains(TEXT("Cannot nativize 'SoftObjectPath' as 'Object'"), ESearchCase::CaseSensitive))
			{
				TSharedPtr<FJsonObject> Hint = MakeShared<FJsonObject>();
				Hint->SetStringField(TEXT("tool"), TEXT("python_execute"));
				Hint->SetStringField(TEXT("reason"),
					TEXT("set_editor_property on an Object UPROPERTY received a SoftObjectPath; "
						 "load the asset first: `obj = unreal.SoftObjectPath('/Game/Path/Asset').try_load()` "
						 "or `obj = unreal.EditorAssetLibrary.load_asset('/Game/Path/Asset')` and pass `obj` "
						 "as the value."));
				return Hint;
			}
		}

		return nullptr;
	}
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
				"SPILL: large results (>8 KB) are written to disk instead of returned inline; the response will "
				"contain `__mcp_spilled__: true` and a `spilled_streams` array -- each entry has a `path` field "
				"(absolute path under Saved/Claireon/Results/) that you should read with the Read tool. "
				"Bypass-mode tool: the bridge will refuse this call if any other Claireon session "
				"(per-asset or editor-wide) is currently held. Call session_release first if needed.");
}

FString ClaireonTool_ExecutePython::GetFullDescription() const
{
	return GetDescription() + TEXT("\n\n")
		+ TEXT("EDITOR-CRASH GUARD-RAIL (E15 / O5 / O6):\n")
		+ TEXT(" - unreal.get_editor_property('Schema') on a UStateTree SEH-crashes the editor (0x00004000). Use claireon.statetree_get_schema(asset_path=...) instead.\n")
		+ TEXT(" - unreal.BlueprintEditorLibrary.generated_class(bp) SEH-crashes intermittently after add_variable + compile (0xC0000005). Use claireon.blueprint_get_generated_class(asset_path=...) instead.\n")
		+ TEXT(" - For any protected/private UPROPERTY on a UObject, prefer claireon.uobject_inspect(object_path=..., property_path=...). It uses FProperty reflection directly, bypassing the access-checked editor-property path that triggers the crashes above.");
}

FString ClaireonTool_ExecutePython::GetPatterns() const
{
	return TEXT("Known editor-crashing reflection calls (use these safe alternatives):\n")
		TEXT("\n")
		TEXT("- UStateTree.Schema (SEH 0x00004000): use claireon.statetree_get_schema(asset_path=...).\n")
		TEXT("- UBlueprint.generated_class (SEH 0xC0000005, after add_variable + compile): use claireon.blueprint_get_generated_class(asset_path=...).\n")
		TEXT("- Any protected/private UPROPERTY: use claireon.uobject_inspect(object_path=..., property_path=...).\n")
		TEXT("\n")
		TEXT("Raw unreal.get_editor_property on a property the engine flags inaccessible takes the access-checked path that SEHs out. The claireon.* alternatives go straight through FProperty and survive.\n")
		TEXT("\n")
		TEXT("Common patterns:\n")
		TEXT("\n")
		TEXT("- Editor world: claireon.world_get_active_world() (claireon-side, PIE-safe). The deprecated unreal.EditorLevelLibrary.get_editor_world() and its replacement unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world() both return null in PIE.\n")
		TEXT("- WorldSettings: world.get_world_settings() (not world.get_editor_property('persistent_level') / 'world_settings' -- those are engine-protected and raise).\n")
		TEXT("- Enum subscript naming: a C++ UENUM `EFooBar::CamelCase` is exposed in Python as `unreal.FooBar.UPPER_SNAKE_CASE`. Drop the leading `E`, convert each CamelCase token to UPPER_SNAKE_CASE (e.g. `EMyMode::OnePerHostAtATime` -> `unreal.MyMode.ONE_PER_HOST_AT_A_TIME`).\n")
		TEXT("- MovieSceneObjectBindingID (UE upstream issue): the no-arg constructor + `set_editor_property('Guid', binding.get_id())` is the only working pattern; the FGuid-taking ctor exposed to Python is non-functional. Default-construct, then set Guid via reflection.\n")
		TEXT("- LevelSequence binding removal: there is no `level_sequence.remove_possessable(guid)` Python API. Use `binding.remove()` on the FMovieSceneBinding (resolve binding via `for b in level_sequence.get_bindings(): if b.get_id() == guid: ...`). Claireon's session-mode tool `claireon.level_sequence_remove_possessable` wraps this.\n");
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
	// This lets us capture Python exceptions and surface them to the caller
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

	// Step 6b: Arm the watchdog.
	// Read the timeout from settings (0 = watchdog disabled).
	// Bump the execution nonce so any stale pending-call from a previous
	// timed-out run is a no-op if it fires at the start of this execution.
	const uint64_t ThisNonce = SPythonExecNonce.fetch_add(1, std::memory_order_acq_rel) + 1;
	const float WatchdogTimeoutSeconds = UClaireonSettings::Get()->PythonExecutionTimeoutSeconds;
	const bool bWatchdogEnabled = (WatchdogTimeoutSeconds > 0.f);

	// Shared flags between Execute() (game thread) and the watchdog (pool thread).
	// Both live on the stack; WatchdogFuture.Wait() below ensures the watchdog
	// exits before Execute() returns, so these references are always valid.
	std::atomic<bool> bWatchdogDone{false};
	std::atomic<bool> bWatchdogFired{false};

	TFuture<void> WatchdogFuture;
	if (bWatchdogEnabled)
	{
		WatchdogFuture = Async(EAsyncExecution::ThreadPool,
			[WatchdogTimeoutSeconds, ThisNonce, &bWatchdogDone, &bWatchdogFired]()
			{
				const double Deadline = FPlatformTime::Seconds() + static_cast<double>(WatchdogTimeoutSeconds);
				constexpr float PollIntervalSec = 0.05f; // 50 ms poll

				while (!bWatchdogDone.load(std::memory_order_acquire))
				{
					FPlatformProcess::Sleep(PollIntervalSec);

					if (bWatchdogDone.load(std::memory_order_acquire))
					{
						break; // execution finished cleanly before deadline
					}

					if (FPlatformTime::Seconds() >= Deadline)
					{
						// Allocate the nonce arg on the heap; the callback owns and
						// frees it (even if execution already ended by the time it fires).
						uint64_t* pNonce = new uint64_t(ThisNonce);
						if (Py_AddPendingCall(&ClaireonPy_WatchdogRaiseTimeout, pNonce) == 0)
						{
							bWatchdogFired.store(true, std::memory_order_release);
						}
						else
						{
							// CPython's pending-call queue is full (max 32 slots); unlikely
							// but recoverable.  Free the allocation and log; the script
							// will run until it finishes or the editor is killed.
							delete pNonce;
							UE_LOG(LogClaireon, Warning,
								TEXT("[MCP Execute] Watchdog: Py_AddPendingCall queue full -- "
								     "timeout injection skipped for this invocation"));
						}
						break; // fire once only
					}
				}
			});
	}

	// Step 7: Execute with engine log capture at the top level.
	// All claireon.* tool calls within Python flow through ClaireonBridge::MCPCallTool,
	// so capturing here covers the entire execution scope.
	FClaireonLogCapture EngineLogCapture;
	const double StartTimeSeconds = FPlatformTime::Seconds();
	const bool bPythonSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
	const double DurationMs = (FPlatformTime::Seconds() - StartTimeSeconds) * 1000.0;

	// Signal the watchdog that execution has finished and wait for it to exit.
	// Must happen before reading bWatchdogFired and before any stack-local the
	// watchdog references goes out of scope.
	bWatchdogDone.store(true, std::memory_order_release);
	if (WatchdogFuture.IsValid())
	{
		WatchdogFuture.Wait();
	}

	const bool bTimedOut = bWatchdogFired.load(std::memory_order_acquire);

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

	// Scan the traceback (regardless of bPythonSuccess -- the user-script
	// template's bare-except branch can absorb the exception while still
	// printing the traceback to stdout, so the hint signal must be available
	// on both result paths). The bridge surfaces Result.Hint on the wire
	// envelope only when valid; null leaves the wire shape byte-identical.
	FinalResult.Hint = Cl622PyHintInternal::Cl622Py_BuildHintFromLogs(Logs);

	if (bTimedOut)
	{
		// Timeout — plain text error
		FinalResult.bIsError = true;
		FinalResult.ErrorMessage = FString::Printf(
			TEXT("Execution timed out after %.0fs. Break the operation into smaller steps, "
			     "or increase PythonExecutionTimeoutSeconds in Editor Preferences > Plugins > Claireon."),
			static_cast<double>(WatchdogTimeoutSeconds));
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
