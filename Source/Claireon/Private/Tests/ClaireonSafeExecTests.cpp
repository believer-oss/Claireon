// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonSafeExec.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"

// ---------------------------------------------------------------------------
// Test tool: deliberately triggers an access violation (null deref)
// ---------------------------------------------------------------------------
class FTestCrashingTool : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("test"); }
	virtual FString GetOperation() const override { return TEXT("crash"); }
	virtual FString GetDescription() const override
	{
		return TEXT("Deliberately crashes via null pointer dereference for SEH testing.");
	}
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		return Schema;
	}
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
	{
		// Trigger access violation: write to null pointer
		volatile int* NullPtr = nullptr;
		*NullPtr = 42;
		// Unreachable
		return MakeErrorResult(TEXT("Should never reach here"));
	}
};

// ---------------------------------------------------------------------------
// Test tool: executes cleanly
// ---------------------------------------------------------------------------
class FTestCleanTool : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("test"); }
	virtual FString GetOperation() const override { return TEXT("clean"); }
	virtual FString GetDescription() const override
	{
		return TEXT("Returns success for baseline SEH comparison.");
	}
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		return Schema;
	}
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
	{
		return MakeSuccessResult(nullptr, TEXT("Clean execution"));
	}
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/**
 * Verify that ClaireonSafeExec::ExecuteTool catches an access violation from a
 * tool that dereferences nullptr, returns a well-formed error result with the
 * exception code, and sets the crash flag.
 */
UNTEST_UNIT_OPTS(Claireon, SafeExec, ExecuteToolCatchesAccessViolation, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSafeExec::ClearCrashFlag();

	FTestCrashingTool CrashTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

	FClaireonSafeExecResult Result = ClaireonSafeExec::ExecuteTool(&CrashTool, Args);

	// Must have caught a fatal exception
	UNTEST_ASSERT_TRUE(Result.bCaughtFatalException);

	// Tool result must be an error
	UNTEST_ASSERT_TRUE(Result.ToolResult.bIsError);

	// Error message must mention FATAL and SEH
	UNTEST_ASSERT_TRUE(Result.ToolResult.ErrorMessage.Contains(TEXT("FATAL")));

#if PLATFORM_WINDOWS
	// On Windows, exception code 0xC0000005 = STATUS_ACCESS_VIOLATION
	UNTEST_ASSERT_TRUE(Result.ExceptionCode == 0xC0000005);
#endif

	// Crash flag must be set
	UNTEST_ASSERT_TRUE(ClaireonSafeExec::DidLastExecutionCrash());

	UE_LOG(LogTemp, Log, TEXT("[SafeExec Test] Access violation caught. Code=0x%08X Desc=%s"),
		Result.ExceptionCode, *Result.ExceptionDescription.Left(200));

	co_return;
}

/**
 * Verify that ClaireonSafeExec::ExecuteAction catches an access violation from
 * a lambda that dereferences nullptr.
 */
UNTEST_UNIT_OPTS(Claireon, SafeExec, ExecuteActionCatchesAccessViolation, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSafeExec::ClearCrashFlag();

	FClaireonSafeActionResult Result = ClaireonSafeExec::ExecuteAction([]()
	{
		volatile int* NullPtr = nullptr;
		*NullPtr = 42;
	});

	UNTEST_ASSERT_FALSE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Result.bCaughtFatalException);
	UNTEST_ASSERT_TRUE(Result.ExceptionDescription.Len() > 0);
	UNTEST_ASSERT_TRUE(ClaireonSafeExec::DidLastExecutionCrash());

	UE_LOG(LogTemp, Log, TEXT("[SafeExec Test] Action access violation caught: %s"),
		*Result.ExceptionDescription.Left(200));

	co_return;
}

/**
 * Verify that a clean tool execution through ClaireonSafeExec succeeds normally,
 * clears the crash flag, and returns the expected result.
 */
UNTEST_UNIT_OPTS(Claireon, SafeExec, CleanExecutionSucceeds, UNTEST_TIMEOUTMS(5000))
{
	// Set crash flag first to verify it gets cleared
	ClaireonSafeExec::SetCrashFlag();
	UNTEST_ASSERT_TRUE(ClaireonSafeExec::DidLastExecutionCrash());

	FTestCleanTool CleanTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

	FClaireonSafeExecResult Result = ClaireonSafeExec::ExecuteTool(&CleanTool, Args);

	// Must NOT have caught an exception
	UNTEST_ASSERT_FALSE(Result.bCaughtFatalException);
	UNTEST_ASSERT_TRUE(Result.ExceptionCode == 0);

	// Tool result must be success
	UNTEST_ASSERT_FALSE(Result.ToolResult.bIsError);
	UNTEST_ASSERT_STREQ(Result.ToolResult.GetContentAsString(), TEXT("Clean execution"));

	// Crash flag must be cleared after successful tool execution
	UNTEST_ASSERT_FALSE(ClaireonSafeExec::DidLastExecutionCrash());

	co_return;
}

/**
 * Verify recovery: crash then clean execution. The crash flag must be set
 * after the crash and cleared after the clean execution. This proves the
 * editor can continue operating after catching a fatal exception.
 */
UNTEST_UNIT_OPTS(Claireon, SafeExec, RecoveryAfterCrash, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSafeExec::ClearCrashFlag();

	// Phase 1: Crash
	FTestCrashingTool CrashTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

	FClaireonSafeExecResult CrashResult = ClaireonSafeExec::ExecuteTool(&CrashTool, Args);
	UNTEST_ASSERT_TRUE(CrashResult.bCaughtFatalException);
	UNTEST_ASSERT_TRUE(ClaireonSafeExec::DidLastExecutionCrash());

	// Phase 2: Clean execution in the same process -- proves recovery
	FTestCleanTool CleanTool;

	FClaireonSafeExecResult CleanResult = ClaireonSafeExec::ExecuteTool(&CleanTool, Args);
	UNTEST_ASSERT_FALSE(CleanResult.bCaughtFatalException);
	UNTEST_ASSERT_FALSE(CleanResult.ToolResult.bIsError);
	UNTEST_ASSERT_STREQ(CleanResult.ToolResult.GetContentAsString(), TEXT("Clean execution"));

	// Crash flag must be cleared -- editor has recovered
	UNTEST_ASSERT_FALSE(ClaireonSafeExec::DidLastExecutionCrash());

	UE_LOG(LogTemp, Log, TEXT("[SafeExec Test] Recovery verified: crash then clean execution succeeded."));

	co_return;
}

#endif // WITH_UNTESTED
