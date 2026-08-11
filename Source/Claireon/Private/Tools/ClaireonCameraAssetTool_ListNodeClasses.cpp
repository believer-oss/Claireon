// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_ListNodeClasses.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"
#include "UObject/Class.h"

#if WITH_GAMEPLAY_CAMERAS

FString FClaireonCameraAssetTool_ListNodeClasses::GetOperation() const { return TEXT("list_node_classes"); }

FString FClaireonCameraAssetTool_ListNodeClasses::GetDescription() const
{
	return TEXT("Enumerate the concrete UCameraNode subclasses accepted by camera_asset_add_node, skipping abstract "
		"and deprecated classes, and return class_name plus super_class for each. Read-only / non-session: "
		"takes no arguments, opens no session, and loads no asset.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_ListNodeClasses::GetInputSchema() const
{
	FToolSchemaBuilder S;
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_ListNodeClasses::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	const TArray<UClass*> Classes = ClaireonCameraAssetHelpers::EnumerateCameraNodeClasses();

	TArray<TSharedPtr<FJsonValue>> ClassesJson;
	ClassesJson.Reserve(Classes.Num());
	for (UClass* Cls : Classes)
	{
		if (!IsValid(Cls))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), Cls->GetName());
		Entry->SetStringField(TEXT("super_class"),
			IsValid(Cls->GetSuperClass()) ? Cls->GetSuperClass()->GetName() : FString());
		// EnumerateCameraNodeClasses already filters CLASS_Abstract; field kept
		// for forward-compat if the filter ever changes.
		Entry->SetBoolField(TEXT("is_abstract"), false);
		ClassesJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("classes"), ClassesJson);
	Data->SetNumberField(TEXT("count"), ClassesJson.Num());
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Listed %d concrete UCameraNode subclass(es)"), ClassesJson.Num()));
}

#endif // WITH_GAMEPLAY_CAMERAS
