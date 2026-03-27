// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SetBlueprintCDOProperty.h"
#include "ClaireonLog.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString ClaireonTool_SetBlueprintCDOProperty::GetName() const
{
	return TEXT("set_blueprint_cdo_property");
}

FString ClaireonTool_SetBlueprintCDOProperty::GetCategory() const
{
	return TEXT("blueprint");
}

FString ClaireonTool_SetBlueprintCDOProperty::GetDescription() const
{
	return TEXT("Set a property on a Blueprint's Class Default Object (CDO) by asset path. "
		"Supports all property types via ImportText serialization including TSoftClassPtr. "
		"Sessionless alternative to edit_blueprint_graph for simple property changes. "
		"Does not support array indexing or component properties.");
}

TSharedPtr<FJsonObject> ClaireonTool_SetBlueprintCDOProperty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Full Unreal asset path to the Blueprint (e.g. /Game/Path/To/BP_MyBlueprint)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> PropertyNameProp = MakeShared<FJsonObject>();
	PropertyNameProp->SetStringField(TEXT("type"), TEXT("string"));
	PropertyNameProp->SetStringField(TEXT("description"),
		TEXT("Name of the property to set on the CDO"));
	Properties->SetObjectField(TEXT("property_name"), PropertyNameProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("string"));
	ValueProp->SetStringField(TEXT("description"),
		TEXT("New value as a string (passed to ImportText_Direct)"));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	TSharedPtr<FJsonObject> PropertyPathProp = MakeShared<FJsonObject>();
	PropertyPathProp->SetStringField(TEXT("type"), TEXT("string"));
	PropertyPathProp->SetStringField(TEXT("description"),
		TEXT("Optional dot-separated path for nested struct properties"));
	Properties->SetObjectField(TEXT("property_path"), PropertyPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("property_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SetBlueprintCDOProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Extract required parameters
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	// Extract optional property_path
	FString PropertyPath;
	Arguments->TryGetStringField(TEXT("property_path"), PropertyPath);

	// Step 1: Validate asset_path
	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Step 2: Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Step 3: Validate GeneratedClass
	if (!Blueprint->GeneratedClass)
	{
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass -- compile it first"));
	}

	// Step 4: Get CDO
	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
	}

	// Step 5: Resolve property
	FProperty* Property = nullptr;
	void* Container = nullptr;

	if (!PropertyPath.IsEmpty())
	{
		// Dot-path struct walk
		TArray<FString> PathParts;
		PropertyPath.ParseIntoArray(PathParts, TEXT("."));

		// Append the property_name as the final segment
		PathParts.Add(PropertyName);

		void* CurrentContainer = CDO;
		UStruct* CurrentStruct = CDO->GetClass();

		for (int32 i = 0; i < PathParts.Num(); i++)
		{
			FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*PathParts[i]));
			if (!Prop)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Property '%s' not found on '%s'"),
					*PathParts[i], *CurrentStruct->GetName()));
			}

			if (i == PathParts.Num() - 1)
			{
				// Final segment -- this is the target property
				Property = Prop;
				Container = CurrentContainer;
			}
			else
			{
				// Intermediate segment -- must be a struct property
				FStructProperty* StructProp = CastField<FStructProperty>(Prop);
				if (!StructProp)
				{
					return MakeErrorResult(FString::Printf(
						TEXT("Property '%s' is not a struct -- cannot walk further"), *PathParts[i]));
				}
				CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
				CurrentStruct = StructProp->Struct;
			}
		}
	}
	else
	{
		// Flat lookup on CDO class
		Property = CDO->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (!Property)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Property '%s' not found on CDO of class '%s'"),
				*PropertyName, *CDO->GetClass()->GetName()));
		}
		Container = CDO;
	}

	// Step 6: Export old value
	FString OldValue;
	Property->ExportTextItem_Direct(OldValue, Property->ContainerPtrToValuePtr<void>(Container), nullptr, nullptr, PPF_None);

	// Step 7: FScopedTransaction + Modify()
	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Blueprint CDO Property")));
	Blueprint->Modify();
	CDO->Modify();

	// Step 8: ImportText_Direct -- check return value for nullptr
	const TCHAR* Result = Property->ImportText_Direct(*Value, Property->ContainerPtrToValuePtr<void>(Container), CDO, PPF_None);
	if (Result == nullptr)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("ImportText_Direct failed for property '%s' with value '%s'. Check that the value format matches the property type."),
			*PropertyName, *Value));
	}

	// Step 9: Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Step 10: Return success JSON
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("old_value"), OldValue);
	Data->SetStringField(TEXT("new_value"), Value);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	FString Summary = FString::Printf(TEXT("Set %s.%s = '%s' (was '%s')"), *AssetPath, *PropertyName, *Value, *OldValue);
	return MakeSuccessResult(Data, Summary);
}
