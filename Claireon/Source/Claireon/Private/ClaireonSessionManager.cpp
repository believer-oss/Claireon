// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonSessionManager.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

FClaireonSessionManager& FClaireonSessionManager::Get()
{
	static FClaireonSessionManager Instance;
	return Instance;
}

FClaireonSessionManager::FClaireonSessionManager()
{
	CleanupTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
	{
		CleanupExpired();
		return true;
	}),
		300.0f);
}

FClaireonSessionManager::~FClaireonSessionManager()
{
	FTSTicker::GetCoreTicker().RemoveTicker(CleanupTickerHandle);
}

FMCPOpenSessionResult FClaireonSessionManager::OpenSession(const FString& AssetPath, const FString& ToolName, double InTimeoutMinutes)
{
	ensureMsgf(ToolName.StartsWith(TEXT("claireon.")), TEXT("Tool name must start with 'claireon.': %s"), *ToolName);

	const FString CanonicalPath = CanonicalizePath(AssetPath);
	if (CanonicalPath.IsEmpty())
	{
		return { EOpenSessionResult::InvalidAssetPath, TEXT(""), {} };
	}

	FScopeLock ScopeLock(&CriticalSection);

	// Opportunistic cleanup
	CleanupExpiredInternal();

	const FString* ExistingSessionId = AssetLocks.Find(CanonicalPath);
	if (ExistingSessionId)
	{
		FMCPSession* ExistingSession = Sessions.Find(*ExistingSessionId);
		if (ExistingSession)
		{
			if (ExistingSession->ToolName == ToolName)
			{
				// Same tool -- reuse
				ExistingSession->Touch();
				ExistingSession->TimeoutMinutes = FMath::Max(ExistingSession->TimeoutMinutes, InTimeoutMinutes);
				UE_LOG(LogClaireon, Verbose, TEXT("Reused existing session %s for tool '%s' on '%s'"),
					*ExistingSession->SessionId, *ToolName, *CanonicalPath);
				return { EOpenSessionResult::ReusedExistingSession, ExistingSession->SessionId, {} };
			}
			else
			{
				// Different tool -- blocked
				FMCPSession BlockingCopy = *ExistingSession;
				UE_LOG(LogClaireon, Warning, TEXT("Session blocked by tool '%s' on asset '%s'"),
					*ExistingSession->ToolName, *CanonicalPath);
				return { EOpenSessionResult::BlockedByOtherTool, TEXT(""), TOptional<FMCPSession>(MoveTemp(BlockingCopy)) };
			}
		}
		else
		{
			// Stale entry in AssetLocks -- remove it and fall through to create new
			AssetLocks.Remove(CanonicalPath);
		}
	}

	// Create new session
	const FString NewSessionId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	const FDateTime Now = FDateTime::UtcNow();

	FMCPSession NewSession;
	NewSession.SessionId = NewSessionId;
	NewSession.ToolName = ToolName;
	NewSession.AssetPath = CanonicalPath;
	NewSession.CreatedTime = Now;
	NewSession.LastAccessTime = Now;
	NewSession.TimeoutMinutes = InTimeoutMinutes;

	Sessions.Add(NewSessionId, NewSession);
	AssetLocks.Add(CanonicalPath, NewSessionId);

	UE_LOG(LogClaireon, Verbose, TEXT("Opened session %s for tool '%s' on '%s'"),
		*NewSessionId, *ToolName, *CanonicalPath);

	return { EOpenSessionResult::Success, NewSessionId, {} };
}

FMCPSession* FClaireonSessionManager::FindSession(const FString& SessionId)
{
	FMCPSessionClosedInfo ExpiredInfo;
	bool bWasExpired = false;

	{
		FScopeLock ScopeLock(&CriticalSection);

		FMCPSession* Found = Sessions.Find(SessionId);
		if (!Found)
		{
			return nullptr;
		}

		if (Found->IsExpired())
		{
			ExpiredInfo.SessionId = Found->SessionId;
			ExpiredInfo.AssetPath = Found->AssetPath;
			ExpiredInfo.ToolName = Found->ToolName;
			AssetLocks.Remove(Found->AssetPath);
			Sessions.Remove(SessionId);
			bWasExpired = true;
		}
		else
		{
			Found->Touch();
			return Found;
		}
	}

	if (bWasExpired)
	{
		OnSessionClosedDelegate.Broadcast(ExpiredInfo);
	}

	return nullptr;
}

bool FClaireonSessionManager::CloseSession(const FString& SessionId)
{
	FMCPSessionClosedInfo ClosedInfo;

	{
		FScopeLock ScopeLock(&CriticalSection);

		FMCPSession* Found = Sessions.Find(SessionId);
		if (!Found)
		{
			return false;
		}

		ClosedInfo.SessionId = Found->SessionId;
		ClosedInfo.AssetPath = Found->AssetPath;
		ClosedInfo.ToolName = Found->ToolName;

		AssetLocks.Remove(Found->AssetPath);
		Sessions.Remove(SessionId);
	}

	UE_LOG(LogClaireon, Verbose, TEXT("Closed session %s"), *ClosedInfo.SessionId);
	OnSessionClosedDelegate.Broadcast(ClosedInfo);
	return true;
}

void FClaireonSessionManager::TouchSession(const FString& SessionId)
{
	FScopeLock ScopeLock(&CriticalSection);

	FMCPSession* Found = Sessions.Find(SessionId);
	if (Found)
	{
		Found->Touch();
	}
}

TArray<FMCPSession> FClaireonSessionManager::ListSessions(const FString& ToolName) const
{
	FScopeLock ScopeLock(&CriticalSection);

	TArray<FMCPSession> Result;
	Result.Reserve(Sessions.Num());

	for (const auto& Pair : Sessions)
	{
		if (ToolName.IsEmpty() || Pair.Value.ToolName == ToolName)
		{
			Result.Add(Pair.Value);
		}
	}

	return Result;
}

bool FClaireonSessionManager::IsAssetLocked(const FString& AssetPath) const
{
	const FString CanonicalPath = CanonicalizePath(AssetPath);
	if (CanonicalPath.IsEmpty())
	{
		return false;
	}

	FScopeLock ScopeLock(&CriticalSection);
	return AssetLocks.Contains(CanonicalPath);
}

int32 FClaireonSessionManager::ReleaseByAssetPath(const FString& AssetPath)
{
	const FString CanonicalPath = CanonicalizePath(AssetPath);
	if (CanonicalPath.IsEmpty())
	{
		return 0;
	}

	FMCPSessionClosedInfo ClosedInfo;

	{
		FScopeLock ScopeLock(&CriticalSection);

		const FString* FoundSessionId = AssetLocks.Find(CanonicalPath);
		if (!FoundSessionId)
		{
			return 0;
		}

		FMCPSession* Found = Sessions.Find(*FoundSessionId);
		if (Found)
		{
			ClosedInfo.SessionId = Found->SessionId;
			ClosedInfo.AssetPath = Found->AssetPath;
			ClosedInfo.ToolName = Found->ToolName;
			Sessions.Remove(*FoundSessionId);
		}

		AssetLocks.Remove(CanonicalPath);
	}

	OnSessionClosedDelegate.Broadcast(ClosedInfo);
	return 1;
}

int32 FClaireonSessionManager::ForceReleaseAll()
{
	TArray<FMCPSessionClosedInfo> ClosedSessions;

	{
		FScopeLock ScopeLock(&CriticalSection);

		ClosedSessions.Reserve(Sessions.Num());
		for (const auto& Pair : Sessions)
		{
			FMCPSessionClosedInfo Info;
			Info.SessionId = Pair.Value.SessionId;
			Info.AssetPath = Pair.Value.AssetPath;
			Info.ToolName = Pair.Value.ToolName;
			ClosedSessions.Add(MoveTemp(Info));
		}

		Sessions.Empty();
		AssetLocks.Empty();
	}

	for (const FMCPSessionClosedInfo& Info : ClosedSessions)
	{
		OnSessionClosedDelegate.Broadcast(Info);
	}

	return ClosedSessions.Num();
}

void FClaireonSessionManager::CleanupExpired()
{
	TArray<FMCPSessionClosedInfo> Removed;

	{
		FScopeLock ScopeLock(&CriticalSection);
		Removed = CleanupExpiredInternal();
	}

	for (const FMCPSessionClosedInfo& Info : Removed)
	{
		OnSessionClosedDelegate.Broadcast(Info);
	}

	if (Removed.Num() > 0)
	{
		UE_LOG(LogClaireon, Verbose, TEXT("Cleanup removed %d expired session(s)"), Removed.Num());
	}
}

TArray<FMCPSessionClosedInfo> FClaireonSessionManager::CleanupExpiredInternal()
{
	TArray<FMCPSessionClosedInfo> Removed;
	TArray<FString> ExpiredIds;

	for (const auto& Pair : Sessions)
	{
		if (Pair.Value.IsExpired())
		{
			ExpiredIds.Add(Pair.Key);
		}
	}

	for (const FString& ExpiredId : ExpiredIds)
	{
		FMCPSession* Session = Sessions.Find(ExpiredId);
		if (Session)
		{
			FMCPSessionClosedInfo Info;
			Info.SessionId = Session->SessionId;
			Info.AssetPath = Session->AssetPath;
			Info.ToolName = Session->ToolName;
			Removed.Add(MoveTemp(Info));

			AssetLocks.Remove(Session->AssetPath);
		}
		Sessions.Remove(ExpiredId);
	}

	return Removed;
}

FOnMCPSessionClosed& FClaireonSessionManager::OnSessionClosed()
{
	return OnSessionClosedDelegate;
}

FString FClaireonSessionManager::CanonicalizePath(const FString& InPath)
{
	auto Result = ClaireonPathResolver::Resolve(InPath);
	if (!Result.bSuccess)
	{
		UE_LOG(LogClaireon, Warning, TEXT("Path canonicalization rejected invalid path: '%s' (%s)"), *InPath, *Result.Error);
		return FString();
	}

	// Session locking only applies to /Game/ assets
	if (!Result.ResolvedPath.Path.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogClaireon, Warning, TEXT("Path canonicalization rejected non-/Game/ path: '%s'"), *InPath);
		return FString();
	}

	return Result.ResolvedPath.Path;
}
