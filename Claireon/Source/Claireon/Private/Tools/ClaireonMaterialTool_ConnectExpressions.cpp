// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ConnectExpressions.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_ConnectExpressions::GetOperation() const { return TEXT("connect_expressions"); }

FString ClaireonMaterialTool_ConnectExpressions::GetDescription() const
{
	return TEXT("Wire an output of one expression to an input of another expression in the material graph.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_ConnectExpressions::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("from_identifier"), TEXT("Source expression identifier."), true);
	Builder.AddString(TEXT("to_identifier"), TEXT("Destination expression identifier."), true);
	Builder.AddString(TEXT("from_output"), TEXT("Optional name of source output pin (default first)."));
	Builder.AddString(TEXT("to_input"), TEXT("Optional name of destination input pin (default first)."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_ConnectExpressions::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString FromIdent, FromOutput, ToIdent, ToInput;
	if (!Arguments->TryGetStringField(TEXT("from_identifier"), FromIdent) || FromIdent.IsEmpty())
	{
		return MakeErrorResult(TEXT("'connect_expressions' requires 'from_identifier'"));
	}
	if (!Arguments->TryGetStringField(TEXT("to_identifier"), ToIdent) || ToIdent.IsEmpty())
	{
		return MakeErrorResult(TEXT("'connect_expressions' requires 'to_identifier'"));
	}
	Arguments->TryGetStringField(TEXT("from_output"), FromOutput);
	Arguments->TryGetStringField(TEXT("to_input"), ToInput);

	UMaterial* Material = Data->Material.Get();
	int32 FromIdx = INDEX_NONE, ToIdx = INDEX_NONE;
	UMaterialExpression* FromExpr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, FromIdent, FromIdx);
	UMaterialExpression* ToExpr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, ToIdent, ToIdx);
	if (!FromExpr) return MakeErrorResult(FString::Printf(TEXT("from_identifier not found: '%s'"), *FromIdent));
	if (!ToExpr)   return MakeErrorResult(FString::Printf(TEXT("to_identifier not found: '%s'"), *ToIdent));

	FString ConnErr;
	if (!ClaireonMaterialHelpers::ConnectExpressions(Material, FromExpr, FromOutput, ToExpr, ToInput, ConnErr))
	{
		return MakeErrorResult(ConnErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: connected #%d.%s -> #%d.%s"),
		FromIdx, *FromOutput, ToIdx, *ToInput);
	return BuildStateResponse(SessionId, Data);
}
