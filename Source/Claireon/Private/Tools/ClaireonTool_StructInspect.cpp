// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StructInspect.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonStructReflection.h"

#include "Dom/JsonObject.h"
#include "UObject/Class.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_StructInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_StructInspect::GetDescription() const
{
	return TEXT("Inspect a USTRUCT's complete field schema. Accepts native paths "
		"(/Script/ModuleName.FStructName) and Blueprint user-defined struct asset paths "
		"(/Game/Path/To/Asset.AssetName — also accepts bare /Game/Path/To/Asset). "
		"Returns fields with name, friendly_name, cpp_type, kind, flags, metadata, and optional default_value. "
		"Use this to diff struct shapes before data migrations.");
}

TSharedPtr<FJsonObject> ClaireonTool_StructInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("struct_path"), TEXT("Path to the USTRUCT (native /Script/... or BP /Game/... asset path)"), true);
	S.AddBoolean(TEXT("include_defaults"), TEXT("If true (default), instantiate a temp struct and report each field's default value"));
	S.AddBoolean(TEXT("include_metadata"), TEXT("If true (default), include per-field UPROPERTY metadata (Category, ToolTip, UIMin, ...)"));
	return S.Build();
}

FToolResult ClaireonTool_StructInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString StructPath;
	if (!Arguments->TryGetStringField(TEXT("struct_path"), StructPath) || StructPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: struct_path"));
	}

	bool bIncludeDefaults = true;
	Arguments->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

	bool bIncludeMetadata = true;
	Arguments->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);

	FString Error;
	UScriptStruct* Struct = ClaireonStructReflection::ResolveStructPath(StructPath, Error);
	if (!Struct)
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = ClaireonStructReflection::SerializeStructSchema(
		Struct, bIncludeDefaults, bIncludeMetadata);

	// Identify BP user-defined structs by class name (avoids header dep, see ClaireonStructReflection.cpp)
	const bool bIsBP = Struct->GetClass() && Struct->GetClass()->GetName() == TEXT("UserDefinedStruct");
	const FString Summary = FString::Printf(
		TEXT("struct_inspect: %s (%s) — %d field(s)"),
		*Struct->GetName(),
		bIsBP ? TEXT("Blueprint") : TEXT("Native"),
		Data.IsValid() ? static_cast<int32>(Data->GetNumberField(TEXT("field_count"))) : 0);

	return MakeSuccessResult(Data, Summary);
}
