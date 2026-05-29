// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_ListNodeTypes.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_ListNodeTypes::GetOperation() const { return TEXT("list_node_types"); }

FString ClaireonPCGGraphTool_ListNodeTypes::GetDescription() const
{
	return TEXT("List available PCG settings classes that can be used with add_node by iterating "
				"UClass reflection. Read-only, stateless reflection query; no session_id required and "
				"no editor session is opened. Supports optional substring filtering to narrow the "
				"result set.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_ListNodeTypes::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("filter"), TEXT("Optional case-insensitive substring to filter class names."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_ListNodeTypes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Filter;
	Arguments->TryGetStringField(TEXT("filter"), Filter);

	TArray<FString> Classes = ClaireonPCGGraphHelpers::GetAvailableSettingsClasses();

	TArray<TSharedPtr<FJsonValue>> TypesArray;
	for (const FString& ClassName : Classes)
	{
		if (!Filter.IsEmpty() && !ClassName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		TypesArray.Add(MakeShared<FJsonValueString>(ClassName));
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetArrayField(TEXT("node_types"), TypesArray);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("%d available PCG node type(s)"), TypesArray.Num()));
}
