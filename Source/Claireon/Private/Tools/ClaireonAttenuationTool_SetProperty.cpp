// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAttenuationTool_SetProperty.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundAttenuation.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// discriminator-prefixed file-local helper to avoid unity collisions with similarly-named
	// helpers across cohort files (e.g. ClaireonConcurrencyTool_SetProperty.cpp uses the same shape).
	FString AttenuationSetProperty_JsonValueToString(const TSharedPtr<FJsonValue>& V)
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

FString FClaireonAttenuationTool_SetProperty::GetCategory() const { return TEXT("attenuation"); }
FString FClaireonAttenuationTool_SetProperty::GetOperation() const { return TEXT("set_property"); }

FString FClaireonAttenuationTool_SetProperty::GetDescription() const
{
	return TEXT("Set a property on a USoundAttenuation asset by reflection (e.g. "
				"property_path=\"Attenuation.FalloffDistance\", value=\"1500.0\"). Non-session, "
				"immediate operation; no session_id is needed. The 'Attenuation.' prefix is added "
				"automatically if missing. Wraps the write in FScopedTransaction and saves on success.");
}

TSharedPtr<FJsonObject> FClaireonAttenuationTool_SetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundAttenuation asset"), true);
	S.AddString(TEXT("property_path"), TEXT("Dot-path of the property to set (e.g. Attenuation.FalloffDistance). 'Attenuation.' prefix is added automatically if missing."), true);
	S.AddString(TEXT("value"), TEXT("Value to write. Enums accept the name (e.g. 'Sphere'). Numbers and booleans accepted as JSON literals."), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonAttenuationTool_SetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		// Backward-compat: also accept field_name (matches bundled tool param shape).
		FString FieldName;
		if (!Arguments->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: property_path"));
		}
		PropertyPath = FString::Printf(TEXT("Attenuation.%s"), *FieldName);
	}
	else if (!PropertyPath.StartsWith(TEXT("Attenuation.")))
	{
		PropertyPath = FString::Printf(TEXT("Attenuation.%s"), *PropertyPath);
	}

	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}
	const FString ValueStr = AttenuationSetProperty_JsonValueToString(ValueJson);

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded)
	{
		return MakeErrorResult(Error);
	}
	USoundAttenuation* Att = Cast<USoundAttenuation>(Loaded);
	if (!Att || Kind != EClaireonAudioAssetKind::Attenuation)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundAttenuation: %s"), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Attenuation Property")));
	Att->Modify();

	if (!ClaireonPropertyUtils::WritePropertyByPath(Att, PropertyPath, ValueStr, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property not found or unwritable: %s (%s)"), *PropertyPath, *Error));
	}

	Att->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Att, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Att->GetPathName());
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("value"), ValueStr);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s = %s"), *PropertyPath, *ValueStr));
}
