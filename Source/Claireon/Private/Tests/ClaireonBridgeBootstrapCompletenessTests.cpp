// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Regression test: every registered tool must survive the Python bootstrap and
// surface in sys.modules['<namespace>'].__all__. If a tool's GetName() returns
// something that is not a valid Python identifier, the bootstrap's
// `def {short_name}` codegen would raise SyntaxError and the entry would be
// silently dropped from __all__/__tools__ (or, with strict-validation,
// rejected upstream and never reach the catalog).
//
// This test asserts, for every namespace observed in the registry:
//   1. len(<ns>.__tools__) == len(<ns>.__all__) (per-entry parity).
//   2. The Python-visible per-namespace tool count matches the C++ count of
//      registered tools in that namespace (minus the python_execute recursion
//      sink, which the bootstrap intentionally skips for the claireon ns).
// Assertion (2) catches the bare-prefix rename hazard: tools returning
// "claireon.<x>" from GetName() would either be dropped (SyntaxError) or be
// rejected by strict-validation, making the Python-visible count lag the
// registered count.

#if WITH_UNTESTED

#include "Untest.h"
#include "IPythonScriptPlugin.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "SquidTasks/Task.h"

namespace ClaireonBridgeBootstrapCompletenessTestsNS
{
	// File-local discriminator to avoid anon-NS collisions under unity batching.

	static FString RunPython(const FString& Code, bool& bOutError, FString& OutErrMsg)
	{
		ClaireonTool_ExecutePython PyTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("code"), Code);
		Args->SetNumberField(TEXT("timeout_ms"), 10000);
		IClaireonTool::FToolResult Result = PyTool.Execute(Args);
		bOutError = Result.bIsError;
		OutErrMsg = Result.ErrorMessage;
		return Result.Logs;
	}

	static int32 FindIntAfter(const FString& Blob, const FString& Tag)
	{
		const int32 Idx = Blob.Find(Tag);
		if (Idx == INDEX_NONE) { return INDEX_NONE; }
		const int32 Start = Idx + Tag.Len();
		int32 End = Start;
		while (End < Blob.Len() && (FChar::IsDigit(Blob[End]) || Blob[End] == TEXT('-')))
		{
			++End;
		}
		if (End == Start) { return INDEX_NONE; }
		const FString Num = Blob.Mid(Start, End - Start);
		return FCString::Atoi(*Num);
	}
}

// Root cause of the historical failure (test defect, no product bug): this body
// used StartServer()/GetServer(), but in commandlet mode StartupModule()
// short-circuits on the GIsEditor/IsRunningCommandlet guard, so Server is never
// constructed. StartServer() logged "called before Server was constructed" and
// returned, GetServer() stayed null, and UNTEST_ASSERT_PTR fired BEFORE the
// zero-registry skip could run. EnsureServerForTest() is the established seam
// (ClaireonPythonBridgeBootstrapTests.cpp) that constructs and populates the
// registry headlessly, so this test now does real work in commandlet runs.
UNTEST_UNIT_OPTS(Claireon, BridgeBootstrapCompleteness, ClaireonModuleAllMatchesRegisteredCount, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonBridgeBootstrapCompletenessTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	// EnsureServerForTest() registers the builtin provider unconditionally and
	// populates the registry process-wide, so an empty registry here is a broken
	// seam, not an environment we have to tolerate. The previous body logged a
	// loud "SKIPPED" and co_returned -- and Untest has no skip primitive, so
	// that scored as a PASS. This file is the one every auditor cites as the
	// correct template, so it has to hard-fail: fail, do not skip.
	if (Tools.Num() == 0)
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[BootstrapCompleteness] tool registry is EMPTY after EnsureServerForTest(); "
			     "the registry seam regressed and the parity check did not run"));
	}
	UNTEST_ASSERT_GT(Tools.Num(), 0);

	// RebuildClaireonModule() and python_execute both need a live interpreter.
	// This is the one precondition in this test that is genuinely outside its
	// control: PythonScriptPlugin can be disabled by project configuration, and
	// there is nothing the test can do about that. It therefore stays a skip --
	// at Warning, so the log is the only way to distinguish "the parity check
	// ran" from "it silently did not", and never at Display.
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[BootstrapCompleteness] SKIPPED -- Python is not available in this "
			     "configuration; parity check did NOT run"));
		co_return;
	}

	// Bucket registered tools by namespace, skipping the python_execute
	// recursion sink. Also count any name that the bootstrap would reject
	// (empty, contains '.', or doesn't start with its category prefix) --
	// those would not appear in sys.modules and would skew the parity check.
	TMap<FString, int32> ExpectedPerNamespace;
	int32 RejectableCount = 0;
	for (const auto& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		if (!Tool.IsValid()) { continue; }
		const FString Namespace = TEXT("claireon");
		const FString Name = Tool->GetName();
		// claireon.python_execute is the recursion sink; the bootstrap drops it
		// before adding to the module, so don't include it in the expected count.
		if (Name == TEXT("python_execute")) { continue; }
		if (Name.IsEmpty() || Name.Contains(TEXT(".")))
		{
			++RejectableCount;
			UE_LOG(LogClaireon, Error,
				TEXT("[BootstrapCompleteness] tool '%s' has invalid bare-identifier name '%s' (legacy prefix or empty)"),
				*Pair.Key, *Name);
			continue;
		}
		// The bootstrap also drops any tool whose GetInputSchema() is null (it
		// cannot generate a signature without one). Mirror that skip here or the
		// coverage-parity assert below would fail on a bootstrap that behaved
		// exactly as designed. Log it: a null schema is still a tool defect worth
		// seeing, just not the one this test guards.
		if (!Tool->GetInputSchema().IsValid())
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[BootstrapCompleteness] tool '%s' returns a null GetInputSchema(); the bootstrap skips it, so it is excluded from the parity count"),
				*Pair.Key);
			continue;
		}
		ExpectedPerNamespace.FindOrAdd(Namespace, 0)++;
	}

	// No tool should ever have a name that the bootstrap would reject -- if
	// any do, the bare-name rename is incomplete.
	UNTEST_EXPECT_EQ(RejectableCount, 0);

	// Force a rebuild to make the test independent of any earlier seeding.
	FClaireonBridge::EnsureRegistered();
	FClaireonBridge::RebuildClaireonModule();

	// Walk every namespace we registered tools under and assert per-NS parity.
	for (const TPair<FString, int32>& NsCount : ExpectedPerNamespace)
	{
		const FString& Ns = NsCount.Key;
		const int32 ExpectedCount = NsCount.Value;

		const FString Code = FString::Printf(TEXT(
			"import sys\n"
			"_ns = '%s'\n"
			"if _ns in sys.modules:\n"
			"    _m = sys.modules[_ns]\n"
			"    print('PARITY_TOOLS=' + str(len(getattr(_m, '__tools__', []))))\n"
			"    print('PARITY_ALL=' + str(len(getattr(_m, '__all__', []))))\n"
			"else:\n"
			"    print('PARITY_TOOLS=-1')\n"
			"    print('PARITY_ALL=-1')\n"),
			*Ns);
		bool bErr = false;
		FString ErrMsg;
		const FString Out = RunPython(Code, bErr, ErrMsg);
		UNTEST_ASSERT_FALSE(bErr);

		const int32 ToolsLen = FindIntAfter(Out, TEXT("PARITY_TOOLS="));
		const int32 AllLen = FindIntAfter(Out, TEXT("PARITY_ALL="));
		UNTEST_ASSERT_TRUE(ToolsLen != INDEX_NONE);
		UNTEST_ASSERT_TRUE(AllLen != INDEX_NONE);

		if (ToolsLen < 0 || AllLen < 0)
		{
			UE_LOG(LogClaireon, Error,
				TEXT("[BootstrapCompleteness] sys.modules['%s'] missing -- bootstrap did not seed this namespace despite %d registered tool(s)."),
				*Ns, ExpectedCount);
			UNTEST_EXPECT_TRUE(false);
			continue;
		}

		// (1) Per-entry parity: __all__ and __tools__ are appended together.
		UNTEST_EXPECT_EQ(ToolsLen, AllLen);
		// (2) Coverage parity: all expected tools made it through.
		UNTEST_EXPECT_EQ(AllLen, ExpectedCount);
	}

	co_return;
}

#endif // WITH_UNTESTED
