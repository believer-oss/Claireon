// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tool-name uniqueness regression test.
// Walks the RAW provider list (every IClaireonTool a provider hands out, before
// the server dedupes them into its name-keyed map) and asserts no two tools
// resolve to the same GetName(). The live server map cannot catch this because
// a collision silently overwrites the earlier entry -- the only runtime signal
// is a "[MCP] Tool name collision" warning. This guards the blend-space
// tools, which live in the "blend_space" category and must not collide with
// anim_inspect / anim_set_property / anim_add_metadata / anim_remove_metadata.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Dom/JsonObject.h"
#include "Features/IModularFeatures.h"
#include "SquidTasks/Task.h"

namespace ClaireonToolNameUniquenessHelpers
{
	/**
	 * Collect every IClaireonTool from every registered provider (raw, not deduped).
	 *
	 * EnsureServerForTest() first: FClaireonModule::StartupModule() early-returns
	 * under IsRunningCommandlet() and Invoke-UntestTests.ps1 always runs
	 * -run=UntestRunTests, so the IClaireonToolProvider modular feature is never
	 * registered by normal startup. Without the seam this collection returned an
	 * empty array unless an unrelated earlier test happened to construct the
	 * registry -- order-dependent, and vacuous on its own. The seam registers
	 * FClaireonBuiltinToolProvider unconditionally; StartServer() would not,
	 * since it refuses to build the registry when StartupModule() was skipped.
	 */
	void CollectAllRegisteredTools(TArray<TSharedPtr<IClaireonTool>>& OutTools)
	{
		OutTools.Reset();
		FClaireonModule::Get().EnsureServerForTest();
		TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (Tool.IsValid())
				{
					OutTools.Add(Tool);
				}
			}
		}
	}
}

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test sweeps the fully-populated
// ~717-tool registry, so give it real headroom instead of restoring the default.
UNTEST_UNIT_OPTS(Claireon, ToolNameUniqueness, AllRegisteredToolNamesAreUnique, UNTEST_TIMEOUTMS(30000))
{
	TArray<TSharedPtr<IClaireonTool>> AllTools;
	ClaireonToolNameUniquenessHelpers::CollectAllRegisteredTools(AllTools);

	// HARD FAILURE, not a skip. The helper calls EnsureServerForTest(), which
	// registers FClaireonBuiltinToolProvider unconditionally, so an empty raw
	// list means the seam broke rather than "commandlet mode". The old form
	// warned and then did UNTEST_EXPECT_TRUE(true) + co_return; Untest has no
	// skip primitive, so the runner scored that branch as a PASS, and in
	// commandlet runs it WAS the normal branch -- the collision sweep never ran.
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	// name -> count, so a collision is reported once with its multiplicity.
	TMap<FString, int32> NameCounts;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		NameCounts.FindOrAdd(Tool->GetName())++;
	}

	int32 Collisions = 0;
	for (const TPair<FString, int32>& Pair : NameCounts)
	{
		if (Pair.Value > 1)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolNameUniqueness] Tool name '%s' is registered %d times "
				     "(category_operation collision)"),
				*Pair.Key, Pair.Value);
			++Collisions;
		}
	}

	UNTEST_EXPECT_EQ(Collisions, 0);
	co_return;
}

// ---------------------------------------------------------------------------
// P1-5: a cross-provider collision must be REFUSED, not warned-then-overwritten.
//
// The sweep above cannot cover this. It walks the raw provider list, and
// EnsureServerForTest() registers only the builtin provider -- FSEditor's ~37
// tools register behind an IsRunningCommandlet() gate and the test runner IS a
// commandlet, so they are never in the swept set. The guard that has no such
// blind spot is the boot-time merge in CollectToolsFromProvider, which is what
// these tests drive through the CollectToolsFromProviderForTest seam.
// ---------------------------------------------------------------------------

namespace ClaireonToolNameUniquenessHelpers
{
	/** Minimal tool that reports a caller-chosen name. */
	class FStubCollidingTool : public IClaireonTool
	{
	public:
		FStubCollidingTool(const FString& InCategory, const FString& InOperation)
			: Category(InCategory), Operation(InOperation) {}

		virtual FString GetCategory() const override { return Category; }
		virtual FString GetOperation() const override { return Operation; }
		virtual FString GetDescription() const override { return TEXT("Claireon uniqueness-test stub."); }
		virtual TSharedPtr<FJsonObject> GetInputSchema() const override { return MakeShared<FJsonObject>(); }
		virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
		{
			return MakeErrorResult(TEXT("stub"));
		}

	private:
		FString Category;
		FString Operation;
	};

	class FStubProvider : public IClaireonToolProvider
	{
	public:
		explicit FStubProvider(const TSharedPtr<IClaireonTool>& InTool) : Tool(InTool) {}
		virtual TArray<TSharedPtr<IClaireonTool>> GetTools() const override { return { Tool }; }
		virtual FName GetProviderName() const override { return FName(TEXT("ClaireonUniquenessTestStub")); }

	private:
		TSharedPtr<IClaireonTool> Tool;
	};
}

UNTEST_UNIT_OPTS(Claireon, ToolNameUniqueness, CollidingProviderIsRefusedAndOriginalRetained, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonToolNameUniquenessHelpers;

	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	// Pick a real builtin to collide with, rather than hard-coding a name that a
	// later rename could quietly turn into a no-collision test.
	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	const TSharedPtr<IClaireonTool> Victim = AllTools[0];
	const FString VictimName = Victim->GetName();

	const TSharedPtr<IClaireonTool> Original = Server->FindTool(VictimName);
	UNTEST_ASSERT_TRUE(Original.IsValid());

	TSharedPtr<IClaireonTool> Stub = MakeShared<FStubCollidingTool>(
		Victim->GetCategory(), Victim->GetOperation());
	UNTEST_ASSERT_STREQ(*Stub->GetName(), *VictimName);

	FStubProvider Provider(Stub);
	// Logs an Error naming both providers. Untest does not derive from
	// FAutomationTestBase, so an Error log does not by itself fail this test --
	// the assertion below is what proves the refusal.
	FClaireonModule::Get().CollectToolsFromProviderForTest(&Provider);

	const TSharedPtr<IClaireonTool> AfterCollision = Server->FindTool(VictimName);
	UNTEST_ASSERT_TRUE(AfterCollision.IsValid());
	// The pre-existing tool is retained. Before the fix the stub won.
	UNTEST_ASSERT_TRUE(AfterCollision == Original);
	UNTEST_ASSERT_TRUE(AfterCollision != Stub);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolNameUniqueness, NonCollidingProviderStillRegisters, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonToolNameUniquenessHelpers;

	// The refusal must not be a blanket refusal of second providers -- that would
	// silently drop FSEditor's whole tool set.
	FClaireonServer* Server = FClaireonModule::Get().EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	TSharedPtr<IClaireonTool> Stub = MakeShared<FStubCollidingTool>(
		TEXT("claireon_uniqueness_test"), TEXT("unique_operation"));
	const FString StubName = Stub->GetName();
	UNTEST_ASSERT_FALSE(Server->FindTool(StubName).IsValid());

	FStubProvider Provider(Stub);
	FClaireonModule::Get().CollectToolsFromProviderForTest(&Provider);

	const TSharedPtr<IClaireonTool> Registered = Server->FindTool(StubName);

	// The registry outlives this test in the same process, so take the stub back
	// out before asserting -- a failing assertion must not also leave a fake tool
	// behind for every later test in the run.
	Server->UnregisterToolsBySource(FName(TEXT("ClaireonUniquenessTestStub")));

	UNTEST_ASSERT_TRUE(Registered.IsValid());
	UNTEST_ASSERT_TRUE(Registered == Stub);
	UNTEST_ASSERT_FALSE(Server->FindTool(StubName).IsValid());
	co_return;
}

#endif // WITH_UNTESTED
