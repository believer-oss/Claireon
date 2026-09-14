// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tests/ClaireonBPEditorFixtures.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "ClaireonSessionManager.h"
#include "Tests/ClaireonTestAssetDeletion.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonBPEditorFixtures
{
	const TCHAR* Root()
	{
		return TEXT("/Game/_ClaireonBPEditor");
	}

	FString UniquePath(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("%s/BP_%s_%s"), Root(), Stem,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	FString FixedPath(const TCHAR* Stem)
	{
		return FString::Printf(TEXT("%s/BP_%s"), Root(), Stem);
	}

	FString SharedBlueprintAssistFixturePath()
	{
		return FixedPath(TEXT("BAFixture"));
	}

	UObject* Resolve(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		return FSoftObjectPath(ObjectPath).ResolveObject();
	}

	UBlueprint* Create(const FString& AssetPath, FString& OutError)
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		const IClaireonTool::FToolResult Result = CreateTool.Execute(Args);
		if (Result.bIsError)
		{
			OutError = Result.ErrorMessage;
			return nullptr;
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(Resolve(AssetPath));
		if (!IsValid(Blueprint))
		{
			OutError = TEXT("bp_create reported success but the Blueprint did not resolve");
		}
		return Blueprint;
	}

	bool Save(UBlueprint* Blueprint)
	{
		if (!IsValid(Blueprint))
		{
			return false;
		}
		UPackage* Package = Blueprint->GetOutermost();
		if (!IsValid(Package))
		{
			return false;
		}
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Blueprint, *FileName, SaveArgs);
	}

	void Teardown(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

		UObject* Asset = Resolve(AssetPath);
		if (!IsValid(Asset))
		{
			return;
		}

		// Skip DeleteSingleObject's reference check; its deletion path also closes the editor.
		ClaireonTestAssetDeletion::DeleteObjectsForTest({ Asset });
	}

	UEdGraph* FirstUbergraph(UBlueprint* Blueprint)
	{
		if (!IsValid(Blueprint))
		{
			return nullptr;
		}
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (IsValid(Graph))
			{
				return Graph;
			}
		}
		return nullptr;
	}
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
