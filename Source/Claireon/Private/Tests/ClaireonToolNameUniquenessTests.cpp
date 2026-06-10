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
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"

namespace ClaireonToolNameUniquenessHelpers
{
	/** Collect every IClaireonTool from every registered provider (raw, not deduped). */
	void CollectAllRegisteredTools(TArray<TSharedPtr<IClaireonTool>>& OutTools)
	{
		OutTools.Reset();
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

UNTEST_UNIT(Claireon, ToolNameUniqueness, AllRegisteredToolNamesAreUnique)
{
	TArray<TSharedPtr<IClaireonTool>> AllTools;
	ClaireonToolNameUniquenessHelpers::CollectAllRegisteredTools(AllTools);

	// Commandlet mode: ClaireonModule::StartupModule short-circuits via
	// IsRunningCommandlet, so no providers register as modular features and the
	// raw list is empty. Skip cleanly (mirrors ClaireonToolNameApiRegexTests).
	if (AllTools.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ToolNameUniqueness] SKIPPED -- no providers registered "
			     "(commandlet mode)"));
		UNTEST_EXPECT_TRUE(true);
		co_return;
	}

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

#endif // WITH_UNTESTED
