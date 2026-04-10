// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonServer.h"
#include "ClaireonSafeExec.h"
#include "Tools/IClaireonTool.h"

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

// Static member initialization
bool FClaireonBridge::bIsRegistered = false;
FString FClaireonBridge::GLastExecuteResult;
FClaireonServer* FClaireonBridge::GServerInstance = nullptr;
TAtomic<int32> FClaireonBridge::GToolCallCount(0);
TArray<FClaireonDeferredAction> FClaireonBridge::GDeferredActions;

// PyMethodDef structs (static storage — CPython holds pointers to these)
static PyMethodDef MCPCallDef = {
	"_mcp_call_tool",
	&FClaireonBridge::MCPCallTool,
	METH_VARARGS,
	"Call an MCP tool by name. Args: (tool_name: str, args_json: str) -> str"
};

static PyMethodDef MCPResultDef = {
	"_mcp_set_result",
	&FClaireonBridge::MCPSetResult,
	METH_VARARGS,
	"Set the execute result JSON. Args: (result_json: str) -> None"
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
	PyObject* ResultFunc = PyCFunction_New(&MCPResultDef, nullptr);

	if (!CallFunc || !ResultFunc)
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP Bridge] Failed to create PyCFunction objects"));
		Py_XDECREF(CallFunc);
		Py_XDECREF(ResultFunc);
		PyGILState_Release(GILState);
		return;
	}

	// Register into the unreal module dict
	PyDict_SetItemString(UnrealDict, "_mcp_call_tool", CallFunc);
	PyDict_SetItemString(UnrealDict, "_mcp_set_result", ResultFunc);

	// PyDict_SetItemString increments refcount, so release our references
	Py_DECREF(CallFunc);
	Py_DECREF(ResultFunc);

	// Add our Python modules directory to sys.path and pre-warm imports
	// to avoid blocking the game thread during first execute call
	{
		const FString PluginPythonDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Claireon"), TEXT("Content"), TEXT("Python")));
		if (FPaths::DirectoryExists(PluginPythonDir))
		{
			const FString WarmupCode = FString::Printf(
				TEXT("import sys; p = r'%s'; p not in sys.path and sys.path.insert(0, p)\n"
					 "try:\n"
					 "    import mcp_output_gate\n"
					 "    import mcp_index_engine\n"
					 "    import mcp_chunkers\n"
					 "except Exception:\n"
					 "    pass\n"),
				*PluginPythonDir);
			PyRun_SimpleString(TCHAR_TO_UTF8(*WarmupCode));
			UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Added Python path and pre-warmed imports: %s"), *PluginPythonDir);
		}
	}

	bIsRegistered = true;
	UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Registered _mcp_call_tool and _mcp_set_result in unreal module"));

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

FString FClaireonBridge::GetLastExecuteResult()
{
	return GLastExecuteResult;
}

void FClaireonBridge::ResetLastExecuteResult()
{
	GLastExecuteResult.Empty();
}

int32 FClaireonBridge::GetToolCallCount()
{
	return GToolCallCount;
}

void FClaireonBridge::ResetToolCallCount()
{
	GToolCallCount = 0;
}

PyObject* FClaireonBridge::MCPCallTool(PyObject* /*Self*/, PyObject* Args)
{
	const char* ToolNameUtf8 = nullptr;
	const char* ArgsJsonUtf8 = nullptr;
	if (!PyArg_ParseTuple(Args, "ss", &ToolNameUtf8, &ArgsJsonUtf8))
	{
		return nullptr; // PyArg_ParseTuple sets the exception
	}

	const FString ToolName = UTF8_TO_TCHAR(ToolNameUtf8);
	const FString ArgsJson = UTF8_TO_TCHAR(ArgsJsonUtf8);

	// Special case: list all registered tool names
	if (ToolName == TEXT("__list_tools__"))
	{
		if (!GServerInstance)
		{
			PyErr_SetString(PyExc_RuntimeError, "MCP server instance not set");
			return nullptr;
		}

		TArray<FString> ToolNames;
		const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = GServerInstance->GetTools();
		ToolsMap.GetKeys(ToolNames);

		// Build JSON array of tool names
		FString JsonArray = TEXT("[");
		for (int32 i = 0; i < ToolNames.Num(); ++i)
		{
			if (i > 0)
			{
				JsonArray += TEXT(",");
			}
			JsonArray += TEXT("\"") + ToolNames[i].Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
		}
		JsonArray += TEXT("]");

		return PyUnicode_FromString(TCHAR_TO_UTF8(*JsonArray));
	}

	// Special case: get input schema for a tool (for positional arg mapping)
	if (ToolName == TEXT("__get_schema__"))
	{
		if (!GServerInstance)
		{
			PyErr_SetString(PyExc_RuntimeError, "MCP server instance not set");
			return nullptr;
		}
		FString SchemaToolName = UTF8_TO_TCHAR(ArgsJsonUtf8);
		TSharedPtr<IClaireonTool> SchemaTool = GServerInstance->FindTool(SchemaToolName);
		if (!SchemaTool.IsValid())
		{
			Py_RETURN_NONE;
		}
		TSharedPtr<FJsonObject> Schema = SchemaTool->GetInputSchema();
		if (!Schema.IsValid())
		{
			Py_RETURN_NONE;
		}
		FString SchemaJson;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> SchemaWriter =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SchemaJson);
		FJsonSerializer::Serialize(Schema.ToSharedRef(), SchemaWriter);
		SchemaWriter->Close();
		return PyUnicode_FromString(TCHAR_TO_UTF8(*SchemaJson));
	}

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

	// Execute the tool with SEH crash protection
	FClaireonSafeExecResult SafeResult = ClaireonSafeExec::ExecuteTool(Tool.Get(), Arguments);
	IClaireonTool::FToolResult Result = MoveTemp(SafeResult.ToolResult);

	// If error, raise Python RuntimeError
	if (Result.bIsError)
	{
		FString ErrorStr = Result.ErrorMessage;
		PyErr_Format(PyExc_RuntimeError, "Tool '%s' failed: %s", ToolNameUtf8, TCHAR_TO_UTF8(*ErrorStr));
		return nullptr;
	}

	// Serialize the result envelope: {"data": ..., "summary": "...", "warnings": [...]}
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();

	// Data field — use the tool's structured data, or empty object if null
	if (Result.Data.IsValid())
	{
		Envelope->SetObjectField(TEXT("data"), Result.Data);
	}
	else
	{
		Envelope->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
	}

	// Summary field
	Envelope->SetStringField(TEXT("summary"), Result.Summary);

	// Warnings array
	TArray<TSharedPtr<FJsonValue>> WarningsArray;
	for (const FString& Warning : Result.Warnings)
	{
		WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
	}
	Envelope->SetArrayField(TEXT("warnings"), WarningsArray);

	// Serialize to condensed JSON string (no extra whitespace)
	FString EnvelopeJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EnvelopeJson);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Writer->Close();

	return PyUnicode_FromString(TCHAR_TO_UTF8(*EnvelopeJson));
}

PyObject* FClaireonBridge::MCPSetResult(PyObject* /*Self*/, PyObject* Args)
{
	const char* ResultJsonUtf8 = nullptr;
	if (!PyArg_ParseTuple(Args, "s", &ResultJsonUtf8))
	{
		return nullptr; // PyArg_ParseTuple sets the exception
	}

	GLastExecuteResult = UTF8_TO_TCHAR(ResultJsonUtf8);
	Py_RETURN_NONE;
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
