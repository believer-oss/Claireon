// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_Close::GetName() const
{
	return TEXT("claireon.behaviortree_close");
}

FString ClaireonBehaviorTreeTool_Close::GetDescription() const
{
	return TEXT("Close a Behavior Tree editing session. Optionally update_asset and/or save before closing.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("update_first"), TEXT("Rebuild the runtime BT from the graph before closing."));
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the BT asset before closing the session."));
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBehaviorTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bUpdateFirst = false;
	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("update_first"), bUpdateFirst);
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bUpdateFirst && Data->IsValid())
	{
		UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
		if (Graph)
		{
			Graph->UpdateAsset();
		}
	}

	if (bSaveFirst && Data->IsValid())
	{
		UBehaviorTree* BT = Data->BehaviorTree.Get();
		UPackage* Package = BT->GetPackage();
		Package->SetDirtyFlag(true);

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		if (!ClaireonSafeExec::DidLastExecutionCrash())
		{
			UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
