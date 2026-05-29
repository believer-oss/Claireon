// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintNodeFactory.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Knot.h"
#include "K2Node_Select.h"
#include "K2Node_MakeArray.h"
#include "K2Node_SpawnActorFromClass.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"

// File-static helper. The name suffix '_BlueprintApplicator' is a discriminator
// to satisfy the v2-Linux non-unity collision rule for free functions across TUs.
// Anonymous namespace is intentionally avoided per the same rule (unity batching
// can collide anon-NS helpers across Module.X.<N>.cpp files).
static TSharedPtr<FJsonObject> NormalizeNodeSpecForFactory_BlueprintApplicator(
	const TSharedPtr<FJsonObject>& NodeObj,
	const FString& NodeType)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

	// 1. Copy all fields verbatim.
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : NodeObj->Values)
	{
		Out->SetField(Kv.Key, Kv.Value);
	}

	// 2. Rename legacy 'function' -> factory canonical 'function_name'.
	if (Out->HasField(TEXT("function")) && !Out->HasField(TEXT("function_name")))
	{
		FString FuncName;
		Out->TryGetStringField(TEXT("function"), FuncName);
		Out->SetStringField(TEXT("function_name"), FuncName);
		Out->RemoveField(TEXT("function"));
	}

	// 3. Map legacy K2Node_* class names to factory canonical short forms.
	//    Set 'node_type' on the output; the factory's alias resolver handles further normalization.
	FString CanonicalType = NodeType;
	if (CanonicalType == TEXT("K2Node_CallFunction"))           { CanonicalType = TEXT("CallFunction"); }
	else if (CanonicalType == TEXT("K2Node_VariableGet"))       { CanonicalType = TEXT("VariableGet"); }
	else if (CanonicalType == TEXT("K2Node_VariableSet"))       { CanonicalType = TEXT("VariableSet"); }
	else if (CanonicalType == TEXT("K2Node_IfThenElse"))        { CanonicalType = TEXT("Branch"); }
	else if (CanonicalType == TEXT("K2Node_ExecutionSequence")) { CanonicalType = TEXT("Sequence"); }
	else if (CanonicalType == TEXT("K2Node_CustomEvent"))       { CanonicalType = TEXT("CustomEvent"); }
	else if (CanonicalType == TEXT("K2Node_DynamicCast"))       { CanonicalType = TEXT("Cast"); }
	else if (CanonicalType == TEXT("K2Node_Knot"))              { CanonicalType = TEXT("Reroute"); }
	else if (CanonicalType == TEXT("K2Node_Select"))            { CanonicalType = TEXT("Select"); }
	else if (CanonicalType == TEXT("K2Node_MakeArray"))         { CanonicalType = TEXT("MakeArray"); }
	else if (CanonicalType == TEXT("K2Node_SpawnActorFromClass")) { CanonicalType = TEXT("SpawnActorFromClass"); }
	else if (CanonicalType == TEXT("EdGraphNode_Comment"))      { CanonicalType = TEXT("Comment"); }
	else if (CanonicalType == TEXT("K2Node_Delay"))
	{
		// Delay is rewritten as a KismetSystemLibrary::Delay CallFunction node.
		CanonicalType = TEXT("CallFunction");
		Out->SetStringField(TEXT("function_name"), TEXT("Delay"));
		Out->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
	}
	else if (CanonicalType.StartsWith(TEXT("K2Node_")))
	{
		// Strip the K2Node_ prefix; the factory's alias resolver handles the remainder.
		CanonicalType = CanonicalType.Mid(7);
	}
	Out->SetStringField(TEXT("node_type"), CanonicalType);

	return Out;
}

bool FClaireonSpecApplicator_Blueprint::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	bool bHasContent = false;

	// Check nodes array
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < NodesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& NodeVal = (*NodesArray)[i];
			if (!NodeVal.IsValid() || NodeVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();

			FString NodeId, NodeType;
			if (!NodeObj->TryGetStringField(TEXT("id"), NodeId) || NodeId.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'id'"), i));
			}
			if (!NodeObj->TryGetStringField(TEXT("type"), NodeType) || NodeType.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'type'"), i));
			}
		}
	}

	// Check connections array
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& ConnVal = (*ConnectionsArray)[i];
			if (!ConnVal.IsValid() || ConnVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();

			FString SourceNode, SourcePin, TargetNode, TargetPin;
			if (!ConnObj->TryGetStringField(TEXT("source_node"), SourceNode) || SourceNode.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'source_node'"), i));
			}
			if (!ConnObj->TryGetStringField(TEXT("source_pin"), SourcePin) || SourcePin.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'source_pin'"), i));
			}
			if (!ConnObj->TryGetStringField(TEXT("target_node"), TargetNode) || TargetNode.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'target_node'"), i));
			}
			if (!ConnObj->TryGetStringField(TEXT("target_pin"), TargetPin) || TargetPin.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("connections[%d]: missing 'target_pin'"), i));
			}
		}
	}

	// Check variables array
	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("variables"), VariablesArray) && VariablesArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < VariablesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& VarVal = (*VariablesArray)[i];
			if (!VarVal.IsValid() || VarVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& VarObj = VarVal->AsObject();

			FString VarName, VarType;
			if (!VarObj->TryGetStringField(TEXT("name"), VarName) || VarName.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("variables[%d]: missing 'name'"), i));
			}
			if (!VarObj->TryGetStringField(TEXT("type"), VarType) || VarType.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("variables[%d]: missing 'type'"), i));
			}
		}
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("Blueprint spec must contain at least one of: 'nodes', 'connections', 'variables'"));
		return false;
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_Blueprint::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	// Resolve path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Try to load existing Blueprint
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	if (!BP)
	{
		// Blueprint supports creation -- create a new one
		FString ValidationError;
		if (!ClaireonBlueprintHelpers::ValidateAssetPath(ResolvedPath, ValidationError))
		{
			OutError = ValidationError;
			return false;
		}

		// Resolve parent_class from the active spec (top-level field), defaulting to Actor.
		FString ParentClassName = TEXT("Actor");
		if (TSharedPtr<FJsonObject> Spec = GetActiveSpec())
		{
			Spec->TryGetStringField(TEXT("parent_class"), ParentClassName);
		}

		ClaireonNameResolver::FNameResolveResult ParentClassResult;
		UClass* ParentClass = ClaireonNameResolver::ResolveClassName(
			ParentClassName, nullptr, ParentClassResult);
		if (!ParentClass)
		{
			OutError = FString::Printf(TEXT("Failed to resolve parent_class '%s': %s"),
				*ParentClassName, *ParentClassResult.Error);
			return false;
		}
		if (!ParentClassResult.ResolutionNote.IsEmpty())
		{
			AddWarning(ParentClassResult.ResolutionNote);
		}

		ClaireonBlueprintHelpers::FCreateBlueprintResult CreateResult;
		ClaireonBlueprintHelpers::CreateBlueprint(ResolvedPath, ParentClass, CreateResult);
		if (!CreateResult.IsOk())
		{
			OutError = CreateResult.Error;
			return false;
		}
		for (const FString& W : CreateResult.Warnings)
		{
			AddWarning(W);
		}
		BP = CreateResult.Blueprint;
	}

	// Find graph
	FString GraphName = TEXT("EventGraph");
	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(BP, GraphName);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *ResolvedPath);
		return false;
	}

	// Open session
	const FString BPPathName = BP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		BPPathName, TEXT("bp"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *BPPathName);
		return false;
	}

	Blueprint = BP;
	ActiveGraph = Graph;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_Blueprint::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UBlueprint* BP = Blueprint.Get();
	UEdGraph* Graph = ActiveGraph.Get();
	if (!BP || !Graph)
	{
		AddError(TEXT("Blueprint or Graph is no longer valid"));
		return false;
	}

	// === VARIABLES (runs FIRST so VariableGet/Set nodes see FProperty by Pass 1.5) ===
	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("variables"), VariablesArray) && VariablesArray)
	{
		int32 VarSuccessCount = 0;
		for (int32 i = 0; i < VariablesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& VarObj = (*VariablesArray)[i]->AsObject();
			if (!VarObj.IsValid()) continue;

			FString VarId;
			VarObj->TryGetStringField(TEXT("id"), VarId);

			FString CreateError;
			if (!ClaireonBlueprintHelpers::CreateVariableFromSpec(BP, VarObj, /*OutResult=*/nullptr, CreateError))
			{
				RecordEntryFailure(VarId, CreateError);
				continue;
			}

			FString VarName;
			if (!VarObj->TryGetStringField(TEXT("variable_name"), VarName))
			{
				VarObj->TryGetStringField(TEXT("name"), VarName);
			}
			RegisterIdMapping(VarId, VarName);
			RecordEntrySuccess(VarId, VarName);
			VarSuccessCount++;
		}
		if (VarSuccessCount > 0)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:Blueprint] Pass 1: Created %d/%d variables"),
			VarSuccessCount, VariablesArray->Num());
	}

	// === NODES (runs SECOND, through factory) ===
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		int32 NodeSuccessCount = 0;
		for (int32 i = 0; i < NodesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& NodeObj = (*NodesArray)[i]->AsObject();
			if (!NodeObj.IsValid()) continue;

			FString SpecId, NodeType;
			NodeObj->TryGetStringField(TEXT("id"), SpecId);
			NodeObj->TryGetStringField(TEXT("type"), NodeType);

			// Parse legacy array-form position into FVector2D; the factory accepts position
			// via its FVector2D parameter rather than in the JSON params object.
			FVector2D Position(i * 300.0, 0.0);
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("position"), PosArray) && PosArray && PosArray->Num() >= 2)
			{
				Position.X = (*PosArray)[0]->AsNumber();
				Position.Y = (*PosArray)[1]->AsNumber();
			}

			TSharedPtr<FJsonObject> FactoryParams =
				NormalizeNodeSpecForFactory_BlueprintApplicator(NodeObj, NodeType);

			ClaireonBlueprintNodeFactory::FCreateResult R =
				ClaireonBlueprintNodeFactory::CreateNode(BP, Graph, FactoryParams, Position);

			if (!R.IsOk())
			{
				RecordEntryFailure(SpecId, R.Error);
				continue;
			}

			// Factory already added the node to Graph and called AllocateDefaultPins +
			// ReconstructNode (for typed branches that need it). Just record the IdMap entry.
			const FString GuidStr = R.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			RegisterIdMapping(SpecId, GuidStr);
			RecordEntrySuccess(SpecId, GuidStr);
			for (const FString& W : R.Warnings) { AddWarning(W); }
			NodeSuccessCount++;
		}
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:Blueprint] Pass 1: Created %d/%d nodes"),
			NodeSuccessCount, NodesArray->Num());
	}

	// === PASS 1.5: narrow safety-net ReconstructNode for VariableGet / VariableSet ===
	// The factory's typed-branch reconstruct list excludes these because AllocateDefaultPins
	// reads the FProperty directly. With vars-first (above), the FProperty exists by the
	// time the node was created. This walk is a safety net for future spec extensions that
	// may register a variable after a VariableGet is constructed.
	for (const TPair<FString, FString>& Pair : GetIdMappings())
	{
		const FString& GuidStr = Pair.Value;
		FGuid NodeGuid;
		if (!FGuid::Parse(GuidStr, NodeGuid)) continue;  // skip non-GUID entries (variables)
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
		if (!Node) continue;
		if (Node->IsA<UK2Node_VariableGet>() || Node->IsA<UK2Node_VariableSet>())
		{
			const int32 PrevX = Node->NodePosX;
			const int32 PrevY = Node->NodePosY;
			Node->ReconstructNode();
			Node->NodePosX = PrevX;
			Node->NodePosY = PrevY;
		}
	}

	return true;
}

bool FClaireonSpecApplicator_Blueprint::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UBlueprint* BP = Blueprint.Get();
	UEdGraph* Graph = ActiveGraph.Get();
	if (!BP || !Graph)
	{
		AddError(TEXT("Blueprint or Graph is no longer valid"));
		return false;
	}

	// --- Set pin defaults on nodes ---
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
		{
			const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
			if (!NodeObj.IsValid()) continue;

			FString SpecId;
			NodeObj->TryGetStringField(TEXT("id"), SpecId);

			if (!IsIdCreated(SpecId)) continue;

			const TSharedPtr<FJsonObject>* PinDefaultsPtr = nullptr;
			if (!NodeObj->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsPtr) || !PinDefaultsPtr || !(*PinDefaultsPtr).IsValid())
			{
				continue;
			}

			FString GuidStr = ResolveId(SpecId);
			FGuid NodeGuid;
			FGuid::Parse(GuidStr, NodeGuid);
			UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
			if (!Node)
			{
				AddWarning(FString::Printf(TEXT("Could not find node '%s' for pin_defaults"), *SpecId));
				continue;
			}

			for (const auto& Pair : (*PinDefaultsPtr)->Values)
			{
				FString PinValue;
				if (!Pair.Value->TryGetString(PinValue)) continue;

				UEdGraphPin* Pin = Node->FindPin(*Pair.Key);
				if (!Pin)
				{
					AddWarning(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *Pair.Key, *SpecId));
					continue;
				}

				K2Schema->TrySetDefaultValue(*Pin, PinValue);
			}
		}
	}

	// --- Wire connections ---
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray)
	{
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

		for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& ConnObj = (*ConnectionsArray)[i]->AsObject();
			if (!ConnObj.IsValid()) continue;

			FString SourceNodeId, SourcePinName, TargetNodeId, TargetPinName;
			ConnObj->TryGetStringField(TEXT("source_node"), SourceNodeId);
			ConnObj->TryGetStringField(TEXT("source_pin"), SourcePinName);
			ConnObj->TryGetStringField(TEXT("target_node"), TargetNodeId);
			ConnObj->TryGetStringField(TEXT("target_pin"), TargetPinName);

			// Resolve source node
			FString SourceGuidStr = ResolveId(SourceNodeId);
			if (SourceGuidStr.IsEmpty())
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: source_node '%s' not found in ID mappings"), i, *SourceNodeId));
				continue;
			}

			FString TargetGuidStr = ResolveId(TargetNodeId);
			if (TargetGuidStr.IsEmpty())
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: target_node '%s' not found in ID mappings"), i, *TargetNodeId));
				continue;
			}

			FGuid SourceGuid, TargetGuid;
			FGuid::Parse(SourceGuidStr, SourceGuid);
			FGuid::Parse(TargetGuidStr, TargetGuid);

			UEdGraphNode* SourceNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, SourceGuid);
			UEdGraphNode* TargetNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, TargetGuid);

			if (!SourceNode || !TargetNode)
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: could not find source or target node in graph"), i));
				continue;
			}

			ClaireonNameResolver::FNameResolveResult SourcePinResult;
			UEdGraphPin* SourcePin = ClaireonNameResolver::ResolvePinName(SourceNode, SourcePinName, EGPD_MAX, SourcePinResult);
			if (!SourcePin)
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: source pin '%s' not found on node '%s'"),
					i, *SourcePinName, *SourceNodeId));
				continue;
			}

			ClaireonNameResolver::FNameResolveResult TargetPinResult;
			UEdGraphPin* TargetPin = ClaireonNameResolver::ResolvePinName(TargetNode, TargetPinName, EGPD_MAX, TargetPinResult);
			if (!TargetPin)
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: target pin '%s' not found on node '%s'"),
					i, *TargetPinName, *TargetNodeId));
				continue;
			}

			FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				AddWarning(FString::Printf(TEXT("connections[%d]: cannot connect: %s"),
					i, *Response.Message.ToString()));
				continue;
			}

			if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A)
			{
				SourcePin->BreakAllPinLinks();
			}
			if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)
			{
				TargetPin->BreakAllPinLinks();
			}
			if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
			{
				SourcePin->BreakAllPinLinks();
				TargetPin->BreakAllPinLinks();
			}

			SourcePin->MakeLinkTo(TargetPin);
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}

	return true;
}

bool FClaireonSpecApplicator_Blueprint::CompileAsset(const FString& SessionId, FString& OutError)
{
	UBlueprint* BP = Blueprint.Get();
	if (!BP)
	{
		OutError = TEXT("Blueprint is no longer valid");
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);

	if (BP->Status == BS_Error)
	{
		OutError = TEXT("Blueprint compilation failed with errors");
		return false;
	}

	return true;
}

bool FClaireonSpecApplicator_Blueprint::SaveAsset(const FString& SessionId, FString& OutError)
{
	UBlueprint* BP = Blueprint.Get();
	if (!BP)
	{
		OutError = TEXT("Blueprint is no longer valid");
		return false;
	}

	UPackage* Package = BP->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("Failed to get package for Blueprint");
		return false;
	}

	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash");
		return false;
	}

	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;

	if (!UPackage::SavePackage(Package, BP, *PackageFileName, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save Blueprint to %s"), *PackageFileName);
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_Blueprint::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
