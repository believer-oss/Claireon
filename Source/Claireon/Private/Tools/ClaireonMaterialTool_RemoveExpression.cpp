// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_RemoveExpression.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialEditingLibrary.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_RemoveExpression::GetOperation() const { return TEXT("remove_expression"); }

FString ClaireonMaterialTool_RemoveExpression::GetDescription() const
{
    return TEXT("Remove an expression from the material graph by identifier (name or '#index'). Session-mode tool: open via material_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_RemoveExpression::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("expression_identifier"), TEXT("Expression identifier (parameter name or '#index')."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_RemoveExpression::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Identifier;
	if (!Arguments->TryGetStringField(TEXT("expression_identifier"), Identifier) || Identifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("'remove_expression' requires 'expression_identifier'"));
	}

	UMaterial* Material = Data->Material.Get();
	int32 OutIndex = INDEX_NONE;
	UMaterialExpression* Expr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, Identifier, OutIndex);
	if (!Expr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Expression not found: '%s'"), *Identifier));
	}

	const FString ClassName = Expr->GetClass()->GetName();
	UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expr);

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: removed expression #%d (%s)"), OutIndex, *ClassName);
	return BuildStateResponse(SessionId, Data);
}
