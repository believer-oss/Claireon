// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test MCP content reload and last-good-text fallback.
// Remove probe files from the loader directory on every exit.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonModule.h"
#include "ClaireonServer.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace ClaireonMCPContentReloadTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	static const TCHAR* ReloadProbe_FileName = TEXT("__untest_reload_probe.md");
	static const TCHAR* ReloadProbe_Uri = TEXT("claireon://instructions/__untest_reload_probe");

	static FString ReloadProbe_InstructionsDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content"), TEXT("MCP"), TEXT("Instructions"));
	}

	static FString ReloadProbe_Path()
	{
		const FString Dir = ReloadProbe_InstructionsDir();
		return Dir.IsEmpty() ? FString() : FPaths::Combine(Dir, ReloadProbe_FileName);
	}

	/** A resource instruction doc whose body is Body. */
	static FString ReloadProbe_ValidDoc(const FString& Body)
	{
		return FString::Printf(
			TEXT("---\n")
			TEXT("name: untest-reload-probe\n")
			TEXT("description: Transient probe written by ClaireonMCPContentReloadTests.\n")
			TEXT("type: resource\n")
			TEXT("uri: %s\n")
			TEXT("---\n")
			TEXT("\n%s\n"),
			ReloadProbe_Uri, *Body);
	}

	/** Match probe filenames by suffix to avoid separator and case dependencies. */
	static bool ReloadProbe_FailedListNamesProbe(const TArray<FString>& FailedFiles)
	{
		for (const FString& Failed : FailedFiles)
		{
			if (Failed.EndsWith(ReloadProbe_FileName))
			{
				return true;
			}
		}
		return false;
	}

	/** Deletes the probe file on every exit path, including an assertion's early return. */
	struct FReloadProbeScope
	{
		FReloadProbeScope() { Remove(); }
		~FReloadProbeScope() { Remove(); }

		static void Remove()
		{
			const FString Path = ReloadProbe_Path();
			if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
			{
				IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
			}
		}

		static bool Write(const FString& Contents)
		{
			const FString Path = ReloadProbe_Path();
			return !Path.IsEmpty() && FFileHelper::SaveStringToFile(Contents, *Path);
		}
	};
}

// Verify the installed guidance resource is served.
UNTEST_UNIT_OPTS(Claireon, MCPContent, MCPContent_InstalledGuidanceResourceIsServed, UNTEST_TIMEOUTMS(15000))
{
	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);

	Server->ReloadMCPContent();

	FString Text;
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(
		TEXT("claireon://instructions/blueprint-authoring"), Text));
	UNTEST_EXPECT_TRUE(Text.Len() > 1000);

	UNTEST_EXPECT_FALSE(Text.StartsWith(TEXT("---")));

	// Require the latent-action exclusion in the served guidance.
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("latent")));
	co_return;
}

// New documents become available after reload.
UNTEST_UNIT_OPTS(Claireon, MCPContent, MCPContent_ReloadPicksUpNewAndEditedDocs, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonMCPContentReloadTestsNS;
	FReloadProbeScope Probe;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	UNTEST_ASSERT_FALSE(ReloadProbe_Path().IsEmpty());

	Server->ReloadMCPContent();
	FString Text;
	UNTEST_EXPECT_FALSE(Server->TryGetResourceText(ReloadProbe_Uri, Text));

	UNTEST_ASSERT_TRUE(FReloadProbeScope::Write(ReloadProbe_ValidDoc(TEXT("BODY ONE"))));
	FClaireonServer::FMCPContentLoadReport Report = Server->ReloadMCPContent();
	UNTEST_EXPECT_TRUE(Report.FatalError.IsEmpty());
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("BODY ONE")));

	// Edits replace served content.
	UNTEST_ASSERT_TRUE(FReloadProbeScope::Write(ReloadProbe_ValidDoc(TEXT("BODY TWO"))));
	Server->ReloadMCPContent();
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("BODY TWO")));
	UNTEST_EXPECT_FALSE(Text.Contains(TEXT("BODY ONE")));

	// Deleted files must disappear instead of using last-good fallback.
	FReloadProbeScope::Remove();
	Server->ReloadMCPContent();
	UNTEST_EXPECT_FALSE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	co_return;
}

// Malformed edits retain the last successfully parsed document.
UNTEST_UNIT_OPTS(Claireon, MCPContent, MCPContent_MalformedDocKeepsPreviousContent, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonMCPContentReloadTestsNS;
	FReloadProbeScope Probe;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);
	UNTEST_ASSERT_FALSE(ReloadProbe_Path().IsEmpty());

	UNTEST_ASSERT_TRUE(FReloadProbeScope::Write(ReloadProbe_ValidDoc(TEXT("GOOD BODY"))));
	Server->ReloadMCPContent();
	FString Text;
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	UNTEST_ASSERT_TRUE(Text.Contains(TEXT("GOOD BODY")));

	// Remove the closing frontmatter delimiter to make identity unavailable.
	UNTEST_ASSERT_TRUE(FReloadProbeScope::Write(TEXT("this file has no frontmatter at all\n")));
	const FClaireonServer::FMCPContentLoadReport Report = Server->ReloadMCPContent();

	UNTEST_EXPECT_TRUE(Report.FatalError.IsEmpty());
	UNTEST_EXPECT_TRUE(ReloadProbe_FailedListNamesProbe(Report.FailedFiles));
	UNTEST_EXPECT_TRUE(Report.CarriedForwardKeys.Contains(ReloadProbe_Uri));

	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("GOOD BODY")));

	// Repaired files replace their carried-forward copy.
	UNTEST_ASSERT_TRUE(FReloadProbeScope::Write(ReloadProbe_ValidDoc(TEXT("REPAIRED BODY"))));
	const FClaireonServer::FMCPContentLoadReport Recovered = Server->ReloadMCPContent();
	UNTEST_EXPECT_FALSE(ReloadProbe_FailedListNamesProbe(Recovered.FailedFiles));
	UNTEST_EXPECT_TRUE(Recovered.CarriedForwardKeys.Num() == 0);
	UNTEST_ASSERT_TRUE(Server->TryGetResourceText(ReloadProbe_Uri, Text));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("REPAIRED BODY")));
	co_return;
}

// A no-change reload preserves all served documents.
UNTEST_UNIT_OPTS(Claireon, MCPContent, MCPContent_ReloadIsIdempotentOnUnchangedTree, UNTEST_TIMEOUTMS(15000))
{
	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_TRUE(Server != nullptr);

	const FClaireonServer::FMCPContentLoadReport First = Server->ReloadMCPContent();
	const FClaireonServer::FMCPContentLoadReport Second = Server->ReloadMCPContent();

	UNTEST_EXPECT_TRUE(First.FatalError.IsEmpty());
	UNTEST_EXPECT_TRUE(Second.FatalError.IsEmpty());
	UNTEST_EXPECT_TRUE(First.NumPrompts > 0);
	UNTEST_EXPECT_TRUE(First.NumResources > 0);
	UNTEST_EXPECT_EQ(Second.NumPrompts, First.NumPrompts);
	UNTEST_EXPECT_EQ(Second.NumResources, First.NumResources);

	// Check carry-forward counts so stale content cannot pass the no-change test.
	UNTEST_EXPECT_TRUE(Second.CarriedForwardKeys.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
