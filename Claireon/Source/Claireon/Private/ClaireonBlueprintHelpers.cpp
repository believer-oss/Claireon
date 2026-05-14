// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonNameResolver.h"
#include "Dom/JsonObject.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "BlueprintEditor.h"
#include "GameplayTagContainer.h"
#include "StructUtils/InstancedStruct.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "UObject/UObjectGlobals.h"

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

	namespace
	{
		// Returns true if TypeString begins with Prefix (case-insensitive) and either
		// ends immediately at Prefix.Len() or continues with characters (used for StartsWith tests).
		bool StartsWithI(const FString& TypeString, const TCHAR* Prefix)
		{
			return TypeString.StartsWith(Prefix, ESearchCase::IgnoreCase);
		}

		// Scan TypeString for a matching '>' starting at OpenIndex (which must point at '<').
		// Returns the index of the matching '>' with balanced <...> nesting, or INDEX_NONE on
		// unbalanced input.
		int32 FindMatchingAngle(const FString& TypeString, int32 OpenIndex)
		{
			if (!TypeString.IsValidIndex(OpenIndex) || TypeString[OpenIndex] != TEXT('<'))
			{
				return INDEX_NONE;
			}
			int32 Depth = 0;
			for (int32 i = OpenIndex; i < TypeString.Len(); ++i)
			{
				const TCHAR C = TypeString[i];
				if (C == TEXT('<'))
				{
					++Depth;
				}
				else if (C == TEXT('>'))
				{
					--Depth;
					if (Depth == 0)
					{
						return i;
					}
				}
			}
			return INDEX_NONE;
		}

		// Split Inner at the first top-level comma (depth 0). Nested generics are ignored.
		// Returns true iff exactly one top-level comma splits Inner into two non-empty parts.
		bool SplitMapInnerAtTopLevelComma(const FString& Inner, FString& OutKey, FString& OutValue, FString& OutError)
		{
			int32 Depth = 0;
			int32 CommaIndex = INDEX_NONE;
			int32 TopLevelCommaCount = 0;
			for (int32 i = 0; i < Inner.Len(); ++i)
			{
				const TCHAR C = Inner[i];
				if (C == TEXT('<'))
				{
					++Depth;
				}
				else if (C == TEXT('>'))
				{
					--Depth;
				}
				else if (C == TEXT(',') && Depth == 0)
				{
					if (TopLevelCommaCount == 0)
					{
						CommaIndex = i;
					}
					++TopLevelCommaCount;
				}
			}

			if (TopLevelCommaCount == 0)
			{
				OutError = TEXT("Map<K,V> requires exactly two type parameters separated by a top-level comma");
				return false;
			}
			if (TopLevelCommaCount > 1)
			{
				OutError = FString::Printf(TEXT("Map<K,V> expects exactly two type parameters; got %d top-level parts"), TopLevelCommaCount + 1);
				return false;
			}

			OutKey = Inner.Mid(0, CommaIndex).TrimStartAndEnd();
			OutValue = Inner.Mid(CommaIndex + 1).TrimStartAndEnd();
			if (OutKey.IsEmpty() || OutValue.IsEmpty())
			{
				OutError = TEXT("Map<K,V> has an empty key or value part");
				return false;
			}
			return true;
		}

		// Resolve a UFunction from a fully-qualified path such as
		// "/Script/FSTargeting.FSAbilityTarget.TargetReplicatedDelegate__DelegateSignature" or
		// "/Script/X.Y__DelegateSignature".
		UFunction* ResolveSignatureFunction(const FString& Path)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			// FindObject supports module-scoped paths with dotted segments. Try direct first.
			UFunction* Fn = FindObject<UFunction>(nullptr, *Path);
			if (Fn)
			{
				return Fn;
			}
			// Fall back: LoadObject resolves redirectors + deferred loads.
			return LoadObject<UFunction>(nullptr, *Path);
		}

		// Populate delegate signature members on PinType from a resolved UFunction.
		void SetDelegateSignature(FEdGraphPinType& PinType, UFunction* SignatureFn)
		{
			if (!SignatureFn)
			{
				return;
			}
			FMemberReference::FillSimpleMemberReference<UFunction>(SignatureFn, PinType.PinSubCategoryMemberReference);
		}

		// Parse a SoftClass<X> / SoftObject<X> generic-angle body. Inner is the contents
		// between the angles (already stripped).
		FParseVariableTypeResult ParseSoftRefClass(const FString& Inner, bool bSoftClass)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = Inner.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				Result.Error = bSoftClass
					? TEXT("SoftClass<Class> requires a class name inside the angle brackets")
					: TEXT("SoftObject<Class> requires a class name inside the angle brackets");
				return Result;
			}
			ClaireonNameResolver::FNameResolveResult Resolve;
			UClass* Cls = ClaireonNameResolver::ResolveClassName(Trimmed, nullptr, Resolve);
			if (!Cls)
			{
				Result.Error = FString::Printf(TEXT("Could not resolve class '%s' for soft reference: %s"), *Trimmed, *Resolve.Error);
				return Result;
			}
			Result.PinType.PinCategory = bSoftClass ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Result.PinType.PinSubCategoryObject = Cls;
			Result.ResolutionNote = Resolve.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}

		// Parse `softclass:/Game/.../X.X_C` or `softobject:/Game/.../X.X` prefix form.
		FParseVariableTypeResult ParseSoftRefPath(const FString& PathStr, bool bSoftClass)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = PathStr.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				Result.Error = TEXT("softclass: / softobject: prefix requires an object path");
				return Result;
			}
			UClass* Cls = FindObject<UClass>(nullptr, *Trimmed);
			if (!Cls)
			{
				Cls = LoadObject<UClass>(nullptr, *Trimmed);
			}
			if (!Cls)
			{
				Result.Error = FString::Printf(TEXT("Could not resolve class path '%s' for soft reference"), *Trimmed);
				return Result;
			}
			Result.PinType.PinCategory = bSoftClass ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Result.PinType.PinSubCategoryObject = Cls;
			Result.bSucceeded = true;
			return Result;
		}

		// Parse `InstancedStruct<FMyStruct>` body. Inner is the contents between angles.
		FParseVariableTypeResult ParseInstancedStructGeneric(const FString& Inner)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = Inner.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				// Bare InstancedStruct<> is valid -- meta info points at FInstancedStruct only.
				Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			ClaireonNameResolver::FNameResolveResult Resolve;
			UScriptStruct* InnerStruct = ClaireonNameResolver::ResolveStructName(Trimmed, Resolve);
			if (!InnerStruct)
			{
				Result.Error = FString::Printf(TEXT("Could not resolve struct '%s' for InstancedStruct<>: %s"), *Trimmed, *Resolve.Error);
				return Result;
			}
			Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
			// PinSubCategoryObject is fixed to FInstancedStruct; InnerStruct is the base-struct
			// hint (written as BaseStruct metadata by downstream ApplyVariableProperties callers).
			// We carry it forward via the resolution note so callers surface the intent.
			Result.ResolutionNote = Resolve.ResolutionNote.IsEmpty()
				? FString::Printf(TEXT("InstancedStruct base struct: %s"), *InnerStruct->GetName())
				: Resolve.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
	} // namespace

	FParseVariableTypeResult ParseVariableTypeChecked(const FString& TypeString)
	{
		FParseVariableTypeResult Result;
		FEdGraphPinType& PinType = Result.PinType;

		if (TypeString.IsEmpty())
		{
			Result.Error = TEXT("variable_type is empty");
			return Result;
		}

		// --- Simple scalars (preserve legacy case-sensitive behavior for these keywords).
		if (TypeString == TEXT("float") || TypeString.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("double") || TypeString.Equals(TEXT("Double"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("int") || TypeString == TEXT("int32") || TypeString.Equals(TEXT("Integer"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("int64") || TypeString.Equals(TEXT("Integer64"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("byte") || TypeString.Equals(TEXT("Byte"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("bool") || TypeString.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || TypeString.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("string") || TypeString.Equals(TEXT("String"), ESearchCase::IgnoreCase) || TypeString == TEXT("FString"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("name") || TypeString.Equals(TEXT("Name"), ESearchCase::IgnoreCase) || TypeString == TEXT("FName"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("text") || TypeString.Equals(TEXT("Text"), ESearchCase::IgnoreCase) || TypeString == TEXT("FText"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			Result.bSucceeded = true;
			return Result;
		}

		// --- Expanded named types (D4/D5).
		// Delegate family: flat-string form is an error without signature_function (per D4).
		{
			const FString LowerName = TypeString.ToLower();
			if (LowerName == TEXT("mcdelegate") || LowerName == TEXT("dispatcher") || LowerName == TEXT("multicastinlinedelegate") || LowerName == TEXT("multicastdelegate"))
			{
				Result.Error = TEXT("multicast delegate requires variable_type_spec.signature_function (caller must supply the UFunction signature path)");
				return Result;
			}
			if (LowerName == TEXT("delegate") || LowerName == TEXT("singledelegate"))
			{
				Result.Error = TEXT("delegate requires variable_type_spec.signature_function (caller must supply the UFunction signature path)");
				return Result;
			}
			if (LowerName == TEXT("instancedstruct"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			if (LowerName == TEXT("gameplaytag"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FGameplayTag::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			if (LowerName == TEXT("gameplaytagcontainer"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FGameplayTagContainer::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
		}

		// --- Prefix forms: softclass:/path, softobject:/path
		if (StartsWithI(TypeString, TEXT("softclass:")))
		{
			return ParseSoftRefPath(TypeString.Mid(10), /*bSoftClass=*/true);
		}
		if (StartsWithI(TypeString, TEXT("softobject:")))
		{
			return ParseSoftRefPath(TypeString.Mid(11), /*bSoftClass=*/false);
		}

		// --- Generic angle forms: Array<T>, Set<T>, Map<K,V>, SoftClass<X>, SoftObject<X>,
		// InstancedStruct<X>, SoftClassReference<X>, SoftObjectReference<X>.
		auto TryStripAngleForm = [&TypeString](const TCHAR* Keyword, int32 KeywordLen, FString& OutInner) -> bool
		{
			if (TypeString.Len() < KeywordLen + 2) return false;
			if (TypeString.Left(KeywordLen).Equals(Keyword, ESearchCase::IgnoreCase) &&
				TypeString[KeywordLen] == TEXT('<') &&
				TypeString.EndsWith(TEXT(">")))
			{
				// Validate angle balance for the whole string.
				const int32 Close = FindMatchingAngle(TypeString, KeywordLen);
				if (Close != TypeString.Len() - 1) return false;
				OutInner = TypeString.Mid(KeywordLen + 1, Close - KeywordLen - 1);
				return true;
			}
			return false;
		};

		FString GenericInner;
		if (TryStripAngleForm(TEXT("Array"), 5, GenericInner))
		{
			FParseVariableTypeResult Inner = ParseVariableTypeChecked(GenericInner.TrimStartAndEnd());
			if (!Inner.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Array<T> element parse failed: %s"), *Inner.Error);
				return Result;
			}
			PinType = Inner.PinType;
			PinType.ContainerType = EPinContainerType::Array;
			Result.ResolutionNote = Inner.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("Set"), 3, GenericInner))
		{
			FParseVariableTypeResult Inner = ParseVariableTypeChecked(GenericInner.TrimStartAndEnd());
			if (!Inner.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Set<T> element parse failed: %s"), *Inner.Error);
				return Result;
			}
			PinType = Inner.PinType;
			PinType.ContainerType = EPinContainerType::Set;
			Result.ResolutionNote = Inner.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("Map"), 3, GenericInner))
		{
			FString KeyStr, ValueStr, SplitErr;
			if (!SplitMapInnerAtTopLevelComma(GenericInner, KeyStr, ValueStr, SplitErr))
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> parse failed: %s"), *SplitErr);
				return Result;
			}

			FParseVariableTypeResult Key = ParseVariableTypeChecked(KeyStr);
			if (!Key.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> key parse failed: key:%s"), *Key.Error);
				return Result;
			}
			FParseVariableTypeResult Value = ParseVariableTypeChecked(ValueStr);
			if (!Value.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> value parse failed: value:%s"), *Value.Error);
				return Result;
			}

			PinType.ContainerType = EPinContainerType::Map;
			PinType.PinCategory = Key.PinType.PinCategory;
			PinType.PinSubCategory = Key.PinType.PinSubCategory;
			PinType.PinSubCategoryObject = Key.PinType.PinSubCategoryObject;
			PinType.PinValueType.TerminalCategory = Value.PinType.PinCategory;
			PinType.PinValueType.TerminalSubCategory = Value.PinType.PinSubCategory;
			PinType.PinValueType.TerminalSubCategoryObject = Value.PinType.PinSubCategoryObject;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("SoftClass"), 9, GenericInner) || TryStripAngleForm(TEXT("SoftClassReference"), 18, GenericInner))
		{
			return ParseSoftRefClass(GenericInner, /*bSoftClass=*/true);
		}
		if (TryStripAngleForm(TEXT("SoftObject"), 10, GenericInner) || TryStripAngleForm(TEXT("SoftObjectReference"), 19, GenericInner))
		{
			return ParseSoftRefClass(GenericInner, /*bSoftClass=*/false);
		}
		if (TryStripAngleForm(TEXT("InstancedStruct"), 15, GenericInner))
		{
			return ParseInstancedStructGeneric(GenericInner);
		}

		// --- Name-resolution fallbacks: UClass, UScriptStruct, UEnum (in order).
		{
			ClaireonNameResolver::FNameResolveResult ClassResult;
			UClass* Class = ClaireonNameResolver::ResolveClassName(TypeString, nullptr, ClassResult);
			if (Class)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Class;
				Result.ResolutionNote = ClassResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}
		{
			ClaireonNameResolver::FNameResolveResult StructResult;
			UScriptStruct* Struct = ClaireonNameResolver::ResolveStructName(TypeString, StructResult);
			if (Struct)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = Struct;
				Result.ResolutionNote = StructResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}
		{
			ClaireonNameResolver::FNameResolveResult EnumResult;
			UEnum* Enum = ClaireonNameResolver::ResolveEnumName(TypeString, EnumResult);
			if (Enum)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				PinType.PinSubCategoryObject = Enum;
				Result.ResolutionNote = EnumResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}

		Result.Error = FString::Printf(TEXT("Unknown variable type '%s' (no matching keyword, class, struct, or enum)"), *TypeString);
		return Result;
	}

	FParseVariableTypeResult ParseVariableTypeSpec(const TSharedPtr<FJsonObject>& Spec)
	{
		FParseVariableTypeResult Result;
		if (!Spec.IsValid())
		{
			Result.Error = TEXT("variable_type_spec is missing or not an object");
			return Result;
		}

		FString Base;
		if (!Spec->TryGetStringField(TEXT("base"), Base) || Base.IsEmpty())
		{
			Result.Error = TEXT("variable_type_spec.base is required");
			return Result;
		}

		const FString LowerBase = Base.ToLower();
		const bool bMultiDelegate = (LowerBase == TEXT("mcdelegate") || LowerBase == TEXT("dispatcher") || LowerBase == TEXT("multicastinlinedelegate") || LowerBase == TEXT("multicastdelegate"));
		const bool bSingleDelegate = (LowerBase == TEXT("delegate") || LowerBase == TEXT("singledelegate"));

		FString SignatureFnPath;
		Spec->TryGetStringField(TEXT("signature_function"), SignatureFnPath);
		FString Subtype;
		Spec->TryGetStringField(TEXT("subtype"), Subtype);

		if (bMultiDelegate || bSingleDelegate)
		{
			if (SignatureFnPath.IsEmpty())
			{
				Result.Error = TEXT("delegate variable requires variable_type_spec.signature_function (caller must supply the UFunction signature path; the tool does not synthesize)");
				return Result;
			}
			UFunction* SigFn = ResolveSignatureFunction(SignatureFnPath);
			if (!SigFn)
			{
				Result.Error = FString::Printf(TEXT("signature_function '%s' could not be resolved to a UFunction"), *SignatureFnPath);
				return Result;
			}
			Result.PinType.PinCategory = bMultiDelegate ? UEdGraphSchema_K2::PC_MCDelegate : UEdGraphSchema_K2::PC_Delegate;
			SetDelegateSignature(Result.PinType, SigFn);
			Result.bSucceeded = true;
			return Result;
		}

		// Soft-ref base with explicit subtype overrides the short-form.
		if (LowerBase == TEXT("softclass") || LowerBase == TEXT("softclassreference"))
		{
			if (Subtype.IsEmpty())
			{
				Result.Error = TEXT("softclass variable_type_spec requires 'subtype' (target class path or name)");
				return Result;
			}
			// Route through the class resolver.
			return ParseSoftRefClass(Subtype, /*bSoftClass=*/true);
		}
		if (LowerBase == TEXT("softobject") || LowerBase == TEXT("softobjectreference"))
		{
			if (Subtype.IsEmpty())
			{
				Result.Error = TEXT("softobject variable_type_spec requires 'subtype' (target class path or name)");
				return Result;
			}
			return ParseSoftRefClass(Subtype, /*bSoftClass=*/false);
		}
		if (LowerBase == TEXT("instancedstruct"))
		{
			if (Subtype.IsEmpty())
			{
				// Bare FInstancedStruct with no base-struct hint is legal.
				Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			return ParseInstancedStructGeneric(Subtype);
		}

		// Otherwise fall through to the short-form parser with just base.
		return ParseVariableTypeChecked(Base);
	}

	FEdGraphPinType ParseVariableType(const FString& TypeString)
	{
		FParseVariableTypeResult Local = ParseVariableTypeChecked(TypeString);
		if (!Local.bSucceeded)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[ParseVariableType] legacy-path parse failed for '%s': %s"),
				*TypeString, *Local.Error);
			return FEdGraphPinType();
		}
		return Local.PinType;
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

	void ApplyVariableProperties(UBlueprint* Blueprint, FName VarName, const TSharedPtr<FJsonObject>& Params, FApplyVariableResult* OutResult)
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

			if (Replication.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
				VarDesc->RepNotifyFunc = NAME_None;
			}
			else if (Replication.Equals(TEXT("Replicated"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags |= CPF_Net;
				VarDesc->PropertyFlags &= ~CPF_RepNotify;
			}
			else if (Replication.Equals(TEXT("RepNotify"), ESearchCase::IgnoreCase)
			         || Replication.Equals(TEXT("rep_notify"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags |= CPF_Net | CPF_RepNotify;

				// Resolve the handler function name: caller-supplied or default OnRep_<VarName>.
				FString RepNotifyFuncStr;
				FName HandlerName;
				if (Params->TryGetStringField(TEXT("rep_notify_func"), RepNotifyFuncStr) && !RepNotifyFuncStr.IsEmpty())
				{
					HandlerName = FName(*RepNotifyFuncStr);
				}
				else
				{
					// Default UE5 convention: OnRep_VarName
					HandlerName = FName(*FString::Printf(TEXT("OnRep_%s"), *VarName.ToString()));
				}
				VarDesc->RepNotifyFunc = HandlerName;

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

				// Auto-create the handler function graph.
				// Follows editor precedent in FBlueprintVarActionDetails::ReplicationChanged
				// (Engine/Source/Editor/Kismet/Private/BlueprintDetailsCustomization.cpp ~lines 2773-2787).
				const FString HandlerNameStr = HandlerName.ToString();
				UEdGraph* FoundGraph = FindObject<UEdGraph>(Blueprint, *HandlerNameStr);

				// If the caller supplied a rep_notify_func that already resolves to a compiled
				// UFunction on the skeleton class, skip graph creation entirely -- they wired it
				// intentionally (proposal Risks #2).
				bool bUserFunctionExists = false;
				if (Blueprint->SkeletonGeneratedClass != nullptr)
				{
					bUserFunctionExists = (Blueprint->SkeletonGeneratedClass->FindFunctionByName(HandlerName) != nullptr);
				}

				if (!bUserFunctionExists && FoundGraph == nullptr)
				{
					UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
						Blueprint,
						HandlerName,
						UEdGraph::StaticClass(),
						UEdGraphSchema_K2::StaticClass());
					FBlueprintEditorUtils::AddFunctionGraph<UClass>(
						Blueprint,
						NewGraph,
						/*bIsUserCreated=*/false,
						/*SignatureFromClass=*/nullptr);
					FoundGraph = NewGraph;

					if (OutResult != nullptr)
					{
						OutResult->bRepNotifyGraphCreated = true;
					}
				}

				if (OutResult != nullptr && FoundGraph != nullptr)
				{
					OutResult->RepNotifyHandlerGraph = HandlerName;
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
					VarDesc->PropertyFlags |= CPF_BlueprintVisible;
				}
				else if (Flag == TEXT("BlueprintReadWrite"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, true);
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, false);
					VarDesc->PropertyFlags |= CPF_BlueprintVisible;
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

	bool IsBlueprintAssetClass(const FString& ClassName)
	{
		return ClassName == TEXT("Blueprint")
			|| ClassName == TEXT("AnimBlueprint")
			|| ClassName == TEXT("WidgetBlueprint");
	}

	UClass* PickK2NodeClassForFunction(const UFunction* Function)
	{
		if (!Function)
		{
			return UK2Node_CallFunction::StaticClass();
		}

		const bool bIsPure = Function->HasAllFunctionFlags(FUNC_BlueprintPure);
		const bool bHasArrayPointerParms = Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam);
		const bool bIsCommutativeAssociativeBinaryOp = Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator);
		const bool bIsMaterialParamCollectionFunc = Function->HasMetaData(FBlueprintMetadata::MD_MaterialParameterCollectionFunction);
		const bool bIsDataTableFunc = Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin);

		// AsyncAction detection: mirror UK2Node_AsyncAction::GetMenuActions filter.
		// Functions whose owning class carries HasDedicatedAsyncNode metadata fall
		// through to the plain UK2Node_CallFunction path (today's behavior). Routing
		// those to dedicated nodes (e.g. UK2Node_LatentAbilityCall) is a follow-up;
		// tracked in proposal R3.
		if (const UClass* OwnerClass = Function->GetOwnerClass())
		{
			if (OwnerClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass())
				&& !OwnerClass->HasMetaData(TEXT("HasDedicatedAsyncNode")))
			{
				if (const FObjectProperty* ReturnProp = CastField<FObjectProperty>(Function->GetReturnProperty()))
				{
					if (ReturnProp->PropertyClass
						&& ReturnProp->PropertyClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
					{
						return UK2Node_AsyncAction::StaticClass();
					}
				}
			}
		}

		if (bIsCommutativeAssociativeBinaryOp && bIsPure)
		{
			return UK2Node_CommutativeAssociativeBinaryOperator::StaticClass();
		}
		if (bIsMaterialParamCollectionFunc)
		{
			return UK2Node_CallMaterialParameterCollectionFunction::StaticClass();
		}
		if (bIsDataTableFunc)
		{
			return UK2Node_CallDataTableFunction::StaticClass();
		}
		if (bHasArrayPointerParms)
		{
			return UK2Node_CallArrayFunction::StaticClass();
		}
		return UK2Node_CallFunction::StaticClass();
	}

	FString GetNodeTypeAliasForClass(const UClass* NodeClass)
	{
		return ::ClaireonNodeTypeAlias::GetAliasForNodeClass(NodeClass);
	}
} // namespace ClaireonBlueprintHelpers
