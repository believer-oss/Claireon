// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_CursorBack.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_CursorBack::GetOperation() const { return TEXT("cursor_back"); }

FString ClaireonPCGGraphTool_CursorBack::GetDescription() const
{
    return TEXT("Move the session cursor back to the previous node by popping the cursor stack. Session-mode tool: open via pcg_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_CursorBack::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_CursorBack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (Data->CursorHistory.Num() == 0)
	{
		Data->LastOperationStatus = TEXT("No cursor history");
		return BuildStateResponse(SessionId, Data);
	}

	Data->FocusedNodeIndex = Data->CursorHistory.Pop();
	Data->LastOperationStatus = FString::Printf(TEXT("Cursor back to [%d]"), Data->FocusedNodeIndex);

	return BuildStateResponse(SessionId, Data);
}
