// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SetBlueprintMetadata.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

FString ClaireonTool_SetBlueprintMetadata::GetOperation() const { return TEXT("set_metadata"); }

FString ClaireonTool_SetBlueprintMetadata::GetCategory() const
{
	return kBPCategory;
}

FString ClaireonTool_SetBlueprintMetadata::GetDescription() const
{
	return TEXT("Set a UBlueprint metadata property (namespace, display_name, description, "
		"category, hide_categories, is_abstract, is_const, is_deprecated, compile_mode). "
		"These are blueprint-level settings distinct from CDO properties. "
		"Reports needs_compile when the change affects the generated class. Immediate-mode tool: no session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_SetBlueprintMetadata::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path to the Blueprint (e.g. /Game/Path/BP_Thing)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> PropertyNameProp = MakeShared<FJsonObject>();
	PropertyNameProp->SetStringField(TEXT("type"), TEXT("string"));
	PropertyNameProp->SetStringField(TEXT("description"),
		TEXT("Metadata property to set"));
	TArray<TSharedPtr<FJsonValue>> EnumValues;
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("namespace")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("display_name")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("description")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("category")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("hide_categories")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("is_abstract")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("is_const")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("is_deprecated")));
	EnumValues.Add(MakeShared<FJsonValueString>(TEXT("compile_mode")));
	PropertyNameProp->SetArrayField(TEXT("enum"), EnumValues);
	Properties->SetObjectField(TEXT("property_name"), PropertyNameProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("string"));
	ValueProp->SetStringField(TEXT("description"),
		TEXT("New value. Booleans: 'true'/'false'. compile_mode: 'Default'/'Development'/'FinalRelease'. "
			"hide_categories: JSON array of strings."));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("property_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SetBlueprintMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// Resolve asset_path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Export old value and apply new value
	FString OldValue;
	bool bNeedsCompile = false;

	// Helper to parse boolean values
	auto ParseBool = [](const FString& Str, bool& OutValue) -> bool
	{
		if (Str.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			OutValue = true;
			return true;
		}
		if (Str.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			OutValue = false;
			return true;
		}
		return false;
	};

	// Begin transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Blueprint Metadata")));
	Blueprint->Modify();

	if (PropertyName == TEXT("namespace"))
	{
		OldValue = Blueprint->BlueprintNamespace;
		Blueprint->BlueprintNamespace = Value;
	}
	else if (PropertyName == TEXT("display_name"))
	{
		OldValue = Blueprint->BlueprintDisplayName;
		Blueprint->BlueprintDisplayName = Value;
	}
	else if (PropertyName == TEXT("description"))
	{
		OldValue = Blueprint->BlueprintDescription;
		Blueprint->BlueprintDescription = Value;
	}
	else if (PropertyName == TEXT("category"))
	{
		OldValue = Blueprint->BlueprintCategory;
		Blueprint->BlueprintCategory = Value;
	}
	else if (PropertyName == TEXT("hide_categories"))
	{
		// Export old value as JSON array
		TArray<TSharedPtr<FJsonValue>> OldArray;
		for (const FString& Cat : Blueprint->HideCategories)
		{
			OldArray.Add(MakeShared<FJsonValueString>(Cat));
		}
		FString OldArrayStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OldArrayStr);
		FJsonSerializer::Serialize(OldArray, Writer);
		OldValue = OldArrayStr;

		// Parse new value as JSON array
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
		TArray<TSharedPtr<FJsonValue>> NewArray;
		if (!FJsonSerializer::Deserialize(Reader, NewArray))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("hide_categories value must be a JSON array of strings. Got: %s"), *Value));
		}

		Blueprint->HideCategories.Empty();
		for (const TSharedPtr<FJsonValue>& Item : NewArray)
		{
			FString Str;
			if (!Item->TryGetString(Str))
			{
				return MakeErrorResult(TEXT("Each element in hide_categories must be a string"));
			}
			Blueprint->HideCategories.Add(Str);
		}
	}
	else if (PropertyName == TEXT("is_abstract"))
	{
		OldValue = Blueprint->bGenerateAbstractClass ? TEXT("true") : TEXT("false");
		bool bNewValue = false;
		if (!ParseBool(Value, bNewValue))
		{
			return MakeErrorResult(TEXT("is_abstract must be 'true' or 'false'"));
		}
		Blueprint->bGenerateAbstractClass = bNewValue ? 1 : 0;
		bNeedsCompile = true;
	}
	else if (PropertyName == TEXT("is_const"))
	{
		OldValue = Blueprint->bGenerateConstClass ? TEXT("true") : TEXT("false");
		bool bNewValue = false;
		if (!ParseBool(Value, bNewValue))
		{
			return MakeErrorResult(TEXT("is_const must be 'true' or 'false'"));
		}
		Blueprint->bGenerateConstClass = bNewValue ? 1 : 0;
		bNeedsCompile = true;
	}
	else if (PropertyName == TEXT("is_deprecated"))
	{
		OldValue = Blueprint->bDeprecate ? TEXT("true") : TEXT("false");
		bool bNewValue = false;
		if (!ParseBool(Value, bNewValue))
		{
			return MakeErrorResult(TEXT("is_deprecated must be 'true' or 'false'"));
		}
		Blueprint->bDeprecate = bNewValue ? 1 : 0;
		bNeedsCompile = true;
	}
	else if (PropertyName == TEXT("compile_mode"))
	{
		// Export old value
		if (const UEnum* CompileModeEnum = StaticEnum<EBlueprintCompileMode>())
		{
			OldValue = CompileModeEnum->GetNameStringByValue(
				static_cast<int64>(Blueprint->CompileMode));
		}

		// Validate and parse new value
		if (Value == TEXT("Default"))
		{
			Blueprint->CompileMode = EBlueprintCompileMode::Default;
		}
		else if (Value == TEXT("Development"))
		{
			Blueprint->CompileMode = EBlueprintCompileMode::Development;
		}
		else if (Value == TEXT("FinalRelease"))
		{
			Blueprint->CompileMode = EBlueprintCompileMode::FinalRelease;
		}
		else
		{
			return MakeErrorResult(FString::Printf(
				TEXT("compile_mode must be 'Default', 'Development', or 'FinalRelease'. Got: %s"),
				*Value));
		}
		bNeedsCompile = true;
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown property_name: '%s'. Valid names: namespace, display_name, "
				"description, category, hide_categories, is_abstract, is_const, "
				"is_deprecated, compile_mode"),
			*PropertyName));
	}

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("old_value"), OldValue);
	Data->SetStringField(TEXT("new_value"), Value);
	Data->SetBoolField(TEXT("needs_compile"), bNeedsCompile);

	FString Summary = FString::Printf(
		TEXT("Set %s.%s = '%s' (was '%s')%s"),
		*AssetPath, *PropertyName, *Value, *OldValue,
		bNeedsCompile ? TEXT(" [needs compile]") : TEXT(""));

	return MakeSuccessResult(Data, Summary);
}
