// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_SetDensity.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_SetDensity::GetOperation() const { return TEXT("set_density"); }

FString ClaireonFoliageTool_SetDensity::GetDescription() const
{
	return TEXT("Adjust instance density in a region. (Not yet implemented; use paint/erase.)");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_SetDensity::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("foliage_type"), TEXT("Name or asset path of the foliage type."));
	Builder.AddObject(TEXT("center"), TEXT("Region center {x, y} in world space."));
	Builder.AddNumber(TEXT("radius"), TEXT("Region radius (uu)."));
	Builder.AddNumber(TEXT("target_density"), TEXT("Target instances per unit area."));
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_SetDensity::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	// Density adjustment is a higher-level operation that combines erase and paint
	// For now, provide a stub that returns a meaningful error directing to paint/erase
	return MakeErrorResult(TEXT("set_density is not yet implemented. Use 'paint' to add and 'erase' to remove instances manually."));
}
