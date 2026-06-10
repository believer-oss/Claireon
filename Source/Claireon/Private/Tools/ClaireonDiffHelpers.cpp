// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDiffHelpers.h"

#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "DiffUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace ClaireonDiffHelpers
{

// ── Resolution Parsing ──────────────────────────────────────────────────

bool ParseResolution(const FString& InString, EDiffResolution& OutResolution, FString& OutError)
{
	if (InString.Equals(TEXT("exists"), ESearchCase::IgnoreCase))
	{
		OutResolution = EDiffResolution::Exists;
		return true;
	}
	if (InString.Equals(TEXT("summary"), ESearchCase::IgnoreCase))
	{
		OutResolution = EDiffResolution::Summary;
		return true;
	}
	if (InString.Equals(TEXT("detailed"), ESearchCase::IgnoreCase))
	{
		OutResolution = EDiffResolution::Detailed;
		return true;
	}

	OutError = FString::Printf(TEXT("Invalid resolution '%s'. Must be 'exists', 'summary', or 'detailed'."), *InString);
	return false;
}

// ── Asset Validation & Loading ──────────────────────────────────────────

bool ValidateAssetPath(const FString& AssetPath, FString& OutError)
{
	auto Result = ClaireonPathResolver::Resolve(AssetPath);
	if (!Result.bSuccess)
	{
		OutError = Result.Error;
		return false;
	}
	return true;
}

UObject* LoadAssetFromPath(const FString& AssetPath, FString& OutError)
{
	const FSoftObjectPath SoftPath(AssetPath);
	UObject* Object = SoftPath.TryLoad();
	if (!Object)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *AssetPath);
		return nullptr;
	}
	return Object;
}

// ── Git Revision Support ────────────────────────────────────────────────

FString ConvertAssetPathToGitRelativePath(const FString& AssetPath)
{
	// /Game/Foo/Bar -> Content/Foo/Bar.uasset
	FString RelativePath = AssetPath;
	RelativePath.ReplaceInline(TEXT("/Game/"), TEXT("Content/"));
	RelativePath += TEXT(".uasset");
	return RelativePath;
}

bool SanitizeRevision(const FString& Revision, FString& OutError)
{
	// Only allow safe git revision characters: alphanumeric, ~, ^, ., /, -
	for (const TCHAR Ch : Revision)
	{
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('~') && Ch != TEXT('^') &&
			Ch != TEXT('.') && Ch != TEXT('/') && Ch != TEXT('-') && Ch != TEXT('_'))
		{
			OutError = FString::Printf(TEXT("Invalid character '%c' in revision string '%s'. Only alphanumeric and ~^./-_ allowed."), Ch, *Revision);
			return false;
		}
	}
	return true;
}

FString GetDiffTempDir()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Temp"), TEXT("MCP"), TEXT("Diff"));
}

FString ExtractAssetFromGitRevision(const FString& GitRelativePath, const FString& Revision, FString& OutError)
{
	// Sanitize revision
	if (!SanitizeRevision(Revision, OutError))
	{
		return FString();
	}

	// Create temp directory
	const FString TempDir = GetDiffTempDir();
	IFileManager::Get().MakeDirectory(*TempDir, true);

	// Generate unique temp file path
	const FString TempFileName = FGuid::NewGuid().ToString() + TEXT(".uasset");
	const FString TempFilePath = FPaths::Combine(TempDir, TempFileName);
	const FString ErrFilePath = TempFilePath + TEXT(".err");

	// Build git show command
	// Use cmd.exe /C with shell redirection for binary-safe extraction
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Normalize paths: forward slashes for git, but we need Windows backslashes for the redirection targets
	FString GitPath = GitRelativePath;
	GitPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString AbsTempFilePath = FPaths::ConvertRelativePathToFull(TempFilePath);
	const FString AbsErrFilePath = FPaths::ConvertRelativePathToFull(ErrFilePath);

	const FString GitCommand = FString::Printf(
		TEXT("git -C \"%s\" show %s:%s"),
		*ProjectDir, *Revision, *GitPath);

	// Use cmd.exe /C with redirection for binary-safe output
	const FString CmdArgs = FString::Printf(
		TEXT("/C \"%s > \"%s\" 2>\"%s\"\""),
		*GitCommand, *AbsTempFilePath, *AbsErrFilePath);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Extracting asset from git: %s:%s"), *Revision, *GitPath);

	// Launch process
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		TEXT("cmd.exe"), *CmdArgs,
		/* bLaunchDetached */ true,
		/* bLaunchHidden */ true,
		/* bLaunchReallyHidden */ true,
		/* OutProcessID */ nullptr,
		/* PriorityModifier */ 0,
		/* OptionalWorkingDirectory */ nullptr,
		/* PipeWriteChild */ nullptr,
		/* PipeReadChild */ nullptr);

	if (!ProcHandle.IsValid())
	{
		OutError = TEXT("Failed to launch git process for asset extraction.");
		return FString();
	}

	// Wait with timeout (30 seconds)
	constexpr double TimeoutSeconds = 30.0;
	const double StartTime = FPlatformTime::Seconds();
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		FPlatformProcess::Sleep(0.1f);
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			bTimedOut = true;
			FPlatformProcess::TerminateProc(ProcHandle, true);
			break;
		}
	}

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
	FPlatformProcess::CloseProc(ProcHandle);

	if (bTimedOut)
	{
		CleanupTempFile(TempFilePath);
		CleanupTempFile(ErrFilePath);
		OutError = TEXT("Git asset extraction timed out after 30 seconds.");
		return FString();
	}

	if (ReturnCode != 0)
	{
		// Read stderr for details
		FString StdErr;
		FFileHelper::LoadFileToString(StdErr, *AbsErrFilePath);
		StdErr.TrimStartAndEndInline();

		CleanupTempFile(TempFilePath);
		CleanupTempFile(ErrFilePath);

		if (StdErr.Contains(TEXT("bad revision")) || StdErr.Contains(TEXT("unknown revision")))
		{
			OutError = FString::Printf(TEXT("Bad git revision: '%s'"), *Revision);
		}
		else if (StdErr.Contains(TEXT("does not exist")) || StdErr.Contains(TEXT("path") ))
		{
			OutError = FString::Printf(TEXT("Asset not found at revision '%s': %s"), *Revision, *GitRelativePath);
		}
		else
		{
			OutError = FString::Printf(TEXT("Git extraction failed (exit %d): %s"), ReturnCode, *StdErr);
		}
		return FString();
	}

	// Clean up stderr file
	CleanupTempFile(ErrFilePath);

	// Verify the temp file exists and is non-empty
	if (!IFileManager::Get().FileExists(*AbsTempFilePath))
	{
		OutError = FString::Printf(TEXT("Git extraction produced no output file for %s:%s"), *Revision, *GitRelativePath);
		return FString();
	}

	const int64 FileSize = IFileManager::Get().FileSize(*AbsTempFilePath);
	if (FileSize <= 0)
	{
		CleanupTempFile(TempFilePath);
		OutError = FString::Printf(TEXT("Git extraction produced empty file for %s:%s"), *Revision, *GitRelativePath);
		return FString();
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Extracted %lld bytes to %s"), FileSize, *TempFilePath);

	// LFS-pointer detection. git show <rev>:<path> on an LFS-tracked file emits the
	// LFS pointer text (<300 bytes typically: "version https://git-lfs.github.com/spec/v1\n
	// oid sha256:<hex>\nsize <N>\n") instead of the real binary. .uasset payloads we care
	// about are far larger than this, so the heuristic is: if the file is small AND begins
	// with "version https://git-lfs.github.com/", smudge it through `git lfs smudge`.
	if (FileSize < 4096)
	{
		TArray<uint8> Head;
		if (FFileHelper::LoadFileToArray(Head, *AbsTempFilePath) && Head.Num() > 0)
		{
			// Treat as ASCII text only when no NULs in first 256 bytes.
			const int32 Probe = FMath::Min<int32>(Head.Num(), 256);
			bool bAscii = true;
			for (int32 i = 0; i < Probe; ++i)
			{
				if (Head[i] == 0)
				{
					bAscii = false;
					break;
				}
			}
			if (bAscii)
			{
				const FString HeadStr(Probe, reinterpret_cast<const char*>(Head.GetData()));
				if (HeadStr.StartsWith(TEXT("version https://git-lfs.github.com/")))
				{
					UE_LOG(LogClaireon, Display, TEXT("[MCP] LFS pointer detected; smudging %s"), *GitRelativePath);

					// Smudge: feed the pointer text through `git lfs smudge -- <path>` whose stdin is the
					// pointer; stdout becomes the real object content. Use the same cmd.exe redirection
					// pattern as the show call above.
					const FString SmudgeOut = AbsTempFilePath + TEXT(".smudged");
					const FString SmudgeErr = AbsTempFilePath + TEXT(".smudge.err");
					const FString SmudgeCommand = FString::Printf(
						TEXT("git -C \"%s\" lfs smudge -- %s"),
						*ProjectDir, *GitPath);
					const FString SmudgeCmdArgs = FString::Printf(
						TEXT("/C \"%s < \"%s\" > \"%s\" 2>\"%s\"\""),
						*SmudgeCommand, *AbsTempFilePath, *SmudgeOut, *SmudgeErr);

					FProcHandle SmudgeProc = FPlatformProcess::CreateProc(
						TEXT("cmd.exe"), *SmudgeCmdArgs,
						true, true, true, nullptr, 0, nullptr, nullptr, nullptr);
					if (!SmudgeProc.IsValid())
					{
						CleanupTempFile(SmudgeOut);
						CleanupTempFile(SmudgeErr);
						OutError = TEXT("Failed to launch git lfs smudge process");
						CleanupTempFile(TempFilePath);
						return FString();
					}

					const double SmudgeStart = FPlatformTime::Seconds();
					bool bSmudgeTimedOut = false;
					while (FPlatformProcess::IsProcRunning(SmudgeProc))
					{
						FPlatformProcess::Sleep(0.1f);
						if (FPlatformTime::Seconds() - SmudgeStart > 60.0)  // LFS fetch can be slow
						{
							bSmudgeTimedOut = true;
							FPlatformProcess::TerminateProc(SmudgeProc, true);
							break;
						}
					}
					int32 SmudgeRet = -1;
					FPlatformProcess::GetProcReturnCode(SmudgeProc, &SmudgeRet);
					FPlatformProcess::CloseProc(SmudgeProc);

					if (bSmudgeTimedOut || SmudgeRet != 0)
					{
						FString SmudgeStdErr;
						FFileHelper::LoadFileToString(SmudgeStdErr, *SmudgeErr);
						SmudgeStdErr.TrimStartAndEndInline();
						CleanupTempFile(SmudgeOut);
						CleanupTempFile(SmudgeErr);
						CleanupTempFile(TempFilePath);
						OutError = FString::Printf(
							TEXT("git lfs smudge failed for %s:%s (exit %d): %s"),
							*Revision, *GitRelativePath, SmudgeRet, *SmudgeStdErr);
						return FString();
					}

					CleanupTempFile(SmudgeErr);

					// Replace pointer file with smudged content.
					if (!IFileManager::Get().Move(*AbsTempFilePath, *SmudgeOut, /*bReplace*/true, /*bEvenIfReadOnly*/true))
					{
						CleanupTempFile(SmudgeOut);
						CleanupTempFile(TempFilePath);
						OutError = TEXT("Failed to replace LFS-pointer temp file with smudged content");
						return FString();
					}

					const int64 SmudgedSize = IFileManager::Get().FileSize(*AbsTempFilePath);
					UE_LOG(LogClaireon, Display, TEXT("[MCP] LFS smudge produced %lld bytes for %s"),
						SmudgedSize, *GitRelativePath);
				}
			}
		}
	}

	return TempFilePath;
}

UPackage* LoadPackageForDiff(const FString& TempFilePath, const FString& OrigAssetPath, FString& OutError)
{
	const FString AbsTempFilePath = FPaths::ConvertRelativePathToFull(TempFilePath);

	// Build FPackagePath from the temp file
	FPackagePath TempPackagePath;
	if (!FPackagePath::TryFromMountedName(AbsTempFilePath, TempPackagePath))
	{
		// Fall back to constructing from local path
		TempPackagePath = FPackagePath::FromLocalPath(AbsTempFilePath);
	}

	// Build FPackagePath for the original asset (for proper outer resolution)
	FString OrigPackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(OrigAssetPath, OrigPackageName))
	{
		OrigPackageName = OrigAssetPath;
	}

	FPackagePath OrigPackagePath;
	FPackagePath::TryFromPackageName(OrigPackageName, OrigPackagePath);

	UPackage* Package = DiffUtils::LoadPackageForDiff(TempPackagePath, OrigPackagePath);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to load diff package from temp file: %s"), *TempFilePath);
		return nullptr;
	}

	return Package;
}

UObject* FindAssetInPackage(UPackage* Package, FString& OutError)
{
	UObject* FoundAsset = nullptr;

	ForEachObjectWithPackage(Package, [&FoundAsset](UObject* Object)
	{
		if (Object && !Object->IsA<UPackage>() && Object->IsAsset())
		{
			FoundAsset = Object;
			return false; // stop iterating
		}
		return true; // continue
	});

	if (!FoundAsset)
	{
		OutError = FString::Printf(TEXT("No asset found in diff package: %s"), *Package->GetName());
	}
	return FoundAsset;
}

void CleanupTempFile(const FString& TempFilePath)
{
	if (!TempFilePath.IsEmpty())
	{
		const FString AbsPath = FPaths::ConvertRelativePathToFull(TempFilePath);
		IFileManager::Get().Delete(*AbsPath, false, false, true);
	}
}

// ── Side Resolution ─────────────────────────────────────────────────────

FResolvedDiffSide ResolveDiffSide(const FString& AssetPath, const FString& Revision, FString& OutError)
{
	FResolvedDiffSide Result;

	// Validate asset path
	if (!ValidateAssetPath(AssetPath, OutError))
	{
		return Result;
	}

	if (Revision.IsEmpty())
	{
		// Load from current editor state
		Result.Object = LoadAssetFromPath(AssetPath, OutError);
		return Result;
	}

	// Extract from git revision
	const FString GitRelativePath = ConvertAssetPathToGitRelativePath(AssetPath);
	const FString TempFilePath = ExtractAssetFromGitRevision(GitRelativePath, Revision, OutError);
	if (TempFilePath.IsEmpty())
	{
		return Result;
	}

	Result.TempFilePath = TempFilePath;

	// Load via DiffUtils
	Result.DiffPackage = LoadPackageForDiff(TempFilePath, AssetPath, OutError);
	if (!Result.DiffPackage)
	{
		CleanupTempFile(TempFilePath);
		Result.TempFilePath.Empty();
		return Result;
	}

	Result.Object = FindAssetInPackage(Result.DiffPackage, OutError);
	if (!Result.Object)
	{
		CleanupTempFile(TempFilePath);
		Result.TempFilePath.Empty();
		return Result;
	}

	return Result;
}

// ── Property Value Export ───────────────────────────────────────────────

FString ExportPropertyValue(const FProperty* Property, const void* ContainerPtr)
{
	if (!Property || !ContainerPtr)
	{
		return TEXT("(null)");
	}

	FString Value;
	Property->ExportTextItem_Direct(Value, ContainerPtr, nullptr, nullptr, PPF_None);
	return Value;
}

// ── Parameter Validation ────────────────────────────────────────────────

bool ValidateDiffParameters(
	const FString& AssetPathA, const FString& RevisionA,
	const FString& AssetPathB, const FString& RevisionB,
	FString& OutError)
{
	// Validate asset paths
	if (!ValidateAssetPath(AssetPathA, OutError))
	{
		return false;
	}

	if (!AssetPathB.IsEmpty() && !ValidateAssetPath(AssetPathB, OutError))
	{
		return false;
	}

	// Sanitize revisions if present
	if (!RevisionA.IsEmpty() && !SanitizeRevision(RevisionA, OutError))
	{
		return false;
	}

	if (!RevisionB.IsEmpty() && !SanitizeRevision(RevisionB, OutError))
	{
		return false;
	}

	// Determine effective path B
	const FString EffectivePathB = AssetPathB.IsEmpty() ? AssetPathA : AssetPathB;

	// At least one revision must be specified, or paths must differ
	const bool bPathsDiffer = !AssetPathA.Equals(EffectivePathB, ESearchCase::IgnoreCase);
	const bool bHasAnyRevision = !RevisionA.IsEmpty() || !RevisionB.IsEmpty();

	if (!bPathsDiffer && !bHasAnyRevision)
	{
		OutError = TEXT("At least one revision must be specified, or asset_path_b must differ from asset_path_a. "
			"Otherwise both sides would be the same object.");
		return false;
	}

	return true;
}

FString FormatSideLabel(const FString& Revision)
{
	return Revision.IsEmpty() ? TEXT("current") : Revision;
}

} // namespace ClaireonDiffHelpers
