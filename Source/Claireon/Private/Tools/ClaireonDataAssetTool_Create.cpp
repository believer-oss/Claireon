// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDataAssetTool_Create.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "ClaireonDataAssetTool_Create"

namespace
{
	// DataAssetCreate_: discriminator-prefixed file-local helpers to avoid unity-batch collisions
	// with similarly-named helpers across cohort files (e.g. ClaireonAttenuationTool_SetProperty.cpp).
	FString DataAssetCreate_JsonValueToString(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return FString();
		FString S;
		if (V->TryGetString(S)) return S;
		double N;
		bool B;
		if (V->TryGetNumber(N))
		{
			if (FMath::IsFinite(N) && FMath::Floor(N) == N && FMath::Abs(N) < 1e15)
			{
				return FString::Printf(TEXT("%lld"), (int64)N);
			}
			return FString::Printf(TEXT("%g"), N);
		}
		if (V->TryGetBool(B)) return B ? TEXT("true") : TEXT("false");
		return FString();
	}

	UClass* DataAssetCreate_ResolveClass(const FString& ClassPath)
	{
		if (ClassPath.StartsWith(TEXT("/Script/")))
		{
			return LoadObject<UClass>(nullptr, *ClassPath);
		}
		return ClaireonAssetUtils::ResolveClassName(ClassPath);
	}
}

FString FClaireonDataAssetTool_Create::GetCategory() const { return TEXT("data_asset"); }
FString FClaireonDataAssetTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonDataAssetTool_Create::GetDescription() const
{
	return TEXT("Create a UDataAsset / UPrimaryDataAsset subclass at a /Game/ path and optionally seed properties. "
				"Supports TSoftObjectPtr<T> fields directly via reflection (string paths are parsed into FSoftObjectPath). "
				"class_path accepts either /Script/Module.ClassName or a bare class name.");
}

TSharedPtr<FJsonObject> FClaireonDataAssetTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination /Game/ path"), true);
	S.AddString(TEXT("class_path"), TEXT("/Script/Module.ClassName or bare class name (must be a UDataAsset subclass)"), true);
	S.AddObject(TEXT("properties"), TEXT("Optional dot-path -> scalar/string map seeded after creation"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonDataAssetTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString ClassPath;
	if (!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_path"));
	}

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path (must start with /Game/)"));
	}
	const FString ObjectName = FPackageName::GetShortName(Canon);
	if (ObjectName.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not derive object name from path: %s"), *Canon));
	}

	if (UObject* Existing = LoadObject<UObject>(nullptr, *Canon))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s"), *Canon));
	}

	UClass* ResolvedClass = DataAssetCreate_ResolveClass(ClassPath);
	if (!ResolvedClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve class: %s"), *ClassPath));
	}
	if (!ResolvedClass->IsChildOf(UDataAsset::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class is not a UDataAsset subclass: %s"), *ResolvedClass->GetName()));
	}
	if (ResolvedClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class is abstract: %s"), *ResolvedClass->GetName()));
	}

	FScopedTransaction Transaction(LOCTEXT("CreateDataAsset", "[Claireon] Create Data Asset"));

	UPackage* Package = CreatePackage(*Canon);
	if (!Package)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("CreatePackage failed"));
	}

	UObject* NewAsset = NewObject<UObject>(Package, ResolvedClass, *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!NewAsset)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("NewObject failed"));
	}

	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	{
		FString AssertError;
		if (!ClaireonAssetUtils::AssertInnerNameMatchesPackage(NewAsset, AssertError))
		{
			// Failure cleanup mirrors the property-seeding failure branch
			// below: cancel transaction, garbage-collect the partially-created
			// asset so it does not linger in subsequent LoadObject queries.
			Transaction.Cancel();
			NewAsset->ClearFlags(RF_Standalone | RF_Public);
			NewAsset->MarkAsGarbage();
			return MakeErrorResult(AssertError);
		}
	}

	// Optional property seeding
	TArray<FString> WrittenPaths;
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropertiesPtr) && PropertiesPtr && PropertiesPtr->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesPtr)->Values)
		{
			const FString& PropertyPath = Pair.Key;
			const FString ValueStr = DataAssetCreate_JsonValueToString(Pair.Value);

			FString WriteError;
			if (!ClaireonPropertyUtils::WritePropertyByPath(NewAsset, PropertyPath, ValueStr, WriteError))
			{
				const FString FullError = FString::Printf(
					TEXT("Failed to set property '%s': %s"), *PropertyPath, *WriteError);

				// Failure cleanup: cancel transaction, mark new asset as garbage so it is not
				// visible in subsequent LoadObject queries.
				Transaction.Cancel();
				NewAsset->ClearFlags(RF_Standalone | RF_Public);
				NewAsset->MarkAsGarbage();

				return MakeErrorResult(FullError);
			}
			WrittenPaths.Add(PropertyPath);
		}
	}

	FString SaveError;
	if (!ClaireonAssetUtils::SaveAsset(NewAsset, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("class_path"), NewAsset->GetClass()->GetPathName());
	if (WrittenPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArr;
		for (const FString& P : WrittenPaths)
		{
			JsonArr.Add(MakeShared<FJsonValueString>(P));
		}
		Data->SetArrayField(TEXT("properties_set"), JsonArr);
	}

	const FString Summary = FString::Printf(TEXT("Created %s at %s"),
		*NewAsset->GetClass()->GetName(), *NewAsset->GetPathName());
	return MakeSuccessResult(Data, Summary);
}

#undef LOCTEXT_NAMESPACE
