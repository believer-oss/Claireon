// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Anthropic API tool-name regex compliance test.
// Iterates every tool registered on the live MCP server and asserts each
// GetName() matches the wire regex `^[a-zA-Z0-9_-]{1,128}$`. Bare names must
// round-trip to the Anthropic API without any sanitisation layer.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"
#include "Internationalization/Regex.h"
#include "SquidTasks/Task.h"

UNTEST_UNIT(Claireon, ToolNameApiRegex, AllRegisteredToolsMatchAnthropicRegex)
{
	FClaireonModule& Module = FClaireonModule::Get();
	bool bWeStartedServer = false;
	if (!Module.IsServerRunning())
	{
		Module.StartServer();
		bWeStartedServer = true;
	}
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// In commandlet mode (UnrealEditor-Cmd.exe with -run=UntestRunTests) the
	// Claireon module's StartupModule() returns early via the
	// `!GIsEditor || IsRunningCommandlet()` guard, so no tool providers are
	// registered as modular features and Server->GetTools() is empty even
	// after StartServer(). Skip cleanly so this regression test passes in
	// commandlet runs while still doing real coverage in -FullEditor / CI.
	if (ToolsMap.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ToolNameApiRegex] SKIPPED -- live server has zero tools "
			     "(commandlet mode: ClaireonModule::StartupModule short-circuits "
			     "via IsRunningCommandlet, so BuiltinToolProvider is never registered)"));
		if (bWeStartedServer)
		{
			Module.StopServer();
		}
		co_return;
	}

	const FRegexPattern Pattern(TEXT("^[a-zA-Z0-9_-]{1,128}$"));

	int32 Failures = 0;
	for (const auto& Pair : ToolsMap)
	{
		const FString& RegisteredName = Pair.Key;
		const FString GetNameValue = Pair.Value->GetName();

		// The registry key must match GetName() exactly.
		if (RegisteredName != GetNameValue)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolNameApiRegex] Registry key '%s' does not match GetName() '%s'"),
				*RegisteredName, *GetNameValue);
			++Failures;
		}

		// Match against the wire regex.
		FRegexMatcher KeyMatcher(Pattern, RegisteredName);
		if (!KeyMatcher.FindNext())
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolNameApiRegex] Tool name '%s' fails Anthropic API regex ^[a-zA-Z0-9_-]{1,128}$"),
				*RegisteredName);
			++Failures;
		}

		// Belt-and-braces: explicit dot check matches the per-tool guard in
		// ClaireonDecomposedToolsSmokeTests.cpp post-S07.
		if (RegisteredName.Contains(TEXT(".")))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolNameApiRegex] Tool name '%s' contains '.' character"),
				*RegisteredName);
			++Failures;
		}
		if (RegisteredName.StartsWith(TEXT("claireon.")))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolNameApiRegex] Tool name '%s' carries the legacy 'claireon.' prefix"),
				*RegisteredName);
			++Failures;
		}
	}

	UNTEST_EXPECT_EQ(Failures, 0);

	if (bWeStartedServer)
	{
		Module.StopServer();
	}

	co_return;
}

#endif // WITH_UNTESTED
