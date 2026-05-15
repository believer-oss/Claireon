// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialInspect.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_MaterialInspect::GetName() const
{
	return TEXT("claireon.material_inspect");
}

FString ClaireonTool_MaterialInspect::GetDescription() const
{
	return TEXT("Read the structure of a UMaterial asset. "
				"Displays shading model, blend mode, material domain, usage flags, "
				"parameter table, expression list, and material attribute connections. "
				"Use detail='summary' for a compact overview or detail='full' to also "
				"include the per-expression dump with positions and connections.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the UMaterial (e.g. /Game/Art/Materials/M_Foo)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for header + parameters + attribute connections, 'full' adds the expression list (default: summary)."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_MaterialInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString DetailLevel = TEXT("summary");
	Arguments->TryGetStringField(TEXT("detail"), DetailLevel);

	FString LoadError;
	UMaterial* Material = ClaireonMaterialHelpers::LoadMaterialAsset(AssetPath, LoadError);
	if (!Material)
	{
		return MakeErrorResult(LoadError);
	}

	const FString Markdown = ClaireonMaterialHelpers::FormatMaterialStructure(Material, DetailLevel);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("detail"), DetailLevel);
	Data->SetStringField(TEXT("structure"), Markdown);

	const FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const int32 ExprCount = Material->GetExpressions().Num();
	const FString Summary = FString::Printf(TEXT("%s: %d expressions"), *AssetName, ExprCount);

	return MakeSuccessResult(Data, Summary);
}
