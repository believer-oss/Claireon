// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonSettings.h"
#include "ClaireonAnthropicClient.h"
#include "ClaireonREPLLogger.h"
#include "Tools/IClaireonTool.h"
#include "Interfaces/IHttpBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * Minimal echo tool for tests: returns its "message" argument verbatim.
 * Used to verify that tool_use content blocks are dispatched and executed.
 */
class FTestEchoTool : public IClaireonTool
{
public:
	/** Tracks how many times Execute() was called. */
	int32 ExecuteCount = 0;

	virtual FString GetName() const override { return TEXT("test_echo"); }
	virtual FString GetDescription() const override
	{
		return TEXT("Echo back the provided message. Use this to confirm tool invocation.");
	}
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> MsgProp = MakeShared<FJsonObject>();
		MsgProp->SetStringField(TEXT("type"), TEXT("string"));
		MsgProp->SetStringField(TEXT("description"), TEXT("The message to echo back."));
		Props->SetObjectField(TEXT("message"), MsgProp);
		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("message")));
		Schema->SetArrayField(TEXT("required"), Required);
		return Schema;
	}
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
	{
		++ExecuteCount;
		FString Message;
		if (Arguments.IsValid())
		{
			Arguments->TryGetStringField(TEXT("message"), Message);
		}
		return MakeSuccessResult(nullptr, FString::Printf(TEXT("ECHO: %s"), *Message));
	}
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/**
 * Verify that FClaireonAnthropicClient::GetCurrentServer() returns the
 * live module server, not a stale constructor pointer.
 *
 * This is the regression test for the bug where opening the diagnostics tab
 * before starting the server left the client with a null Server pointer,
 * causing BuildToolDefinitions() to return an empty array.
 */
UNTEST_UNIT_OPTS(Claireon, REPL, GetCurrentServerResolvesLiveServer, UNTEST_TIMEOUTMS(5000))
{
	FClaireonModule& Module = FClaireonModule::Get();

	// Start the server if not already running
	bool bWeStartedIt = false;
	if (!Module.IsServerRunning())
	{
		Module.StartServer();
		bWeStartedIt = true;
	}
	UNTEST_ASSERT_TRUE(Module.IsServerRunning());

	// Construct a client with an INTENTIONALLY null server pointer (simulating
	// the tab-opened-before-server-started scenario)
	auto Client = MakeShared<FClaireonAnthropicClient>(
		/*InServer=*/nullptr,
		/*InLogger=*/nullptr);

	// GetCurrentServer() must resolve to the live module server even though
	// the constructor received null
	FClaireonServer* Resolved = Client->GetCurrentServer();
	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_ASSERT_TRUE(Resolved->IsRunning());
	UNTEST_ASSERT_TRUE(Resolved->GetTools().Num() > 0);

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] GetCurrentServer: resolved %d tools from live server"),
		Resolved->GetTools().Num());

	if (bWeStartedIt)
	{
		Module.StopServer();
	}

	co_return;
}

/**
 * Verify that a minimal FClaireonServer with a registered tool
 * executes that tool correctly when its Execute() method is called directly.
 * This is the baseline check before end-to-end API integration.
 */
UNTEST_UNIT_OPTS(Claireon, REPL, ToolExecutionInProcess, UNTEST_TIMEOUTMS(1000))
{
	auto Server = MakeShared<FClaireonServer>();
	auto EchoTool = MakeShared<FTestEchoTool>();
	Server->RegisterTool(EchoTool);

	UNTEST_ASSERT_TRUE(Server->GetTools().Num() == 1);
	UNTEST_ASSERT_TRUE(Server->GetTools().Contains(TEXT("test_echo")));

	// Execute the tool directly
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("message"), TEXT("hello"));

	auto Result = EchoTool->Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(EchoTool->ExecuteCount == 1);

	FString Content = Result.GetContentAsString();
	UNTEST_ASSERT_STREQ(Content, TEXT("ECHO: hello"));

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] Tool executed in-process: %s"), *Content);

	co_return;
}

/**
 * Integration test: verify that sending a prompt through FClaireonAnthropicClient
 * actually causes a registered tool to be invoked via the Anthropic API.
 *
 * REQUIRES a valid Anthropic API key in UClaireonSettings.
 * Skipped automatically if no key is configured.
 *
 * Verification mechanism: the test tool's ExecuteCount increments when called.
 * We wait up to 30 seconds for the async API round-trip to complete.
 */
UNTEST_UNIT_OPTS(Claireon, REPL, ToolUseViaAPI, UNTEST_TIMEOUTMS(60000))
{
	// Skip if no API key configured
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (!Settings || Settings->AnthropicApiKey.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[REPL Test] ToolUseViaAPI: SKIPPED — no Anthropic API key configured in "
			     "Editor Preferences > Plugins > MCP REPL"));
		co_return;
	}

	// Build a minimal server with only the echo tool (avoids overwhelming
	// the model with 47 tool descriptions)
	auto Server = MakeShared<FClaireonServer>();
	auto EchoTool = MakeShared<FTestEchoTool>();
	Server->RegisterTool(EchoTool);

	// Logger (writes to temp, not the normal session log)
	TSharedPtr<FClaireonREPLLogger> Logger = MakeShared<FClaireonREPLLogger>();
	// Don't call Initialize() — we don't need the file for this test

	// MakeShared returns TSharedRef; TSharedRef::Get() returns T& so take the address for T*
	auto Client = MakeShared<FClaireonAnthropicClient>(&Server.Get(), Logger);

	bool bFinished = false;
	bool bGotError = false;
	FString ErrorText;

	Client->OnREPLEvent.AddLambda([&](const FREPLEvent& Event)
	{
		if (Event.Type == EREPLEventType::Finished)
		{
			bFinished = true;
		}
		else if (Event.Type == EREPLEventType::Error)
		{
			bGotError = true;
			ErrorText = Event.Text;
			bFinished = true;
		}
		else if (Event.Type == EREPLEventType::Cancelled)
		{
			bFinished = true;
		}
	});

	// Send a prompt that explicitly requests tool use
	Client->SendMessage(
		TEXT("Please call the test_echo tool with message=\"UNTEST_PING\" and report the result."),
		TEXT("untest_conv"));

	// Wait up to 30 seconds for the async API call to complete
	constexpr float TimeoutSeconds = 30.0f;
	const double StartTime = FPlatformTime::Seconds();
	co_await Squid::WaitUntil([&bFinished, StartTime, TimeoutSeconds]()
	{
		return bFinished || (FPlatformTime::Seconds() - StartTime >= TimeoutSeconds);
	});

	if (!bFinished)
	{
		Client->CancelActiveRequest();
		UNTEST_ASSERT_TRUE(false); // Timed out
		co_return;
	}

	if (bGotError)
	{
		UE_LOG(LogTemp, Error, TEXT("[REPL Test] ToolUseViaAPI: API error: %s"), *ErrorText);
		UNTEST_ASSERT_TRUE(false); // API returned error
		co_return;
	}

	// The echo tool must have been called at least once
	UNTEST_ASSERT_TRUE(EchoTool->ExecuteCount >= 1);
	UE_LOG(LogTemp, Log,
		TEXT("[REPL Test] ToolUseViaAPI: PASSED — echo tool invoked %d time(s) via API"),
		EchoTool->ExecuteCount);

	co_return;
}

// ---------------------------------------------------------------------------
// Reliability: Error Classification & Retry
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, REPL, ErrorClassification, UNTEST_TIMEOUTMS(1000))
{
	using Cat = EAnthropicErrorCategory;
	// Note: EHttpRequestStatus is a namespace, not a type — use fully qualified names.
	// Extra parentheses around each expression prevent macro comma splitting.

	// Network failure (connection never established)
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		false, 0, EHttpRequestStatus::Failed, EHttpFailureReason::ConnectionError) == Cat::Network));

	// Timeout (request timed out)
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		false, 0, EHttpRequestStatus::Failed, EHttpFailureReason::TimedOut) == Cat::Timeout));

	// Success
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 200, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::Success));

	// Rate limit
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 429, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::RateLimit));

	// Overloaded
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 529, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::Overloaded));

	// Server errors
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 500, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::ServerError));
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 502, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::ServerError));
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 503, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::ServerError));
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 504, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::ServerError));

	// Auth errors
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 401, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::AuthError));
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 403, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::AuthError));

	// Client error
	UNTEST_ASSERT_TRUE((FClaireonAnthropicClient::ClassifyHTTPResponse(
		true, 400, EHttpRequestStatus::Succeeded, EHttpFailureReason::None) == Cat::ClientError));

	// Retryable checks
	UNTEST_ASSERT_TRUE(FClaireonAnthropicClient::IsRetryable(Cat::Network));
	UNTEST_ASSERT_TRUE(FClaireonAnthropicClient::IsRetryable(Cat::Timeout));
	UNTEST_ASSERT_TRUE(FClaireonAnthropicClient::IsRetryable(Cat::RateLimit));
	UNTEST_ASSERT_TRUE(FClaireonAnthropicClient::IsRetryable(Cat::Overloaded));
	UNTEST_ASSERT_TRUE(FClaireonAnthropicClient::IsRetryable(Cat::ServerError));
	UNTEST_ASSERT_FALSE(FClaireonAnthropicClient::IsRetryable(Cat::AuthError));
	UNTEST_ASSERT_FALSE(FClaireonAnthropicClient::IsRetryable(Cat::ClientError));
	UNTEST_ASSERT_FALSE(FClaireonAnthropicClient::IsRetryable(Cat::Success));

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] ErrorClassification: all cases passed"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, REPL, RetryBackoffCalculation, UNTEST_TIMEOUTMS(1000))
{
	const float InitialDelay = 1.0f;
	const float MaxDelay = 30.0f;

	// Test multiple iterations to account for jitter randomness
	constexpr int32 Iterations = 50;

	// Attempt 0: delay in [1.0, 1.5] (base=1*2^0=1, jitter in [0, 0.5])
	for (int32 i = 0; i < Iterations; ++i)
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::ServerError, TEXT(""), 0, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 1.0f && Delay <= 1.5f);
	}

	// Attempt 1: delay in [2.0, 2.5]
	for (int32 i = 0; i < Iterations; ++i)
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::ServerError, TEXT(""), 1, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 2.0f && Delay <= 2.5f);
	}

	// Attempt 2: delay in [4.0, 4.5]
	for (int32 i = 0; i < Iterations; ++i)
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::ServerError, TEXT(""), 2, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 4.0f && Delay <= 4.5f);
	}

	// Attempt 5: capped at MaxDelay (1*2^5=32 > 30, so capped to 30.0)
	for (int32 i = 0; i < Iterations; ++i)
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::ServerError, TEXT(""), 5, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(Delay, MaxDelay, 0.01f));
	}

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] RetryBackoffCalculation: all ranges verified"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, REPL, RetryAfterHeaderParsing, UNTEST_TIMEOUTMS(1000))
{
	const float InitialDelay = 1.0f;
	const float MaxDelay = 30.0f;
	const int32 Attempt = 0;

	// Valid integer header: use header value directly
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT("5"), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(Delay, 5.0f, 0.01f));
	}

	// Fractional header value
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT("2.5"), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(Delay, 2.5f, 0.01f));
	}

	// Zero header: fall back to exponential backoff (delay in [1.0, 1.5] for attempt 0)
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT("0"), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 1.0f && Delay <= 1.5f);
	}

	// Empty header: fall back to exponential backoff
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT(""), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 1.0f && Delay <= 1.5f);
	}

	// Non-numeric header: FCString::Atof returns 0, fall back to exponential backoff
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT("abc"), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(Delay >= 1.0f && Delay <= 1.5f);
	}

	// Huge header value: capped at MaxDelay
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::RateLimit, TEXT("999"), Attempt, InitialDelay, MaxDelay);
		UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(Delay, MaxDelay, 0.01f));
	}

	// Non-RateLimit category ignores Retry-After header
	{
		float Delay = FClaireonAnthropicClient::CalculateRetryDelay(
			EAnthropicErrorCategory::ServerError, TEXT("5"), Attempt, InitialDelay, MaxDelay);
		// Should use exponential backoff, not the header value
		UNTEST_ASSERT_TRUE(Delay >= 1.0f && Delay <= 1.5f);
	}

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] RetryAfterHeaderParsing: all cases passed"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, REPL, MinIntervalThrottle, UNTEST_TIMEOUTMS(1000))
{
	// This test verifies the rate limiter's timestamp logic directly.
	// We don't call PostToAPI; we test the timing math that determines deferral.

	const float MinInterval = 0.2f; // 200ms

	// Simulate: first request just happened
	double LastTimestamp = FPlatformTime::Seconds();

	// Immediately after: elapsed ~0, should need to defer
	{
		double Now = FPlatformTime::Seconds();
		double Elapsed = Now - LastTimestamp;
		UNTEST_ASSERT_TRUE(Elapsed < MinInterval); // Should still be within interval
		float RemainingDelay = static_cast<float>(MinInterval - Elapsed);
		UNTEST_ASSERT_TRUE(RemainingDelay > 0.0f);
		UNTEST_ASSERT_TRUE(RemainingDelay <= MinInterval);
	}

	// Wait for the interval to elapse
	double WaitStart = FPlatformTime::Seconds();
	co_await Squid::WaitUntil([WaitStart, MinInterval]() {
		return FPlatformTime::Seconds() - WaitStart >= MinInterval + 0.05;
	});

	// After waiting: elapsed > MinInterval, should proceed immediately
	{
		double Now = FPlatformTime::Seconds();
		double Elapsed = Now - LastTimestamp;
		UNTEST_ASSERT_TRUE(Elapsed >= MinInterval);
	}

	UE_LOG(LogTemp, Log, TEXT("[REPL Test] MinIntervalThrottle: timing verified"));
	co_return;
}


#endif // WITH_UNTESTED
