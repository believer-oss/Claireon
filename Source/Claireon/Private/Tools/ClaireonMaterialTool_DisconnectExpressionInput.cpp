// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_DisconnectExpressionInput.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_DisconnectExpressionInput::GetOperation() const { return TEXT("disconnect_expression_input"); }

FString ClaireonMaterialTool_DisconnectExpressionInput::GetDescription() const
{
    return TEXT("Disconnect a named input on a target expression in the open material editing session. Session-mode tool: open via material_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_DisconnectExpressionInput::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("target_identifier"), TEXT("Target expression identifier."), true);
	Builder.AddString(TEXT("input_name"), TEXT("Name of the input pin to disconnect."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_DisconnectExpressionInput::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Identifier, InputName;
	if (!Arguments->TryGetStringField(TEXT("target_identifier"), Identifier) || Identifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("'disconnect_expression_input' requires 'target_identifier'"));
	}
	if (!Arguments->TryGetStringField(TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'disconnect_expression_input' requires 'input_name'"));
	}

	UMaterial* Material = Data->Material.Get();
	int32 OutIndex = INDEX_NONE;
	UMaterialExpression* Expr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, Identifier, OutIndex);
	if (!Expr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Expression not found: '%s'"), *Identifier));
	}

	FString DiscErr;
	if (!ClaireonMaterialHelpers::DisconnectExpressionInput(Material, Expr, InputName, DiscErr))
	{
		return MakeErrorResult(DiscErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: disconnected #%d.%s"), OutIndex, *InputName);
	return BuildStateResponse(SessionId, Data);
}
