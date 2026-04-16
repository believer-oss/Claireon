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
FClaireonServer* FClaireonBridge::GServerInstance = nullptr;
TAtomic<int32> FClaireonBridge::GToolCallCount(0);
TArray<FClaireonDeferredAction> FClaireonBridge::GDeferredActions;
FDelegateHandle FClaireonBridge::ToolsChangedHandle;
std::atomic<bool> FClaireonBridge::bClaireonModuleStale{false};

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

	// Add our Python modules directory to sys.path and pre-warm imports
	// to avoid blocking the game thread during first execute call
	{
		const FString PluginPythonDir = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Claireon"), TEXT("Content"), TEXT("Python")));
		if (FPaths::DirectoryExists(PluginPythonDir))
		{
			const FString WarmupCode = FString::Printf(
				TEXT("import sys; p = r'%s'; p not in sys.path and sys.path.insert(0, p)\n"
					 "import mcp_output_gate\n"
					 "import mcp_index_engine\n"
					 "import mcp_chunkers\n"),
				*PluginPythonDir);
			PyRun_SimpleString(TCHAR_TO_UTF8(*WarmupCode));
			UE_LOG(LogClaireon, Display, TEXT("[MCP Bridge] Added Python path and pre-warmed imports: %s"), *PluginPythonDir);
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

	if (!Result.Logs.IsEmpty())
	{
		Envelope->SetStringField(TEXT("logs"), Result.Logs);
	}

	if (!Result.UELog.IsEmpty())
	{
		Envelope->SetStringField(TEXT("ue_log"), Result.UELog);
	}

	// Serialize to condensed JSON string (no extra whitespace)
	FString EnvelopeJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&EnvelopeJson);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Writer->Close();

	return PyUnicode_FromString(TCHAR_TO_UTF8(*EnvelopeJson));
}

// --- Claireon Python module bootstrap ---

void FClaireonBridge::BuildAndRunBootstrap()
{
	// Build the tool catalog JSON from the current registry
	FString CatalogJson = TEXT("[]");
	if (GServerInstance)
	{
		const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = GServerInstance->GetTools();

		// Build a JSON array manually for the catalog
		FString JsonArray = TEXT("[");
		bool bFirst = true;

		for (const auto& Pair : Tools)
		{
			const FString& ToolName = Pair.Key;
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;

			// Skip python_execute to avoid recursion
			if (ToolName == TEXT("claireon.python_execute"))
			{
				continue;
			}

			// Skip tools that don't start with claireon.
			if (!ToolName.StartsWith(TEXT("claireon.")))
			{
				UE_LOG(LogClaireon, Warning, TEXT("[MCP Bootstrap] Skipping tool '%s' -- does not start with 'claireon.'"), *ToolName);
				continue;
			}

			TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
			if (!Schema.IsValid())
			{
				UE_LOG(LogClaireon, Warning, TEXT("[MCP Bootstrap] Skipping tool '%s' -- GetInputSchema() returned null"), *ToolName);
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
			if (!bFirst)
			{
				JsonArray += TEXT(",");
			}
			bFirst = false;

			// Escape description for JSON embedding
			FString Description = Tool->GetDescription();
			Description.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Description.ReplaceInline(TEXT("\""), TEXT("\\\""));
			Description.ReplaceInline(TEXT("\n"), TEXT("\\n"));
			Description.ReplaceInline(TEXT("\r"), TEXT(""));
			Description.ReplaceInline(TEXT("\t"), TEXT("\\t"));

			JsonArray += TEXT("{\"name\":\"") + ToolName.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			JsonArray += TEXT(",\"description\":\"") + Description + TEXT("\"");

			// Required array
			JsonArray += TEXT(",\"required\":[");
			for (int32 i = 0; i < RequiredNames.Num(); ++i)
			{
				if (i > 0) JsonArray += TEXT(",");
				JsonArray += TEXT("\"") + RequiredNames[i].Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			}
			JsonArray += TEXT("]");

			// Properties array (just the names)
			JsonArray += TEXT(",\"properties\":[");
			for (int32 i = 0; i < PropertyNames.Num(); ++i)
			{
				if (i > 0) JsonArray += TEXT(",");
				JsonArray += TEXT("\"") + PropertyNames[i].Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\"");
			}
			JsonArray += TEXT("]}");
		}

		JsonArray += TEXT("]");
		CatalogJson = JsonArray;
	}
	else
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP Bootstrap] GServerInstance is null -- claireon module will have no tools"));
	}

	// Escape single quotes in catalog JSON for Python string literal embedding
	// JSON uses double quotes, so single quotes should be rare, but be safe
	FString EscapedCatalog = CatalogJson;
	EscapedCatalog.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	EscapedCatalog.ReplaceInline(TEXT("'"), TEXT("\\'"));

	// Build the full bootstrap string: sleep patch + catalog injection + module construction
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
		// --- Catalog injection ---
		"_CATALOG_JSON = '%s'\n"
		"\n"
		// --- Bootstrap: build sys.modules['claireon'] ---
		"import sys, types\n"
		"_claireon = None\n"
		"try:\n"
		"    import json, inspect, logging\n"
		"    import unreal as _u\n"
		"\n"
		"    _log = logging.getLogger('claireon.bootstrap')\n"
		"    _catalog = json.loads(_CATALOG_JSON)\n"
		"    _claireon = types.ModuleType('claireon')\n"
		"    _claireon.__doc__ = 'Claireon MCP tool bridge'\n"
		"\n"
		"    class _Unset:\n"
		"        \"\"\"Sentinel for omitted optional params.\"\"\"\n"
		"        __slots__ = ()\n"
		"        def __repr__(self): return '<unset>'\n"
		"    _UNSET = _Unset()\n"
		"\n"
		"    def _make_tool_fn(full_name, short_name, doc, required, optional):\n"
		"        \"\"\"Generate an MCP-dispatched tool function using exec().\"\"\"\n"
		"        param_parts = list(required) + [f'{p}=_UNSET' for p in optional]\n"
		"        param_str = ', '.join(param_parts)\n"
		"        all_params = list(required) + list(optional)\n"
		"        dict_expr = '{' + ', '.join(f'\"' + p + '\": ' + p for p in all_params) + '}'\n"
		"        safe_doc = doc.replace('\\\\', '\\\\\\\\').replace('\"\"\"', \"'''\")\n"
		"        func_code = f'def {short_name}({param_str}):\\n'\n"
		"        func_code += f'    \"\"\"{safe_doc}\"\"\"\\n'\n"
		"        func_code += f'    _all = {dict_expr}\\n'\n"
		"        func_code += '    payload = {k: v for k, v in _all.items() if v is not _UNSET}\\n'\n"
		"        func_code += f'    return json.loads(_u._mcp_call_tool({full_name!r}, json.dumps(payload)))\\n'\n"
		"        namespace = {'_UNSET': _UNSET, '_u': _u, 'json': json}\n"
		"        exec(func_code, namespace)\n"
		"        fn = namespace[short_name]\n"
		"        fn.__module__ = 'claireon'\n"
		"        fn.__qualname__ = f'claireon.{short_name}'\n"
		"        return fn\n"
		"\n"
		"    for _entry in _catalog:\n"
		"        _full = _entry['name']\n"
		"        _short = _full[len('claireon.'):] if _full.startswith('claireon.') else _full\n"
		"        _required = _entry.get('required', [])\n"
		"        _optional = [p for p in _entry.get('properties', []) if p not in _required]\n"
		"        try:\n"
		"            setattr(_claireon, _short, _make_tool_fn(\n"
		"                _full, _short, _entry.get('description', ''), _required, _optional))\n"
		"        except SyntaxError:\n"
		"            _log.warning('Failed to generate tool function %%s (SyntaxError); skipping', _short)\n"
		"\n"
		"    # --- Index helper generation via introspection ---\n"
		"    _INDEX_METHOD_MAP = {\n"
		"        'search':        'index_search',\n"
		"        'get_index_info': 'index_info',\n"
		"        'list_indexes':  'index_list',\n"
		"        'stats':         'index_stats',\n"
		"        'search_all':    'index_search_all',\n"
		"        'dump':          'index_dump',\n"
		"        'load':          'index_load',\n"
		"        'clear':         'index_clear',\n"
		"        'expire':        'index_expire',\n"
		"    }\n"
		"    _INDEX_RETURN_WRAPPERS = {\n"
		"        'search':      \"{'index_id': index_id, 'query': query, 'results': _result}\",\n"
		"        'list_indexes': \"{'indexes': _result}\",\n"
		"        'search_all':  \"{'query': query, 'results': _result}\",\n"
		"    }\n"
		"\n"
		"    def _generate_index_helpers():\n"
		"        try:\n"
		"            import mcp_index_engine\n"
		"        except ImportError:\n"
		"            _log.warning('mcp_index_engine not importable; index helpers will not be available')\n"
		"            return []\n"
		"        engine_cls = mcp_index_engine.IndexEngine\n"
		"        helpers = []\n"
		"        for method_name, claireon_name in _INDEX_METHOD_MAP.items():\n"
		"            method = getattr(engine_cls, method_name, None)\n"
		"            if method is None or not callable(method):\n"
		"                _log.warning('IndexEngine.%%s not found or not callable; skipping', method_name)\n"
		"                continue\n"
		"            sig = inspect.signature(method)\n"
		"            params = [(name, p) for name, p in sig.parameters.items() if name != 'self']\n"
		"            required = [name for name, p in params if p.default is inspect.Parameter.empty]\n"
		"            optional = [name for name, p in params if p.default is not inspect.Parameter.empty]\n"
		"            doc = (method.__doc__ or '').strip().split('\\n')[0]\n"
		"            safe_doc = doc.replace('\\\\', '\\\\\\\\').replace('\"\"\"', \"'''\")\n"
		"            param_parts = list(required) + [f'{p}=_UNSET' for p in optional]\n"
		"            param_str = ', '.join(param_parts)\n"
		"            all_params = required + optional\n"
		"            dict_expr = '{' + ', '.join(f'\"' + p + '\": ' + p for p in all_params) + '}'\n"
		"            if method_name in _INDEX_RETURN_WRAPPERS:\n"
		"                return_expr = _INDEX_RETURN_WRAPPERS[method_name]\n"
		"                call_line = f'    _result = mcp_index_engine.get_engine().{method_name}(**kw)'\n"
		"                ret_line = f'    return {return_expr}'\n"
		"            else:\n"
		"                call_line = f'    return mcp_index_engine.get_engine().{method_name}(**kw)'\n"
		"                ret_line = ''\n"
		"            func_code = f'def {claireon_name}({param_str}):\\n'\n"
		"            func_code += f'    \"\"\"{safe_doc}\"\"\"\\n'\n"
		"            func_code += f'    _all = {dict_expr}\\n'\n"
		"            func_code += '    kw = {k: v for k, v in _all.items() if v is not _UNSET}\\n'\n"
		"            func_code += call_line + '\\n'\n"
		"            if ret_line:\n"
		"                func_code += ret_line + '\\n'\n"
		"            try:\n"
		"                namespace = {'_UNSET': _UNSET, 'mcp_index_engine': mcp_index_engine}\n"
		"                exec(func_code, namespace)\n"
		"            except SyntaxError:\n"
		"                _log.warning('Failed to generate index helper %%s (SyntaxError); skipping', claireon_name)\n"
		"                continue\n"
		"            fn = namespace[claireon_name]\n"
		"            fn.__module__ = 'claireon'\n"
		"            fn.__qualname__ = f'claireon.{claireon_name}'\n"
		"            setattr(_claireon, claireon_name, fn)\n"
		"            helpers.append(claireon_name)\n"
		"        return helpers\n"
		"\n"
		"    _index_helpers = _generate_index_helpers()\n"
		"    _claireon.__all__ = [\n"
		"        _e['name'][len('claireon.'):] if _e['name'].startswith('claireon.') else _e['name']\n"
		"        for _e in _catalog\n"
		"    ] + _index_helpers\n"
		"    _claireon.__tools__ = _catalog\n"
		"    sys.modules['claireon'] = _claireon\n"
		"    _log.info('claireon module bootstrapped: %%d tools + %%d index helpers', len(_catalog), len(_index_helpers))\n"
		"\n"
		"except Exception as _bootstrap_err:\n"
		"    import traceback, logging as _logging\n"
		"    _logging.getLogger('claireon.bootstrap').error('claireon bootstrap failed: %%s', _bootstrap_err)\n"
		"    _logging.getLogger('claireon.bootstrap').error(traceback.format_exc())\n"
		"    if _claireon is None:\n"
		"        _claireon = types.ModuleType('claireon')\n"
		"    _claireon.__bootstrap_error__ = str(_bootstrap_err)\n"
		"    sys.modules['claireon'] = _claireon\n"
	), *EscapedCatalog);

	PyRun_SimpleString(TCHAR_TO_UTF8(*BootstrapCode));
	UE_LOG(LogClaireon, Display, TEXT("[MCP Bootstrap] Claireon Python module bootstrap executed"));
}

void FClaireonBridge::RebuildClaireonModule()
{
	PyGILState_STATE GILState = PyGILState_Ensure();
	BuildAndRunBootstrap();
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
