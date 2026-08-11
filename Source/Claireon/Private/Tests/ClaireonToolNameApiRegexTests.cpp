// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Anthropic API tool-name regex compliance test.
// Iterates every tool in the live tool registry and asserts each
// GetName() matches the wire regex `^[a-zA-Z0-9_-]{1,128}$`. Bare names must
// round-trip to the Anthropic API without any sanitisation layer.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"
#include "Internationalization/Regex.h"
#include "SquidTasks/Task.h"

// Root causes of the historical failure (both test defects, no product bug):
//   1. Timeout: the default unit budget is 0.5ms. The old body called
//      Module.StartServer(), which binds a listener and registers with the live
//      MCP proxy -- ~70ms of socket work, so the test always overran.
//   2. It only needed the tool REGISTRY, never the HTTP listener. Standing up a
//      real server also perturbed a developer's running proxy session mid-test.
// EnsureServerForTest() is the established seam (see
// ClaireonPythonBridgeBootstrapTests.cpp) that constructs and populates the
// registry headlessly -- no listener, no proxy, and it works in commandlet mode
// where StartupModule() short-circuits on the GIsEditor/IsRunningCommandlet
// guard and GetServer() would otherwise be null.
UNTEST_UNIT_OPTS(Claireon, ToolNameApiRegex, AllRegisteredToolsMatchAnthropicRegex, UNTEST_TIMEOUTMS(30000))
{
	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// EnsureServerForTest() registers the builtin provider UNCONDITIONALLY and
	// populates the registry process-wide, so by this line the registry is
	// populated -- there is no legitimate zero-tool outcome. The old body logged
	// a loud "SKIPPED" and co_returned here, which Untest scores as a PASS: the
	// whole name-regex regression could vanish and the suite would stay green.
	// Fail instead. A zero-tool sweep means the seam regressed, and that is
	// itself worth failing on.
	if (ToolsMap.Num() == 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[ToolNameApiRegex] tool registry is EMPTY after EnsureServerForTest(); "
			     "the registry seam regressed and name-regex coverage did not run"));
	}
	UNTEST_ASSERT_GT(ToolsMap.Num(), 0);

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

	co_return;
}

#endif // WITH_UNTESTED
