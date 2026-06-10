// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialListExpressions.h"
#include "Tools/ClaireonMaterialHelpers.h"

#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

FString ClaireonTool_MaterialListExpressions::GetCategory() const  { return TEXT("material"); }
FString ClaireonTool_MaterialListExpressions::GetOperation() const { return TEXT("list_expressions"); }

FString ClaireonTool_MaterialListExpressions::GetDescription() const
{
	// Companion to material_inspect that returns expressions as structured data
	// (one record per expression with index, class short-name, parameter name when
	// applicable, optional description). Suitable for programmatic iteration.
	return TEXT("Enumerate UMaterial expressions as structured data. "
				"Returns [{index, class, parameter_name?, description?}, ...]. "
				"Useful when you need to walk parameter expressions programmatically "
				"(rename_parameter, set_expression_property by name, etc.). "
				"Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialListExpressions::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the UMaterial. Alias: `path`."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> PathAliasProp = MakeShared<FJsonObject>();
	PathAliasProp->SetStringField(TEXT("type"), TEXT("string"));
	PathAliasProp->SetStringField(TEXT("description"), TEXT("Alias for asset_path."));
	Properties->SetObjectField(TEXT("path"), PathAliasProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	// asset_path or path required (runtime-checked).
	TArray<TSharedPtr<FJsonValue>> Required;
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MaterialListExpressions::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path (or path)"));
	}

	FString LoadError;
	UMaterial* Material = ClaireonMaterialHelpers::LoadMaterialAsset(AssetPath, LoadError);
	if (!Material)
	{
		return MakeErrorResult(LoadError);
	}

	TArray<TSharedPtr<FJsonValue>> Items;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
	for (int32 i = 0; i < Expressions.Num(); ++i)
	{
		UMaterialExpression* Expr = Expressions[i];
		if (!Expr)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), i);
		// Strip "MaterialExpression" prefix for the short class name (mirrors FindExpressionByIdentifier priority 4).
		FString ClassName = Expr->GetClass()->GetName();
		if (ClassName.StartsWith(TEXT("MaterialExpression")))
		{
			ClassName = ClassName.RightChop(18);
		}
		Entry->SetStringField(TEXT("class"), ClassName);
		if (Expr->HasAParameterName())
		{
			Entry->SetStringField(TEXT("parameter_name"), Expr->GetParameterName().ToString());
		}
		const FString Desc = Expr->GetDescription();
		if (!Desc.IsEmpty())
		{
			Entry->SetStringField(TEXT("description"), Desc);
		}
		Items.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("expressions"), Items);
	Data->SetNumberField(TEXT("count"), Items.Num());

	const FString Summary = FString::Printf(TEXT("%s: %d expression(s)"), *Material->GetName(), Items.Num());
	return MakeSuccessResult(Data, Summary);
}
