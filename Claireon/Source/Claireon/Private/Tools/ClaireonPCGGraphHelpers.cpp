// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGCommon.h"

// ============================================================================
// Asset Loading
// ============================================================================

UPCGGraph* ClaireonPCGGraphHelpers::LoadPCGGraphAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UPCGGraph* Graph = Cast<UPCGGraph>(LoadedObj);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a PCG Graph (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return Graph;
}

// ============================================================================
// Node Lookup
// ============================================================================

UPCGNode* ClaireonPCGGraphHelpers::FindNodeByIdentifier(UPCGGraph* Graph, const FString& Identifier, int32& OutIndex)
{
	if (!Graph)
	{
		OutIndex = INDEX_NONE;
		return nullptr;
	}

	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();

	// Try numeric index first
	if (Identifier.IsNumeric())
	{
		int32 Index = FCString::Atoi(*Identifier);
		if (Index >= 0 && Index < Nodes.Num())
		{
			OutIndex = Index;
			return Nodes[Index];
		}
	}

	// Check special identifiers for input/output nodes
	if (Identifier.Equals(TEXT("input"), ESearchCase::IgnoreCase) || Identifier.Equals(TEXT("GraphInput"), ESearchCase::IgnoreCase))
	{
		UPCGNode* InputNode = Graph->GetInputNode();
		if (InputNode)
		{
			OutIndex = Nodes.IndexOfByKey(InputNode);
			return InputNode;
		}
	}
	if (Identifier.Equals(TEXT("output"), ESearchCase::IgnoreCase) || Identifier.Equals(TEXT("GraphOutput"), ESearchCase::IgnoreCase))
	{
		UPCGNode* OutputNode = Graph->GetOutputNode();
		if (OutputNode)
		{
			OutIndex = Nodes.IndexOfByKey(OutputNode);
			return OutputNode;
		}
	}

	// Search by node title or settings class name
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		UPCGNode* Node = Nodes[i];
		if (!Node)
		{
			continue;
		}

		// Match by node title
		if (Node->NodeTitle != NAME_None && Node->NodeTitle.ToString().Equals(Identifier, ESearchCase::IgnoreCase))
		{
			OutIndex = i;
			return Node;
		}

		// Match by display name
		FString DisplayName = GetNodeDisplayName(Node);
		if (DisplayName.Equals(Identifier, ESearchCase::IgnoreCase))
		{
			OutIndex = i;
			return Node;
		}
	}

	OutIndex = INDEX_NONE;
	return nullptr;
}

FString ClaireonPCGGraphHelpers::GetNodeDisplayName(const UPCGNode* Node)
{
	if (!Node)
	{
		return TEXT("(null)");
	}

	if (Node->NodeTitle != NAME_None)
	{
		return Node->NodeTitle.ToString();
	}

	const UPCGSettings* Settings = Node->GetSettings();
	if (Settings)
	{
		return GetSettingsShortName(Settings);
	}

	return TEXT("Unknown");
}

FString ClaireonPCGGraphHelpers::GetSettingsShortName(const UPCGSettings* Settings)
{
	if (!Settings)
	{
		return TEXT("Unknown");
	}

	FString ClassName = Settings->GetClass()->GetName();

	// Strip common prefixes/suffixes for readability
	ClassName.RemoveFromStart(TEXT("PCG"));
	ClassName.RemoveFromEnd(TEXT("Settings"));

	if (ClassName.IsEmpty())
	{
		ClassName = Settings->GetClass()->GetName();
	}

	return ClassName;
}

// ============================================================================
// Settings Class Resolution
// ============================================================================

UClass* ClaireonPCGGraphHelpers::ResolveSettingsClass(const FString& ClassName, FString& OutError)
{
	// Build candidate names to try
	TArray<FString> Candidates;
	Candidates.Add(ClassName);
	Candidates.Add(FString::Printf(TEXT("U%s"), *ClassName));
	Candidates.Add(FString::Printf(TEXT("UPCG%sSettings"), *ClassName));
	Candidates.Add(FString::Printf(TEXT("PCG%sSettings"), *ClassName));
	Candidates.Add(FString::Printf(TEXT("U%sSettings"), *ClassName));
	if (!ClassName.StartsWith(TEXT("PCG")))
	{
		Candidates.Add(FString::Printf(TEXT("UPCG%s"), *ClassName));
	}

	for (const FString& Candidate : Candidates)
	{
		ClaireonNameResolver::FNameResolveResult NameResult;
		UClass* FoundClass = ClaireonNameResolver::ResolveClassName(Candidate, UPCGSettings::StaticClass(), NameResult);
		if (FoundClass)
		{
			return FoundClass;
		}
	}

	OutError = FString::Printf(TEXT("Could not find PCG settings class: %s. Try 'list_node_types' to see available classes."), *ClassName);
	return nullptr;
}

// ============================================================================
// Formatting
// ============================================================================

FString ClaireonPCGGraphHelpers::FormatGraphStructure(const UPCGGraph* Graph, const FString& DetailLevel)
{
	if (!Graph)
	{
		return TEXT("(null graph)");
	}

	const bool bFull = DetailLevel.Equals(TEXT("full"), ESearchCase::IgnoreCase);
	const bool bOutline = DetailLevel.Equals(TEXT("outline"), ESearchCase::IgnoreCase);
	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();

	FString Output;
	Output += FString::Printf(TEXT("PCG Graph: %s\n"), *Graph->GetPathName());
	Output += FString::Printf(TEXT("Nodes: %d\n\n"), Nodes.Num());

	// Show input node
	if (UPCGNode* InputNode = Graph->GetInputNode())
	{
		Output += FormatNodeDetail(Graph, InputNode, INDEX_NONE, bFull);
		Output += TEXT("\n");
	}

	// Show all graph nodes
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const UPCGNode* Node = Nodes[i];
		if (!Node)
		{
			continue;
		}

		// Skip input/output nodes as they're shown separately
		if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
		{
			continue;
		}

		if (bOutline)
		{
			Output += FString::Printf(TEXT("[%d] %s\n"), i, *GetNodeDisplayName(Node));
		}
		else
		{
			Output += FormatNodeDetail(Graph, Node, i, bFull);
			Output += TEXT("\n");
		}
	}

	// Show output node
	if (UPCGNode* OutputNode = Graph->GetOutputNode())
	{
		Output += FormatNodeDetail(Graph, OutputNode, INDEX_NONE, bFull);
		Output += TEXT("\n");
	}

	return Output;
}

FString ClaireonPCGGraphHelpers::FormatNodeDetail(const UPCGGraph* Graph, const UPCGNode* Node, int32 NodeIndex, bool bIncludeProperties)
{
	if (!Node)
	{
		return TEXT("(null node)\n");
	}

	FString Output;

	// Node header
	bool bIsInput = (Graph && Node == Graph->GetInputNode());
	bool bIsOutput = (Graph && Node == Graph->GetOutputNode());

	if (bIsInput)
	{
		Output += TEXT("[Input] GraphInput");
	}
	else if (bIsOutput)
	{
		Output += TEXT("[Output] GraphOutput");
	}
	else if (NodeIndex != INDEX_NONE)
	{
		Output += FString::Printf(TEXT("[%d] %s"), NodeIndex, *GetNodeDisplayName(Node));
	}
	else
	{
		Output += FString::Printf(TEXT("%s"), *GetNodeDisplayName(Node));
	}

	const UPCGSettings* Settings = Node->GetSettings();
	if (Settings && !bIsInput && !bIsOutput)
	{
		Output += FString::Printf(TEXT(" (%s)"), *Settings->GetClass()->GetName());
	}
	Output += TEXT("\n");

	// Input pins
	for (const TObjectPtr<UPCGPin>& Pin : Node->GetInputPins())
	{
		if (!Pin)
		{
			continue;
		}
		Output += FString::Printf(TEXT("  In: \"%s\""), *Pin->Properties.Label.ToString());
		Output += FormatPinConnections(Graph, Pin);
		Output += TEXT("\n");
	}

	// Output pins
	for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
	{
		if (!Pin)
		{
			continue;
		}
		Output += FString::Printf(TEXT("  Out: \"%s\""), *Pin->Properties.Label.ToString());
		Output += FormatPinConnections(Graph, Pin);
		Output += TEXT("\n");
	}

	// Properties
	if (bIncludeProperties && Settings)
	{
		FString Props = ReadNodeProperties(Node);
		if (!Props.IsEmpty())
		{
			Output += TEXT("  Properties:\n");
			// Indent each line
			TArray<FString> Lines;
			Props.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				Output += FString::Printf(TEXT("    %s\n"), *Line);
			}
		}
	}

	return Output;
}

FString ClaireonPCGGraphHelpers::FormatPinConnections(const UPCGGraph* Graph, const UPCGPin* Pin)
{
	if (!Pin || !Pin->IsConnected())
	{
		return TEXT("");
	}

	FString Output;
	bool bIsOutput = Pin->IsOutputPin();

	for (const TObjectPtr<UPCGEdge>& Edge : Pin->Edges)
	{
		if (!Edge || !Edge->IsValid())
		{
			continue;
		}

		const UPCGPin* OtherPin = Edge->GetOtherPin(Pin);
		if (!OtherPin || !OtherPin->Node)
		{
			continue;
		}

		const UPCGNode* OtherNode = OtherPin->Node;
		FString OtherNodeName;

		if (Graph && OtherNode == Graph->GetInputNode())
		{
			OtherNodeName = TEXT("GraphInput");
		}
		else if (Graph && OtherNode == Graph->GetOutputNode())
		{
			OtherNodeName = TEXT("GraphOutput");
		}
		else
		{
			int32 OtherIndex = INDEX_NONE;
			if (Graph)
			{
				OtherIndex = Graph->GetNodes().IndexOfByKey(OtherNode);
			}
			if (OtherIndex != INDEX_NONE)
			{
				OtherNodeName = FString::Printf(TEXT("[%d] %s"), OtherIndex, *GetNodeDisplayName(OtherNode));
			}
			else
			{
				OtherNodeName = GetNodeDisplayName(OtherNode);
			}
		}

		if (bIsOutput)
		{
			Output += FString::Printf(TEXT(" -> %s.\"%s\""), *OtherNodeName, *OtherPin->Properties.Label.ToString());
		}
		else
		{
			Output += FString::Printf(TEXT(" <- %s.\"%s\""), *OtherNodeName, *OtherPin->Properties.Label.ToString());
		}
	}

	return Output;
}

// ============================================================================
// Property Access
// ============================================================================

FString ClaireonPCGGraphHelpers::ReadNodeProperties(const UPCGNode* Node)
{
	if (!Node)
	{
		return TEXT("");
	}

	const UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		return TEXT("");
	}

	FString Output;
	const UClass* SettingsClass = Settings->GetClass();

	for (TFieldIterator<FProperty> It(SettingsClass); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property)
		{
			continue;
		}

		// Skip internal/non-editable properties
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		// Skip properties from the base UPCGSettings class (we want user-facing settings)
		if (Property->GetOwnerClass() == UPCGSettings::StaticClass() ||
			Property->GetOwnerClass() == UObject::StaticClass())
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
		Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);

		Output += FString::Printf(TEXT("%s: %s\n"), *Property->GetName(), *ValueStr);
	}

	return Output;
}

bool ClaireonPCGGraphHelpers::SetNodeProperty(UPCGNode* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
{
	if (!Node)
	{
		OutError = TEXT("Node is null");
		return false;
	}

	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		OutError = TEXT("Node has no settings object");
		return false;
	}

	FProperty* Property = Settings->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		// Try case-insensitive search
		for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				Property = *It;
				break;
			}
		}
	}

	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Settings->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
	const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Settings, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' — value could not be parsed"), *PropertyName, *PropertyValue);
		return false;
	}

	return true;
}

// ============================================================================
// Graph Notification
// ============================================================================

void ClaireonPCGGraphHelpers::NotifyGraphChanged(UPCGGraph* Graph)
{
	if (!Graph)
	{
		return;
	}

	Graph->OnGraphChangedDelegate.Broadcast(Graph, EPCGChangeType::Structural);
}

// ============================================================================
// Available Settings Classes
// ============================================================================

TArray<FString> ClaireonPCGGraphHelpers::GetAvailableSettingsClasses()
{
	TArray<FString> Result;

	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(UPCGSettings::StaticClass(), DerivedClasses, true);

	for (const UClass* Class : DerivedClasses)
	{
		if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		FString Name = Class->GetName();
		Name.RemoveFromStart(TEXT("PCG"));
		Name.RemoveFromEnd(TEXT("Settings"));

		if (!Name.IsEmpty())
		{
			Result.Add(FString::Printf(TEXT("%s (%s)"), *Name, *Class->GetName()));
		}
		else
		{
			Result.Add(Class->GetName());
		}
	}

	Result.Sort();
	return Result;
}
