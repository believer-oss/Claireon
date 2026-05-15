// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_SetExpressionProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_SetExpressionProperty::GetName() const
{
	return TEXT("claireon.material_set_expression_property");
}

FString ClaireonMaterialTool_SetExpressionProperty::GetDescription() const
{
	return TEXT("Set a property by name on an expression, parsed from text value via UE reflection.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_SetExpressionProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("target_identifier"), TEXT("Target expression identifier."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name on the expression."), true);
	Builder.AddString(TEXT("text_value"), TEXT("Text-encoded value to parse and assign."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_SetExpressionProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Identifier, PropertyName, TextValue;
	if (!Arguments->TryGetStringField(TEXT("target_identifier"), Identifier) || Identifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_expression_property' requires 'target_identifier'"));
	}
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_expression_property' requires 'property_name'"));
	}
	if (!Arguments->TryGetStringField(TEXT("text_value"), TextValue))
	{
		return MakeErrorResult(TEXT("'set_expression_property' requires 'text_value'"));
	}

	UMaterial* Material = Data->Material.Get();
	int32 OutIndex = INDEX_NONE;
	UMaterialExpression* Expr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, Identifier, OutIndex);
	if (!Expr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Expression not found: '%s'"), *Identifier));
	}

	FString PropErr;
	if (!ClaireonMaterialHelpers::SetExpressionProperty(Material, Expr, PropertyName, TextValue, PropErr))
	{
		return MakeErrorResult(PropErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set #%d.%s = %s"), OutIndex, *PropertyName, *TextValue);
	return BuildStateResponse(SessionId, Data);
}
