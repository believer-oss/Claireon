// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "PCGGraph.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonPCGGraphTool_Save::GetDescription() const
{
	return TEXT("Save the PCG Graph asset to disk.");
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
	if (!Package)
	{
		return MakeErrorResult(TEXT("Could not find package for PCG Graph"));
	}

	FString PackageFilename;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
	{
		return MakeErrorResult(FString::Printf(TEXT("Package file not found for: %s"), *Package->GetName()));
	}

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);

	Data->LastOperationStatus = bSaved ? TEXT("Saved successfully") : TEXT("Save failed");
	return BuildStateResponse(SessionId, Data);
}
