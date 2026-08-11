// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

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

namespace ClaireonPathResolverInternal
{
	// Named file-local namespace (NOT a raw anonymous namespace): under
	// linux-build-server-v2 unity batching, anonymous namespaces from separate
	// .cpp merge into one TU and collide -- a named namespace is the isolation.
	// Splits an object path into its package-prefix form (everything before the
	// first '.' after the last '/').
	FString ClaireonPathResolver_PackagePartOf(const FString& ObjectPath)
	{
		int32 DotIndex = INDEX_NONE;
		if (ObjectPath.FindChar(TEXT('.'), DotIndex))
		{
			return ObjectPath.Left(DotIndex);
		}
		return ObjectPath;
	}

	// Attempts a direct in-memory lookup for a rooted path that the disk-based
	// heuristics cannot classify. Game-thread only; returns nullptr off-thread.
	// UPackage hits are rejected: a package-level input (e.g. '/Temp/Untitled_1'
	// for an unsaved map) must keep the historical pass-through + object-name
	// append behavior rather than resolving to the package object itself.
	UObject* ClaireonPathResolver_TryStaticFind(const FString& Path)
	{
		if (!IsInGameThread())
		{
			return nullptr;
		}
		UObject* Found = StaticFindObject(UObject::StaticClass(), /*Outer=*/nullptr, *Path, /*ExactClass=*/false);
		if (IsValid(Found) && Found->IsA<UPackage>())
		{
			return nullptr;
		}
		return Found;
	}
} // namespace ClaireonPathResolverInternal

using namespace ClaireonPathResolverInternal;

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
	// Step 4.5: Rooted in-memory mounts (/Memory/, /Temp/)
	// -----------------------------------------------------------------
	// These packages never exist on disk, so the filesystem/Content/
	// heuristics below would misclassify them as absolute filesystem paths
	// and fail with a misleading error. Resolve directly via StaticFindObject
	// when possible; otherwise pass the path through unchanged so the
	// caller's own lookup can report a precise "not found" error.
	if (Path.StartsWith(TEXT("/Memory/")) || Path.StartsWith(TEXT("/Temp/")))
	{
		if (UObject* InMemoryObject = ClaireonPathResolver_TryStaticFind(Path); IsValid(InMemoryObject))
		{
			Result.bSuccess = true;
			Result.ResolvedPath.Path = InMemoryObject->GetPathName();
			Result.ResolvedPath.PackagePath = InMemoryObject->GetOutermost()->GetName();
			Result.ResolvedPath.Kind = EPathKind::PackagePath;
			AppendTrace(Result.ResolvedPath.NormalizationTrace,
				TEXT("Resolved in-memory mount via StaticFindObject"));
			return Result;
		}

		// Not currently in memory (or off game thread): pass through as-is,
		// mirroring the object-name append of Step 12.5 for bare package paths.
		Result.ResolvedPath.PackagePath = ClaireonPathResolver_PackagePartOf(Path);
		{
			const int32 LastSlash = Path.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			const FString FinalSegment = (LastSlash != INDEX_NONE) ? Path.Mid(LastSlash + 1) : Path;
			if (!FinalSegment.IsEmpty() && !FinalSegment.Contains(TEXT(".")))
			{
				Path = Path + TEXT(".") + FinalSegment;
				AppendTrace(Result.ResolvedPath.NormalizationTrace, TEXT("Appended object-name suffix"));
			}
		}
		Result.bSuccess = true;
		Result.ResolvedPath.Path = Path;
		Result.ResolvedPath.Kind = EPathKind::PackagePath;
		AppendTrace(Result.ResolvedPath.NormalizationTrace,
			TEXT("In-memory mount passed through (object not found in memory)"));
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
					else if (UObject* InMemoryObject = ClaireonPathResolver_TryStaticFind(Path); IsValid(InMemoryObject))
					{
						// Last chance before failing: the rooted path may name
						// an object that lives only in memory on a mount
						// FPackageName does not know about (e.g. transient
						// packages backing live PIE state).
						Result.bSuccess = true;
						Result.ResolvedPath.Path = InMemoryObject->GetPathName();
						Result.ResolvedPath.PackagePath = InMemoryObject->GetOutermost()->GetName();
						Result.ResolvedPath.Kind = EPathKind::PackagePath;
						AppendTrace(Result.ResolvedPath.NormalizationTrace,
							TEXT("Resolved rooted non-mounted path via StaticFindObject"));
						return Result;
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

UObject* ResolveObjectFromPath(const FString& InPath, bool bAllowLoad, FString& OutError,
	FString* OutCoercionNote)
{
	UObject* Found = nullptr;
	bool bIsSubObject = false;

	FResolveResult Resolved = Resolve(InPath);
	if (Resolved.bSuccess)
	{
		const FString& Path = Resolved.ResolvedPath.Path;

		if (Resolved.ResolvedPath.Kind == EPathKind::NativeClassPath)
		{
			// (a) Native class path -> CDO.
			UClass* ResolvedClass = FindObject<UClass>(nullptr, *Path);
			if (!IsValid(ResolvedClass))
			{
				ResolvedClass = FindFirstObjectSafe<UClass>(*Path);
			}
			if (!IsValid(ResolvedClass) && bAllowLoad)
			{
				ResolvedClass = LoadObject<UClass>(nullptr, *Path);
			}
			if (IsValid(ResolvedClass))
			{
				return ResolvedClass->GetDefaultObject();
			}
			// Deliberately fall through rather than erroring here: a CDO SUB-OBJECT
			// ("/Script/Mod.Default__Foo.MyComp") is classified NativeClassPath by the
			// grammar but is not a class, and the raw fallback below resolves it.
		}
		else
		{
			// (b) Asset path or sub-object path.
			Found = StaticFindObject(
				UObject::StaticClass(),
				/*Outer=*/nullptr,
				*Path,
				/*ExactClass=*/false);

			bIsSubObject = Path.Contains(TEXT(":"));
			if (!IsValid(Found) && bIsSubObject)
			{
				// ANY_PACKAGE is deprecated in UE 5.1+; use FindFirstObjectSafe for
				// sub-object / world-actor paths that may not have a known outer.
				Found = FindFirstObjectSafe<UObject>(*Path);
			}

			if (!IsValid(Found) && !bIsSubObject && bAllowLoad)
			{
				Found = StaticLoadObject(
					UObject::StaticClass(),
					/*Outer=*/nullptr,
					*Path,
					/*Filename=*/nullptr,
					LOAD_None);
			}
		}
	}

	// (c) Raw-path fallback, tried LAST so nothing above changes behavior.
	//
	// Resolve() normalizes for ASSET paths -- appending an object-name suffix,
	// stripping _C. Applied to a path that is already a precise object path it can
	// produce something that resolves to nothing: a CDO sub-object, or an object in a
	// transient / test world whose package name contains dots. Those are exactly the
	// paths GetPathName() hands back, so they must work.
	//
	// A hit here is unambiguous -- the caller named a real object -- and this can only
	// convert a previous failure into a success.
	if (!IsValid(Found))
	{
		Found = StaticFindObject(
			UObject::StaticClass(),
			/*Outer=*/nullptr,
			*InPath,
			/*ExactClass=*/false);
		if (!IsValid(Found))
		{
			Found = FindFirstObjectSafe<UObject>(*InPath);
		}
	}

	if (!IsValid(Found))
	{
		if (!Resolved.bSuccess)
		{
			OutError = FString::Printf(
				TEXT("Could not resolve path '%s': %s"),
				*InPath,
				*Resolved.Error);
		}
		else if (!bAllowLoad && !bIsSubObject)
		{
			OutError = FString::Printf(
				TEXT("Object '%s' is not loaded (allow_load=false)."),
				*InPath);
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Could not find object '%s'."),
				*InPath);
		}
		return nullptr;
	}

	// (d) P0-8b: coerce a class reference to the object whose properties the
	// caller meant.
	//
	// bIsClassReference was computed by the grammar above and then ignored, and
	// only /Script/ native-class paths got CDO coercion (branch (a)). So
	// "/Game/.../BP_Foo.BP_Foo_C" resolved to the UBlueprintGeneratedClass
	// itself and a plain asset path to the UBlueprint -- and uobject_inspect /
	// uobject_set_property then walked the wrong object's property list and
	// failed with "Property 'X' not found on 'BlueprintGeneratedClass'".
	//
	// This is also why the SCS-subobject redirect looked broken: it exists, but
	// only fires when the property walk starts from the CDO.
	//
	// Narrow by construction: all three callers of this function are
	// property-oriented (uobject_inspect, uobject_set_property,
	// component_reregister), so none of them wants the raw UBlueprint asset. A
	// caller that did want it would need its own lookup rather than this one.
	if (UClass* AsClass = Cast<UClass>(Found))
	{
		if (UObject* DefaultObject = AsClass->GetDefaultObject())
		{
			if (OutCoercionNote)
			{
				*OutCoercionNote = FString::Printf(
					TEXT("Path named the class '%s'; resolved to its class default object. "
					     "Property reads and writes apply to the CDO."),
					*AsClass->GetPathName());
			}
			return DefaultObject;
		}
	}
	else if (const UBlueprint* AsBlueprint = Cast<UBlueprint>(Found))
	{
		if (UClass* GeneratedClass = AsBlueprint->GeneratedClass)
		{
			if (UObject* DefaultObject = GeneratedClass->GetDefaultObject())
			{
				if (OutCoercionNote)
				{
					*OutCoercionNote = FString::Printf(
						TEXT("Path named the Blueprint asset '%s'; resolved to the class default object of "
						     "its generated class. Property reads and writes apply to the CDO."),
						*AsBlueprint->GetPathName());
				}
				return DefaultObject;
			}
		}
	}

	return Found;
}

} // namespace ClaireonPathResolver
