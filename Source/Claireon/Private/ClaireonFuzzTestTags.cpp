// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonFuzzTestTags.h"

#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace ClaireonFuzzTestTags
{
	void RegisterFuzzTestTags()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return;
		}
		// The search path supports late registration and ignores duplicates.
		UGameplayTagsManager::Get().AddTagIniSearchPath(
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("Tags")));
	}
}
