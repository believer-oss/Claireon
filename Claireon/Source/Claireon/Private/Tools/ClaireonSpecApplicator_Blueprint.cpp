// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonBlueprintHelpers.h"
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
#include "K2Node_Event.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_GetArrayItem.h"
#include "EdGraphNode_Comment.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"

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
	const TSharedPtr<FJsonObject>& Spec = GetActiveSpec();

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

		FString PackageName = ResolvedPath;
		FString AssetName;
		if (ResolvedPath.Contains(TEXT(".")))
		{
			ResolvedPath.Split(TEXT("."), &PackageName, &AssetName);
		}
		else
		{
			int32 LastSlash;
			if (PackageName.FindLastChar('/', LastSlash))
			{
				AssetName = PackageName.Mid(LastSlash + 1);
			}
			else
			{
				AssetName = TEXT("NewBlueprint");
			}
		}

		// Delete existing file if present
		FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (FPaths::FileExists(PackageFileName))
		{
			IFileManager::Get().Delete(*PackageFileName, false, true);
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
			return false;
		}

		// Resolve parent_class from spec if provided; otherwise default to Actor for
		// backward compatibility.
		UClass* ResolvedParentClass = AActor::StaticClass();
		if (Spec.IsValid())
		{
			FString ParentClassName;
			if (Spec->TryGetStringField(TEXT("parent_class"), ParentClassName) && !ParentClassName.IsEmpty())
			{
				ClaireonNameResolver::FNameResolveResult PR;
				UClass* PC = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, PR);
				if (PC)
				{
					ResolvedParentClass = PC;
				}
				else
				{
					OutError = FString::Printf(TEXT("Failed to resolve parent_class '%s': %s"), *ParentClassName, *PR.Error);
					return false;
				}
			}
		}

		BP = FKismetEditorUtilities::CreateBlueprint(
			ResolvedParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);

		if (!BP)
		{
			OutError = FString::Printf(TEXT("Failed to create Blueprint at %s"), *ResolvedPath);
			return false;
		}

		Package->SetIsExternallyReferenceable(true);
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BP);

		{
			FString AssertError;
			if (!ClaireonAssetUtils::AssertInnerNameMatchesPackage(BP, AssertError))
			{
				// bool-returning caller -- propagate via OutError + return false.
				// Cleanup mirrors the BP-create path: clear flags + garbage.
				BP->ClearFlags(RF_Public | RF_Standalone);
				BP->MarkAsGarbage();
				OutError = AssertError;
				return false;
			}
		}
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

	int32 SuccessCount = 0;

	// --- Create nodes ---
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		for (int32 i = 0; i < NodesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& NodeObj = (*NodesArray)[i]->AsObject();
			if (!NodeObj.IsValid()) continue;

			FString SpecId, NodeType;
			NodeObj->TryGetStringField(TEXT("id"), SpecId);
			NodeObj->TryGetStringField(TEXT("type"), NodeType);

			// Parse optional position
			FVector2D Position(i * 300.0, 0.0);
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("position"), PosArray) && PosArray && PosArray->Num() >= 2)
			{
				Position.X = (*PosArray)[0]->AsNumber();
				Position.Y = (*PosArray)[1]->AsNumber();
			}

			UEdGraphNode* NewNode = nullptr;

			if (NodeType == TEXT("K2Node_CallFunction"))
			{
				FString FunctionName;
				if (!NodeObj->TryGetStringField(TEXT("function"), FunctionName))
				{
					NodeObj->TryGetStringField(TEXT("function_name"), FunctionName);
				}
				FString FunctionClass;
				NodeObj->TryGetStringField(TEXT("function_class"), FunctionClass);

				UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
				if (!FunctionClass.IsEmpty())
				{
					UClass* OwnerClass = FindFirstObject<UClass>(*FunctionClass);
					if (OwnerClass)
					{
						CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
					}
					else
					{
						CallNode->FunctionReference.SetSelfMember(FName(*FunctionName));
					}
				}
				else
				{
					// For common functions like PrintString, try library classes
					UClass* LibClass = FindFirstObject<UClass>(TEXT("KismetSystemLibrary"));
					if (LibClass && LibClass->FindFunctionByName(FName(*FunctionName)))
					{
						CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), LibClass);
					}
					else
					{
						CallNode->FunctionReference.SetSelfMember(FName(*FunctionName));
					}
				}
				NewNode = CallNode;
			}
			else if (NodeType == TEXT("K2Node_VariableGet"))
			{
				FString VarName;
				NodeObj->TryGetStringField(TEXT("variable_name"), VarName);
				UK2Node_VariableGet* VarNode = NewObject<UK2Node_VariableGet>(Graph);
				VarNode->VariableReference.SetSelfMember(FName(*VarName));
				NewNode = VarNode;
			}
			else if (NodeType == TEXT("K2Node_VariableSet"))
			{
				FString VarName;
				NodeObj->TryGetStringField(TEXT("variable_name"), VarName);
				UK2Node_VariableSet* VarNode = NewObject<UK2Node_VariableSet>(Graph);
				VarNode->VariableReference.SetSelfMember(FName(*VarName));
				NewNode = VarNode;
			}
			else if (NodeType == TEXT("K2Node_IfThenElse") || NodeType == TEXT("Branch"))
			{
				NewNode = NewObject<UK2Node_IfThenElse>(Graph);
			}
			else if (NodeType == TEXT("K2Node_ExecutionSequence") || NodeType == TEXT("Sequence"))
			{
				NewNode = NewObject<UK2Node_ExecutionSequence>(Graph);
			}
			else if (NodeType == TEXT("K2Node_CustomEvent"))
			{
				FString EventName;
				NodeObj->TryGetStringField(TEXT("event_name"), EventName);
				UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
				EventNode->CustomFunctionName = FName(*EventName);
				NewNode = EventNode;
			}
			else if (NodeType == TEXT("K2Node_DynamicCast") || NodeType == TEXT("Cast"))
			{
				FString TargetClass;
				NodeObj->TryGetStringField(TEXT("target_class"), TargetClass);
				UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
				if (!TargetClass.IsEmpty())
				{
					UClass* CastClass = FindFirstObject<UClass>(*TargetClass);
					if (CastClass)
					{
						CastNode->TargetType = CastClass;
					}
				}
				NewNode = CastNode;
			}
			else if (NodeType == TEXT("K2Node_Knot") || NodeType == TEXT("Reroute"))
			{
				NewNode = NewObject<UK2Node_Knot>(Graph);
			}
			else if (NodeType == TEXT("K2Node_Select"))
			{
				NewNode = NewObject<UK2Node_Select>(Graph);
			}
			else if (NodeType == TEXT("K2Node_MakeArray"))
			{
				NewNode = NewObject<UK2Node_MakeArray>(Graph);
			}
			else if (NodeType == TEXT("K2Node_SpawnActorFromClass"))
			{
				NewNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
			}
			else if (NodeType == TEXT("EdGraphNode_Comment") || NodeType == TEXT("Comment"))
			{
				UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(Graph);
				FString CommentText;
				if (NodeObj->TryGetStringField(TEXT("comment_text"), CommentText))
				{
					CommentNode->NodeComment = CommentText;
				}
				NewNode = CommentNode;
			}
			else if (NodeType == TEXT("K2Node_Delay"))
			{
				// Delay is actually a CallFunction to Delay
				UK2Node_CallFunction* DelayNode = NewObject<UK2Node_CallFunction>(Graph);
				UClass* LibClass = FindFirstObject<UClass>(TEXT("KismetSystemLibrary"));
				if (LibClass)
				{
					DelayNode->FunctionReference.SetExternalMember(FName(TEXT("Delay")), LibClass);
				}
				NewNode = DelayNode;
			}
			else if (NodeType == TEXT("K2Node_Event"))
			{
				FString EventName, EventClass;
				NodeObj->TryGetStringField(TEXT("event_name"), EventName);
				NodeObj->TryGetStringField(TEXT("event_class"), EventClass);
				bool bOverride = false;
				NodeObj->TryGetBoolField(TEXT("override_function"), bOverride);

				UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
				EventNode->bOverrideFunction = bOverride;
				if (!EventClass.IsEmpty())
				{
					UClass* OwnerClass = FindFirstObject<UClass>(*EventClass, EFindFirstObjectOptions::NativeFirst);
					if (OwnerClass && !EventName.IsEmpty())
					{
						EventNode->EventReference.SetExternalMember(FName(*EventName), OwnerClass);
					}
				}
				NewNode = EventNode;
			}
			else if (NodeType == TEXT("K2Node_AsyncAction"))
			{
				FString ProxyClassName, ProxyFactoryFunc;
				NodeObj->TryGetStringField(TEXT("proxy_class"), ProxyClassName);
				if (!NodeObj->TryGetStringField(TEXT("proxy_factory_function"), ProxyFactoryFunc))
				{
					NodeObj->TryGetStringField(TEXT("function_name"), ProxyFactoryFunc);
				}

				UK2Node_AsyncAction* AsyncNode = NewObject<UK2Node_AsyncAction>(Graph);
				if (!ProxyClassName.IsEmpty())
				{
					UClass* ProxyClass = FindFirstObject<UClass>(*ProxyClassName, EFindFirstObjectOptions::NativeFirst);
					if (ProxyClass)
					{
						// UK2Node_BaseAsyncTask's ProxyClass/ProxyFactoryClass/ProxyFactoryFunctionName are
						// `protected` C++ fields but are reflected (UPROPERTY); write via reflection.
						UClass* BaseClass = UK2Node_AsyncAction::StaticClass();
						if (FObjectProperty* PC = CastField<FObjectProperty>(BaseClass->FindPropertyByName(TEXT("ProxyClass"))))
						{
							PC->SetObjectPropertyValue_InContainer(AsyncNode, ProxyClass);
						}
						if (FObjectProperty* PFC = CastField<FObjectProperty>(BaseClass->FindPropertyByName(TEXT("ProxyFactoryClass"))))
						{
							PFC->SetObjectPropertyValue_InContainer(AsyncNode, ProxyClass);
						}
						if (!ProxyFactoryFunc.IsEmpty())
						{
							if (FNameProperty* PFFN = CastField<FNameProperty>(BaseClass->FindPropertyByName(TEXT("ProxyFactoryFunctionName"))))
							{
								PFFN->SetPropertyValue_InContainer(AsyncNode, FName(*ProxyFactoryFunc));
							}
						}
					}
				}
				NewNode = AsyncNode;
			}
			else if (NodeType == TEXT("K2Node_GetArrayItem"))
			{
				NewNode = NewObject<UK2Node_GetArrayItem>(Graph);
			}
			else
			{
				// Generic fallback: try to find the class by name
				UClass* NodeClass = FindFirstObject<UClass>(*NodeType, EFindFirstObjectOptions::NativeFirst);
				if (NodeClass && NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
				{
					NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
				}
				else
				{
					RecordEntryFailure(SpecId, FString::Printf(TEXT("Unknown node type: %s"), *NodeType));
					continue;
				}
			}

			if (NewNode)
			{
				NewNode->NodePosX = Position.X;
				NewNode->NodePosY = Position.Y;
				NewNode->CreateNewGuid();
				NewNode->PostPlacedNewNode();
				NewNode->AllocateDefaultPins();
				Graph->AddNode(NewNode, true, false);

				FString GuidStr = NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				RegisterIdMapping(SpecId, GuidStr);
				RecordEntrySuccess(SpecId, GuidStr);
				SuccessCount++;
			}
		}

		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:Blueprint] Pass 1: Created %d/%d nodes"),
			SuccessCount, NodesArray->Num());
	}

	// --- Create variables ---
	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("variables"), VariablesArray) && VariablesArray)
	{
		for (int32 i = 0; i < VariablesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& VarObj = (*VariablesArray)[i]->AsObject();
			if (!VarObj.IsValid()) continue;

			FString VarId, VarName, VarType;
			VarObj->TryGetStringField(TEXT("id"), VarId);
			VarObj->TryGetStringField(TEXT("name"), VarName);
			VarObj->TryGetStringField(TEXT("type"), VarType);

			// Check for duplicate
			bool bDuplicate = false;
			for (const FBPVariableDescription& Existing : BP->NewVariables)
			{
				if (Existing.VarName == FName(*VarName))
				{
					bDuplicate = true;
					break;
				}
			}
			if (bDuplicate)
			{
				RecordEntryFailure(VarId, FString::Printf(TEXT("Variable '%s' already exists"), *VarName));
				continue;
			}

			ClaireonBlueprintHelpers::FParseVariableTypeResult VarParseResult = ClaireonBlueprintHelpers::ParseVariableTypeChecked(VarType);
			if (!VarParseResult.bSucceeded)
			{
				RecordEntryFailure(VarId, FString::Printf(TEXT("Failed to parse variable type '%s': %s"), *VarType, *VarParseResult.Error));
				continue;
			}
			FEdGraphPinType PinType = VarParseResult.PinType;

			FBPVariableDescription NewVar;
			NewVar.VarName = FName(*VarName);
			NewVar.VarType = PinType;
			NewVar.FriendlyName = VarName;
			NewVar.Category = FText::FromString(TEXT("Default"));

			// Parse flags
			const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
			if (VarObj->TryGetArrayField(TEXT("flags"), FlagsArray) && FlagsArray)
			{
				TArray<FString> Flags;
				for (const TSharedPtr<FJsonValue>& FlagVal : *FlagsArray)
				{
					Flags.Add(FlagVal->AsString());
				}
				NewVar.PropertyFlags = ClaireonBlueprintHelpers::ParsePropertyFlags(Flags);
			}

			// Default value
			FString DefaultValue;
			if (VarObj->TryGetStringField(TEXT("default_value"), DefaultValue))
			{
				NewVar.DefaultValue = DefaultValue;
			}

			BP->NewVariables.Add(NewVar);
			RegisterIdMapping(VarId, VarName);
			RecordEntrySuccess(VarId, VarName);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
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
