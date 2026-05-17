// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace ClaireonPathResolver
{

void AppendTrace(FString& Trace, const TCHAR* Step)
{
	if (!Trace.IsEmpty())
	{
		Trace += TEXT("; ");
	}
	Trace += Step;
}

FResolveResult Resolve(const FString& InPath)
{
	FResolveResult Result;
	Result.ResolvedPath.OriginalInput = InPath;

	// -----------------------------------------------------------------
	// Step 1: Reject degenerate input
	// -----------------------------------------------------------------
	FString Path = InPath;
	Path.TrimStartAndEndInline();

	if (Path.IsEmpty())
	{
		Result.bSuccess = false;
		Result.Error = TEXT("Path is empty.");
		return Result;
	}

	// -----------------------------------------------------------------
	// Step 2: Trim and normalize slashes
	// -----------------------------------------------------------------
	{
		const FString Before = Path;

		Path.ReplaceCharInline(TEXT('\\'), TEXT('/'));

		while (Path.Contains(TEXT("//")))
		{
			Path.ReplaceInline(TEXT("//"), TEXT("/"));
		}

		if (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}

		FPaths::CollapseRelativeDirectories(Path);

		if (Path != Before)
		{
			AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Normalized slashes/whitespace"));
		}
	}

	// -----------------------------------------------------------------
	// Step 3: Strip file extension (.uasset, .umap) - case-insensitive
	// -----------------------------------------------------------------
	if (Path.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
	{
		Path.LeftChopInline(7);
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Stripped .uasset extension"));
	}
	else if (Path.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
	{
		Path.LeftChopInline(5);
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Stripped .umap extension"));
	}

	if (Path.IsEmpty())
	{
		Result.bSuccess = false;
		Result.Error = TEXT("Path contained only a file extension.");
		return Result;
	}

	// -----------------------------------------------------------------
	// Step 4: Early exit for /Script/ paths
	// -----------------------------------------------------------------
	if (Path.StartsWith(TEXT("/Script/")))
	{
		Result.bSuccess = true;
		Result.ResolvedPath.Path = Path;
		Result.ResolvedPath.PackagePath = Path;
		Result.ResolvedPath.Kind = EPathKind::NativeClassPath;
		Result.ResolvedPath.bIsClassReference = false;
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Native class path (/Script/)"));
		return Result;
	}

	// -----------------------------------------------------------------
	// Step 5: Strip duplicate object name only
	// -----------------------------------------------------------------
	{
		const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (LastSlash != INDEX_NONE)
		{
			const FString Segment = Path.Mid(LastSlash + 1);
			// Check each dot position to handle asset names containing dots (e.g. BP_V2.0.BP_V2.0)
			int32 SearchFrom = 0;
			while (SearchFrom < Segment.Len())
			{
				const int32 DotPos = Segment.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
				if (DotPos == INDEX_NONE)
				{
					break;
				}
				const FString AssetName = Segment.Left(DotPos);
				const FString ObjectName = Segment.Mid(DotPos + 1);
				if (!AssetName.IsEmpty() && AssetName == ObjectName)
				{
					Path.LeftInline(LastSlash + 1 + DotPos);
					AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Stripped duplicate object name"));
					break;
				}
				SearchFrom = DotPos + 1;
			}
		}
	}

	// -----------------------------------------------------------------
	// Step 6: Detect and convert filesystem absolute path
	// -----------------------------------------------------------------
	{
		bool bIsAbsolute = false;

		// Cross-platform absolute path detection using Unreal system utilities.
		// FPaths::IsRelative handles drive letters on Windows and root-relative
		// paths on Unix/Mac. We exclude Unreal package paths (/Game/, /Engine/,
		// plugin mounts, etc.) which also start with / but aren't filesystem paths.
		if (!FPaths::IsRelative(Path))
		{
			FString MappedFilename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Path, MappedFilename))
			{
				bIsAbsolute = true;
			}
		}

		if (bIsAbsolute)
		{
			const int32 ContentIdx = Path.Find(TEXT("Content/"), ESearchCase::IgnoreCase);
			if (ContentIdx != INDEX_NONE)
			{
				const FString AfterContent = Path.Mid(ContentIdx + 8); // 8 = len("Content/")
				if (AfterContent.IsEmpty())
				{
					Path = TEXT("/Game");
				}
				else
				{
					Path = TEXT("/Game/") + AfterContent;
				}
				AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Converted absolute filesystem path via Content/"));
			}
			else
			{
				// Check for bare "Content" at end
				if (Path.EndsWith(TEXT("Content"), ESearchCase::IgnoreCase))
				{
					Path = TEXT("/Game");
					AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Converted absolute filesystem path (bare Content)"));
				}
				else
				{
					// Fallback: try engine package name conversion
					FString OutPackageName;
					if (FPackageName::TryConvertFilenameToLongPackageName(Path, OutPackageName))
					{
						Path = OutPackageName;
						AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Converted absolute path via FPackageName"));
					}
					else
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(
							TEXT("Absolute path does not contain 'Content/' and could not be mapped to a package path: %s"),
							*InPath);
						return Result;
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------
	// Step 7: Detect and convert Content/ relative path
	// -----------------------------------------------------------------
	if (Path.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase))
	{
		Path = TEXT("/Game/") + Path.Mid(8); // 8 = len("Content/")
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Converted Content/ relative path"));
	}
	else if (Path.Equals(TEXT("Content"), ESearchCase::IgnoreCase))
	{
		Path = TEXT("/Game");
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Converted bare Content to /Game"));
	}

	// -----------------------------------------------------------------
	// Step 8: Detect and convert Game/ without leading slash
	// -----------------------------------------------------------------
	if (Path.StartsWith(TEXT("Game/"), ESearchCase::IgnoreCase))
	{
		Path = TEXT("/") + Path;
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Prepended / to Game/ path"));
	}

	// -----------------------------------------------------------------
	// Step 9: Handle bare relative paths
	// -----------------------------------------------------------------
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = TEXT("/Game/") + Path;
		UE_LOG(LogClaireon, Verbose, TEXT("ClaireonPathResolver: Treated bare path '%s' as /Game/-relative -> '%s'"),
			*InPath, *Path);
		AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Assumed /Game/-relative"));
	}

	// -----------------------------------------------------------------
	// Step 10: Detect _C class suffix
	// -----------------------------------------------------------------
	{
		const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		const FString FinalSegment = (LastSlash != INDEX_NONE) ? Path.Mid(LastSlash + 1) : Path;

		if (FinalSegment.Len() > 2 && FinalSegment.EndsWith(TEXT("_C")))
		{
			if (IsInGameThread())
			{
				IAssetRegistry& Registry =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

				// Check if path as-is exists in the registry
				FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
				if (AssetData.IsValid())
				{
					// The _C asset exists directly -- keep as-is
					Result.ResolvedPath.bIsClassReference = false;
					AppendTrace(Result.ResolvedPath.NormalizationTrace,
						TEXT("Path ending in _C exists as asset; kept as-is"));
				}
				else
				{
					// Strip _C and check again
					FString StrippedPath = Path.LeftChop(2);
					FAssetData StrippedData = Registry.GetAssetByObjectPath(FSoftObjectPath(StrippedPath));
					if (StrippedData.IsValid())
					{
						Path = StrippedPath;
						Result.ResolvedPath.bIsClassReference = true;
						AppendTrace(Result.ResolvedPath.NormalizationTrace,
							TEXT("Stripped _C suffix; asset found at stripped path"));
					}
					else
					{
						// Neither found -- best-effort strip
						Path = StrippedPath;
						Result.ResolvedPath.bIsClassReference = true;
						AppendTrace(Result.ResolvedPath.NormalizationTrace,
							TEXT("Stripped _C suffix (best-effort; neither path found in registry)"));
					}
				}
			}
			else
			{
				// Off game thread -- strip unconditionally
				Path.LeftChopInline(2);
				Result.ResolvedPath.bIsClassReference = true;
				UE_LOG(LogClaireon, Warning,
					TEXT("ClaireonPathResolver: _C disambiguation skipped (not on game thread) for path '%s'"),
					*InPath);
				AppendTrace(Result.ResolvedPath.NormalizationTrace,
					TEXT("Stripped _C suffix unconditionally (off game thread)"));
			}
		}
	}

	// -----------------------------------------------------------------
	// Step 11: Fallback for unrecognized paths
	// -----------------------------------------------------------------
	if (Path.StartsWith(TEXT("/"))
		&& !Path.StartsWith(TEXT("/Game/"))
		&& !Path.StartsWith(TEXT("/Game"))  // bare /Game
		&& !Path.StartsWith(TEXT("/Engine/"))
		&& !Path.StartsWith(TEXT("/Script/")))
	{
		FString OutPackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(Path, OutPackageName))
		{
			Path = OutPackageName;
			AppendTrace(Result.ResolvedPath.NormalizationTrace,
				TEXT("Converted unrecognized mount via FPackageName"));
		}
		// else: pass through as-is (may be a valid plugin or custom mount point)
	}

	// -----------------------------------------------------------------
	// Step 12: Validate
	// -----------------------------------------------------------------
	if (!Path.StartsWith(TEXT("/")))
	{
		Result.bSuccess = false;
		Result.Error = FString::Printf(
			TEXT("Internal error: resolved path does not start with '/': %s"), *Path);
		return Result;
	}

	// -----------------------------------------------------------------
	// Step 12.5: Object-name append for PackagePath kind
	// -----------------------------------------------------------------
	// Capture the package-prefix form before any append so folder-path callers
	// have access to it via ResolvedPath.PackagePath.
	Result.ResolvedPath.PackagePath = Path;

	{
		const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		const FString FinalSegment = (LastSlash != INDEX_NONE) ? Path.Mid(LastSlash + 1) : Path;
		if (!FinalSegment.IsEmpty() && !FinalSegment.Contains(TEXT(".")))
		{
			Path = Path + TEXT(".") + FinalSegment;
			AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Appended object-name suffix"));
		}
	}

	// -----------------------------------------------------------------
	// Step 13: Return success
	// -----------------------------------------------------------------
	Result.bSuccess = true;
	Result.ResolvedPath.Path = Path;
	Result.ResolvedPath.Kind = EPathKind::PackagePath;
	return Result;
}

} // namespace ClaireonPathResolver
