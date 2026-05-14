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
	// File-local discriminator (per feedback_anon_namespace_unity_collision.md).

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

UNTEST_UNIT_OPTS(Claireon, BridgeBootstrapCompleteness, ClaireonModuleAllMatchesRegisteredCount, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonBridgeBootstrapCompletenessTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = !Module.IsServerRunning();
	if (bWeStartedServer)
	{
		Module.StartServer();
	}
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	// Skip in commandlet/headless where the registry is empty.
	if (Tools.Num() == 0)
	{
		if (bWeStartedServer) { Module.StopServer(); }
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
		const FString Namespace = Tool->GetNamespace();
		const FString Name = Tool->GetName();
		// claireon.python_execute is the recursion sink; the bootstrap drops it
		// before adding to the module, so don't include it in the expected count.
		if (Namespace == TEXT("claireon") && Name == TEXT("python_execute")) { continue; }
		if (Name.IsEmpty() || Name.Contains(TEXT(".")))
		{
			++RejectableCount;
			UE_LOG(LogClaireon, Error,
				TEXT("[BootstrapCompleteness] tool '%s' has invalid bare-identifier name '%s' (legacy prefix or empty)"),
				*Pair.Key, *Name);
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

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

#endif // WITH_UNTESTED
