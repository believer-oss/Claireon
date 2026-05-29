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

FString ClaireonTool_MaterialInspect::GetCategory() const { return TEXT("material"); }
FString ClaireonTool_MaterialInspect::GetOperation() const { return TEXT("inspect"); }

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

	// asset_path - required (alias: `path`)
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the UMaterial (e.g. /Game/Art/Materials/M_Foo). Alias: `path`."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// path - alias for asset_path
	TSharedPtr<FJsonObject> PathAliasProp = MakeShared<FJsonObject>();
	PathAliasProp->SetStringField(TEXT("type"), TEXT("string"));
	PathAliasProp->SetStringField(TEXT("description"), TEXT("Alias for asset_path. Use either; asset_path wins if both present."));
	Properties->SetObjectField(TEXT("path"), PathAliasProp);

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

	// asset_path OR path is required; we validate at runtime since JSON Schema doesn't
	// express "one of A/B required" cleanly. The runtime check in Execute() returns a clear
	// error message if neither is provided.
	TArray<TSharedPtr<FJsonValue>> Required;
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_MaterialInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	// Accept either `asset_path` or `path` as alias; most other claireon.* tools accept
	// `path`, and prior versions of this tool diverged from that convention.
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		if (!Arguments->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required field: asset_path (or path)"));
		}
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
