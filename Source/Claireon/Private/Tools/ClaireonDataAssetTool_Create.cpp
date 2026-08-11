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

namespace ClaireonDataAssetTool_Create_Private
{
	// Retire a half-built asset so the path is genuinely free again.
	//
	// The three failure branches below used to clear RF_Standalone|RF_Public and call
	// MarkAsGarbage, on the stated assumption that this made the asset "not visible in
	// subsequent LoadObject queries". It does not. A garbage-marked object still occupies
	// its name inside its package until a GC actually collects it, and LoadObject keeps
	// resolving it until then -- so a failed create left a half-built asset reachable, and
	// the retry path's "Asset already exists" pre-check then wedged that path until editor
	// restart, which is exactly the outcome the save branch's comment says it prevents.
	//
	// Renaming into the transient package under a unique name is what actually frees the
	// name; MarkAsGarbage alone never did. Same reasoning, and the same collision hazard,
	// as ClaireonAssetUtils::EvictInMemoryObject -- keep the unique name, or two failed
	// creates of the same path in one session collide in the transient package and
	// UObject::Rename treats that as fatal.
	void DataAssetCreate_RetireHalfBuiltAsset(UObject* NewAsset)
	{
		if (!IsValid(NewAsset))
		{
			return;
		}
		NewAsset->ClearFlags(RF_Standalone | RF_Public);
		const FName Retired = MakeUniqueObjectName(
			GetTransientPackage(), NewAsset->GetClass(), NewAsset->GetFName());
		NewAsset->Rename(*Retired.ToString(), GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		NewAsset->MarkAsGarbage();
	}

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
using namespace ClaireonDataAssetTool_Create_Private;

FString FClaireonDataAssetTool_Create::GetCategory() const { return TEXT("data_asset"); }
FString FClaireonDataAssetTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonDataAssetTool_Create::GetDescription() const
{
	return TEXT("Create a UDataAsset / UPrimaryDataAsset subclass instance at a /Game/ path and optionally seed "
				"properties from a dot-path map. class_path accepts /Script/Module.ClassName or a bare class "
				"name. TSoftObjectPtr<T> fields are settable directly (string paths parse into "
				"FSoftObjectPath). Non-session and immediate: creates and saves in one call, opening no "
				"editing session.");
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

	if (UObject* Existing = LoadObject<UObject>(nullptr, *Canon); IsValid(Existing))
	{
		// IsValid() check: the failure branches below MarkAsGarbage a
		// partially-created asset, but with pending-kill disabled (UE5 default)
		// StaticFindObject still returns garbage-flagged objects until the next
		// GC. A dead leftover must not wedge the path as 'Asset already exists';
		// NewObject below replaces it in place (standard StaticAllocateObject
		// behavior for same-name allocation).
		if (IsValid(Existing))
		{
			return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s"), *Canon));
		}
	}

	UClass* ResolvedClass = DataAssetCreate_ResolveClass(ClassPath);
	if (!IsValid(ResolvedClass))
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
	if (!IsValid(Package))
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("CreatePackage failed"));
	}

	UObject* NewAsset = NewObject<UObject>(Package, ResolvedClass, *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!IsValid(NewAsset))
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
			DataAssetCreate_RetireHalfBuiltAsset(NewAsset);
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

				// Failure cleanup: cancel the transaction and retire the half-built asset so
				// it is genuinely gone from the path, not merely flagged.
				Transaction.Cancel();
				DataAssetCreate_RetireHalfBuiltAsset(NewAsset);

				return MakeErrorResult(FullError);
			}
			WrittenPaths.Add(PropertyPath);
		}
	}

	FString SaveError;
	if (!ClaireonAssetUtils::SaveAsset(NewAsset, SaveError))
	{
		// Failure cleanup: identical to the earlier failure branches. Without
		// this, the AssetCreated registration above leaks an in-memory asset
		// with nothing on disk, and a retry's LoadObject pre-check reports
		// 'Asset already exists' -- wedging the path until editor restart.
		Transaction.Cancel();
		DataAssetCreate_RetireHalfBuiltAsset(NewAsset);

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
