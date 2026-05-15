// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ConnectToMaterialOutput.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_ConnectToMaterialOutput::GetName() const
{
	return TEXT("claireon.material_connect_to_material_output");
}

FString ClaireonMaterialTool_ConnectToMaterialOutput::GetDescription() const
{
	return TEXT("Wire an expression output to a root material attribute (e.g. BaseColor, EmissiveColor, Opacity).");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_ConnectToMaterialOutput::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("from_identifier"), TEXT("Source expression identifier."), true);
	Builder.AddString(TEXT("attribute"), TEXT("Root material attribute (BaseColor, EmissiveColor, Opacity, etc.)."), true);
	Builder.AddString(TEXT("from_output"), TEXT("Optional source output pin (default first)."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_ConnectToMaterialOutput::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString FromIdent, FromOutput, Attribute;
	if (!Arguments->TryGetStringField(TEXT("from_identifier"), FromIdent) || FromIdent.IsEmpty())
	{
		return MakeErrorResult(TEXT("'connect_to_material_output' requires 'from_identifier'"));
	}
	if (!Arguments->TryGetStringField(TEXT("attribute"), Attribute) || Attribute.IsEmpty())
	{
		return MakeErrorResult(TEXT("'connect_to_material_output' requires 'attribute'"));
	}
	Arguments->TryGetStringField(TEXT("from_output"), FromOutput);

	UMaterial* Material = Data->Material.Get();
	int32 FromIdx = INDEX_NONE;
	UMaterialExpression* FromExpr = ClaireonMaterialHelpers::FindExpressionByIdentifier(Material, FromIdent, FromIdx);
	if (!FromExpr)
	{
		return MakeErrorResult(FString::Printf(TEXT("from_identifier not found: '%s'"), *FromIdent));
	}

	FString AttrErr;
	if (!ClaireonMaterialHelpers::ConnectToMaterialAttribute(Material, FromExpr, Attribute, FromOutput, AttrErr))
	{
		return MakeErrorResult(AttrErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: connected #%d.%s -> %s"),
		FromIdx, *FromOutput, *Attribute);
	return BuildStateResponse(SessionId, Data);
}
