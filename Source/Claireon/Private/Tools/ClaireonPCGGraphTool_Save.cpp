// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Save.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "UObject/Package.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonPCGGraphTool_Save::GetDescription() const
{
	return TEXT("Save the PCG Graph asset to disk within the current session and clear the dirty "
				"flag. Requires session_id from pcg_graph.open; the session stays open so further "
				"transactional edits (add/remove nodes, connect/disconnect pins, set node properties) "
				"remain available after the save.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid -- PCG Graph is no longer loaded"));
	}

	UPackage* Package = Data->PCGGraph->GetOutermost();
	if (!IsValid(Package))
	{
		return MakeErrorResult(TEXT("Could not find package for PCG Graph"));
	}

	// P2-15: route through ClaireonAssetUtils::SaveAsset, which resolves a save
	// filename for freshly created in-memory packages instead of erroring from
	// an un-backstopped DoesPackageExist (the anti-pattern data_asset_create was
	// cured of). SaveAsset also carries the post-crash guard. This is a write
	// path, so the status names exactly what was written (P0-7 discipline).
	FString SaveError;
	const bool bSaved = ClaireonAssetUtils::SaveAsset(Data->PCGGraph.Get(), SaveError);
	if (!bSaved)
	{
		return MakeErrorResult(SaveError);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Saved package %s to disk"), *Package->GetName());
	FToolResult Result = BuildStateResponse(SessionId, Data);

	// Saving alone does not invalidate compiled graphs. Offer refresh when live components use this graph.
	const int32 LiveComponents =
		ClaireonPCGGraphHelpers::CountLiveComponentsUsingGraph(Data->PCGGraph.Get());
	if (LiveComponents > 0)
	{
		TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
		HintArgs->SetStringField(TEXT("session_id"), SessionId);
		HintArgs->SetBoolField(TEXT("regenerate_components"), true);

		Result.AddHint(MakeGuidanceHint(
			TEXT("pcg_refresh"),
			FString::Printf(
				TEXT("%d placed PCGComponent(s) use this graph and will regenerate from the cached ")
				TEXT("compiled graph until it is evicted, producing output identical to before these ")
				TEXT("edits. pcg_refresh evicts it and regenerates them."),
				LiveComponents),
			HintArgs));
	}

	return Result;
}
