// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"

// ---------------------------------------------------------------------------
// Regression tests for RunWorldTransitionBarrier's Python purge script.
//
// The bug these pin: the purge was dispatched with
// EPythonCommandExecutionMode::ExecuteStatement, which maps to CPython's
// Py_single_input and accepts only a single statement. The script is ~50 lines
// beginning with `import gc, sys, unreal`, so it raised SyntaxError at COMPILE
// time -- before any cleanup ran. Because the ExecPythonCommandEx return value
// was discarded and the command carried EPythonCommandFlags::Unattended, the
// failure was completely silent, and the Python-side reference nulling had
// never executed at any of the four call sites (MapDuplicate x2, MapOpen,
// PIEStart).
//
// These tests pin the COMPILE, not the effect. That is deliberate on two
// counts: the compile is precisely what broke, and actually executing the purge
// inside a test suite would null unreal.Object references that other tests are
// holding. compile() parses without executing, which gives the durable
// assertion at no risk.
//
// Per project memory: UNTEST_ASSERT_* macros expand to co_return and cannot
// live inside lambda bodies, and anonymous-namespace helpers need a file-local
// discriminator under unity batching -- hence the `BarrierTests625_` prefix on
// free functions kept outside any lambda.
// ---------------------------------------------------------------------------

namespace BarrierTests625_Helpers
{
	static bool BarrierTests625_PythonReady()
	{
		IPythonScriptPlugin* Py = IPythonScriptPlugin::Get();
		return Py && Py->IsPythonAvailable();
	}

	/**
	 * Build a probe that compile()s the barrier script under the given CPython mode.
	 *
	 * The script is embedded in a raw triple-quoted literal. That is safe without escaping
	 * because the script contains no backslashes (its newlines are real, not C escapes) and
	 * no triple-double-quote sequence. If either ever becomes untrue, these tests will fail
	 * loudly rather than silently probe the wrong text.
	 */
	static FString BarrierTests625_BuildCompileProbe(const FString& Script, const TCHAR* Mode)
	{
		return FString::Printf(
			TEXT("_claireon_barrier_src = r\"\"\"%s\"\"\"\n")
			TEXT("compile(_claireon_barrier_src, '<barrier>', '%s')\n")
			TEXT("del _claireon_barrier_src\n"),
			*Script, Mode);
	}

	/** Run a multi-statement probe under ExecuteFile. Returns whether Python reported success. */
	static bool BarrierTests625_RunProbe(const FString& Code, FString& OutResult)
	{
		FPythonCommandEx Cmd;
		Cmd.Command = Code;
		Cmd.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Cmd.Flags = EPythonCommandFlags::Unattended;
		const bool bOk = IPythonScriptPlugin::Get()->ExecPythonCommandEx(Cmd);
		OutResult = Cmd.CommandResult;
		return bOk;
	}
}

// The script must stay multi-statement and keep its leading import. If someone ever
// collapses it to a single statement, the ExecuteFile requirement stops being load-bearing
// and the mode tests below would silently start passing for the wrong reason.
UNTEST_UNIT_OPTS(Claireon, BridgeWorldTransitionBarrier, ScriptIsMultiStatement, UNTEST_TIMEOUTMS(5000))
{
	const FString Script = FClaireonBridge::GetWorldTransitionPurgeScript();
	UNTEST_ASSERT_FALSE(Script.IsEmpty());
	UNTEST_ASSERT_TRUE(Script.Contains(TEXT("import gc, sys, unreal")));
	UNTEST_ASSERT_TRUE(Script.Contains(TEXT("gc.collect()")));

	// Many real newlines -- this is what Py_single_input cannot accept.
	int32 NewlineCount = 0;
	for (const TCHAR Ch : Script)
	{
		if (Ch == TEXT('\n'))
		{
			++NewlineCount;
		}
	}
	UNTEST_ASSERT_GT(NewlineCount, 20);

	// Preconditions for the raw-literal embedding used by the compile probes.
	UNTEST_ASSERT_FALSE(Script.Contains(TEXT("\\")));
	UNTEST_ASSERT_FALSE(Script.Contains(TEXT("\"\"\"")));
	co_return;
}

// The purge reports how many references it nulled. Somebody previously computed that count
// and then destroyed it with `del _protected, _nulled` without ever reporting it; this pins
// the reporting so it cannot regress back into being computed-and-discarded.
UNTEST_UNIT_OPTS(Claireon, BridgeWorldTransitionBarrier, ScriptReportsNulledCount, UNTEST_TIMEOUTMS(5000))
{
	const FString Script = FClaireonBridge::GetWorldTransitionPurgeScript();
	UNTEST_ASSERT_TRUE(Script.Contains(TEXT("[claireon-barrier] nulled=")));
	// The count must be emitted BEFORE it is deleted.
	const int32 PrintAt = Script.Find(TEXT("[claireon-barrier] nulled="));
	const int32 DelAt = Script.Find(TEXT("del _protected, _nulled"));
	UNTEST_ASSERT_TRUE(PrintAt != INDEX_NONE);
	UNTEST_ASSERT_TRUE(DelAt != INDEX_NONE);
	UNTEST_ASSERT_TRUE(PrintAt < DelAt);
	co_return;
}

// THE regression assertion: the script compiles under the mode the barrier actually uses.
// This is what would have caught the original bug, and it needs no map transition.
UNTEST_UNIT_OPTS(Claireon, BridgeWorldTransitionBarrier, ScriptCompilesUnderFileMode, UNTEST_TIMEOUTMS(15000))
{
	if (!BarrierTests625_Helpers::BarrierTests625_PythonReady())
	{
		// Headless/commandlet runs without Python cannot exercise this; the string-level
		// tests above still hold.
		co_return;
	}

	const FString Script = FClaireonBridge::GetWorldTransitionPurgeScript();
	FString Result;
	const bool bCompiled = BarrierTests625_Helpers::BarrierTests625_RunProbe(
		BarrierTests625_Helpers::BarrierTests625_BuildCompileProbe(Script, TEXT("exec")), Result);

	if (!bCompiled)
	{
		// Surface the Python trace before failing -- the assert alone would not say why.
		UE_LOG(LogClaireon, Error,
			TEXT("Barrier script failed to compile under 'exec' (ExecuteFile). This is the "
				 "RunWorldTransitionBarrier regression. Python said: %s"),
			Result.IsEmpty() ? TEXT("<no result>") : *Result);
	}
	UNTEST_ASSERT_TRUE(bCompiled);
	co_return;
}

// Documents WHY ExecuteFile is required rather than merely asserting it: the identical
// script is rejected under 'single' (Py_single_input), the mode the barrier used to pass.
// If this ever starts passing, Py_single_input has changed semantics and the comments in
// RunWorldTransitionBarrier need revisiting.
UNTEST_UNIT_OPTS(Claireon, BridgeWorldTransitionBarrier, ScriptRejectedUnderSingleMode, UNTEST_TIMEOUTMS(15000))
{
	if (!BarrierTests625_Helpers::BarrierTests625_PythonReady())
	{
		co_return;
	}

	const FString Script = FClaireonBridge::GetWorldTransitionPurgeScript();
	FString Result;
	const bool bCompiled = BarrierTests625_Helpers::BarrierTests625_RunProbe(
		BarrierTests625_Helpers::BarrierTests625_BuildCompileProbe(Script, TEXT("single")), Result);

	if (bCompiled)
	{
		UE_LOG(LogClaireon, Error,
			TEXT("Barrier script unexpectedly compiled under 'single' (Py_single_input). The "
				 "ExecuteFile requirement documented in RunWorldTransitionBarrier no longer holds."));
	}
	UNTEST_ASSERT_FALSE(bCompiled);
	co_return;
}

#endif // WITH_UNTESTED
