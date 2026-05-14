// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_AddExpression.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "MaterialEditingLibrary.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_AddExpression::GetOperation() const { return TEXT("add_expression"); }

FString ClaireonMaterialTool_AddExpression::GetDescription() const
{
	return TEXT("Add a UMaterialExpression to the material graph. Optionally sets parameter name, position, and initial properties.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_AddExpression::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("expression_class"), TEXT("Expression class name (e.g. 'ScalarParameter', 'Add')."), true);
	Builder.AddString(TEXT("expression_name"), TEXT("Optional parameter name (for parameter expressions)."));
	Builder.AddNumber(TEXT("x"), TEXT("Optional X position in graph (default auto-layout)."));
	Builder.AddNumber(TEXT("y"), TEXT("Optional Y position in graph (default auto-layout)."));
	Builder.AddObject(TEXT("initial_properties"), TEXT("Optional map of property name -> text value to apply immediately after creation."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_AddExpression::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ClassStr;
	if (!Arguments->TryGetStringField(TEXT("expression_class"), ClassStr) || ClassStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_expression' requires 'expression_class'"));
	}

	FString ResolveErr;
	UClass* ExprClass = ClaireonMaterialHelpers::ResolveExpressionClass(ClassStr, ResolveErr);
	if (!ExprClass)
	{
		return MakeErrorResult(ResolveErr);
	}

	UMaterial* Material = Data->Material.Get();
	const int32 PriorCount = Material->GetExpressions().Num();

	double X = -400.0;
	double Y = static_cast<double>(PriorCount) * 120.0;
	Arguments->TryGetNumberField(TEXT("x"), X);
	Arguments->TryGetNumberField(TEXT("y"), Y);

	UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
		Material, ExprClass, static_cast<int32>(X), static_cast<int32>(Y));
	if (!Expr)
	{
		return MakeErrorResult(FString::Printf(TEXT("CreateMaterialExpression returned null for class '%s'"), *ClassStr));
	}

	FString ExpressionName;
	if (Arguments->TryGetStringField(TEXT("expression_name"), ExpressionName) && !ExpressionName.IsEmpty())
	{
		if (UMaterialExpressionParameter* AsParam = Cast<UMaterialExpressionParameter>(Expr))
		{
			AsParam->Modify();
			AsParam->ParameterName = FName(*ExpressionName);
			FPropertyChangedEvent Event(nullptr);
			AsParam->PostEditChangeProperty(Event);
		}
	}

	const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("initial_properties"), PropsObjPtr) && PropsObjPtr && PropsObjPtr->IsValid())
	{
		for (const auto& Pair : (*PropsObjPtr)->Values)
		{
			if (!Pair.Value.IsValid()) continue;
			const FString TextValue = Pair.Value->AsString();
			FString PropErr;
			ClaireonMaterialHelpers::SetExpressionProperty(Material, Expr, Pair.Key, TextValue, PropErr);
		}
	}

	const int32 NewIndex = Material->GetExpressions().IndexOfByKey(Expr);
	Data->LastOperationStatus = FString::Printf(TEXT("Effect: added expression #%d (%s)"),
		NewIndex, *Expr->GetClass()->GetName());

	return BuildStateResponse(SessionId, Data);
}
