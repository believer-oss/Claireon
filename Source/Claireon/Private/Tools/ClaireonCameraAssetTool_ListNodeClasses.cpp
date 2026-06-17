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
	return TEXT("Enumerate concrete UCameraNode subclasses (filters CLASS_Abstract / CLASS_Deprecated) for caller discovery.");
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
		if (!Cls)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), Cls->GetName());
		Entry->SetStringField(TEXT("super_class"),
			Cls->GetSuperClass() ? Cls->GetSuperClass()->GetName() : FString());
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
