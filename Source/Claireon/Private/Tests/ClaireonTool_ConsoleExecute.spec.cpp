// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for console_execute (P1-6).
//
// The defect these pin: a console command's output can arrive on either of two
// channels -- the FOutputDevice the handler is given, or the log -- and the tool
// used to report only the first. Commands that report solely through the log
// (`stat dumpframe`, `obj list`) came back with output: ''.
//
// The tests drive a console command registered here whose ONLY output is a
// UE_LOG line, so the two channels are unambiguously separated. Engine commands
// were rejected as fixtures because they straddle both channels: `LOG LIST`,
// which the catalog suggested, actually writes to Ar (LogSuppressionInterface.cpp
// Exec_Runtime), so it would have passed against the unfixed code.
//
// PIE dispatch (APlayerController::ConsoleCommand) is verified manually.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_ConsoleExecute.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions across other Tests TUs.
namespace ClaireonToolConsoleExecuteSpec
{
static const TCHAR* const kCommandName = TEXT("Claireon.Test.EmitLogOnly");
static const TCHAR* const kLogMarker = TEXT("ClaireonConsoleExecSpec_LogOnlyMarker");

static void EmitLogOnly()
{
	// Log verbosity on purpose: this is the level real console commands
	// report at, and the level FClaireonLogCapture's default floor drops.
	UE_LOG(LogClaireon, Log, TEXT("%s"), kLogMarker);
}

// RAII registration so a failing assertion cannot leave the command behind
// for the next test in the process.
struct FScopedTestCommand
{
	IConsoleCommand* Command = nullptr;

	FScopedTestCommand()
	{
		Command = IConsoleManager::Get().RegisterConsoleCommand(
			kCommandName,
			TEXT("Claireon spec fixture: emits one Log-verbosity line and nothing to Ar."),
			FConsoleCommandDelegate::CreateStatic(&EmitLogOnly),
			ECVF_Default);
	}

	~FScopedTestCommand()
	{
		if (Command != nullptr)
		{
			IConsoleManager::Get().UnregisterConsoleObject(Command);
		}
	}
};
} // namespace ClaireonToolConsoleExecuteSpec

// ---------------------------------------------------------------------------
// Tool surface: the new channel must be declared, not just returned.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ConsoleExecute, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_ConsoleExecute Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("console_execute"));

	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());

	const TSharedPtr<FJsonObject>* Props = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props) && Props != nullptr);
	UNTEST_ASSERT_TRUE((*Props)->HasField(TEXT("capture_log")));

	// The description has to tell a caller which field to read, or the second
	// channel is only discoverable by accident.
	UNTEST_EXPECT_TRUE(Tool.GetDescription().Contains(TEXT("log_output")));
	co_return;
}

// ---------------------------------------------------------------------------
// The regression itself: log-only output reaches the caller.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, ConsoleExecute, CapturesLogOnlyOutput)
{
	using namespace ClaireonToolConsoleExecuteSpec;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	FScopedTestCommand Registered;
	UNTEST_ASSERT_PTR(Registered.Command);

	const ClaireonTool_ConsoleExecute::FConsoleDispatchOutput Out =
		ClaireonTool_ConsoleExecute::DispatchInWorld(World, kCommandName, /*bCaptureLog=*/true);

	UNTEST_ASSERT_TRUE(Out.LogOutput.Contains(kLogMarker));
	// The return channel stays the return channel: the marker never went to Ar,
	// so finding it there would mean the two channels had been merged.
	UNTEST_EXPECT_FALSE(Out.ReturnChannel.Contains(kLogMarker));
	co_return;
}

// ---------------------------------------------------------------------------
// Opting out reproduces the pre-fix behavior exactly.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, ConsoleExecute, CaptureLogFalseReturnsNoLogOutput)
{
	using namespace ClaireonToolConsoleExecuteSpec;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	FScopedTestCommand Registered;
	UNTEST_ASSERT_PTR(Registered.Command);

	const ClaireonTool_ConsoleExecute::FConsoleDispatchOutput Out =
		ClaireonTool_ConsoleExecute::DispatchInWorld(World, kCommandName, /*bCaptureLog=*/false);

	UNTEST_ASSERT_TRUE(Out.LogOutput.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// A command that writes to Ar still comes back on `output`, so the fix did not
// move the existing channel.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, ConsoleExecute, ReturnChannelStillPopulated)
{
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	// `LOG LIST` writes one Ar line per registered log category and needs no PIE.
	const ClaireonTool_ConsoleExecute::FConsoleDispatchOutput Out =
		ClaireonTool_ConsoleExecute::DispatchInWorld(World, TEXT("LOG LIST"), /*bCaptureLog=*/true);

	UNTEST_ASSERT_FALSE(Out.ReturnChannel.IsEmpty());
	UNTEST_EXPECT_TRUE(Out.ReturnChannel.Contains(TEXT("LogClaireon")));
	co_return;
}

#endif // WITH_UNTESTED
