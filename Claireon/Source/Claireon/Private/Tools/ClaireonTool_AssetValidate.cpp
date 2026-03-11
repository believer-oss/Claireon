// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetValidate.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

FString ClaireonTool_AssetValidate::GetName() const
{
	return TEXT("validate_assets");
}

FString ClaireonTool_AssetValidate::GetDescription() const
{
	return TEXT("Validate asset integrity and check for broken references. Scans the Asset Registry for issues without loading assets.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetValidate::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// contentPath - optional
	TSharedPtr<FJsonObject> ContentPathProp = MakeShared<FJsonObject>();
	ContentPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ContentPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path to validate (default: /Game). E.g. /Game/Characters"));
	Properties->SetObjectField(TEXT("contentPath"), ContentPathProp);

	// checkReferences - optional
	TSharedPtr<FJsonObject> CheckRefsProp = MakeShared<FJsonObject>();
	CheckRefsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CheckRefsProp->SetStringField(TEXT("description"),
		TEXT("Check asset dependencies for broken references (default: false). This is more thorough but slower."));
	Properties->SetObjectField(TEXT("checkReferences"), CheckRefsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetValidate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ContentPath = TEXT("/Game");
	Arguments->TryGetStringField(TEXT("contentPath"), ContentPath);

	bool bCheckReferences = false;
	Arguments->TryGetBoolField(TEXT("checkReferences"), bCheckReferences);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Gather all assets under the given path
	TArray<FAssetData> AllAssets;
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*ContentPath));
	Filter.bRecursivePaths = true;
	AssetRegistry.GetAssets(Filter, AllAssets);

	int32 ValidatedCount = AllAssets.Num();
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	TArray<TSharedPtr<FJsonValue>> IssuesArray;

	for (const FAssetData& Asset : AllAssets)
	{
		// Check for missing packages (broken hard references)
		if (bCheckReferences)
		{
			TArray<FName> Dependencies;
			AssetRegistry.GetDependencies(Asset.PackageName, Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Hard);

			for (const FName& Dep : Dependencies)
			{
				// If the dependency has no package data, it's a broken reference
				TOptional<FAssetPackageData> DepData = AssetRegistry.GetAssetPackageDataCopy(Dep);
				if (!DepData.IsSet())
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("asset_path"), Asset.GetSoftObjectPath().ToString());
					Issue->SetStringField(TEXT("severity"), TEXT("error"));
					Issue->SetStringField(TEXT("message"),
						FString::Printf(TEXT("Broken hard reference to missing package: %s"), *Dep.ToString()));
					IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
					++ErrorCount;
				}
			}
		}

		// Check for redirect assets (they should typically be fixed up)
		if (Asset.IsRedirector())
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("asset_path"), Asset.GetSoftObjectPath().ToString());
			Issue->SetStringField(TEXT("severity"), TEXT("warning"));
			Issue->SetStringField(TEXT("message"), TEXT("Asset is a redirector and should be fixed up"));
			IssuesArray.Add(MakeShared<FJsonValueObject>(Issue));
			++WarningCount;
		}
	}

	int32 IssueCount = ErrorCount + WarningCount;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("validated_count"), ValidatedCount);
	Data->SetNumberField(TEXT("issue_count"), IssueCount);
	Data->SetArrayField(TEXT("issues"), IssuesArray);

	FString Summary = FString::Printf(
		TEXT("Validated %d assets: %d issues (%d error%s, %d warning%s)"),
		ValidatedCount,
		IssueCount,
		ErrorCount, ErrorCount == 1 ? TEXT("") : TEXT("s"),
		WarningCount, WarningCount == 1 ? TEXT("") : TEXT("s"));

	return MakeSuccessResult(Data, Summary);
}
