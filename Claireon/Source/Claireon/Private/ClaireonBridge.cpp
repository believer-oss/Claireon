// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonServer.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "Misc/ScopeExit.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_ExecutePython.h"

// CPython C API headers — provided by the Python3 module dependency.
// Must be included after UE headers to avoid macro conflicts.
THIRD_PARTY_INCLUDES_START
#include "Python.h"
THIRD_PARTY_INCLUDES_END

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "IPythonScriptPlugin.h"
#include "UObject/UObjectGlobals.h"
#include "Editor.h"
#include "ClaireonWorldReadiness.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "PackageTools.h"
#include "FileHelpers.h"
#include "HAL/ThreadHeartBeat.h"

TSharedPtr<FJsonObject> FClaireonBridge::BuildResultEnvelope(
	const IClaireonTool::FToolResult& Result)
{
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();

	if (Result.Data.IsValid())
	{
		Envelope->SetObjectField(TEXT("data"), Result.Data);
	}
	else
	{
		Envelope->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
	}

	if (Result.Hint.IsValid())
	{
		Envelope->SetObjectField(TEXT("hint"), Result.Hint);
	}

	Envelope->SetStringField(TEXT("summary"), Result.Summary);

	TArray<TSharedPtr<FJsonValue>> WarningsArray;
	for (const FString& Warning : Result.Warnings)
	{
		WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
	}
	Envelope->SetArrayField(TEXT("warnings"), WarningsArray);

	if (!Result.Logs.IsEmpty())
	{
		Envelope->SetStringField(TEXT("logs"), Result.Logs);
	}

	if (!Result.UELog.IsEmpty())
	{
		Envelope->SetStringField(TEXT("ue_log"), Result.UELog);
	}

	return Envelope;
}

// Static member initialization
bool FClaireonBridge::bIsRegistered = false;
FClaireonServer* FClaireonBridge::GServerInstance = nullptr;
TAtomic<int32> FClaireonBridge::GToolCallCount(0);
TArray<FClaireonDeferredAction> FClaireonBridge::GDeferredActions;
FDelegateHandle FClaireonBridge::ToolsChangedHandle;
std::atomic<bool> FClaireonBridge::bClaireonModuleStale{false};
FString FClaireonBridge::GCurrentConversationId = TEXT("default");
TArray<FString> FClaireonBridge::GDeferredActionAborts;
TSet<FString> FClaireonBridge::PreviousNamespaces;

// PyMethodDef structs (static storage — CPython holds pointers to these)
static PyMethodDef MCPCallDef = {
	"_mcp_call_tool",
	&FClaireonBridge::MCPCallTool,
	METH_VARARGS,
	"Call an MCP tool by name. Args: (tool_name: str, args_json: str) -> str"
};

void FClaireonBridge::SetToolRegistry(FClaireonServer* Server)
{
	GServerInstance = Server;
}

void FClaireonBridge::RegisterBridgeFunctions()
{
	// Acquire the GIL — we may be called from the game thread outside of Python execution
	PyGILState_STATE GILState = PyGILState_Ensure();

	// PyImport_AddModule returns a borrowed reference (no Py_DECREF needed)
	PyObject* UnrealModule = PyImport_AddModule("unreal");
	if (!UnrealModule)
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP Bridge] Failed to get 'unreal' Python module"));
		PyGILState_Release(GILState);
		return;
	}

	// PyModule_GetDict returns a borrowed reference
	PyObject* UnrealDict = PyModule_GetDict(UnrealModule);
	if (!UnrealDict)
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP Bridge] Failed to get 'unreal' module dict"));
		PyGILState_Release(GILState);
		return;
	}

	// Create PyCFunction objects from our method defs
	PyObject* CallFunc = PyCFunction_New(&MCPCallDef, nullptr);

	if (!CallFunc)
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP Bridge] Failed to create PyCFunction objects"));
		Py_XDECREF(CallFunc);
		PyGILState_Release(GILState);
		return;
	}

	// Register into the unreal module dict
	PyDict_SetItemString(UnrealDict, "_mcp_call_tool", CallFunc);

	// PyDict_SetItemString increments refcount, so release our references
	Py_DECREF(CallFunc);

	// Register the nearest-string tool-catalog bindings (_tool_catalog_build /
	// _tool_catalog_nearest) into the same 'unreal' module dict.  Per
	// CLAIREON_DISK_RESULTS/tool-catalog-rewrite.md these replace the old
	// mcp_index_engine hybrid matcher with a dependency-free C++ BM25-lite.
	ClaireonTool_ExecutePython::RegisterToolCatalogBindings(UnrealDict);

	// Add our Python modules directory to sys.path. The output gate and
	// tool-catalog matcher live in C++; mcp_tool_catalog.py is imported lazily
	// from ClaireonTool_SearchTools.cpp and does not need prewarming.
	{
		const FString PluginPythonDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Claireon"), TEXT("Content"), TEXT("Python")));
		if (FPaths::DirectoryExists(PluginPythonDir))
		{
			const FString PathInjectCode = FString::Printf(
				TEXT("import sys; p = r'%s'; p not in sys.path and sys.path.insert(0, p)\n"),
				*PluginPythonDir);
			PyRun_SimpleString(TCHAR_TO_UTF8(*PathInjectCode));
			UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Added Python path: %s"), *PluginPythonDir);
		}
	}

	// Build the claireon Python module from the tool catalog and run the bootstrap
	BuildAndRunBootstrap();

	// Subscribe to OnToolsChanged so the module gets rebuilt when tools are added/removed
	if (GServerInstance && !ToolsChangedHandle.IsValid())
	{
		ToolsChangedHandle = GServerInstance->OnToolsChanged.AddLambda([]()
		{
			bClaireonModuleStale.store(true);
		});
	}

	bIsRegistered = true;
	UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Registered _mcp_call_tool in unreal module"));

	PyGILState_Release(GILState);
}

void FClaireonBridge::EnsureRegistered()
{
	if (bIsRegistered)
	{
		return;
	}

	// Check if Python is available via PythonScriptPlugin
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP Bridge] Python not available yet — deferring bridge registration"));
		return;
	}

	RegisterBridgeFunctions();
}

int32 FClaireonBridge::GetToolCallCount()
{
	return GToolCallCount;
}

void FClaireonBridge::ResetToolCallCount()
{
	GToolCallCount = 0;
}

void FClaireonBridge::SetCurrentConversationId(const FString& InConversationId)
{
	GCurrentConversationId = InConversationId.IsEmpty() ? FString(TEXT("default")) : InConversationId;
}

const FString& FClaireonBridge::GetCurrentConversationId()
{
	return GCurrentConversationId;
}

PyObject* FClaireonBridge::MCPCallTool(PyObject* /*Self*/, PyObject* Args)
{
	const char* ToolNameUtf8 = nullptr;
	const char* ArgsJsonUtf8 = nullptr;
	const char* ConversationIdUtf8 = nullptr;
	if (!PyArg_ParseTuple(Args, "ss|s", &ToolNameUtf8, &ArgsJsonUtf8, &ConversationIdUtf8))
	{
		return nullptr; // PyArg_ParseTuple sets the exception
	}

	const FString ToolName = UTF8_TO_TCHAR(ToolNameUtf8);
	const FString ArgsJson = UTF8_TO_TCHAR(ArgsJsonUtf8);
	const FString ConversationId = (ConversationIdUtf8 && *ConversationIdUtf8)
		? FString(UTF8_TO_TCHAR(ConversationIdUtf8))
		: FString(TEXT("default"));

	// Validate server instance
	if (!GServerInstance)
	{
		PyErr_SetString(PyExc_RuntimeError, "MCP server instance not set");
		return nullptr;
	}

	// Increment tool call counter for audit logging
	++GToolCallCount;

	// Look up tool in registry
	TSharedPtr<IClaireonTool> Tool = GServerInstance->FindTool(ToolName);
	if (!Tool.IsValid())
	{
		PyErr_Format(PyExc_KeyError, "Unknown tool: %s", ToolNameUtf8);
		return nullptr;
	}

	// Deserialize arguments JSON to FJsonObject
	TSharedPtr<FJsonObject> Arguments;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ArgsJson);
	if (!FJsonSerializer::Deserialize(JsonReader, Arguments) || !Arguments.IsValid())
	{
		PyErr_Format(PyExc_ValueError, "Invalid JSON arguments for tool '%s': %s", ToolNameUtf8, ArgsJsonUtf8);
		return nullptr;
	}

	// Precondition: block tools that require no PIE session
	if (Tool->RequiresNoPIE() && IsValid(GEditor) && GEditor->IsPlaySessionInProgress())
	{
		PyErr_Format(PyExc_RuntimeError, "Tool '%s' cannot be used while PIE is running. Stop PIE first.", TCHAR_TO_UTF8(*ToolName));
		return nullptr;
	}

	// Precondition: block tools that require an editor world
	if (Tool->RequiresEditorWorld())
	{
		FClaireonWorldReadinessResult WorldResult = FClaireonWorldReadiness::Check();
		if (!WorldResult.bReady)
		{
			PyErr_Format(PyExc_RuntimeError, "Tool '%s' failed: %s %s", TCHAR_TO_UTF8(*ToolName), TCHAR_TO_UTF8(*WorldResult.Message), TCHAR_TO_UTF8(*WorldResult.RecoveryHint));
			return nullptr;
		}
	}

	// Helper: build a BlockedByOtherTool error result with the same shape as the
	// per-asset template at Tools/ClaireonBlueprintGraphTool_Open.cpp:184-194.
	auto MakeBlockedResult = [&ToolName](const FMCPSession& Blocker) -> IClaireonTool::FToolResult
	{
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return IClaireonTool::MakeErrorResult(FString::Printf(
			TEXT("Tool '%s' blocked: %s session %s holds the lock (last activity %dm %ds ago). Close that session first, or call session_release with session_id='%s'."),
			*ToolName, *Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*Blocker.SessionId));
	};

	// Session-mode dispatch. See EClaireonToolSessionMode in IClaireonTool.h.
	const EClaireonToolSessionMode SessionMode = Tool->GetSessionMode();

	// Carve-out: session_release and session_list are always allowed
	// regardless of held sessions, so an operator can always recover from a stuck lock.
	// Tool names are bare; registry key matches GetName() exactly.
	const bool bIsSessionRecoveryTool =
		(ToolName == TEXT("session_release")) ||
		(ToolName == TEXT("session_list"));

	FString EditorWideSessionIdToCloseAfterExecute; // Empty unless we acquired one below.
	IClaireonTool::FToolResult Result;
	bool bExecuted = false;

	if (!bIsSessionRecoveryTool)
	{
		switch (SessionMode)
		{
		case EClaireonToolSessionMode::ReadOnly:
			// Forward unconditionally; no FClaireonSessionManager interaction.
			break;

		case EClaireonToolSessionMode::RequiresSession:
			// The tool (or its RAII helper inside Execute) calls OpenSession on the
			// asset(s) it mutates and surfaces BlockedByOtherTool on contention. The
			// bridge does not pre-acquire here because per-asset locks are keyed by
			// asset path, which the bridge does not know without parsing tool args.
			break;

		case EClaireonToolSessionMode::Bypass:
		{
			// Refuse if any session (per-asset or editor-wide) is held by a different tool.
			const TArray<FMCPSession> Held = FClaireonSessionManager::Get().ListSessions();
			const FMCPSession* Conflicting = nullptr;
			for (const FMCPSession& S : Held)
			{
				if (S.ToolName != ToolName)
				{
					Conflicting = &S;
					break;
				}
			}
			if (Conflicting)
			{
				Result = MakeBlockedResult(*Conflicting);
				bExecuted = true;
			}
			break;
		}

		case EClaireonToolSessionMode::EditorWide:
		{
			FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenEditorWideSession(ToolName);
			if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
			{
				const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
				Result = MakeBlockedResult(Blocker);
				bExecuted = true;
			}
			else if (OpenResult.Result == EOpenSessionResult::Success)
			{
				EditorWideSessionIdToCloseAfterExecute = OpenResult.SessionId;
			}
			break;
		}
		}
	}

	// Guarantee release of the editor-wide lock on every code path.
	ON_SCOPE_EXIT
	{
		if (!EditorWideSessionIdToCloseAfterExecute.IsEmpty())
		{
			FClaireonSessionManager::Get().CloseEditorWideSession(EditorWideSessionIdToCloseAfterExecute);
		}
	};

	// Execute the tool with SEH crash protection (skip when short-circuited above).
	if (!bExecuted)
	{
		FClaireonSafeExecResult SafeResult = ClaireonSafeExec::ExecuteTool(Tool.Get(), Arguments);
		Result = MoveTemp(SafeResult.ToolResult);
	}

	// If error, raise Python RuntimeError with a structured envelope matching
	// the success-path shape at lines 256-278 so callers can read
	// e.args[1]["data"], e.args[1]["summary"], and e.args[1]["warnings"].
	if (Result.bIsError)
	{
		FString ErrorStr = Result.ErrorMessage;

		// Build error envelope matching the success-path shape.
		TSharedPtr<FJsonObject> ErrorEnvelope = MakeShared<FJsonObject>();

		if (Result.Data.IsValid())
		{
			ErrorEnvelope->SetObjectField(TEXT("data"), Result.Data);
		}
		else
		{
			ErrorEnvelope->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
		}

		if (Result.Hint.IsValid())
		{
			ErrorEnvelope->SetObjectField(TEXT("hint"), Result.Hint);
		}

		ErrorEnvelope->SetStringField(TEXT("summary"), Result.Summary);

		TArray<TSharedPtr<FJsonValue>> ErrorWarningsArray;
		for (const FString& Warning : Result.Warnings)
		{
			ErrorWarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		ErrorEnvelope->SetArrayField(TEXT("warnings"), ErrorWarningsArray);

		// Serialize envelope to JSON string (same writer policy as success path).
		FString ErrorEnvelopeJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> ErrorWriter =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorEnvelopeJson);
		FJsonSerializer::Serialize(ErrorEnvelope.ToSharedRef(), ErrorWriter);
		ErrorWriter->Close();

		// Build the message string.
		FString MessageStr = FString::Printf(
			TEXT("Tool '%s' failed: %s"), *ToolName, *ErrorStr);

		// Parse envelope JSON to a Python dict via json.loads so the object
		// graph matches the success path exactly.
		PyObject* JsonModule = PyImport_ImportModule("json");
		PyObject* EnvelopePy = nullptr;
		if (JsonModule)
		{
			EnvelopePy = PyObject_CallMethod(JsonModule, "loads", "s", TCHAR_TO_UTF8(*ErrorEnvelopeJson));
			Py_DECREF(JsonModule);
		}
		if (!EnvelopePy)
		{
			// Clear any exception set by the failed json.loads call and fall
			// back to the message-only path so behaviour never regresses.
			PyErr_Clear();
			PyErr_Format(PyExc_RuntimeError, "Tool '%s' failed: %s", ToolNameUtf8, TCHAR_TO_UTF8(*ErrorStr));
			return nullptr;
		}

		PyObject* MessagePy = PyUnicode_FromString(TCHAR_TO_UTF8(*MessageStr));
		PyObject* ArgsTuple = PyTuple_Pack(2, MessagePy, EnvelopePy);

		PyErr_SetObject(PyExc_RuntimeError, ArgsTuple);

		Py_DECREF(MessagePy);
		Py_DECREF(EnvelopePy);
		Py_DECREF(ArgsTuple);
		return nullptr;
	}

	// No spill routing on this path. ClaireonBridge is the in-process Python entry
	// (claireon.<tool>(...) called from a python_execute script). The result becomes
	// a Python dict consumed by the running script -- it never crosses a wire
	// where spill saves context. Only the OUTER python_execute call benefits from
	// spill, and that is owned by ClaireonTool_ExecutePython::Execute which routes
	// its own stdout/uelog streams. HTTP `tools/call` requests reach the agent
	// via ClaireonServer.cpp and are returned inline as well.
	//
	// Previously this path ran FClaireonOutputGate::RouteResult with GenericData,
	// which spilled inner-tool results above 8 KiB and silently turned them into
	// {"__mcp_spilled__": true, "spilled_streams": [...]} -- so naive script
	// code like `claireon.uobject_inspect(...)["data"]["properties"]` returned [].
	// Spill here was strictly harmful: it cost a disk write to save context that
	// was never going to be sent anywhere.

	// Serialize the result envelope: {"data": ..., "summary": "...", "warnings": [...]}
	// Field shape is owned by BuildResultEnvelope so tests can exercise the
	// same code path without dragging in the CPython C API.
	TSharedPtr<FJsonObject> Envelope = BuildResultEnvelope(Result);

	// Serialize to condensed JSON string (no extra whitespace)
	FString EnvelopeJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EnvelopeJson);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Writer->Close();

	return PyUnicode_FromString(TCHAR_TO_UTF8(*EnvelopeJson));
}

// --- Claireon Python module bootstrap ---

namespace ClaireonBridgeBootstrapInternal
{
	/** Validates a token is a Python-identifier-safe bare name:
	 *  matches `[A-Za-z_][A-Za-z0-9_]*`. Empty is rejected; '.' is rejected. */
	static bool IsValidPyIdentifier(const FString& Token)
	{
		if (Token.IsEmpty())
		{
			return false;
		}
		const TCHAR First = Token[0];
		const bool bFirstOk = (First == TEXT('_'))
			|| (First >= TEXT('A') && First <= TEXT('Z'))
			|| (First >= TEXT('a') && First <= TEXT('z'));
		if (!bFirstOk)
		{
			return false;
		}
		for (int32 i = 1; i < Token.Len(); ++i)
		{
			const TCHAR Ch = Token[i];
			const bool bOk = (Ch == TEXT('_'))
				|| (Ch >= TEXT('A') && Ch <= TEXT('Z'))
				|| (Ch >= TEXT('a') && Ch <= TEXT('z'))
				|| (Ch >= TEXT('0') && Ch <= TEXT('9'));
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	/** Escapes a string for embedding inside a JSON string literal. */
	static void JsonEscapeInline(FString& InOut)
	{
		InOut.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		InOut.ReplaceInline(TEXT("\""), TEXT("\\\""));
		InOut.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		InOut.ReplaceInline(TEXT("\r"), TEXT(""));
		InOut.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	}
}

void FClaireonBridge::BuildAndRunBootstrap()
{
	using namespace ClaireonBridgeBootstrapInternal;

	// Per-namespace catalog. Always seed "claireon" so `import claireon` works
	// even when the registry has zero claireon-namespaced tools.
	TMap<FString, TArray<FString>> NamespaceEntries; // namespace -> array of JSON entry strings
	NamespaceEntries.FindOrAdd(TEXT("claireon"));
	TMap<FString, int32> NamespaceCounts;
	NamespaceCounts.FindOrAdd(TEXT("claireon"), 0);

	if (GServerInstance)
	{
		const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = GServerInstance->GetTools();

		for (const auto& Pair : Tools)
		{
			const FString& ToolKey = Pair.Key;
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid())
			{
				continue;
			}

			// Resolve namespace + name from the tool itself (single source of
			// truth). Strict-separation contract per Spec B: '.' is disallowed
			// in both. Recursion guard targets (claireon, python_execute).
			const FString Namespace = Tool->GetNamespace();
			const FString Name = Tool->GetName();

			// Validate namespace and name as Python-identifier-safe bare tokens.
			// Name is sealed as GetCategory() + "_" + GetOperation(); both
			// halves must themselves be bare identifiers and the composed
			// result must contain no dot.
			if (!IsValidPyIdentifier(Namespace) || !IsValidPyIdentifier(Name))
			{
				UE_LOG(LogClaireon, Error,
					TEXT("[MCP Bootstrap] REJECTED tool '%s' -- name '%s' or namespace '%s' is not a valid Python identifier (GetCategory() and GetOperation() must each be bare identifiers)."),
					*ToolKey, *Name, *Namespace);
				continue;
			}

			// Skip python_execute to avoid recursion (only the canonical
			// python_execute is the recursion sink).
			if (Namespace == TEXT("claireon") && Name == TEXT("python_execute"))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
			if (!Schema.IsValid())
			{
				UE_LOG(LogClaireon, Warning, TEXT("[MCP Bootstrap] Skipping tool '%s' -- GetInputSchema() returned null"), *ToolKey);
				continue;
			}

			// Extract required array
			TArray<FString> RequiredNames;
			const TArray<TSharedPtr<FJsonValue>>* RequiredArray = nullptr;
			if (Schema->TryGetArrayField(TEXT("required"), RequiredArray))
			{
				for (const TSharedPtr<FJsonValue>& Val : *RequiredArray)
				{
					FString ReqName;
					if (Val->TryGetString(ReqName))
					{
						RequiredNames.Add(ReqName);
					}
				}
			}

			// Extract properties keys
			TArray<FString> PropertyNames;
			const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
			if (Schema->TryGetObjectField(TEXT("properties"), PropertiesObj))
			{
				(*PropertiesObj)->Values.GetKeys(PropertyNames);
			}

			// Build this tool's JSON entry
			FString Description = Tool->GetDescription();
			JsonEscapeInline(Description);

			FString EntryJson;
			EntryJson += TEXT("{\"namespace\":\"") + Namespace.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			EntryJson += TEXT(",\"name\":\"") + Name.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			EntryJson += TEXT(",\"key\":\"") + ToolKey.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			EntryJson += TEXT(",\"description\":\"") + Description + TEXT("\"");

			// Required array
			EntryJson += TEXT(",\"required\":[");
			for (int32 i = 0; i < RequiredNames.Num(); ++i)
			{
				if (i > 0) EntryJson += TEXT(",");
				EntryJson += TEXT("\"") + RequiredNames[i].Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			}
			EntryJson += TEXT("]");

			// Properties array (just the names)
			EntryJson += TEXT(",\"properties\":[");
			for (int32 i = 0; i < PropertyNames.Num(); ++i)
			{
				if (i > 0) EntryJson += TEXT(",");
				EntryJson += TEXT("\"") + PropertyNames[i].Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			}
			EntryJson += TEXT("]}");

			NamespaceEntries.FindOrAdd(Namespace).Add(MoveTemp(EntryJson));
			NamespaceCounts.FindOrAdd(Namespace, 0)++;
		}
	}
	else
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP Bootstrap] GServerInstance is null -- claireon module will have no tools"));
	}

	// Build flat catalog JSON (one array, each entry carries `namespace`).
	// The Python bootstrap groups internally per Spec A.
	FString CatalogJson = TEXT("[");
	{
		bool bFirst = true;
		for (const auto& NsPair : NamespaceEntries)
		{
			for (const FString& Entry : NsPair.Value)
			{
				if (!bFirst) CatalogJson += TEXT(",");
				bFirst = false;
				CatalogJson += Entry;
			}
		}
	}
	CatalogJson += TEXT("]");

	// Build the namespaces-to-seed JSON list. Always includes "claireon" plus
	// every namespace observed in the registry (so empty namespaces still
	// show up as importable modules with __all__ == []).
	FString NamespacesJson = TEXT("[");
	{
		bool bFirst = true;
		for (const auto& NsPair : NamespaceEntries)
		{
			if (!bFirst) NamespacesJson += TEXT(",");
			bFirst = false;
			NamespacesJson += TEXT("\"") + NsPair.Key.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
		}
	}
	NamespacesJson += TEXT("]");

	// Update PreviousNamespaces for the next RebuildClaireonModule pass.
	PreviousNamespaces.Empty();
	for (const auto& NsPair : NamespaceEntries)
	{
		PreviousNamespaces.Add(NsPair.Key);
	}

	// Escape single quotes for Python single-quoted-string-literal embedding.
	FString EscapedCatalog = CatalogJson;
	EscapedCatalog.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedCatalog.ReplaceInline(TEXT("'"), TEXT("\\'"));

	FString EscapedNamespaces = NamespacesJson;
	EscapedNamespaces.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedNamespaces.ReplaceInline(TEXT("'"), TEXT("\\'"));

	// Build the full bootstrap string: sleep patch + catalog injection + per-namespace module construction.
	// Index helpers (`index_search`, etc.) are scoped to `claireon` only -- non-default namespaces
	// hold only their own tools, no index helpers, no cross-namespace dispatch.
	const FString BootstrapCode = FString::Printf(TEXT(
		// --- Sleep monkey-patch (runs once, idempotent via hasattr guard) ---
		"import time as _time\n"
		"if not hasattr(_time, '_original_sleep'):\n"
		"    _time._original_sleep = _time.sleep\n"
		"    def _noop_sleep(seconds=0):\n"
		"        import warnings\n"
		"        warnings.warn(f'time.sleep({seconds}) intercepted and skipped -- sleeping is disabled in Claireon MCP to prevent editor hangs.', RuntimeWarning, stacklevel=2)\n"
		"    _time.sleep = _noop_sleep\n"
		"try:\n"
		"    import asyncio as _asyncio\n"
		"    if not hasattr(_asyncio, '_original_sleep'):\n"
		"        _asyncio._original_sleep = _asyncio.sleep\n"
		"        async def _noop_async_sleep(seconds=0, result=None):\n"
		"            import warnings\n"
		"            warnings.warn(f'asyncio.sleep({seconds}) intercepted and skipped.', RuntimeWarning, stacklevel=2)\n"
		"            return result\n"
		"        _asyncio.sleep = _noop_async_sleep\n"
		"except ImportError:\n"
		"    pass\n"
		"\n"
		// --- Catalog + namespace injection ---
		"_CATALOG_JSON = '%s'\n"
		"_NAMESPACES_JSON = '%s'\n"
		"\n"
		// --- Bootstrap: build sys.modules['<ns>'] for each namespace ---
		"import sys, types\n"
		"_modules_built = {}\n"
		"try:\n"
		"    import json, inspect, logging\n"
		"    import unreal as _u\n"
		"\n"
		"    _log = logging.getLogger('claireon.bootstrap')\n"
		"    _catalog = json.loads(_CATALOG_JSON)\n"
		"    _namespaces = json.loads(_NAMESPACES_JSON)\n"
		"\n"
		"    # Seed every observed namespace (including 'claireon' even if empty)\n"
		"    # with an empty module shell. Per-namespace __doc__/__all__/__tools__\n"
		"    # are populated below.\n"
		"    for _ns_name in _namespaces:\n"
		"        _mod = types.ModuleType(_ns_name)\n"
		"        if _ns_name == 'claireon':\n"
		"            _mod.__doc__ = 'Claireon MCP tool bridge'\n"
		"        else:\n"
		"            _mod.__doc__ = f\"Claireon MCP tool bridge ('{_ns_name}' namespace)\"\n"
		"        _mod.__all__ = []\n"
		"        _mod.__tools__ = []\n"
		"        sys.modules[_ns_name] = _mod\n"
		"        _modules_built[_ns_name] = _mod\n"
		"\n"
		"    class _Unset:\n"
		"        \"\"\"Sentinel for omitted optional params.\"\"\"\n"
		"        __slots__ = ()\n"
		"        def __repr__(self): return '<unset>'\n"
		"    _UNSET = _Unset()\n"
		"\n"
		"    def _make_tool_fn(ns_name, full_name, short_name, dispatch_key, doc, required, optional):\n"
		"        \"\"\"Generate an MCP-dispatched tool function using exec().\n"
		"        Function definition uses short_name (bare identifier).\n"
		"        Dispatch goes to dispatch_key (registry key, e.g. 'foo').\"\"\"\n"
		"        param_parts = list(required) + [f'{p}=_UNSET' for p in optional]\n"
		"        param_str = ', '.join(param_parts)\n"
		"        _sep = ', ' if param_str else ''\n"
		"        all_params = list(required) + list(optional)\n"
		"        dict_expr = '{' + ', '.join(f'\"' + p + '\": ' + p for p in all_params) + '}'\n"
		"        safe_doc = doc.replace('\\\\', '\\\\\\\\').replace('\"\"\"', \"'''\")\n"
		"        func_code = f'def {short_name}({param_str}{_sep}**kwargs):\\n'\n"
		"        func_code += f'    \"\"\"{safe_doc}\"\"\"\\n'\n"
		"        func_code += f'    _all = {dict_expr}\\n'\n"
		"        func_code += '    payload = {k: v for k, v in _all.items() if v is not _UNSET}\\n'\n"
		"        func_code += '    payload.update(kwargs)\\n'\n"
		"        func_code += f'    return json.loads(_u._mcp_call_tool({dispatch_key!r}, json.dumps(payload)))\\n'\n"
		"        namespace = {'_UNSET': _UNSET, '_u': _u, 'json': json}\n"
		"        exec(func_code, namespace)\n"
		"        fn = namespace[short_name]\n"
		"        fn.__module__ = ns_name\n"
		"        fn.__qualname__ = full_name\n"
		"        return fn\n"
		"\n"
		"    for _entry in _catalog:\n"
		"        _ns = _entry['namespace']\n"
		"        _short = _entry['name']\n"
		"        _dispatch_key = _entry.get('key', _short)\n"
		"        _full = _ns + '.' + _short\n"
		"        _required = _entry.get('required', [])\n"
		"        _optional = [p for p in _entry.get('properties', []) if p not in _required]\n"
		"        _mod = _modules_built.get(_ns)\n"
		"        if _mod is None:\n"
		"            # Defensive: namespace wasn't in the seed list (shouldn't happen post-validation).\n"
		"            _mod = types.ModuleType(_ns)\n"
		"            _mod.__doc__ = f\"Claireon MCP tool bridge ('{_ns}' namespace)\"\n"
		"            _mod.__all__ = []\n"
		"            _mod.__tools__ = []\n"
		"            sys.modules[_ns] = _mod\n"
		"            _modules_built[_ns] = _mod\n"
		"        try:\n"
		"            _fn = _make_tool_fn(_ns, _full, _short, _dispatch_key,\n"
		"                _entry.get('description', ''), _required, _optional)\n"
		"            setattr(_mod, _short, _fn)\n"
		"            _mod.__all__.append(_short)\n"
		"            _mod.__tools__.append(_entry)\n"
		"        except SyntaxError:\n"
		"            _log.warning('Failed to generate tool function %%s.%%s (SyntaxError); skipping', _ns, _short)\n"
		"\n"
		"    for _ns_name in _namespaces:\n"
		"        _mod = _modules_built.get(_ns_name)\n"
		"        if _mod is not None:\n"
		"            _log.info('%%s module bootstrapped: %%d tools', _ns_name, len(_mod.__all__))\n"
		"\n"
		"except Exception as _bootstrap_err:\n"
		"    import traceback, logging as _logging\n"
		"    _logging.getLogger('claireon.bootstrap').error('claireon bootstrap failed: %%s', _bootstrap_err)\n"
		"    _logging.getLogger('claireon.bootstrap').error(traceback.format_exc())\n"
		"    if 'claireon' not in sys.modules:\n"
		"        _claireon = types.ModuleType('claireon')\n"
		"        _claireon.__bootstrap_error__ = str(_bootstrap_err)\n"
		"        sys.modules['claireon'] = _claireon\n"
	), *EscapedCatalog, *EscapedNamespaces);

	PyRun_SimpleString(TCHAR_TO_UTF8(*BootstrapCode));
	UE_LOG(LogClaireon, Display, TEXT("[MCP Bootstrap] Claireon Python module bootstrap executed (%d namespaces)"), NamespaceEntries.Num());
}

void FClaireonBridge::RebuildClaireonModule()
{
	PyGILState_STATE GILState = PyGILState_Ensure();

	// Capture the previous namespace set so we can purge any that drop out.
	const TSet<FString> Previous = PreviousNamespaces;

	BuildAndRunBootstrap();

	// Determine which namespaces were materialised by the previous pass but
	// not by this pass; delete their sys.modules entries so the next `import`
	// fails cleanly (Python convention: stale references already held by user
	// code keep working until the next re-import). Always preserve `claireon`.
	TArray<FString> Stale;
	for (const FString& Ns : Previous)
	{
		if (Ns == TEXT("claireon"))
		{
			continue;
		}
		if (!PreviousNamespaces.Contains(Ns))
		{
			Stale.Add(Ns);
		}
	}
	if (Stale.Num() > 0)
	{
		FString DelCode = TEXT("import sys\n");
		for (const FString& Ns : Stale)
		{
			DelCode += FString::Printf(TEXT("sys.modules.pop('%s', None)\n"),
				*Ns.Replace(TEXT("'"), TEXT("\\'")));
		}
		PyRun_SimpleString(TCHAR_TO_UTF8(*DelCode));
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Bootstrap] Cleaned up %d orphaned namespace(s) after rebuild"), Stale.Num());
	}

	PyGILState_Release(GILState);
	UE_LOG(LogClaireon, Display, TEXT("[MCP Bootstrap] Claireon Python module rebuilt after tool registry change"));
}

// --- Deferred world-transition actions ---

void FClaireonBridge::EnqueueDeferredAction(FClaireonDeferredAction Action)
{
	UE_LOG(LogClaireon, Log, TEXT("[MCP Bridge] Enqueued deferred action type=%d payload=%s"),
		static_cast<int32>(Action.Type), *Action.Payload);
	GDeferredActions.Add(MoveTemp(Action));
}

bool FClaireonBridge::HasDeferredActions()
{
	return GDeferredActions.Num() > 0;
}

bool FClaireonBridge::HasDeferredWorldTransition()
{
	for (const FClaireonDeferredAction& Action : GDeferredActions)
	{
		switch (Action.Type)
		{
			case EClaireonDeferredActionType::LoadMap:
			case EClaireonDeferredActionType::DuplicateAndOpenMap:
			case EClaireonDeferredActionType::PIEStart:
			case EClaireonDeferredActionType::PIEStop:
				return true;
			default:
				break;
		}
	}
	return false;
}

TArray<FClaireonDeferredAction> FClaireonBridge::DrainDeferredActions()
{
	TArray<FClaireonDeferredAction> Actions = MoveTemp(GDeferredActions);
	GDeferredActions.Empty();
	return Actions;
}

void FClaireonBridge::RunWorldTransitionBarrier()
{
	UE_LOG(LogClaireon, Log, TEXT("[MCP Bridge] Running world-transition barrier"));

	if (IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get())
	{
		FPythonCommandEx PurgeCommand;
		PurgeCommand.Command = TEXT(
			"import gc, sys, unreal\n"
			"\n"
			"# Build a set of dict ids that belong to infrastructure and must\n"
			"# never be modified: module dicts, type/class dicts, builtins.\n"
			"_protected = set()\n"
			"\n"
			"# Protect every loaded module's __dict__  (unreal, sys, json, mcp_*, etc.)\n"
			"for _mod in sys.modules.values():\n"
			"    if hasattr(_mod, '__dict__'):\n"
			"        _protected.add(id(vars(_mod)))\n"
			"\n"
			"# Protect __dict__ of every type/class reachable from the unreal module\n"
			"# (e.g. unreal.Actor, unreal.Object, unreal.EditorAssetLibrary, ...)\n"
			"for _name in dir(unreal):\n"
			"    try:\n"
			"        _attr = getattr(unreal, _name)\n"
			"    except Exception:\n"
			"        continue\n"
			"    if isinstance(_attr, type) and hasattr(_attr, '__dict__'):\n"
			"        _protected.add(id(vars(_attr)))\n"
			"        # Also protect bases (MRO) so inherited class dicts are safe\n"
			"        for _base in type.mro(_attr):\n"
			"            if hasattr(_base, '__dict__'):\n"
			"                _protected.add(id(vars(_base)))\n"
			"\n"
			"# Protect builtins\n"
			"import builtins as _builtins\n"
			"_protected.add(id(vars(_builtins)))\n"
			"_protected.add(id(vars(type)))\n"
			"_protected.add(id(vars(object)))\n"
			"\n"
			"# Walk all tracked objects; only null unreal.Object refs in\n"
			"# unprotected dicts (orphaned execution namespaces, user containers).\n"
			"_nulled = 0\n"
			"for _obj in gc.get_objects():\n"
			"    if isinstance(_obj, dict) and id(_obj) not in _protected:\n"
			"        for _k, _v in list(_obj.items()):\n"
			"            if isinstance(_v, unreal.Object):\n"
			"                _obj[_k] = None\n"
			"                _nulled += 1\n"
			"            elif isinstance(_v, (list, tuple)):\n"
			"                if _v and isinstance(_v[0], unreal.Object):\n"
			"                    _obj[_k] = None\n"
			"                    _nulled += 1\n"
			"\n"
			"gc.collect()\n"
			"gc.collect()\n"
			"gc.collect()\n"
			"del _protected, _nulled\n");
		PurgeCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
		PurgeCommand.Flags = EPythonCommandFlags::Unattended;
		PythonPlugin->ExecPythonCommandEx(PurgeCommand);
	}

	// Unreal GC pass to finalize objects Python just released
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	UE_LOG(LogClaireon, Log, TEXT("[MCP Bridge] World-transition barrier complete"));
}

namespace ClaireonBridgeUnsavedWorkFilter
{
	/** Return true if a package name should be kept (i.e. counted as unsaved
	 *  user work). Pure-string predicate so unit tests can call it without
	 *  constructing UPackages. */
	static bool ShouldKeepPackageName(const FString& PackageName)
	{
		if (PackageName.IsEmpty()) { return false; }
		if (PackageName.StartsWith(TEXT("/Temp/"))) { return false; }
		if (PackageName.StartsWith(TEXT("/Memory/"))) { return false; }
		return true;
	}
}

bool FClaireonBridge::EnsureNoLeakedWorlds(TArray<FClaireonLeakedWorld>& OutRemaining)
{
	OutRemaining.Reset();

	// Identify the editor world's outer package once -- streaming
	// sublevels of the active map share that package and must NOT
	// be flagged as leaks.
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UPackage* EditorWorldPackage = EditorWorld ? EditorWorld->GetOutermost() : nullptr;

	// Walk every loaded World and decide skip vs. candidate-for-unload.
	TArray<UPackage*> NonDirtyCandidates;
	for (TObjectIterator<UWorld> It; It; ++It)
	{
		UWorld* W = *It;
		if (!W) { continue; }

		// Skip editor world.
		if (W == EditorWorld) { continue; }

		// Skip PIE / preview worlds. EditorPreview covers asset thumbnails.
		const EWorldType::Type Type = W->WorldType;
		if (Type == EWorldType::PIE
			|| Type == EWorldType::GamePreview
			|| Type == EWorldType::EditorPreview)
		{
			continue;
		}

		// Skip Worlds whose outer package IS the editor world's
		// package (streaming sublevels of the active map).
		UPackage* WP = W->GetOutermost();
		if (WP && WP == EditorWorldPackage) { continue; }

		// Skip transient Worlds (parity with EditorServer.cpp Map_Load).
		if (WP == GetTransientPackage() || !WP) { continue; }

		// Candidate.
		FClaireonLeakedWorld Entry;
		Entry.PackageName = WP->GetName();
		if (WP->IsDirty())
		{
			Entry.bDirty = true;
			OutRemaining.Add(Entry);
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP Guard] Leaked dirty World detected (will not auto-unload): %s"),
				*Entry.PackageName);
			continue;
		}

		Entry.bUnloadAttempted = true;
		NonDirtyCandidates.Add(WP);
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Guard] Leaked World detected, attempting unload: %s"),
			*Entry.PackageName);
	}

	// Attempt to unload non-dirty candidates inside heart-beat
	// suppression scopes (the unload can be slow).
	if (NonDirtyCandidates.Num() > 0)
	{
		FSlowHeartBeatScope SuspendHeartBeat;
		FDisableHitchDetectorScope SuspendHitchDetector;

		// FUnloadPackageParams (PackageTools.h) holds OutErrorMessage
		// BY VALUE -- read it back from Params after the call.
		UPackageTools::FUnloadPackageParams Params(NonDirtyCandidates);
		Params.bUnloadDirtyPackages = false;
		Params.bResetTransBuffer = true;
		const bool bUnloadResult = UPackageTools::UnloadPackages(Params);
		if (!bUnloadResult)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP Guard] UnloadPackages reported failure: %s"),
				*Params.OutErrorMessage.ToString());
		}

		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	// Re-iterate to find anything still loaded after unload+GC.
	// Anything dirty we already added to OutRemaining; here we add
	// any non-dirty package that survived the unload pass (pinned
	// by another reference).
	{
		TSet<FString> StillLoadedNames;
		for (TObjectIterator<UWorld> It; It; ++It)
		{
			UWorld* W = *It;
			if (!W) { continue; }
			if (W == EditorWorld) { continue; }
			const EWorldType::Type Type = W->WorldType;
			if (Type == EWorldType::PIE
				|| Type == EWorldType::GamePreview
				|| Type == EWorldType::EditorPreview) { continue; }
			UPackage* WP = W->GetOutermost();
			if (!WP || WP == EditorWorldPackage || WP == GetTransientPackage()) { continue; }
			StillLoadedNames.Add(WP->GetName());
		}

		for (UPackage* WP : NonDirtyCandidates)
		{
			if (!WP) { continue; }
			const FString PackageName = WP->GetName();
			if (StillLoadedNames.Contains(PackageName))
			{
				FClaireonLeakedWorld Entry;
				Entry.PackageName = PackageName;
				Entry.bUnloadAttempted = true;
				Entry.bUnloadSucceeded = false;
				OutRemaining.Add(Entry);
				UE_LOG(LogClaireon, Error,
					TEXT("[MCP Guard] Failed to unload leaked World (still pinned): %s"),
					*PackageName);
			}
		}
	}

	return OutRemaining.Num() == 0;
}

FString FClaireonBridge::FormatLeakedWorldError(const TArray<FClaireonLeakedWorld>& Leaks)
{
	if (Leaks.Num() == 0)
	{
		return FString();
	}

	TArray<FString> DirtyNames;
	TArray<FString> PinnedNames;
	for (const FClaireonLeakedWorld& L : Leaks)
	{
		if (L.bDirty) { DirtyNames.Add(L.PackageName); }
		else { PinnedNames.Add(L.PackageName); }
	}

	FString Msg = TEXT("[MCP Guard] Aborting world transition: leaked World(s) prevent transition.");
	if (DirtyNames.Num() > 0)
	{
		Msg += TEXT("\n  dirty (save or discard before transitioning): ");
		Msg += FString::Join(DirtyNames, TEXT(", "));
	}
	if (PinnedNames.Num() > 0)
	{
		Msg += TEXT("\n  pinned: ");
		Msg += FString::Join(PinnedNames, TEXT(", "));
	}
	Msg += TEXT("\nUse duplicate_and_open_map_async to duplicate+open atomically.");
	return Msg;
}

void FClaireonBridge::ReportDeferredActionAbort(const FString& Message)
{
	GDeferredActionAborts.Add(Message);
}

bool FClaireonBridge::EnsureNoUnsavedWork(TArray<FClaireonUnsavedPackage>& OutDirty)
{
	OutDirty.Reset();

	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
	FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

	for (UPackage* Pkg : DirtyPackages)
	{
		if (!Pkg) { continue; }
		if (Pkg == GetTransientPackage()) { continue; }
		const FString PackageName = Pkg->GetName();
		if (!ClaireonBridgeUnsavedWorkFilter::ShouldKeepPackageName(PackageName))
		{
			continue;
		}

		FClaireonUnsavedPackage Entry;
		Entry.PackageName = PackageName;
		Entry.bIsMapPackage = Pkg->ContainsMap();
		OutDirty.Add(Entry);
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Guard] Unsaved %s detected: %s"),
			Entry.bIsMapPackage ? TEXT("map") : TEXT("asset"),
			*Entry.PackageName);
	}

	return OutDirty.Num() == 0;
}

FString FClaireonBridge::FormatUnsavedWorkError(const TArray<FClaireonUnsavedPackage>& Dirty)
{
	if (Dirty.Num() == 0)
	{
		return FString();
	}

	TArray<FString> MapNames;
	TArray<FString> AssetNames;
	for (const FClaireonUnsavedPackage& D : Dirty)
	{
		if (D.bIsMapPackage) { MapNames.Add(D.PackageName); }
		else { AssetNames.Add(D.PackageName); }
	}

	FString Msg = TEXT("[MCP Guard] Aborting world transition: the editor has unsaved work that would be lost.");
	if (MapNames.Num() > 0)
	{
		Msg += TEXT("\n  unsaved map(s) (save or discard before transitioning): ");
		Msg += FString::Join(MapNames, TEXT(", "));
	}
	if (AssetNames.Num() > 0)
	{
		Msg += TEXT("\n  unsaved asset(s) (save or discard before transitioning): ");
		Msg += FString::Join(AssetNames, TEXT(", "));
	}
	Msg += TEXT("\nSave these packages (File > Save All / Ctrl+S) or revert them, then retry map_open. ");
	Msg += TEXT("Enable auto-save in Project Settings > Plugins > Claireon to save automatically before deferred actions.");
	return Msg;
}

bool FClaireonBridge::ShouldAbortDeferredLoadMap(FString& OutError)
{
	OutError.Reset();

	TArray<FClaireonLeakedWorld> Leaks;
	if (!EnsureNoLeakedWorlds(Leaks))
	{
		OutError = FormatLeakedWorldError(Leaks);
		return true;
	}

	TArray<FClaireonUnsavedPackage> Dirty;
	if (!EnsureNoUnsavedWork(Dirty))
	{
		OutError = FormatUnsavedWorkError(Dirty);
		return true;
	}

	return false;
}

TArray<FString> FClaireonBridge::DrainDeferredActionAborts()
{
	TArray<FString> Out = MoveTemp(GDeferredActionAborts);
	GDeferredActionAborts.Empty();
	return Out;
}
