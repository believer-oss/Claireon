// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetMove.h"
#include "ClaireonPathResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_AssetMove::GetCategory() const  { return TEXT("asset"); }
FString ClaireonTool_AssetMove::GetOperation() const { return TEXT("move"); }

FString ClaireonTool_AssetMove::GetDescription() const
{
	// Wraps IAssetTools::RenameAssets with a single FAssetRenameData. Supports two
	// argument shapes: full destination (new_path) for move-and-rename, or new_name only
	// for same-package rename. Auto-runs FixupRedirectors unless keep_redirectors=true.
	return TEXT("Move or rename an asset. Two argument shapes:\n"
				"  1) asset_path + new_path  -- move to a different /Game/ location (and optional new name).\n"
				"  2) asset_path + new_name  -- same-package rename.\n"
				"Wraps IAssetTools::RenameAssets. Auto-fixes up redirectors unless keep_redirectors=true. "
				"Non-session, refuses to run while PIE is active.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetMove::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	auto MkProp = [](const TCHAR* Type, const TCHAR* Desc) {
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	};

	Properties->SetObjectField(TEXT("asset_path"),       MkProp(TEXT("string"),  TEXT("Source asset path (e.g. /Game/Old/M_Foo).")));
	Properties->SetObjectField(TEXT("new_path"),         MkProp(TEXT("string"),  TEXT("Full destination path (e.g. /Game/New/M_Foo). Use either new_path or new_name.")));
	Properties->SetObjectField(TEXT("new_name"),         MkProp(TEXT("string"),  TEXT("New asset name for same-package rename. Use either new_path or new_name.")));
	Properties->SetObjectField(TEXT("keep_redirectors"), MkProp(TEXT("boolean"), TEXT("If true, leave redirectors behind. Default false (auto-fixup).")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetMove::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString AssetPath, NewPath, NewName;
	bool bKeepRedirectors = false;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	Arguments->TryGetStringField(TEXT("new_path"), NewPath);
	Arguments->TryGetStringField(TEXT("new_name"), NewName);
	Arguments->TryGetBoolField(TEXT("keep_redirectors"), bKeepRedirectors);

	if (NewPath.IsEmpty() && NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Provide either new_path or new_name"));
	}

	// Resolve source asset
	ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(AssetPath);
	if (!R.bSuccess)
	{
		return MakeErrorResult(R.Error);
	}

	FString SrcObjectPath = R.ResolvedPath.Path;
	UObject* SrcAsset = LoadObject<UObject>(nullptr, *SrcObjectPath);
	if (!SrcAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load source asset: %s"), *SrcObjectPath));
	}

	// Compute destination package + name.
	FString DestPackagePath, DestObjectName;
	{
		// Source package: strip sub-object .X suffix.
		FString SrcPackage = SrcObjectPath;
		int32 Dot = INDEX_NONE;
		if (SrcPackage.FindChar(TEXT('.'), Dot))
		{
			SrcPackage = SrcPackage.Left(Dot);
		}
		const FString SrcLongName = FPackageName::GetLongPackagePath(SrcPackage);  // /Game/Old
		const FString SrcShort    = FPackageName::GetShortName(SrcPackage);        // M_Foo

		if (!NewName.IsEmpty() && NewPath.IsEmpty())
		{
			DestPackagePath = SrcLongName;
			DestObjectName  = NewName;
		}
		else
		{
			// NewPath dominates. May be either a folder (ends with /) or a full /Game/.../Name path.
			FString NormPath = NewPath;
			NormPath.TrimStartAndEndInline();
			if (NormPath.EndsWith(TEXT("/")))
			{
				NormPath = NormPath.LeftChop(1);
				DestPackagePath = NormPath;
				DestObjectName  = NewName.IsEmpty() ? SrcShort : NewName;
			}
			else
			{
				DestPackagePath = FPackageName::GetLongPackagePath(NormPath);
				DestObjectName  = FPackageName::GetShortName(NormPath);
				if (!NewName.IsEmpty())
				{
					DestObjectName = NewName;
				}
			}
		}
	}

	if (DestPackagePath.IsEmpty() || DestObjectName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Failed to compute destination package/name from inputs"));
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	IAssetTools& AssetTools = AssetToolsModule.Get();

	TArray<FAssetRenameData> RenameData;
	RenameData.Emplace(SrcAsset, DestPackagePath, DestObjectName);

	const bool bOk = AssetTools.RenameAssets(RenameData);
	if (!bOk)
	{
		return MakeErrorResult(FString::Printf(TEXT("RenameAssets failed for %s -> %s/%s"),
			*SrcObjectPath, *DestPackagePath, *DestObjectName));
	}

	// FixupRedirectors unless caller asked to keep them.
	if (!bKeepRedirectors)
	{
		// The source package now contains a redirector. Find it via Asset Registry and fix up.
		FString SrcPkgOnly = SrcObjectPath;
		int32 Dot = INDEX_NONE;
		if (SrcPkgOnly.FindChar(TEXT('.'), Dot))
		{
			SrcPkgOnly = SrcPkgOnly.Left(Dot);
		}
		const FString SrcParent = FPackageName::GetLongPackagePath(SrcPkgOnly);

		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Redirectors;
		FARFilter Filter;
		Filter.PackagePaths.Add(*SrcParent);
		Filter.bRecursivePaths = false;
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/CoreUObject"), TEXT("ObjectRedirector")));
		AR.GetAssets(Filter, Redirectors);

		if (Redirectors.Num() > 0)
		{
			TArray<UObjectRedirector*> RedirectorPtrs;
			for (const FAssetData& D : Redirectors)
			{
				if (UObjectRedirector* Red = Cast<UObjectRedirector>(D.GetAsset()))
				{
					RedirectorPtrs.Add(Red);
				}
			}
			if (RedirectorPtrs.Num() > 0)
			{
				AssetTools.FixupReferencers(RedirectorPtrs);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("from"), SrcObjectPath);
	Data->SetStringField(TEXT("to"), FString::Printf(TEXT("%s/%s.%s"), *DestPackagePath, *DestObjectName, *DestObjectName));
	Data->SetBoolField(TEXT("kept_redirectors"), bKeepRedirectors);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Moved %s -> %s/%s"),
		*SrcObjectPath, *DestPackagePath, *DestObjectName));
}
