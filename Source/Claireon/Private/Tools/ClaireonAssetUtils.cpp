// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
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

UObject* LoadAssetForEditing(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Try loading as Blueprint first
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	if (Blueprint)
	{
		if (!Blueprint->GeneratedClass)
		{
			OutError = FString::Printf(TEXT("Blueprint '%s' has no GeneratedClass"), *ResolvedPath);
			return nullptr;
		}
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		if (!CDO)
		{
			OutError = FString::Printf(TEXT("Failed to get CDO from Blueprint '%s'"), *ResolvedPath);
			return nullptr;
		}
		GCDOToBlueprintMap.Add(CDO, Blueprint);
		return CDO;
	}

	// Not a Blueprint — try loading as a native UObject
	UObject* Asset = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at '%s'"), *ResolvedPath);
		return nullptr;
	}

	return Asset;
}

TArray<FAssetData> FindAssetsByClass(UClass* Class, const FString& NameFilter, int32 Limit)
{
	TArray<FAssetData> Results;
	if (!Class)
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
				if (!ParentClass)
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

				if (ParentClass && ParentClass->IsChildOf(Class))
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
	if (!BaseClass)
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
	if (!Asset)
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
		if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName))
		{
			OutError = FString::Printf(TEXT("Package file not found for '%s'"), *Package->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
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
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName))
	{
		OutError = FString::Printf(TEXT("Package file not found for '%s'"), *Package->GetName());
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
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
	if (!Asset || !GEditor) return;
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem) return;
	if (Subsystem->FindEditorForAsset(Asset, false) != nullptr)
	{
		Subsystem->CloseAllEditorsForAsset(Asset);
		Subsystem->OpenEditorForAsset(Asset);
	}
}

void OpenAssetEditorIfHeadless(UObject* Asset)
{
	if (!Asset || !GEditor) return;
	if (!GIsEditor || IsRunningCommandlet()) return;
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem) return;
	Subsystem->OpenEditorForAsset(Asset);
}

void EmitSessionHintIfNeeded(
	TSharedPtr<FJsonObject>& ResponseData,
	int32 ConsecutiveAssetPathCalls,
	const FString& AssetPath,
	const FString& SessionId,
	FString& OutSummaryTag)
{
	OutSummaryTag.Reset();
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
		OutSummaryTag = FString::Printf(
			TEXT("\n\n[hint] session_hint: reuse session_id for '%s' (session=%s)."),
			*AssetPath,
			*SessionId);
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

bool AssertInnerNameMatchesPackage(const UObject* Asset, FString& OutError)
{
	OutError.Reset();

	if (!Asset)
	{
		OutError = TEXT("AssertInnerNameMatchesPackage: Asset is null");
		return false;
	}

	const UPackage* Package = Asset->GetPackage();
	if (!Package)
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
