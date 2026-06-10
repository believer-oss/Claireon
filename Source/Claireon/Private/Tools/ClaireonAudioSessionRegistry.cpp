// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioSessionRegistry.h"
#include "ClaireonSessionManager.h"

namespace
{
	// namespace-isolated helpers. Names are unique enough across the Claireon module that no
	// further file-local discriminator prefix is needed (verified by basename uniqueness scan).
	TMap<FString, FAudioEditToolData>& GetSoundCueSessions()
	{
		static TMap<FString, FAudioEditToolData> Map;
		return Map;
	}
	TMap<FString, FAudioEditToolData>& GetMetaSoundSessions()
	{
		static TMap<FString, FAudioEditToolData> Map;
		return Map;
	}
	TMap<FString, FAudioEditToolData>& MapFor(ESoundCohort Cohort)
	{
		return Cohort == ESoundCohort::SoundCue ? GetSoundCueSessions() : GetMetaSoundSessions();
	}
	bool& DelegateRegistered()
	{
		static bool b = false;
		return b;
	}
}

void ClaireonAudioSessionRegistry::EnsureDelegateRegistered()
{
	if (!DelegateRegistered())
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonAudioSessionRegistry::HandleSessionClosed);
		DelegateRegistered() = true;
	}
}

FAudioEditToolData* ClaireonAudioSessionRegistry::CreateSession(const FString& SessionId, ESoundCohort Cohort)
{
	return &MapFor(Cohort).Add(SessionId, FAudioEditToolData{});
}

FAudioEditToolData* ClaireonAudioSessionRegistry::FindSession(const FString& SessionId, ESoundCohort Cohort)
{
	return MapFor(Cohort).Find(SessionId);
}

void ClaireonAudioSessionRegistry::ReleaseSession(const FString& SessionId, ESoundCohort Cohort)
{
	MapFor(Cohort).Remove(SessionId);
}

void ClaireonAudioSessionRegistry::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	// filter on the literal "audio_edit" so external tests/callers continue to observe
	// the same lock-name string after decomposition.
	if (Info.ToolName == TEXT("audio_edit"))
	{
		// We don't know which cohort the session was in, so try both (Remove is a no-op if absent).
		GetSoundCueSessions().Remove(Info.SessionId);
		GetMetaSoundSessions().Remove(Info.SessionId);
	}
}
