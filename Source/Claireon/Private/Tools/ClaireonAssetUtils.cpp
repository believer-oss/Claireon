// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAssetUtils.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "UObject/UObjectIterator.h"
#include "Misc/ScopedSlowTask.h"

namespace ClaireonAssetUtils
{

// Map of CDO -> Blueprint for save routing
static TMap<TWeakObjectPtr<UObject>, TWeakObjectPtr<UBlueprint>> GCDOToBlueprintMap;

// Resolve the on-disk filename a package should be saved to. For packages that already exist on
// disk, DoesPackageExist returns their current filename. For freshly created in-memory packages
// (e.g. data_asset_create, or a level authored in a demo) there is no disk file yet, so
// DoesPackageExist is false even though the package is perfectly saveable -- fall back to
// synthesizing the target filename from the mounted long package name, at the extension the
// package's own ContainsMap() calls for. Fails when the package name is under no mounted content
// root, or when a shadow package with the wrong extension already owns the name.
bool ResolvePackageSaveFilename(const UPackage* Package, FString& OutFileName, FString& OutError)
{
	if (!IsValid(Package))
	{
		OutError = TEXT("Cannot resolve a save filename: package is null");
		return false;
	}

	const FString PackageName = Package->GetName();

	// The extension has to come from the package, because nothing else can supply it.
	//
	// A package file records no extension internally -- it is chosen by whoever calls
	// SavePackage(Package, Asset, *Filename, Args). A level's package is an ordinary UPackage
	// that happens to carry PKG_ContainsMap, so nothing forces .umap on it. And name-based
	// resolution searches .uasset FIRST: FPackagePath::GetPossibleExtensions returns a slice
	// starting at EPackageExtension::Asset (PackagePath.cpp:304-318).
	//
	// Those three facts compose into a silent corruption. One save that hardcodes the asset
	// extension writes /Game/Foo/L_Bar to L_Bar.uasset; from that moment every resolution by
	// package name finds the shadow first, the .umap is orphaned, and every later save lands in
	// the shadow. The engine's own UEditorAssetSubsystem::SaveAsset does not have this problem
	// -- it delegates to UEditorLoadingAndSavingUtils::SavePackages, which synthesizes .umap for
	// a world package with no existing filename (FileHelpers.cpp:716).
	const bool bIsMap = Package->ContainsMap();
	const FString RequiredExtension = bIsMap
		? FPackageName::GetMapPackageExtension()
		: FPackageName::GetAssetPackageExtension();

	if (FPackageName::DoesPackageExist(PackageName, &OutFileName))
	{
		// DoesPackageExist resolves by NAME, so once a shadow exists it hands back the wrong
		// file and saving into it deepens the damage. Refuse instead: the symptoms of carrying
		// on are why this costs an hour to find -- the save reports success, no package reports
		// dirty, actors appear not to persist across a reload, and the stray file reads as
		// BuiltData in git. The tell is a size/date inversion, a large recent .uasset beside a
		// small stale .umap.
		const FString FoundExtension = FPaths::GetExtension(OutFileName, /*bIncludeDot=*/true);
		if (!FoundExtension.IsEmpty() && !FoundExtension.Equals(RequiredExtension, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Refusing to save package '%s': it %s a map, but the file resolved by package "
					 "name is '%s'. A shadow package with the wrong extension already exists, and "
					 "every save by name lands in it while the '%s' file is orphaned. Fix the files "
					 "on disk first -- switch the editor off this package, rename the shadow to the "
					 "correct extension, and delete the orphan."),
				*PackageName,
				bIsMap ? TEXT("IS") : TEXT("is NOT"),
				*OutFileName,
				*RequiredExtension);
			OutFileName.Reset();
			return false;
		}
		return true;
	}

	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, OutFileName, RequiredExtension))
	{
		return true;
	}
	OutError = FString::Printf(
		TEXT("Cannot resolve a save filename for package '%s': it does not exist on disk and is not under any mounted content root"),
		*PackageName);
	return false;
}

UObject* LoadAssetForEditing(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Immediate edit calls need the loaded Blueprint and CDO before routing the operation and its save.
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	if (IsValid(Blueprint))
	{
		if (!IsValid(Blueprint->GeneratedClass))
		{
			OutError = FString::Printf(TEXT("Blueprint '%s' has no GeneratedClass"), *ResolvedPath);
			return nullptr;
		}
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		if (!IsValid(CDO))
		{
			OutError = FString::Printf(TEXT("Failed to get CDO from Blueprint '%s'"), *ResolvedPath);
			return nullptr;
		}
		GCDOToBlueprintMap.Add(CDO, Blueprint);
		return CDO;
	}

	// Not a Blueprint — try loading as a native UObject
	UObject* Asset = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!IsValid(Asset))
	{
		OutError = FString::Printf(TEXT("Failed to load asset at '%s'"), *ResolvedPath);
		return nullptr;
	}

	return Asset;
}

TArray<FAssetData> FindAssetsByClass(UClass* Class, const FString& NameFilter, int32 Limit)
{
	TArray<FAssetData> Results;
	if (!IsValid(Class))
	{
		return Results;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// First try direct class search (works for native data assets)
	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssetsByClass(Class->GetClassPathName(), AllAssets, true);

	// If no results and the class is Blueprint-able, search for Blueprint assets
	// whose parent class matches. Many GAS types (GameplayEffect, GameplayAbility, etc.)
	// are stored as Blueprint assets in the AssetRegistry.
	if (AllAssets.IsEmpty())
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> BlueprintAssets;
		AssetRegistry.GetAssets(Filter, BlueprintAssets);

		FString TargetClassName = Class->GetClassPathName().ToString();
		// Also match by short name for common types
		FString TargetShortName = Class->GetName();

		for (const FAssetData& Asset : BlueprintAssets)
		{
			// Check the NativeParentClass or ParentClass tag
			FAssetTagValueRef ParentClassTag = Asset.TagsAndValues.FindTag(FName(TEXT("NativeParentClass")));
			if (!ParentClassTag.IsSet())
			{
				ParentClassTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			}

			if (ParentClassTag.IsSet())
			{
				FString ParentClassPath = ParentClassTag.GetValue();
				// Remove wrapping quotes/apostrophes if present (tag format varies)
				ParentClassPath.RemoveFromStart(TEXT("'"));
				ParentClassPath.RemoveFromEnd(TEXT("'"));

				// Try loading by full path first
				UClass* ParentClass = FindObject<UClass>(nullptr, *ParentClassPath);
				if (!IsValid(ParentClass))
				{
					// Extract short class name and search all loaded classes
					FString ShortName;
					if (ParentClassPath.Split(TEXT("."), nullptr, &ShortName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
					{
						ShortName.RemoveFromEnd(TEXT("'"));
						for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
						{
							if (ClassIt->GetName() == ShortName)
							{
								ParentClass = *ClassIt;
								break;
							}
						}
					}
				}

				if (IsValid(ParentClass) && ParentClass->IsChildOf(Class))
				{
					AllAssets.Add(Asset);
				}
			}
		}
	}

	for (const FAssetData& Asset : AllAssets)
	{
		// Skip redirector assets
		if (Asset.IsRedirector())
		{
			continue;
		}
		if (!NameFilter.IsEmpty() && !Asset.AssetName.ToString().Contains(NameFilter))
		{
			continue;
		}
		Results.Add(Asset);
		if (Limit > 0 && Results.Num() >= Limit)
		{
			break;
		}
	}

	return Results;
}

TArray<UClass*> FindDerivedClasses(UClass* BaseClass, bool bIncludeAbstract, const FString& NameFilter)
{
	TArray<UClass*> Results;
	if (!IsValid(BaseClass))
	{
		return Results;
	}

	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(BaseClass, DerivedClasses, true);

	for (UClass* DerivedClass : DerivedClasses)
	{
		if (!bIncludeAbstract && DerivedClass->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		if (!NameFilter.IsEmpty() && !DerivedClass->GetName().Contains(NameFilter))
		{
			continue;
		}
		Results.Add(DerivedClass);
	}

	return Results;
}

bool SaveAsset(UObject* Asset, FString& OutError)
{
	if (!IsValid(Asset))
	{
		OutError = TEXT("Null asset");
		return false;
	}

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}

	// Check if this is a Blueprint CDO
	TWeakObjectPtr<UBlueprint>* BlueprintPtr = GCDOToBlueprintMap.Find(Asset);
	if (BlueprintPtr && BlueprintPtr->IsValid())
	{
		UBlueprint* Blueprint = BlueprintPtr->Get();
		Blueprint->Modify();
		Asset->Modify();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		UPackage* Package = Blueprint->GetOutermost();
		FString PackageFileName;
		if (!ResolvePackageSaveFilename(Package, PackageFileName, OutError))
		{
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		// Return I/O failures to the caller so its mutation result survives save failure.
		SaveArgs.SaveFlags |= SAVE_NoError;
		FSavePackageResultStruct Result = UPackage::Save(Package, Blueprint, *PackageFileName, SaveArgs);
		if (Result.Result != ESavePackageResult::Success)
		{
			OutError = FString::Printf(TEXT("Failed to save Blueprint package '%s'"), *Package->GetName());
			return false;
		}
		return true;
	}

	// Native UObject save
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();

	FString PackageFileName;
	if (!ResolvePackageSaveFilename(Package, PackageFileName, OutError))
	{
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	// Return I/O failures to the caller so its mutation result survives save failure.
	SaveArgs.SaveFlags |= SAVE_NoError;
	FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *PackageFileName, SaveArgs);
	if (Result.Result != ESavePackageResult::Success)
	{
		OutError = FString::Printf(TEXT("Failed to save package '%s'"), *Package->GetName());
		return false;
	}

	return true;
}

TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& Data)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("path"), Data.GetObjectPathString());
	Obj->SetStringField(TEXT("name"), Data.AssetName.ToString());
	Obj->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());
	return Obj;
}

void RefreshAssetEditorIfOpen(UObject* Asset)
{
	if (!IsValid(Asset) || !IsValid(GEditor)) return;
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!IsValid(Subsystem)) return;
	if (Subsystem->FindEditorForAsset(Asset, false) != nullptr)
	{
		Subsystem->CloseAllEditorsForAsset(Asset);
		Subsystem->OpenEditorForAsset(Asset);
	}
}

void EmitSessionHintIfNeeded(
	TSharedPtr<FJsonObject>& ResponseData,
	int32 ConsecutiveAssetPathCalls,
	const FString& AssetPath,
	const FString& SessionId,
	const FString& ToolName,
	TSharedPtr<FJsonObject>& OutHint)
{
	OutHint.Reset();
	if (ConsecutiveAssetPathCalls > 5 && ConsecutiveAssetPathCalls % 5 == 1)
	{
		const FString HintText = FString::Printf(
			TEXT("You've called tools on '%s' %d times in a row with asset_path (no session_id). ")
			TEXT("Session %s is still locked on that asset and will not release until idle timeout. ")
			TEXT("For multi-step edits on this asset, read Data.session_id from any response and pass ")
			TEXT("it on subsequent calls. Call operation='close' when done to release the lock."),
			*AssetPath,
			ConsecutiveAssetPathCalls,
			*SessionId);
		ResponseData->SetStringField(TEXT("session_hint"), HintText);

		// Keep hints separate from Summary, which contains serialized JSON. Omit args
		// because the next call depends on the tool.
		OutHint = IClaireonTool::MakeGuidanceHint(ToolName, HintText);
	}
}

UClass* ResolveClassName(const FString& ClassName)
{
	// UClass::GetName() omits the U/A prefix, so strip it for matching
	FString Stripped = ClassName;
	if (Stripped.Len() > 1 && (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('A')) && FChar::IsUpper(Stripped[1]))
	{
		Stripped.RightChopInline(1);
	}
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == Stripped || It->GetName() == ClassName) return *It;
	}
	return nullptr;
}

// A candidate eviction name is only usable if the two names DERIVED from it are free
// as well. UBlueprint::Rename forwards to RenameGeneratedClasses, which builds the
// generated- and skeleton-class names from the new Blueprint name via
// UBlueprint::GetBlueprintClassNames -- "<Name>_C" and "SKEL_<Name>_C" -- and then calls
// UClass::Rename on each. MakeUniqueObjectName guarantees only that the BASE name is
// free, so it cannot by itself rule out a collision on those two.
//
// This is a guard, not a routinely-hit path: the per-(Outer, Class) suffix counter
// normally hands out a number it has never used, whose derived names are therefore also
// unused. What makes the check load-bearing is name REUSE -- MakeUniqueObjectName's
// MakeUniqueObjectNameReusingNumber path can hand back a number freed by GC, and a
// Blueprint and its generated class are not collected in lockstep (the Blueprint is
// marked garbage here; the class stays alive while its CDO or a derived class holds a
// reference). A reused number whose "_C" survived is exactly the case that would fatal.
//
// RenameGeneratedClasses' own TryFreeCDOName moves a colliding CDO aside but never the
// class itself, so it does not protect against this.
static bool AssetUtils_IsEvictionNameFree(UPackage* Outer, const FName Candidate)
{
	const FString Base = Candidate.ToString();

	// Deliberately the exact lookup UObject::Rename performs before it decides a name is
	// taken (Obj.cpp: StaticFindObject with a null class and ExactClass=true). Using a
	// near-equivalent -- StaticFindObjectFast, or a class-qualified search -- risks
	// disagreeing with Rename about garbage-marked or differently-classed occupants, and
	// disagreeing in the permissive direction is fatal rather than merely wrong.
	const auto IsFree = [Outer](const FString& Name)
	{
		return StaticFindObject(/*Class=*/nullptr, Outer, *Name, /*ExactClass=*/true) == nullptr;
	};

	return IsFree(Base)
		&& IsFree(FString::Printf(TEXT("%s_C"), *Base))
		&& IsFree(FString::Printf(TEXT("SKEL_%s_C"), *Base));
}

void EvictInMemoryObject(UPackage* Package, const FString& AssetName)
{
	if (!IsValid(Package) || AssetName.IsEmpty())
	{
		return;
	}

	UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *AssetName);
	if (!IsValid(Existing))
	{
		return;
	}

	UPackage* const Transient = GetTransientPackage();

	// Evict under a NEW unique name, never the object's current one.
	//
	// Passing a null name to Rename() keeps the name, so the transient package ends up
	// holding "BP_Foo" (plus the derived "BP_Foo_C"). Evicting a second same-named asset
	// in the same process then renames on top of that first eviction, which is a fatal
	// error, not a recoverable one. It is reachable from any tool that creates the same
	// asset path twice in one editor session -- bp_create, animbp create/duplicate,
	// widgetbp_create and the WidgetBP spec applicator all route through here.
	FName Evicted = NAME_None;
	for (int32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		// Monotonically increasing suffix, so this makes progress on every call.
		const FName Candidate = MakeUniqueObjectName(Transient, Existing->GetClass(), Existing->GetFName());
		if (AssetUtils_IsEvictionNameFree(Transient, Candidate))
		{
			Evicted = Candidate;
			break;
		}
	}

	if (Evicted.IsNone())
	{
		// Better to leave the name occupied and let the caller's create API fail with an
		// ordinary error than to rename onto a live object and take the process down.
		UE_LOG(LogClaireon, Error,
			TEXT("EvictInMemoryObject: no free transient name for '%s'; leaving it in place."),
			*AssetName);
		return;
	}

	Existing->ClearFlags(RF_Public | RF_Standalone);
	Existing->Rename(*Evicted.ToString(), Transient,
		REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	Existing->MarkAsGarbage();
}

bool AssertInnerNameMatchesPackage(const UObject* Asset, FString& OutError)
{
	OutError.Reset();

	if (!IsValid(Asset))
	{
		OutError = TEXT("AssertInnerNameMatchesPackage: Asset is null");
		return false;
	}

	const UPackage* Package = Asset->GetPackage();
	if (!IsValid(Package))
	{
		OutError = FString::Printf(
			TEXT("AssertInnerNameMatchesPackage: asset %s has no outer package"),
			*Asset->GetName());
		return false;
	}

	const FString PackageName = Package->GetName();
	const FString PackageShortName = FPackageName::GetShortName(PackageName);
	const FString InnerName = Asset->GetName();

	if (InnerName == PackageShortName)
	{
		return true;
	}

	OutError = FString::Printf(
		TEXT("AssertInnerNameMatchesPackage: inner-name/package-short-name mismatch -- ")
		TEXT("package_path='%s' package_short_name='%s' inner_name='%s' asset_class='%s'. ")
		TEXT("This is the bug class detected by claireon.asset_check_inner_name_invariant. ")
		TEXT("See Docs/Proposals/6619-claireon-name-mismatch-audit/PROPOSAL.md."),
		*PackageName, *PackageShortName, *InnerName,
		*Asset->GetClass()->GetName());
	return false;
}

} // namespace ClaireonAssetUtils
