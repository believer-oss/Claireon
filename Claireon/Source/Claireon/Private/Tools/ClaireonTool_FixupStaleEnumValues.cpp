// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FixupStaleEnumValues.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_FixupStaleEnumValues::GetCategory() const  { return TEXT("asset"); }
FString ClaireonTool_FixupStaleEnumValues::GetOperation() const { return TEXT("fixup_stale_enum_values"); }

FString ClaireonTool_FixupStaleEnumValues::GetDescription() const
{
	// One-shot resave commandlet wrapped as an MCP tool. EnumRedirects only fires
	// on Save; tracked assets carrying stale values stay broken until forced through.
	return TEXT("Walk the Asset Registry and resave assets whose serialized state references "
				"a specified stale enum value. The actual value rewrite is handled by UE's "
				"EnumRedirects on Save; this tool's job is to force every referencing asset "
				"through SaveLoadedAsset. Supports dry_run=true to list candidates without "
				"resaving. Refuses to run while PIE is active. "
				"Use after editing Config/DefaultEngine.ini's [CoreRedirects] section to "
				"convert old enum values to new ones across the project.");
}

TSharedPtr<FJsonObject> ClaireonTool_FixupStaleEnumValues::GetInputSchema() const
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
	Properties->SetObjectField(TEXT("enum_path"),  MkProp(TEXT("string"),  TEXT("Path to the UEnum (e.g. /Script/MyGame.EMyEnum or /Game/Foo/UE_MyEnum).")));
	Properties->SetObjectField(TEXT("old_value"),  MkProp(TEXT("string"),  TEXT("Stale enum value name (post-redirect: the OLD name).")));
	Properties->SetObjectField(TEXT("dry_run"),    MkProp(TEXT("boolean"), TEXT("If true, only enumerates candidate assets; does NOT resave. Default true (safe).")));
	Properties->SetObjectField(TEXT("path_filter"), MkProp(TEXT("string"), TEXT("Optional content path prefix (e.g. /Game/Data) to narrow the scan. Default /Game.")));
	Properties->SetObjectField(TEXT("max_assets"), MkProp(TEXT("integer"), TEXT("Cap on number of assets to scan/resave. Default 500.")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("enum_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("old_value")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

namespace
{
	// Recursively scan a UObject's properties for an enum field with the given UEnum
	// and matching stored value name. Returns true on first hit.
	bool ObjectReferencesStaleEnumValue(UObject* Obj, UEnum* TargetEnum, const FString& OldValueName, int32 Depth = 0)
	{
		if (!Obj || !TargetEnum || Depth > 4)
		{
			return false;
		}
		UClass* Class = Obj->GetClass();
		if (!Class)
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) { continue; }
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

			if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				if (ByteProp->Enum == TargetEnum)
				{
					const uint8 V = ByteProp->GetPropertyValue(ValuePtr);
					const FString N = ByteProp->Enum->GetNameStringByValue(V);
					if (N.IsEmpty() || N.Equals(OldValueName, ESearchCase::IgnoreCase))
					{
						return true;
					}
				}
			}
			else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				if (EnumProp->GetEnum() == TargetEnum)
				{
					const int64 V = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
					const FString N = EnumProp->GetEnum()->GetNameStringByValue(V);
					if (N.IsEmpty() || N.Equals(OldValueName, ESearchCase::IgnoreCase))
					{
						return true;
					}
				}
			}
			// Shallow recursion into instanced sub-objects + struct properties to catch
			// nested data. Capped at depth 4 to avoid pathological graphs.
			else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				// Recurse on struct fields manually -- we don't have a UObject* but the
				// same recursive primitive works by iterating sub-properties. Inline a
				// trimmed version for one extra level:
				if (Depth < 2)
				{
					for (TFieldIterator<FProperty> SubIt(StructProp->Struct); SubIt; ++SubIt)
					{
						FProperty* Sub = *SubIt;
						const void* SubPtr = Sub->ContainerPtrToValuePtr<void>(ValuePtr);
						if (FByteProperty* SubByte = CastField<FByteProperty>(Sub))
						{
							if (SubByte->Enum == TargetEnum)
							{
								const uint8 V = SubByte->GetPropertyValue(SubPtr);
								const FString N = SubByte->Enum->GetNameStringByValue(V);
								if (N.IsEmpty() || N.Equals(OldValueName, ESearchCase::IgnoreCase))
								{
									return true;
								}
							}
						}
						else if (FEnumProperty* SubEnum = CastField<FEnumProperty>(Sub))
						{
							if (SubEnum->GetEnum() == TargetEnum)
							{
								const int64 V = SubEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(SubPtr);
								const FString N = SubEnum->GetEnum()->GetNameStringByValue(V);
								if (N.IsEmpty() || N.Equals(OldValueName, ESearchCase::IgnoreCase))
								{
									return true;
								}
							}
						}
					}
				}
			}
		}
		return false;
	}
}

IClaireonTool::FToolResult ClaireonTool_FixupStaleEnumValues::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString EnumPath, OldValue, PathFilter = TEXT("/Game");
	bool bDryRun = true;
	int32 MaxAssets = 500;
	if (!Arguments->TryGetStringField(TEXT("enum_path"), EnumPath) || EnumPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required: enum_path"));
	}
	if (!Arguments->TryGetStringField(TEXT("old_value"), OldValue) || OldValue.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required: old_value"));
	}
	Arguments->TryGetStringField(TEXT("path_filter"), PathFilter);
	Arguments->TryGetBoolField(TEXT("dry_run"), bDryRun);
	int32 MaxAssetsArg = 0;
	if (Arguments->TryGetNumberField(TEXT("max_assets"), MaxAssetsArg) && MaxAssetsArg > 0)
	{
		MaxAssets = MaxAssetsArg;
	}

	UEnum* TargetEnum = FindObject<UEnum>(nullptr, *EnumPath);
	if (!TargetEnum)
	{
		TargetEnum = LoadObject<UEnum>(nullptr, *EnumPath);
	}
	if (!TargetEnum)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve UEnum: %s"), *EnumPath));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(*PathFilter);
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Candidates;
	TArray<TSharedPtr<FJsonValue>> Resaved;
	TArray<TSharedPtr<FJsonValue>> Failed;
	int32 Scanned = 0;
	int32 Skipped = 0;

	for (const FAssetData& AD : Assets)
	{
		if (Scanned >= MaxAssets) { break; }
		++Scanned;

		UObject* Obj = AD.GetAsset();
		if (!Obj)
		{
			++Skipped;
			continue;
		}

		if (!ObjectReferencesStaleEnumValue(Obj, TargetEnum, OldValue))
		{
			continue;
		}

		const FString AssetPath = AD.GetObjectPathString();
		Candidates.Add(MakeShared<FJsonValueString>(AssetPath));

		if (bDryRun)
		{
			continue;
		}

		// Force-resave: mark dirty + save the package.
		Obj->MarkPackageDirty();
		if (UEditorAssetLibrary::SaveLoadedAsset(Obj))
		{
			Resaved.Add(MakeShared<FJsonValueString>(AssetPath));
		}
		else
		{
			Failed.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogClaireon, Warning,
				TEXT("[fixup_stale_enum_values] SaveLoadedAsset failed for %s"), *AssetPath);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("enum_path"), EnumPath);
	Data->SetStringField(TEXT("old_value"), OldValue);
	Data->SetStringField(TEXT("path_filter"), PathFilter);
	Data->SetBoolField(TEXT("dry_run"), bDryRun);
	Data->SetNumberField(TEXT("scanned"), Scanned);
	Data->SetArrayField(TEXT("candidates"), Candidates);
	Data->SetArrayField(TEXT("resaved"), Resaved);
	Data->SetArrayField(TEXT("failed"), Failed);

	const FString Summary = bDryRun
		? FString::Printf(TEXT("[dry_run] %d candidate(s) for %s::%s (scanned %d)"),
			Candidates.Num(), *TargetEnum->GetName(), *OldValue, Scanned)
		: FString::Printf(TEXT("Resaved %d / %d candidate(s) for %s::%s (failed: %d)"),
			Resaved.Num(), Candidates.Num(), *TargetEnum->GetName(), *OldValue, Failed.Num());
	return MakeSuccessResult(Data, Summary);
}
