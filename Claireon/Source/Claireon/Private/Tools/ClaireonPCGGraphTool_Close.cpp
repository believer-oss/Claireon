// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "PCGGraph.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonPCGGraphTool_Close::GetDescription() const
{
	return TEXT("Close a PCG Graph editing session. Optionally save the graph before closing.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save"), TEXT("Save the PCG graph before closing the session."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSave = false;
	Arguments->TryGetBoolField(TEXT("save"), bSave);

	if (bSave && Data->IsValid())
	{
		UPackage* Package = Data->PCGGraph->GetOutermost();
		if (Package)
		{
			FString PackageFilename;
			if (FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
			{
				if (!ClaireonSafeExec::DidLastExecutionCrash())
				{
					FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Save PCG Graph")));
					UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
				}
			}
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetBoolField(TEXT("saved"), bSave);

	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s closed%s"), *SessionId, bSave ? TEXT(" (saved)") : TEXT("")));
}
