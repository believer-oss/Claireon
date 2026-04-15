// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonNameResolver.h"
#include "Dom/JsonObject.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "BlueprintEditor.h"

// FScopedBlueprintEditor Implementation

FScopedBlueprintEditor::FScopedBlueprintEditor(UBlueprint* InBlueprint, bool bInSilent, bool bInCloseOnDestroy)
	: Blueprint(InBlueprint)
	, bWasAlreadyOpen(false)
	, bCloseOnDestroy(bInCloseOnDestroy)
{
	if (!InBlueprint)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[FScopedBlueprintEditor] Null Blueprint provided"));
		return;
	}

	// Check if the Blueprint is already open
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		UE_LOG(LogClaireon, Error, TEXT("[FScopedBlueprintEditor] Failed to get AssetEditorSubsystem"));
		return;
	}

	IAssetEditorInstance* ExistingEditor = AssetEditorSubsystem->FindEditorForAsset(InBlueprint, false);
	if (ExistingEditor)
	{
		bWasAlreadyOpen = true;
		UE_LOG(LogClaireon, Verbose, TEXT("[FScopedBlueprintEditor] Blueprint %s already open"), *InBlueprint->GetPathName());
	}
	else
	{
		// Open the Blueprint editor
		AssetEditorSubsystem->OpenEditorForAsset(InBlueprint);
		ExistingEditor = AssetEditorSubsystem->FindEditorForAsset(InBlueprint, false);
		if (ExistingEditor)
		{
			UE_LOG(LogClaireon, Verbose, TEXT("[FScopedBlueprintEditor] Opened Blueprint %s"), *InBlueprint->GetPathName());
		}
		else
		{
			UE_LOG(LogClaireon, Error, TEXT("[FScopedBlueprintEditor] Failed to open Blueprint %s"), *InBlueprint->GetPathName());
		}
	}

	// Extract FBlueprintEditor from the IAssetEditorInstance
	if (ExistingEditor)
	{
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(ExistingEditor);
		TSharedRef<FAssetEditorToolkit> ToolkitRef = Toolkit->AsShared();
		BlueprintEditor = StaticCastSharedRef<FBlueprintEditor>(ToolkitRef);
	}
}

FScopedBlueprintEditor::~FScopedBlueprintEditor()
{
	if (bCloseOnDestroy && !bWasAlreadyOpen && Blueprint.IsValid())
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint.Get());
			UE_LOG(LogClaireon, Verbose, TEXT("[FScopedBlueprintEditor] Closed Blueprint %s"), *Blueprint->GetPathName());
		}
	}
}

TSharedPtr<SGraphEditor> FScopedBlueprintEditor::GetGraphEditor(UEdGraph* Graph)
{
	if (!BlueprintEditor.IsValid() || !Graph)
	{
		return nullptr;
	}

	// The Blueprint editor contains graph editors for each visible graph
	// We need to find the one for our specific graph
	// Note: This is a simplified implementation. In practice, you might need to
	// navigate through the tab manager to find the specific graph editor widget.

	// For now, return nullptr as we'd need more complex tab management
	// This will be sufficient for operations that don't require the SGraphEditor widget
	UE_LOG(LogClaireon, Warning, TEXT("[FScopedBlueprintEditor] GetGraphEditor not fully implemented - returning nullptr"));
	return nullptr;
}

// ClaireonBlueprintHelpers Namespace Implementation

namespace ClaireonBlueprintHelpers
{
	TArray<UEdGraphPin*> GetExecPins(UEdGraphNode* Node, bool bInputOnly, bool bOutputOnly)
	{
		TArray<UEdGraphPin*> ExecPins;

		if (!Node)
		{
			return ExecPins;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (bInputOnly && Pin->Direction != EGPD_Input)
				{
					continue;
				}
				if (bOutputOnly && Pin->Direction != EGPD_Output)
				{
					continue;
				}
				ExecPins.Add(Pin);
			}
		}

		return ExecPins;
	}

	bool HasExecInputPins(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
			{
				return true;
			}
		}

		return false;
	}

	bool HasExecOutputPins(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return false;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
			{
				return true;
			}
		}

		return false;
	}

	TArray<UEdGraphNode*> FindRootNodes(UEdGraph* Graph)
	{
		TArray<UEdGraphNode*> RootNodes;

		if (!Graph)
		{
			return RootNodes;
		}

		// Root nodes are nodes that have exec output pins but no exec input pins
		// These are typically Event nodes or entry points
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && HasExecOutputPins(Node) && !HasExecInputPins(Node))
			{
				RootNodes.Add(Node);
			}
		}

		return RootNodes;
	}

	bool ValidateAssetPath(const FString& AssetPath, FString& OutError)
	{
		auto Result = ClaireonPathResolver::Resolve(AssetPath);
		if (!Result.bSuccess)
		{
			OutError = Result.Error;
			return false;
		}
		return true;
	}

	UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FGuid& NodeGuid, FGuid* OutCorrectedGuid)
	{
		if (!Graph || !NodeGuid.IsValid())
		{
			return nullptr;
		}

		// Exact match
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}

		// Fallback: match on the A field only. Blueprint recompilation can
		// regenerate B/C/D while preserving A, causing GUIDs returned by
		// get_blueprint_graph to go stale by the time edit_blueprint_graph
		// tries to resolve them.  If exactly one node shares the A field we
		// treat it as the same logical node and log a warning.
		UEdGraphNode* AFieldMatch = nullptr;
		int32 AFieldMatchCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid.A == NodeGuid.A)
			{
				AFieldMatch = Node;
				++AFieldMatchCount;
			}
		}

		if (AFieldMatchCount == 1 && AFieldMatch)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[FindNodeByGuid] Exact GUID match failed — recovered via A-field fallback. ")
				TEXT("Requested=%s  Matched=%s  Node='%s'. ")
				TEXT("This usually means the blueprint was recompiled between get and edit calls."),
				*NodeGuid.ToString(),
				*AFieldMatch->NodeGuid.ToString(),
				*AFieldMatch->GetNodeTitle(ENodeTitleType::ListView).ToString());
			if (OutCorrectedGuid)
			{
				*OutCorrectedGuid = AFieldMatch->NodeGuid;
			}
			return AFieldMatch;
		}

		return nullptr;
	}

	TArray<UEdGraphNode*> FindNodesByTitle(UEdGraph* Graph, const FString& NodeTitle, bool bExactMatch)
	{
		TArray<UEdGraphNode*> MatchingNodes;

		if (!Graph || NodeTitle.IsEmpty())
		{
			return MatchingNodes;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FString CurrentTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

			if (bExactMatch)
			{
				if (CurrentTitle.Equals(NodeTitle, ESearchCase::IgnoreCase))
				{
					MatchingNodes.Add(Node);
				}
			}
			else
			{
				if (CurrentTitle.Contains(NodeTitle, ESearchCase::IgnoreCase))
				{
					MatchingNodes.Add(Node);
				}
			}
		}

		return MatchingNodes;
	}

	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		// Check UbergraphPages (EventGraph)
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check FunctionGraphs
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check MacroGraphs
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check DelegateSignatureGraphs
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		return nullptr;
	}

	TArray<UEdGraphPin*> FindCompatiblePins(UEdGraphNode* Node, UEdGraphPin* Pin)
	{
		TArray<UEdGraphPin*> CompatiblePins;

		if (!Node || !Pin)
		{
			return CompatiblePins;
		}

		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetSchema());
		if (!Schema)
		{
			return CompatiblePins;
		}

		// Find pins with opposite direction
		EEdGraphPinDirection OppositeDirection = (Pin->Direction == EGPD_Input) ? EGPD_Output : EGPD_Input;

		for (UEdGraphPin* NodePin : Node->Pins)
		{
			if (NodePin && NodePin->Direction == OppositeDirection)
			{
				// Check if connection is possible
				FPinConnectionResponse Response = Schema->CanCreateConnection(Pin, NodePin);
				if (Response.Response == CONNECT_RESPONSE_MAKE || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
				{
					CompatiblePins.Add(NodePin);
				}
			}
		}

		return CompatiblePins;
	}

	FEdGraphPinType ParseVariableType(const FString& TypeString)
	{
		FEdGraphPinType PinType;

		// Simple types
		if (TypeString == TEXT("float"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
		}
		else if (TypeString == TEXT("double"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (TypeString == TEXT("int") || TypeString == TEXT("int32"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (TypeString == TEXT("int64"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (TypeString == TEXT("byte"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (TypeString == TEXT("bool"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (TypeString == TEXT("string"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (TypeString == TEXT("name"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (TypeString == TEXT("text"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		// Arrays: "Array<float>", "Array<Actor>", etc.
		else if (TypeString.StartsWith(TEXT("Array<")))
		{
			PinType.ContainerType = EPinContainerType::Array;

			// Extract element type
			FString ElementType = TypeString.Mid(6, TypeString.Len() - 7);	 // Remove "Array<" and ">"
			FEdGraphPinType ElementPinType = ParseVariableType(ElementType); // Recursive
			PinType.PinCategory = ElementPinType.PinCategory;
			PinType.PinSubCategory = ElementPinType.PinSubCategory;
			PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
		}
		// Sets: "Set<int>", etc.
		else if (TypeString.StartsWith(TEXT("Set<")))
		{
			PinType.ContainerType = EPinContainerType::Set;
			FString ElementType = TypeString.Mid(4, TypeString.Len() - 5);
			FEdGraphPinType ElementPinType = ParseVariableType(ElementType);
			PinType.PinCategory = ElementPinType.PinCategory;
			PinType.PinSubCategory = ElementPinType.PinSubCategory;
			PinType.PinSubCategoryObject = ElementPinType.PinSubCategoryObject;
		}
		// Maps: "Map<string,int>", etc.
		else if (TypeString.StartsWith(TEXT("Map<")))
		{
			PinType.ContainerType = EPinContainerType::Map;
			// For maps, default to string->string for now
			// TODO: Parse key and value types properly
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		// Objects, Structs, Enums
		else
		{
			// Try to find as UClass
			ClaireonNameResolver::FNameResolveResult ClassResult;
			UClass* Class = ClaireonNameResolver::ResolveClassName(TypeString, nullptr, ClassResult);
			if (Class)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Class;
			}
			else
			{
				// Try to find as UScriptStruct
				ClaireonNameResolver::FNameResolveResult StructResult;
				UScriptStruct* Struct = ClaireonNameResolver::ResolveStructName(TypeString, StructResult);
				if (Struct)
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					PinType.PinSubCategoryObject = Struct;
				}
				else
				{
					// Try to find as UEnum
					ClaireonNameResolver::FNameResolveResult EnumResult;
					UEnum* Enum = ClaireonNameResolver::ResolveEnumName(TypeString, EnumResult);
					if (Enum)
					{
						PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
						PinType.PinSubCategoryObject = Enum;
					}
					else
					{
						// Unknown type - default to string
						UE_LOG(LogClaireon, Warning, TEXT("[ParseVariableType] Unknown type '%s', defaulting to string"), *TypeString);
						PinType.PinCategory = UEdGraphSchema_K2::PC_String;
					}
				}
			}
		}

		return PinType;
	}

	uint64 ParsePropertyFlags(const TArray<FString>& FlagStrings)
	{
		uint64 Flags = CPF_None;

		for (const FString& Flag : FlagStrings)
		{
			if (Flag == TEXT("BlueprintReadOnly"))
			{
				Flags |= CPF_BlueprintVisible | CPF_BlueprintReadOnly;
			}
			else if (Flag == TEXT("BlueprintReadWrite"))
			{
				Flags |= CPF_BlueprintVisible;
			}
			else if (Flag == TEXT("EditAnywhere"))
			{
				Flags |= CPF_Edit;
			}
			else if (Flag == TEXT("EditDefaultsOnly"))
			{
				Flags |= CPF_Edit | CPF_DisableEditOnInstance;
			}
			else if (Flag == TEXT("EditInstanceOnly"))
			{
				Flags |= CPF_Edit;
				Flags &= ~CPF_DisableEditOnInstance;
			}
			else if (Flag == TEXT("VisibleAnywhere"))
			{
				Flags |= CPF_Edit | CPF_EditConst;
			}
			else if (Flag == TEXT("Transient"))
			{
				Flags |= CPF_Transient;
			}
			else if (Flag == TEXT("Config"))
			{
				Flags |= CPF_Config;
			}
			else if (Flag == TEXT("SaveGame"))
			{
				Flags |= CPF_SaveGame;
			}
			else if (Flag == TEXT("Interp"))
			{
				Flags |= CPF_Interp;
			}
			else if (Flag == TEXT("ExposeOnSpawn"))
			{
				Flags |= CPF_ExposeOnSpawn;
			}
			else if (Flag == TEXT("Net") || Flag == TEXT("Replicated"))
			{
				Flags |= CPF_Net;
			}
			else if (Flag == TEXT("RepNotify"))
			{
				Flags |= CPF_Net | CPF_RepNotify;
			}
			else if (Flag == TEXT("AdvancedDisplay"))
			{
				Flags |= CPF_AdvancedDisplay;
			}
			else if (Flag == TEXT("AssetRegistrySearchable"))
			{
				Flags |= CPF_AssetRegistrySearchable;
			}
			else if (Flag == TEXT("SimpleDisplay"))
			{
				Flags |= CPF_SimpleDisplay;
			}
			else if (Flag == TEXT("DisableEditOnTemplate"))
			{
				Flags |= CPF_DisableEditOnTemplate;
			}
		}

		return Flags;
	}

	TArray<FString> FormatPropertyFlags(uint64 PropertyFlags)
	{
		TArray<FString> Flags;

		// Blueprint visibility (compound flags)
		if (PropertyFlags & CPF_BlueprintVisible)
		{
			if (PropertyFlags & CPF_BlueprintReadOnly)
			{
				Flags.Add(TEXT("BlueprintReadOnly"));
			}
			else
			{
				Flags.Add(TEXT("BlueprintReadWrite"));
			}
		}

		// Replication (compound flags)
		if (PropertyFlags & CPF_Net)
		{
			if (PropertyFlags & CPF_RepNotify)
			{
				Flags.Add(TEXT("RepNotify"));
			}
			else
			{
				Flags.Add(TEXT("Net"));
			}
		}

		// Edit specifiers (compound flags)
		if (PropertyFlags & CPF_Edit)
		{
			if (PropertyFlags & CPF_DisableEditOnInstance)
			{
				Flags.Add(TEXT("EditDefaultsOnly"));
			}
			else if (PropertyFlags & CPF_EditConst)
			{
				Flags.Add(TEXT("VisibleAnywhere"));
			}
			else
			{
				Flags.Add(TEXT("EditAnywhere"));
			}
		}

		// Simple flags
		if (PropertyFlags & CPF_Transient)
		{
			Flags.Add(TEXT("Transient"));
		}
		if (PropertyFlags & CPF_Config)
		{
			Flags.Add(TEXT("Config"));
		}
		if (PropertyFlags & CPF_SaveGame)
		{
			Flags.Add(TEXT("SaveGame"));
		}
		if (PropertyFlags & CPF_Interp)
		{
			Flags.Add(TEXT("Interp"));
		}
		if (PropertyFlags & CPF_ExposeOnSpawn)
		{
			Flags.Add(TEXT("ExposeOnSpawn"));
		}
		if (PropertyFlags & CPF_AdvancedDisplay)
		{
			Flags.Add(TEXT("AdvancedDisplay"));
		}
		if (PropertyFlags & CPF_AssetRegistrySearchable)
		{
			Flags.Add(TEXT("AssetRegistrySearchable"));
		}
		if (PropertyFlags & CPF_SimpleDisplay)
		{
			Flags.Add(TEXT("SimpleDisplay"));
		}
		if (PropertyFlags & CPF_DisableEditOnTemplate)
		{
			Flags.Add(TEXT("DisableEditOnTemplate"));
		}

		return Flags;
	}

	void ApplyVariableProperties(UBlueprint* Blueprint, FName VarName, const TSharedPtr<FJsonObject>& Params)
	{
		if (!Blueprint || !Params.IsValid())
		{
			return;
		}

		// Find the variable description
		FBPVariableDescription* VarDesc = nullptr;
		for (FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarName == VarName)
			{
				VarDesc = &Var;
				break;
			}
		}
		if (!VarDesc)
		{
			return;
		}

		// Track whether the replication field was explicitly provided
		bool bReplicationFieldProvided = false;

		// 1. Apply category
		FString Category;
		if (Params->TryGetStringField(TEXT("category"), Category))
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(Category));
		}

		// 2. Apply tooltip
		FString Tooltip;
		if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		}

		// 3. Apply display_name
		FString DisplayName;
		if (Params->TryGetStringField(TEXT("display_name"), DisplayName))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_DisplayName, DisplayName);
		}

		// 4. Apply replication
		FString Replication;
		if (Params->TryGetStringField(TEXT("replication"), Replication))
		{
			bReplicationFieldProvided = true;

			if (Replication == TEXT("None"))
			{
				VarDesc->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
				VarDesc->RepNotifyFunc = NAME_None;
			}
			else if (Replication == TEXT("Replicated"))
			{
				VarDesc->PropertyFlags |= CPF_Net;
				VarDesc->PropertyFlags &= ~CPF_RepNotify;
			}
			else if (Replication == TEXT("RepNotify"))
			{
				VarDesc->PropertyFlags |= CPF_Net | CPF_RepNotify;

				// Set RepNotifyFunc if provided
				FString RepNotifyFunc;
				if (Params->TryGetStringField(TEXT("rep_notify_func"), RepNotifyFunc))
				{
					VarDesc->RepNotifyFunc = FName(*RepNotifyFunc);
				}
				else
				{
					// Default UE5 convention: OnRep_VarName
					VarDesc->RepNotifyFunc = FName(*FString::Printf(TEXT("OnRep_%s"), *VarName.ToString()));
				}

				// Set ReplicationCondition if provided
				FString ReplicationCondition;
				if (Params->TryGetStringField(TEXT("replication_condition"), ReplicationCondition))
				{
					const UEnum* CondEnum = StaticEnum<ELifetimeCondition>();
					if (CondEnum)
					{
						int64 CondValue = CondEnum->GetValueByNameString(ReplicationCondition);
						if (CondValue != INDEX_NONE)
						{
							VarDesc->ReplicationCondition = static_cast<ELifetimeCondition>(CondValue);
						}
					}
				}
			}
		}

		// 5. Apply metadata entries
		const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
		if (Params->TryGetObjectField(TEXT("metadata"), MetadataObj) && MetadataObj && (*MetadataObj).IsValid())
		{
			for (const auto& Pair : (*MetadataObj)->Values)
			{
				FString Value;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FName(*Pair.Key), Value);
				}
			}
		}

		// 6. Apply flags[]
		const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("flags"), FlagsArray))
		{
			TArray<FString> RemainingFlags;

			for (const TSharedPtr<FJsonValue>& FlagValue : *FlagsArray)
			{
				FString Flag = FlagValue->AsString();

				if (Flag == TEXT("Interp"))
				{
					FBlueprintEditorUtils::SetInterpFlag(Blueprint, VarName, true);
				}
				else if (Flag == TEXT("BlueprintReadOnly"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, true);
				}
				else if (Flag == TEXT("BlueprintReadWrite"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, true);
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, false);
				}
				else if (bReplicationFieldProvided && (Flag == TEXT("Net") || Flag == TEXT("Replicated") || Flag == TEXT("RepNotify")))
				{
					// Skip replication flags if replication field was explicitly provided
					continue;
				}
				else
				{
					RemainingFlags.Add(Flag);
				}
			}

			if (RemainingFlags.Num() > 0)
			{
				VarDesc->PropertyFlags |= ParsePropertyFlags(RemainingFlags);
			}
		}

		// 7. Apply clear_flags[]
		const TArray<TSharedPtr<FJsonValue>>* ClearFlagsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("clear_flags"), ClearFlagsArray))
		{
			TArray<FString> RemainingClearFlags;

			for (const TSharedPtr<FJsonValue>& FlagValue : *ClearFlagsArray)
			{
				FString Flag = FlagValue->AsString();

				if (Flag == TEXT("Interp"))
				{
					FBlueprintEditorUtils::SetInterpFlag(Blueprint, VarName, false);
				}
				else if (Flag == TEXT("BlueprintReadOnly"))
				{
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, false);
				}
				else if (Flag == TEXT("BlueprintReadWrite"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
				}
				else
				{
					RemainingClearFlags.Add(Flag);
				}
			}

			if (RemainingClearFlags.Num() > 0)
			{
				VarDesc->PropertyFlags &= ~ParsePropertyFlags(RemainingClearFlags);
			}
		}
	}

	UEdGraphPin* GetFirstOutputPin(UEdGraphNode* Node)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	FString FormatAvailableNodes(UEdGraph* Graph, int32 MaxCount)
	{
		if (!Graph)
			return TEXT("");

		FString Result;
		int32 TotalNodes = Graph->Nodes.Num();
		int32 Count = 0;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
				continue;
			if (Count >= MaxCount)
				break;

			FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			FString ClassName = Node->GetClass()->GetName();
			Result += FString::Printf(TEXT("  %s - %s \"%s\"\n"),
				*Node->NodeGuid.ToString(), *ClassName, *Title);
			Count++;
		}

		if (TotalNodes > MaxCount)
		{
			Result += FString::Printf(TEXT("  ... and %d more\n"), TotalNodes - MaxCount);
		}

		return FString::Printf(TEXT("Available nodes (%d of %d):\n%s"),
			FMath::Min(Count, MaxCount), TotalNodes, *Result);
	}

	FString FormatAvailablePins(UEdGraphNode* Node)
	{
		if (!Node)
			return TEXT("");

		FString Result;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
				continue;
			FString Direction = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
			Result += FString::Printf(TEXT("  %s (%s)\n"), *Pin->GetName(), *Direction);
		}
		return Result;
	}

	TArray<UEdGraphNode*> FindNodesByClassAndTitle(UEdGraph* Graph, const FString& ClassName, const FString& Title)
	{
		TArray<UEdGraphNode*> Results;
		if (!Graph)
			return Results;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
				continue;

			bool bClassMatch = ClassName.IsEmpty() || Node->GetClass()->GetName().Contains(ClassName);
			bool bTitleMatch = Title.IsEmpty() || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(Title);

			if (bClassMatch && bTitleMatch)
			{
				Results.Add(Node);
			}
		}
		return Results;
	}
} // namespace ClaireonBlueprintHelpers
