// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_PCGGraph.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

bool FClaireonSpecApplicator_PCGGraph::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	bool bHasContent = false;

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < NodesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*NodesArray)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString Id, Type;
			if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'id'"), i));
			if (!Obj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'type'"), i));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*ConnectionsArray)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();

			FString SN, SP, TN, TP;
			if (!Obj->TryGetStringField(TEXT("source_node"), SN) || SN.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'source_node'"), i));
			if (!Obj->TryGetStringField(TEXT("source_pin"), SP) || SP.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'source_pin'"), i));
			if (!Obj->TryGetStringField(TEXT("target_node"), TN) || TN.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'target_node'"), i));
			if (!Obj->TryGetStringField(TEXT("target_pin"), TP) || TP.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'target_pin'"), i));
		}
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("PCG spec must contain at least one of: 'nodes', 'connections'"));
		return false;
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_PCGGraph::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(ResolvedPath, OutError);
	if (!Graph)
	{
		return false;
	}

	const FString GraphPathName = Graph->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		GraphPathName, TEXT("claireon.pcg_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *GraphPathName);
		return false;
	}

	PCGGraph = Graph;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_PCGGraph::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UPCGGraph* Graph = PCGGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("PCG Graph is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
	{
		return true;
	}

	int32 SuccessCount = 0;

	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& NodeObj = (*NodesArray)[i]->AsObject();
		if (!NodeObj.IsValid()) continue;

		FString SpecId, NodeType;
		NodeObj->TryGetStringField(TEXT("id"), SpecId);
		NodeObj->TryGetStringField(TEXT("type"), NodeType);

		FString Error;
		UClass* SettingsClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(NodeType, Error);
		if (!SettingsClass)
		{
			RecordEntryFailure(SpecId, Error);
			continue;
		}

		UPCGSettings* DefaultSettings = nullptr;
		UPCGNode* NewNode = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
		if (!NewNode)
		{
			RecordEntryFailure(SpecId, FString::Printf(TEXT("Failed to add PCG node of type: %s"), *NodeType));
			continue;
		}

		// Set properties
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (NodeObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
		{
			for (const auto& Prop : (*PropsPtr)->Values)
			{
				FString PropValue;
				if (Prop.Value->TryGetString(PropValue))
				{
					FString PropError;
					if (!ClaireonPCGGraphHelpers::SetNodeProperty(NewNode, Prop.Key, PropValue, PropError))
					{
						AddWarning(FString::Printf(TEXT("Node '%s' property '%s': %s"), *SpecId, *Prop.Key, *PropError));
					}
				}
			}
		}

		int32 NodeIndex = Graph->GetNodes().IndexOfByKey(NewNode);
		FString IndexStr = FString::FromInt(NodeIndex);
		RegisterIdMapping(SpecId, IndexStr);
		RecordEntrySuccess(SpecId, IndexStr);
		SuccessCount++;
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Graph);

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:PCGGraph] Pass 1: Created %d/%d nodes"),
		SuccessCount, NodesArray->Num());

	return true;
}

bool FClaireonSpecApplicator_PCGGraph::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UPCGGraph* Graph = PCGGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("PCG Graph is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("connections"), ConnectionsArray) || !ConnectionsArray)
	{
		return true;
	}

	for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& ConnObj = (*ConnectionsArray)[i]->AsObject();
		if (!ConnObj.IsValid()) continue;

		FString SourceNodeId, SourcePinLabel, TargetNodeId, TargetPinLabel;
		ConnObj->TryGetStringField(TEXT("source_node"), SourceNodeId);
		ConnObj->TryGetStringField(TEXT("source_pin"), SourcePinLabel);
		ConnObj->TryGetStringField(TEXT("target_node"), TargetNodeId);
		ConnObj->TryGetStringField(TEXT("target_pin"), TargetPinLabel);

		// Resolve node IDs to indices
		FString SourceIndexStr = ResolveId(SourceNodeId);
		FString TargetIndexStr = ResolveId(TargetNodeId);

		if (SourceIndexStr.IsEmpty())
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: source_node '%s' not found"), i, *SourceNodeId));
			continue;
		}
		if (TargetIndexStr.IsEmpty())
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: target_node '%s' not found"), i, *TargetNodeId));
			continue;
		}

		int32 SourceIndex = FCString::Atoi(*SourceIndexStr);
		int32 TargetIndex = FCString::Atoi(*TargetIndexStr);

		// Find nodes by index -- use FindNodeByIdentifier for robustness
		int32 DummyIndex;
		UPCGNode* SourceNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Graph, SourceIndexStr, DummyIndex);
		UPCGNode* TargetNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Graph, TargetIndexStr, DummyIndex);

		if (!SourceNode)
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: source node index %d not found"), i, SourceIndex));
			continue;
		}
		if (!TargetNode)
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: target node index %d not found"), i, TargetIndex));
			continue;
		}

		// Verify pins
		UPCGPin* SourcePin = SourceNode->GetOutputPin(FName(*SourcePinLabel));
		if (!SourcePin)
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: output pin '%s' not found on source node"), i, *SourcePinLabel));
			continue;
		}

		UPCGPin* TargetPin = TargetNode->GetInputPin(FName(*TargetPinLabel));
		if (!TargetPin)
		{
			AddWarning(FString::Printf(TEXT("connections[%d]: input pin '%s' not found on target node"), i, *TargetPinLabel));
			continue;
		}

		Graph->AddEdge(SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Graph);

	return true;
}

bool FClaireonSpecApplicator_PCGGraph::CompileAsset(const FString& SessionId, FString& OutError)
{
	// PCG graphs do not have explicit compilation. NotifyGraphChanged was called in Pass 2.
	return true;
}

bool FClaireonSpecApplicator_PCGGraph::SaveAsset(const FString& SessionId, FString& OutError)
{
	UPCGGraph* Graph = PCGGraph.Get();
	if (!Graph)
	{
		OutError = TEXT("PCG Graph is no longer valid");
		return false;
	}

	UPackage* Package = Graph->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Could not find package for PCG Graph");
		return false;
	}

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash");
		return false;
	}

	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
	if (!bSaved)
	{
		OutError = TEXT("Failed to save PCG Graph package");
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_PCGGraph::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
