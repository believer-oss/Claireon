// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonScopedAssetLock.h"
#include "ClaireonLog.h"

FClaireonScopedAssetLock::FClaireonScopedAssetLock(const FString& AssetPath, const FString& ToolName, double TimeoutMinutes)
{
	HeldAssetPath = AssetPath;

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(AssetPath, ToolName, TimeoutMinutes);

	switch (OpenResult.Result)
	{
	case EOpenSessionResult::Success:
		SessionId = OpenResult.SessionId;
		bAcquired = true;
		bShouldCloseOnDestroy = true;
		break;

	case EOpenSessionResult::ReusedExistingSession:
		// Same tool, same asset: caller already holds it. Don't close on destroy --
		// the original holder owns the lifetime.
		SessionId = OpenResult.SessionId;
		bAcquired = true;
		bShouldCloseOnDestroy = false;
		break;

	case EOpenSessionResult::BlockedByOtherTool:
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		ErrorResult = IClaireonTool::MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use session_release with session_id='%s' to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*Blocker.SessionId));
		bAcquired = false;
		break;
	}

	case EOpenSessionResult::InvalidAssetPath:
		ErrorResult = IClaireonTool::MakeErrorResult(FString::Printf(
			TEXT("Invalid asset path: %s"), *AssetPath));
		bAcquired = false;
		break;
	}
}

FClaireonScopedAssetLock::~FClaireonScopedAssetLock()
{
	if (bShouldCloseOnDestroy && !SessionId.IsEmpty())
	{
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}
