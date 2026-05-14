// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundClassTool_SetProperty.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundClass.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString SoundClassSetProperty_JsonValueToString(const TSharedPtr<FJsonValue>& V)
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
}

FString FClaireonSoundClassTool_SetProperty::GetCategory() const { return TEXT("soundclass"); }
FString FClaireonSoundClassTool_SetProperty::GetOperation() const { return TEXT("set_property"); }

FString FClaireonSoundClassTool_SetProperty::GetDescription() const
{
	return TEXT("Set a property on a USoundClass via reflection (e.g. property_path=\"Properties.Volume\"). "
				"Wraps in FScopedTransaction; saves on success.");
}

TSharedPtr<FJsonObject> FClaireonSoundClassTool_SetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundClass asset"), true);
	S.AddString(TEXT("property_path"), TEXT("Dot-path of the property (e.g. Properties.Volume). 'Properties.' prefix added if missing."), true);
	S.AddString(TEXT("value"), TEXT("Value to write."), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundClassTool_SetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString PropertyPath;
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		FString FieldName;
		if (!Arguments->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: property_path"));
		}
		PropertyPath = FString::Printf(TEXT("Properties.%s"), *FieldName);
	}
	else if (!PropertyPath.StartsWith(TEXT("Properties.")))
	{
		PropertyPath = FString::Printf(TEXT("Properties.%s"), *PropertyPath);
	}

	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}
	const FString ValueStr = SoundClassSetProperty_JsonValueToString(ValueJson);

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded)
	{
		return MakeErrorResult(Error);
	}
	USoundClass* SoundClass = Cast<USoundClass>(Loaded);
	if (!SoundClass || Kind != EClaireonAudioAssetKind::SoundClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundClass: %s"), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set SoundClass Property")));
	SoundClass->Modify();

	if (!ClaireonPropertyUtils::WritePropertyByPath(SoundClass, PropertyPath, ValueStr, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property not found or unwritable: %s (%s)"), *PropertyPath, *Error));
	}

	SoundClass->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(SoundClass, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), SoundClass->GetPathName());
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("value"), ValueStr);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s = %s"), *PropertyPath, *ValueStr));
}
