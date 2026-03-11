// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonTraceSession.h"
#include "ClaireonLog.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"

bool FClaireonTraceSession::IsExpired(double TimeoutSeconds) const
{
	const double IdleSeconds = (FDateTime::UtcNow() - LastAccessTime).GetTotalSeconds();
	return IdleSeconds > TimeoutSeconds;
}

void FClaireonTraceSession::Touch()
{
	LastAccessTime = FDateTime::UtcNow();
}

const TraceServices::ITimingProfilerProvider* FClaireonTraceSession::GetTimingProvider() const
{
	if (!AnalysisSession.IsValid())
	{
		return nullptr;
	}
	return TraceServices::ReadTimingProfilerProvider(*AnalysisSession);
}

const TraceServices::IFrameProvider* FClaireonTraceSession::GetFrameProvider() const
{
	if (!AnalysisSession.IsValid())
	{
		return nullptr;
	}
	return &TraceServices::ReadFrameProvider(*AnalysisSession);
}

const TraceServices::IThreadProvider* FClaireonTraceSession::GetThreadProvider() const
{
	if (!AnalysisSession.IsValid())
	{
		return nullptr;
	}
	return &TraceServices::ReadThreadProvider(*AnalysisSession);
}

// ---

FClaireonTraceSessionManager& FClaireonTraceSessionManager::Get()
{
	static FClaireonTraceSessionManager Instance;
	return Instance;
}

FString FClaireonTraceSessionManager::GenerateSessionId()
{
	return FString::Printf(TEXT("trace_%d"), SessionCounter++);
}

FClaireonTraceSession* FClaireonTraceSessionManager::OpenSession(const FString& FilePath, FString& OutError)
{
	// Cleanup any expired sessions first
	CleanupExpiredSessions();

	// Validate file exists
	if (!IFileManager::Get().FileExists(*FilePath))
	{
		OutError = FString::Printf(TEXT("File not found: %s"), *FilePath);
		return nullptr;
	}

	// Validate file extension
	const FString Extension = FPaths::GetExtension(FilePath);
	if (!Extension.Equals(TEXT("utrace"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Expected .utrace file, got .%s"), *Extension);
		return nullptr;
	}

	// Load the TraceServices module
	ITraceServicesModule* TraceServicesModule = FModuleManager::GetModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
	if (!TraceServicesModule)
	{
		TraceServicesModule = FModuleManager::LoadModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
		if (!TraceServicesModule)
		{
			OutError = TEXT("Failed to load TraceServices module");
			return nullptr;
		}
	}

	TSharedPtr<TraceServices::IAnalysisService> AnalysisService = TraceServicesModule->GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		AnalysisService = TraceServicesModule->CreateAnalysisService();
		if (!AnalysisService.IsValid())
		{
			OutError = TEXT("Failed to create analysis service");
			return nullptr;
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Starting trace analysis for: %s"), *FilePath);

	// Run synchronous analysis (blocks until complete)
	TSharedPtr<const TraceServices::IAnalysisSession> AnalysisSession = AnalysisService->Analyze(*FilePath);
	if (!AnalysisSession.IsValid())
	{
		OutError = FString::Printf(TEXT("Failed to analyze trace file: %s"), *FilePath);
		return nullptr;
	}

	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*AnalysisSession);
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Trace analysis complete. Duration: %.2fs"), AnalysisSession->GetDurationSeconds());
	}

	// Create the session
	FScopeLock Lock(&CriticalSection);

	FString SessionId = GenerateSessionId();
	TSharedPtr<FClaireonTraceSession> Session = MakeShared<FClaireonTraceSession>();
	Session->SessionId = SessionId;
	Session->FilePath = FilePath;
	Session->AnalysisSession = AnalysisSession;
	Session->Touch();

	Sessions.Add(SessionId, Session);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Trace session created: %s"), *SessionId);

	return Session.Get();
}

FClaireonTraceSession* FClaireonTraceSessionManager::FindSession(const FString& SessionId)
{
	FScopeLock Lock(&CriticalSection);

	TSharedPtr<FClaireonTraceSession>* Found = Sessions.Find(SessionId);
	if (!Found || !Found->IsValid())
	{
		return nullptr;
	}

	FClaireonTraceSession* Session = Found->Get();

	if (Session->IsExpired())
	{
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Trace session expired: %s"), *SessionId);
		Sessions.Remove(SessionId);
		return nullptr;
	}

	Session->Touch();
	return Session;
}

bool FClaireonTraceSessionManager::CloseSession(const FString& SessionId)
{
	FScopeLock Lock(&CriticalSection);

	TSharedPtr<FClaireonTraceSession>* Found = Sessions.Find(SessionId);
	if (!Found || !Found->IsValid())
	{
		return false;
	}

	FClaireonTraceSession* Session = Found->Get();

	// Stop the analysis session if still running
	if (Session->AnalysisSession.IsValid())
	{
		Session->AnalysisSession->Stop(true);
	}

	Sessions.Remove(SessionId);
	UE_LOG(LogClaireon, Display, TEXT("[MCP] Trace session closed: %s"), *SessionId);
	return true;
}

void FClaireonTraceSessionManager::CleanupExpiredSessions()
{
	FScopeLock Lock(&CriticalSection);

	TArray<FString> ExpiredIds;
	for (const auto& Pair : Sessions)
	{
		if (Pair.Value.IsValid() && Pair.Value->IsExpired())
		{
			ExpiredIds.Add(Pair.Key);
		}
	}

	for (const FString& Id : ExpiredIds)
	{
		TSharedPtr<FClaireonTraceSession>* Found = Sessions.Find(Id);
		if (Found && Found->IsValid() && (*Found)->AnalysisSession.IsValid())
		{
			(*Found)->AnalysisSession->Stop(true);
		}
		Sessions.Remove(Id);
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Expired trace session removed: %s"), *Id);
	}
}
