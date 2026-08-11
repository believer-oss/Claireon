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
#include "Misc/ScopeExit.h"
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
	if (!IsValid(Object))
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *AssetPath);
		return nullptr;
	}
	return Object;
}

// ── Git Revision Support ────────────────────────────────────────────────

FString ConvertAssetPathToGitRelativePath(const FString& AssetPath)
{
	// Strip a trailing .ObjectName: "/Game/Foo/Bar.Bar" names the object, and
	// git names the file. Package names cannot contain '.', so a dot in the last
	// segment is always the object separator.
	FString PackageName = AssetPath;
	{
		int32 LastSlash = INDEX_NONE;
		PackageName.FindLastChar(TEXT('/'), LastSlash);
		int32 Dot = INDEX_NONE;
		if (PackageName.FindLastChar(TEXT('.'), Dot) && Dot > LastSlash)
		{
			PackageName.LeftInline(Dot);
		}
	}

	// Ask the package system for the on-disk filename. This is what knows about
	// plugin mount points and about .umap-vs-.uasset; the old hard-coded
	// "/Game/ -> Content/" + ".uasset" answered wrongly for both, and wrongly in
	// the quiet way -- git then reported the path as missing at that revision.
	FString Filename;
	if (!FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		{
			// Unmounted or malformed: fall back to the historical mapping rather
			// than returning nothing, so the caller still gets a git error naming
			// a path instead of an empty-path error naming nothing.
			FString RelativePath = PackageName;
			RelativePath.ReplaceInline(TEXT("/Game/"), TEXT("Content/"));
			RelativePath += TEXT(".uasset");
			return RelativePath;
		}
	}

	Filename = FPaths::ConvertRelativePathToFull(Filename);
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::MakePathRelativeTo(Filename, *ProjectDir);
	return Filename;
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

namespace
{
	/** Wraps one argv entry for CreateProcess. Unconditional quoting is correct
	 *  here: SanitizeRevision rejects quotes and backslashes never reach us
	 *  (paths are normalised to forward slashes), so there is nothing to escape.
	 *  File-local prefix to avoid anon-NS collisions under unity batching. */
	FString Cl613Diff_QuoteArg(const FString& Arg)
	{
		return FString::Printf(TEXT("\"%s\""), *Arg);
	}
}

FGitCommandResult RunGitCommand(
	const TArray<FString>& Args,
	const TArray<uint8>* OptionalStdIn,
	double TimeoutSeconds)
{
	FGitCommandResult Result;

	FString Parms;
	for (const FString& Arg : Args)
	{
		if (!Parms.IsEmpty())
		{
			Parms += TEXT(" ");
		}
		Parms += Cl613Diff_QuoteArg(Arg);
	}

	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdErrRead = nullptr;
	void* StdErrWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;

	ON_SCOPE_EXIT
	{
		if (StdOutRead || StdOutWrite) { FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite); }
		if (StdErrRead || StdErrWrite) { FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite); }
		if (StdInRead || StdInWrite) { FPlatformProcess::ClosePipe(StdInRead, StdInWrite); }
	};

	if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite))
	{
		return Result;
	}
	if (!FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite))
	{
		return Result;
	}
	if (OptionalStdIn != nullptr)
	{
		// The child reads this end, so the WRITE end is the local one.
		if (!FPlatformProcess::CreatePipe(StdInRead, StdInWrite, /*bWritePipeLocal=*/ true))
		{
			return Result;
		}
	}

	// bLaunchDetached must stay false: a detached child does not inherit the
	// pipe handles, and the reads below would then block until the timeout.
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		TEXT("git"), *Parms,
		/* bLaunchDetached */ false,
		/* bLaunchHidden */ true,
		/* bLaunchReallyHidden */ true,
		/* OutProcessID */ nullptr,
		/* PriorityModifier */ 0,
		/* OptionalWorkingDirectory */ nullptr,
		/* PipeWriteChild */ StdOutWrite,
		/* PipeReadChild */ StdInRead,
		/* PipeStdErrChild */ StdErrWrite);

	if (!ProcHandle.IsValid())
	{
		return Result;
	}
	Result.bLaunched = true;

	if (OptionalStdIn != nullptr)
	{
		if (OptionalStdIn->Num() > 0)
		{
			FPlatformProcess::WritePipe(StdInWrite, OptionalStdIn->GetData(), OptionalStdIn->Num());
		}
		// Close our write end so the child sees EOF rather than hanging.
		FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
		StdInRead = nullptr;
		StdInWrite = nullptr;
	}

	// Drain both pipes WHILE the process runs. Waiting first would deadlock on
	// any payload larger than the pipe buffer, which every real .uasset is.
	TArray<uint8> Chunk;
	TArray<uint8> ErrBytes;
	const double StartTime = FPlatformTime::Seconds();

	auto DrainPipes = [&]()
	{
		while (FPlatformProcess::ReadPipeToArray(StdOutRead, Chunk))
		{
			Result.StdOut.Append(Chunk);
		}
		while (FPlatformProcess::ReadPipeToArray(StdErrRead, Chunk))
		{
			ErrBytes.Append(Chunk);
		}
	};

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		DrainPipes();
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			Result.bTimedOut = true;
			FPlatformProcess::TerminateProc(ProcHandle, true);
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}

	// Whatever the child wrote between the last poll and exit is still buffered.
	DrainPipes();

	FPlatformProcess::GetProcReturnCode(ProcHandle, &Result.ReturnCode);
	FPlatformProcess::CloseProc(ProcHandle);

	if (ErrBytes.Num() > 0)
	{
		ErrBytes.Add(0);
		Result.StdErr = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(ErrBytes.GetData())));
		Result.StdErr.TrimStartAndEndInline();
	}

	return Result;
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

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// git wants forward slashes in the pathspec on every platform.
	FString GitPath = GitRelativePath;
	GitPath.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString AbsTempFilePath = FPaths::ConvertRelativePathToFull(TempFilePath);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Extracting asset from git: %s:%s"), *Revision, *GitPath);

	// `<rev>:<path>` is ONE argv entry. Interpolating it into a shell string is
	// what let cmd.exe eat the `^` in `HEAD^` and report HEAD's own bytes.
	const TArray<FString> Args = {
		TEXT("-C"), ProjectDir,
		TEXT("show"), FString::Printf(TEXT("%s:%s"), *Revision, *GitPath)
	};

	constexpr double TimeoutSeconds = 30.0;
	const FGitCommandResult GitResult = RunGitCommand(Args, /*OptionalStdIn=*/ nullptr, TimeoutSeconds);

	if (!GitResult.bLaunched)
	{
		OutError = TEXT("Failed to launch git process for asset extraction. Is git on PATH?");
		return FString();
	}

	if (GitResult.bTimedOut)
	{
		OutError = TEXT("Git asset extraction timed out after 30 seconds.");
		return FString();
	}

	if (GitResult.ReturnCode != 0)
	{
		const FString& StdErr = GitResult.StdErr;

		// git's wording varies by subcommand and version. "invalid object name"
		// is what `git show <bad-rev>:<path>` actually says -- matching only
		// "bad revision"/"unknown revision" pushed the commonest case into the
		// generic branch, where the caller had to read raw git text to learn
		// that the revision, not the asset, was the problem.
		if (StdErr.Contains(TEXT("bad revision"))
			|| StdErr.Contains(TEXT("unknown revision"))
			|| StdErr.Contains(TEXT("invalid object name"))
			|| StdErr.Contains(TEXT("Needed a single revision")))
		{
			OutError = FString::Printf(TEXT("Bad git revision: '%s' (git: %s)"), *Revision, *StdErr);
		}
		else if (StdErr.Contains(TEXT("does not exist")) || StdErr.Contains(TEXT("path")))
		{
			OutError = FString::Printf(TEXT("Asset not found at revision '%s': %s (git: %s)"),
				*Revision, *GitRelativePath, *StdErr);
		}
		else
		{
			OutError = FString::Printf(TEXT("Git extraction failed (exit %d): %s"), GitResult.ReturnCode, *StdErr);
		}
		return FString();
	}

	if (GitResult.StdOut.Num() <= 0)
	{
		OutError = FString::Printf(TEXT("Git extraction produced no bytes for %s:%s"), *Revision, *GitRelativePath);
		return FString();
	}

	if (!FFileHelper::SaveArrayToFile(GitResult.StdOut, *AbsTempFilePath))
	{
		OutError = FString::Printf(TEXT("Failed to write extracted asset to temp file: %s"), *AbsTempFilePath);
		return FString();
	}

	const int64 FileSize = GitResult.StdOut.Num();

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Extracted %lld bytes to %s"), FileSize, *TempFilePath);

	// LFS-pointer detection. git show <rev>:<path> on an LFS-tracked file emits the
	// LFS pointer text (<300 bytes typically: "version https://git-lfs.github.com/spec/v1\n
	// oid sha256:<hex>\nsize <N>\n") instead of the real binary. .uasset payloads we care
	// about are far larger than this, so the heuristic is: if the file is small AND begins
	// with "version https://git-lfs.github.com/", smudge it through `git lfs smudge`.
	if (FileSize < 4096)
	{
		const TArray<uint8>& Head = GitResult.StdOut;

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

				// Smudge: feed the pointer bytes to `git lfs smudge -- <path>` on
				// stdin; stdout is the real object content. Same no-shell path as
				// the show above, with the pointer written to the child's stdin
				// pipe rather than redirected by cmd.exe.
				const TArray<FString> SmudgeArgs = {
					TEXT("-C"), ProjectDir,
					TEXT("lfs"), TEXT("smudge"), TEXT("--"), GitPath
				};

				const FGitCommandResult SmudgeResult =
					RunGitCommand(SmudgeArgs, &GitResult.StdOut, /*TimeoutSeconds=*/ 60.0); // LFS fetch can be slow

				if (!SmudgeResult.Succeeded())
				{
					CleanupTempFile(TempFilePath);
					OutError = FString::Printf(
						TEXT("git lfs smudge failed for %s:%s (exit %d): %s"),
						*Revision, *GitRelativePath, SmudgeResult.ReturnCode, *SmudgeResult.StdErr);
					return FString();
				}

				if (SmudgeResult.StdOut.Num() <= 0)
				{
					CleanupTempFile(TempFilePath);
					OutError = FString::Printf(
						TEXT("git lfs smudge produced no bytes for %s:%s"), *Revision, *GitRelativePath);
					return FString();
				}

				if (!FFileHelper::SaveArrayToFile(SmudgeResult.StdOut, *AbsTempFilePath))
				{
					CleanupTempFile(TempFilePath);
					OutError = TEXT("Failed to replace LFS-pointer temp file with smudged content");
					return FString();
				}

				UE_LOG(LogClaireon, Display, TEXT("[MCP] LFS smudge produced %d bytes for %s"),
					SmudgeResult.StdOut.Num(), *GitRelativePath);
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
	if (!IsValid(Package))
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
		if (IsValid(Object) && !Object->IsA<UPackage>() && Object->IsAsset())
		{
			FoundAsset = Object;
			return false; // stop iterating
		}
		return true; // continue
	});

	if (!IsValid(FoundAsset))
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
	if (!IsValid(Result.DiffPackage))
	{
		CleanupTempFile(TempFilePath);
		Result.TempFilePath.Empty();
		return Result;
	}

	Result.Object = FindAssetInPackage(Result.DiffPackage, OutError);
	if (!IsValid(Result.Object))
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
