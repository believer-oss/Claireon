// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/ClaireonPCGEditorSync.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "PCGGraph.h"
#include "PCGComponent.h"
#include "PCGSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "Editor.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGCommon.h"
#include "Elements/PCGUserParameterGet.h"
#include "StructUtils/PropertyBag.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Data/PCGPointData.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Misc/EngineVersionComparison.h"
#include "PCGManagedResource.h"
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Data/PCGBasePointData.h"
#endif


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
	if (!IsValid(LoadedObj))
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UPCGGraph* Graph = Cast<UPCGGraph>(LoadedObj);
	if (!IsValid(Graph))
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
	if (!IsValid(Graph))
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
		if (IsValid(InputNode))
		{
			OutIndex = Nodes.IndexOfByKey(InputNode);
			return InputNode;
		}
	}
	if (Identifier.Equals(TEXT("output"), ESearchCase::IgnoreCase) || Identifier.Equals(TEXT("GraphOutput"), ESearchCase::IgnoreCase))
	{
		UPCGNode* OutputNode = Graph->GetOutputNode();
		if (IsValid(OutputNode))
		{
			OutIndex = Nodes.IndexOfByKey(OutputNode);
			return OutputNode;
		}
	}

	// Search by node title or settings class name
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		UPCGNode* Node = Nodes[i];
		if (!IsValid(Node))
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
	if (!IsValid(Node))
	{
		return TEXT("(null)");
	}

	if (Node->NodeTitle != NAME_None)
	{
		return Node->NodeTitle.ToString();
	}

	const UPCGSettings* Settings = Node->GetSettings();
	if (IsValid(Settings))
	{
		return GetSettingsShortName(Settings);
	}

	return TEXT("Unknown");
}

FString ClaireonPCGGraphHelpers::GetSettingsShortName(const UPCGSettings* Settings)
{
	if (!IsValid(Settings))
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
		if (IsValid(FoundClass))
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
	if (!IsValid(Graph))
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
	if (UPCGNode* InputNode = Graph->GetInputNode(); IsValid(InputNode))
	{
		Output += FormatNodeDetail(Graph, InputNode, INDEX_NONE, bFull);
		Output += TEXT("\n");
	}

	// Show all graph nodes
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const UPCGNode* Node = Nodes[i];
		if (!IsValid(Node))
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
	if (UPCGNode* OutputNode = Graph->GetOutputNode(); IsValid(OutputNode))
	{
		Output += FormatNodeDetail(Graph, OutputNode, INDEX_NONE, bFull);
		Output += TEXT("\n");
	}

	return Output;
}

FString ClaireonPCGGraphHelpers::FormatNodeDetail(const UPCGGraph* Graph, const UPCGNode* Node, int32 NodeIndex, bool bIncludeProperties)
{
	if (!IsValid(Node))
	{
		return TEXT("(null node)\n");
	}

	FString Output;

	// Node header
	bool bIsInput = (IsValid(Graph) && Node == Graph->GetInputNode());
	bool bIsOutput = (IsValid(Graph) && Node == Graph->GetOutputNode());

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
	if (IsValid(Settings) && !bIsInput && !bIsOutput)
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
	if (bIncludeProperties && IsValid(Settings))
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
	if (!IsValid(Pin) || !Pin->IsConnected())
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
		if (!IsValid(OtherPin) || !OtherPin->Node)
		{
			continue;
		}

		const UPCGNode* OtherNode = OtherPin->Node;
		FString OtherNodeName;

		if (IsValid(Graph) && OtherNode == Graph->GetInputNode())
		{
			OtherNodeName = TEXT("GraphInput");
		}
		else if (IsValid(Graph) && OtherNode == Graph->GetOutputNode())
		{
			OtherNodeName = TEXT("GraphOutput");
		}
		else
		{
			int32 OtherIndex = INDEX_NONE;
			if (IsValid(Graph))
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


namespace ClaireonPCGGraphHelpers_Readback
{
	/** Render user-facing properties and one level of instanced subobjects using writable dotted paths. */
	void AppendPropertyLines(const UObject* Object, const FString& Prefix, int32 Depth, FString& Output)
	{
		if (!IsValid(Object))
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			// Skip UObject bookkeeping; skip UPCGSettings base fields only at the root.
			const UClass* Owner = Property->GetOwnerClass();
			if (Owner == UObject::StaticClass() ||
				(Depth == 0 && Owner == UPCGSettings::StaticClass()))
			{
				continue;
			}

			const FString Name = Prefix.IsEmpty()
				? Property->GetName()
				: FString::Printf(TEXT("%s.%s"), *Prefix, *Property->GetName());

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);

			FString ValueStr;
			Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
			Output += FString::Printf(TEXT("%s: %s\n"), *Name, *ValueStr);

			if (Depth > 0)
			{
				continue;
			}

			// Recurse only into owned instanced objects, not referenced assets.
			const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
			if (!ObjectProperty || !Property->HasAnyPropertyFlags(CPF_InstancedReference))
			{
				continue;
			}

			if (const UObject* SubObject = ObjectProperty->GetObjectPropertyValue(ValuePtr))
			{
				AppendPropertyLines(SubObject, Name, Depth + 1, Output);
			}
		}
	}
} // namespace ClaireonPCGGraphHelpers_Readback

FString ClaireonPCGGraphHelpers::ReadNodeProperties(const UPCGNode* Node)
{
	if (!IsValid(Node))
	{
		return TEXT("");
	}

	const UPCGSettings* Settings = Node->GetSettings();
	if (!IsValid(Settings))
	{
		return TEXT("");
	}

	FString Output;
	ClaireonPCGGraphHelpers_Readback::AppendPropertyLines(Settings, FString(), 0, Output);
	return Output;
}

bool ClaireonPCGGraphHelpers::SetNodeProperty(UPCGNode* Node, const FString& PropertyPath, const FString& PropertyValue, FString& OutError)
{
	EPCGChangeType Unused = EPCGChangeType::None;
	return SetNodeProperty(Node, PropertyPath, PropertyValue, OutError, Unused);
}

bool ClaireonPCGGraphHelpers::SetNodeProperty(UPCGNode* Node, const FString& PropertyPath, const FString& PropertyValue,
	FString& OutError, EPCGChangeType& OutChangeType)
{
	OutChangeType = GetChangeTypeForOp(EPCGGraphEditOp::SetNodeProperty);

	if (!IsValid(Node))
	{
		OutError = TEXT("Node is null");
		return false;
	}

	UPCGSettings* Settings = Node->GetSettings();
	if (!IsValid(Settings))
	{
		OutError = TEXT("Node has no settings object");
		return false;
	}

	// Resolve nested/indexed paths with the shared property resolver.
	void* LeafContainer = nullptr;
	UObject* LeafOwner = Settings;
	FProperty* Property =
		ClaireonPropertyUtils::ResolvePropertyByPath(Settings, PropertyPath, LeafContainer, OutError, &LeafOwner);
	if (!Property || !LeafContainer)
	{
		return false;
	}
	if (!IsValid(LeafOwner))
	{
		LeafOwner = Settings;
	}

	// Reject deprecated fields whose reflected names omit _DEPRECATED and direct callers to live properties.
	if (Property->HasAnyPropertyFlags(CPF_Deprecated))
	{
		OutError = FString::Printf(
			TEXT("Property '%s' on %s is deprecated and is not read by the node; writing it would ")
			TEXT("silently do nothing. Use the live property instead -- it is usually nested (for ")
			TEXT("example 'Parameters.%s' or 'SubgraphInstance.Graph'). Call pcg_get_node_properties ")
			TEXT("to see the effective layout."),
			*PropertyPath, *Settings->GetClass()->GetName(), *Property->GetName());
		return false;
	}

	// Resolve graph-parameter names to GUIDs; well-formed GUIDs pass through.
	FString EffectiveValue = PropertyValue;
	const bool bIsUserParamGuid =
		Settings->IsA<UPCGUserParameterGetSettings>() && PropertyPath.Equals(TEXT("PropertyGuid"));
	if (bIsUserParamGuid)
	{
		FGuid AlreadyAGuid;
		if (!FGuid::Parse(PropertyValue, AlreadyAGuid))
		{
			const UPCGGraph* OwningGraph = Node->GetGraph();
			const FInstancedPropertyBag* Bag =
				OwningGraph ? OwningGraph->GetUserParametersStruct() : nullptr;
			const FPropertyBagPropertyDesc* Desc =
				Bag ? Bag->FindPropertyDescByName(FName(*PropertyValue)) : nullptr;
			if (!Desc)
			{
				TArray<FString> Known;
				if (const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr)
				{
					for (const FPropertyBagPropertyDesc& Candidate : BagStruct->GetPropertyDescs())
					{
						Known.Add(Candidate.Name.ToString());
					}
				}
				OutError = FString::Printf(
					TEXT("'%s' is neither a GUID nor the name of a parameter on this graph. Declare it ")
					TEXT("with pcg_add_user_parameter first (it returns property_guid). Known ")
					TEXT("parameters: %s"),
					*PropertyValue, Known.Num() ? *FString::Join(Known, TEXT(", ")) : TEXT("(none)"));
				return false;
			}
			EffectiveValue = Desc->ID.ToString(EGuidFormats::Digits);
		}
	}

	// Array-index leaves resolve to element pointers; other leaves resolve to their container.
	const bool bTrailingIndex = PropertyPath.TrimEnd().EndsWith(TEXT("]"));
	void* ValuePtr = bTrailingIndex ? LeafContainer : Property->ContainerPtrToValuePtr<void>(LeafContainer);

	// Record Settings and the nested LeafOwner so undo includes the actual modified bytes.
	Settings->Modify();
	if (LeafOwner != Settings)
	{
		LeafOwner->Modify();
	}

	// Object import can report success while resolving to null; preserve the previous object on invalid paths.
	FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
	UObject* PreviousObject = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(ValuePtr) : nullptr;

#if WITH_EDITOR
	// Observe the node change delegate to capture the actual pin-rebuild flags from PostEditChangeProperty.
	EPCGChangeType Observed = EPCGChangeType::None;
	bool bObservedChange = false;
	const FDelegateHandle NodeChangedHandle = Node->OnNodeChangedDelegate.AddLambda(
		[&Observed, &bObservedChange](UPCGNode* /*ChangedNode*/, EPCGChangeType ChangeType)
		{
			Observed |= ChangeType;
			bObservedChange = true;
		});

	// Use the shared writer for native setters, object paths, container clears, and instancing.
	// ValueSet is required because PCG ignores Interactive changes.
	const bool bWritten =
		ClaireonPropertyUtils::WritePropertyByPath(Settings, PropertyPath, EffectiveValue, OutError);

	Node->OnNodeChangedDelegate.Remove(NodeChangedHandle);

	if (!bWritten)
	{
		return false;
	}

	if (ObjectProperty)
	{
		const FString Trimmed = PropertyValue.TrimStartAndEnd();
		const bool bMeantToClear = Trimmed.IsEmpty()
			|| Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("nullptr"), ESearchCase::IgnoreCase);

		if (!bMeantToClear && ObjectProperty->GetObjectPropertyValue(ValuePtr) == nullptr)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, PreviousObject);
			const UClass* Expected = ObjectProperty->PropertyClass;
			OutError = FString::Printf(
				TEXT("Property '%s' expects a %s; '%s' did not resolve to one, so the property was left unchanged"),
				*PropertyPath,
				IsValid(Expected) ? *Expected->GetName() : TEXT("object"),
				*PropertyValue);
			return false;
		}
	}

	// The shared writer notifies the root; also notify a nested leaf owner for its own edit-time effects.
	if (LeafOwner != Settings)
	{
		FPropertyChangedEvent OwnerChangedEvent(Property, EPropertyChangeType::ValueSet);
		LeafOwner->PostEditChangeProperty(OwnerChangedEvent);
	}

	if (bObservedChange && Observed != EPCGChangeType::None)
	{
		OutChangeType = Observed;
	}
	// Keep conservative invalidation if no change notification was observed.

	// Refresh downstream dynamic pins only when the pin layout changed.
	if (EnumHasAnyFlags(OutChangeType, EPCGChangeType::Node) || Settings->HasDynamicPins())
	{
		TSet<UPCGNode*> TouchedNodes;
		OutChangeType |= Node->PropagateDynamicPinTypes(TouchedNodes);
		OutChangeType |= EPCGChangeType::Node;
	}
#else
	if (!ClaireonPropertyUtils::WritePropertyByPath(Settings, PropertyPath, EffectiveValue, OutError))
	{
		return false;
	}
#endif

	// Synchronize PropertyName with PropertyGuid, then rebuild pins through SetSettingsInterface.
	if (bIsUserParamGuid)
	{
		if (UPCGUserParameterGetSettings* GetSettings = Cast<UPCGUserParameterGetSettings>(Settings))
		{
			const UPCGGraph* OwningGraph = Node->GetGraph();
			const FInstancedPropertyBag* Bag =
				OwningGraph ? OwningGraph->GetUserParametersStruct() : nullptr;
			if (const FPropertyBagPropertyDesc* Desc =
					Bag ? Bag->FindPropertyDescByID(GetSettings->PropertyGuid) : nullptr)
			{
				// Assign PropertyName directly because UpdatePropertyName is not exported.
				GetSettings->PropertyName = Desc->Name;
			}
			Node->SetSettingsInterface(GetSettings);
			OutChangeType |= EPCGChangeType::Node;
		}
	}

	return true;
}

void ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(const UPCGGraph* Graph,
	TArray<UPCGComponent*>& OutComponents, int32& OutSkipped, int32 MaxComponents,
	const FString& ActorLabelFilter)
{
	OutSkipped = 0;

	if (!IsValid(Graph) || !IsValid(GEditor))
	{
		return;
	}

	const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!IsValid(EditorWorld))
	{
		return;
	}

	for (TObjectIterator<UPCGComponent> It; It; ++It)
	{
		UPCGComponent* Component = *It;

		// GetGraph resolves graph-instance indirection.
		if (!IsValid(Component) || Component->GetGraph() != Graph)
		{
			continue;
		}
		if (Component->IsTemplate() || Component->GetWorld() != EditorWorld || !IsValid(Component->GetOwner()))
		{
			continue;
		}
		// Filter by actor before applying the component cap.
		if (!ActorLabelFilter.IsEmpty() && Component->GetOwner()->GetActorLabel() != ActorLabelFilter)
		{
			continue;
		}
		if (OutComponents.Num() >= MaxComponents)
		{
			++OutSkipped;
			continue;
		}
		OutComponents.Add(Component);
	}
}

int32 ClaireonPCGGraphHelpers::CountLiveComponentsUsingGraph(const UPCGGraph* Graph)
{
	TArray<UPCGComponent*> Components;
	int32 Skipped = 0;
	CollectLiveComponentsUsingGraph(Graph, Components, Skipped, TNumericLimits<int32>::Max());
	return Components.Num();
}

int32 ClaireonPCGGraphHelpers::CountManagedISMInstances(const UPCGComponent* Component)
{
	if (!IsValid(Component))
	{
		return 0;
	}

	int32 Total = 0;
	// ForEachManagedResource is non-const on the component; the walk itself only reads.
	const_cast<UPCGComponent*>(Component)->ForEachManagedResource([&Total](UPCGManagedResource* Resource)
	{
		const UPCGManagedISMComponent* Managed = Cast<UPCGManagedISMComponent>(Resource);
		const UInstancedStaticMeshComponent* Ism = Managed ? Managed->GetComponent() : nullptr;
		if (IsValid(Ism))
		{
			Total += Ism->GetInstanceCount();
		}
	});
	return Total;
}

void ClaireonPCGGraphHelpers::FPointStat::Add(double Value)
{
	Min = FMath::Min(Min, Value);
	Max = FMath::Max(Max, Value);
	Sum += Value;
	++Count;
}

TSharedPtr<FJsonObject> ClaireonPCGGraphHelpers::FPointStat::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("min"), Count ? Min : 0.0);
	Obj->SetNumberField(TEXT("max"), Count ? Max : 0.0);
	Obj->SetNumberField(TEXT("mean"), Mean());
	return Obj;
}

namespace ClaireonPCGGraphHelpers_PointStats
{
	/** Include default metadata values for points with invalid entry keys when folding numeric attributes. */
	template <typename T>
	static bool FoldTyped(const UPCGMetadata& Metadata, FName Name, const TArray<int64>& EntryKeys,
		ClaireonPCGGraphHelpers::FPointStat& Stat)
	{
		const FPCGMetadataAttribute<T>* Attr = Metadata.GetConstTypedAttribute<T>(Name);
		if (!Attr)
		{
			return false;
		}
		for (const int64 Key : EntryKeys)
		{
			Stat.Add(static_cast<double>(Attr->GetValueFromItemKey(Key)));
		}
		return true;
	}

	static bool FoldNumeric(const UPCGMetadata& Metadata, FName Name, EPCGMetadataTypes Type,
		const TArray<int64>& EntryKeys, ClaireonPCGGraphHelpers::FPointStat& Stat)
	{
		switch (Type)
		{
		case EPCGMetadataTypes::Float:     return FoldTyped<float>(Metadata, Name, EntryKeys, Stat);
		case EPCGMetadataTypes::Double:    return FoldTyped<double>(Metadata, Name, EntryKeys, Stat);
		case EPCGMetadataTypes::Integer32: return FoldTyped<int32>(Metadata, Name, EntryKeys, Stat);
		case EPCGMetadataTypes::Integer64: return FoldTyped<int64>(Metadata, Name, EntryKeys, Stat);
		default:                           return false;
		}
	}
}

bool ClaireonPCGGraphHelpers::AccumulatePointData(const UPCGData* Data, FPointStatistics& Out)
{
	using namespace ClaireonPCGGraphHelpers_PointStats;

	TArray<int64> EntryKeys;

#if UE_VERSION_OLDER_THAN(5, 6, 0)
	const UPCGPointData* PointData = Cast<UPCGPointData>(Data);
	if (!PointData)
	{
		return false;
	}
	const TArray<FPCGPoint>& Points = PointData->GetPoints();
	Out.Points += Points.Num();
	EntryKeys.Reserve(Points.Num());
	for (const FPCGPoint& Point : Points)
	{
		Out.Density.Add(Point.Density);
		Out.PositionZ.Add(Point.Transform.GetLocation().Z);
		EntryKeys.Add(Point.MetadataEntry);
	}
#else
	// Use the common point-data base accessors for both storage layouts.
	const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data);
	if (!PointData)
	{
		return false;
	}
	const int32 NumPoints = PointData->GetNumPoints();
	Out.Points += NumPoints;
	EntryKeys.Reserve(NumPoints);
	for (int32 Index = 0; Index < NumPoints; ++Index)
	{
		Out.Density.Add(PointData->GetDensity(Index));
		Out.PositionZ.Add(PointData->GetTransform(Index).GetLocation().Z);
		EntryKeys.Add(PointData->GetMetadataEntry(Index));
	}
#endif

	const UPCGMetadata* Metadata = PointData->ConstMetadata();
	if (!Metadata)
	{
		return true;
	}

	TArray<FName> AttributeNames;
	TArray<EPCGMetadataTypes> AttributeTypes;
	Metadata->GetAttributes(AttributeNames, AttributeTypes);
	for (int32 i = 0; i < AttributeNames.Num(); ++i)
	{
		FPointStat& Stat = Out.Attributes.FindOrAdd(AttributeNames[i]);
		if (!FoldNumeric(*Metadata, AttributeNames[i], AttributeTypes[i], EntryKeys, Stat))
		{
			Out.NonNumericAttributes.Add(AttributeNames[i]);
		}
	}
	return true;
}

bool ClaireonPCGGraphHelpers::GenerateAndWait(const TArray<UPCGComponent*>& Components, int32 TimeoutMs,
	bool bForce, int32& OutFrames, FString& OutError)
{
	OutFrames = 0;

	if (Components.IsEmpty())
	{
		OutError = TEXT("No components to generate");
		return false;
	}

	UPCGSubsystem* Subsystem = nullptr;
	for (const UPCGComponent* Component : Components)
	{
		if (IsValid(Component) && IsValid(Component->GetWorld()))
		{
			Subsystem = UPCGSubsystem::GetInstance(Component->GetWorld());
			break;
		}
	}
	if (!IsValid(Subsystem))
	{
		OutError = TEXT("No UPCGSubsystem for these components, so generation cannot be driven");
		return false;
	}

	for (UPCGComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}
		if (bForce)
		{
			Component->DirtyGenerated(EPCGComponentDirtyFlag::Actor);
		}
		Component->Generate();
	}

	TimeoutMs = FMath::Clamp(TimeoutMs, 1000, 300000);
	const double DeltaSeconds = 1.0 / 60.0;
	const double Deadline = FPlatformTime::Seconds() + (TimeoutMs / 1000.0);

	auto AnyStillGenerating = [&Components]()
	{
		for (const UPCGComponent* Component : Components)
		{
			if (IsValid(Component) && Component->IsGenerating())
			{
				return true;
			}
		}
		return false;
	};

	// Pump before polling because Generate may not have assigned CurrentGenerationTask yet.
	constexpr int32 MinimumFrames = 4;
	while (OutFrames < MinimumFrames || AnyStillGenerating())
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			break;
		}

		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		if (IsValid(GEditor))
		{
			GEditor->GetTimerManager()->Tick(static_cast<float>(DeltaSeconds));
		}
		Subsystem->Tick(static_cast<float>(DeltaSeconds));
		++OutFrames;
	}

	if (AnyStillGenerating())
	{
		OutError = FString::Printf(
			TEXT("Generation did not finish within %d ms (still running after %d pumped frames). ")
			TEXT("Raise timeout_ms, or generate fewer components per call."), TimeoutMs, OutFrames);
		return false;
	}
	return true;
}


EPCGChangeType ClaireonPCGGraphHelpers::GetChangeTypeForOp(EPCGGraphEditOp Op)
{
	// Use conservative invalidation when the change type is unknown.
	switch (Op)
	{
	case EPCGGraphEditOp::Connect:
	case EPCGGraphEditOp::Disconnect:
	case EPCGGraphEditOp::DisconnectAll:
		return EPCGChangeType::Edge | EPCGChangeType::Structural;

	case EPCGGraphEditOp::AddNode:
	case EPCGGraphEditOp::RemoveNode:
		return EPCGChangeType::Node | EPCGChangeType::Structural;

	case EPCGGraphEditOp::SetNodeProperty:
		// Fallback flags apply only when engine-derived property flags are unavailable.
		return EPCGChangeType::Settings | EPCGChangeType::Node;

	case EPCGGraphEditOp::Batch:
		// The pause scope unions per-edit flags; this supplies a minimum for otherwise unspecified batches.
		return EPCGChangeType::Node | EPCGChangeType::Edge | EPCGChangeType::Structural;

	case EPCGGraphEditOp::Count:
	default:
		return EPCGChangeType::Structural;
	}
}

FString ClaireonPCGGraphHelpers::ChangeTypeToString(EPCGChangeType ChangeType)
{
	if (ChangeType == EPCGChangeType::None)
	{
		return TEXT("None");
	}

	TArray<FString> Parts;
	auto Append = [&Parts, ChangeType](EPCGChangeType Flag, const TCHAR* Name)
	{
		if (EnumHasAnyFlags(ChangeType, Flag))
		{
			Parts.Add(Name);
		}
	};

	Append(EPCGChangeType::Cosmetic, TEXT("Cosmetic"));
	Append(EPCGChangeType::Settings, TEXT("Settings"));
	Append(EPCGChangeType::Input, TEXT("Input"));
	Append(EPCGChangeType::Edge, TEXT("Edge"));
	Append(EPCGChangeType::Node, TEXT("Node"));
	Append(EPCGChangeType::Structural, TEXT("Structural"));
	Append(EPCGChangeType::GenerationGrid, TEXT("GenerationGrid"));
	Append(EPCGChangeType::ShaderSource, TEXT("ShaderSource"));

	return FString::Join(Parts, TEXT("|"));
}

void ClaireonPCGGraphHelpers::NotifyGraphChanged(UPCGGraph* Graph, EPCGChangeType ChangeType)
{
	if (!IsValid(Graph))
	{
		return;
	}

#if WITH_EDITOR
	// Use the engine notification path to invalidate compiled and grid-size caches; pause scopes accumulate it.
	Graph->ForceNotificationForEditor(ChangeType);

	// Queue structural view reconstruction after the transaction; settings and cosmetic edits preserve view state.
	if (EnumHasAnyFlags(ChangeType, EPCGChangeType::Structural | EPCGChangeType::Node | EPCGChangeType::Edge))
	{
		ClaireonPCGEditorSync::RequestReconstruct(Graph);
	}
#endif
}

void ClaireonPCGGraphHelpers::NotifyGraphChanged(UPCGGraph* Graph, EPCGGraphEditOp Op)
{
	NotifyGraphChanged(Graph, GetChangeTypeForOp(Op));
}

ClaireonPCGGraphHelpers::FPCGGraphNotifyPauseScope::FPCGGraphNotifyPauseScope(UPCGGraph* InGraph)
	: Graph(InGraph)
{
#if WITH_EDITOR
	if (IsValid(InGraph))
	{
		InGraph->DisableNotificationsForEditor();
		bPaused = true;
	}
#endif
}

ClaireonPCGGraphHelpers::FPCGGraphNotifyPauseScope::~FPCGGraphNotifyPauseScope()
{
#if WITH_EDITOR
	// Release only an acquired pause on a still-live graph.
	if (bPaused)
	{
		if (UPCGGraph* Live = Graph.Get(); IsValid(Live))
		{
			Live->EnableNotificationsForEditor();
		}
		bPaused = false;
	}
#endif
}


namespace ClaireonPCGGraphHelpers_Placement
{
	constexpr int32 OriginX = 352;
	constexpr int32 OriginY = 16;
	constexpr int32 StepX = 288;
	constexpr int32 StepY = 176;
	constexpr int32 RowsPerColumn = 6;
	constexpr int32 OccupiedRadius = 32;

	/** True when some other node already sits at (or nearly at) this spot. */
	static bool IsSlotTaken(const UPCGGraph* Graph, const UPCGNode* Exclude, int32 X, int32 Y)
	{
		if (!IsValid(Graph))
		{
			return false;
		}

		auto Blocks = [Exclude, X, Y](const UPCGNode* Other)
		{
			if (!IsValid(Other) || Other == Exclude)
			{
				return false;
			}
			return FMath::Abs(Other->PositionX - X) < OccupiedRadius
				&& FMath::Abs(Other->PositionY - Y) < OccupiedRadius;
		};

		if (Blocks(Graph->GetInputNode()) || Blocks(Graph->GetOutputNode()))
		{
			return true;
		}
		for (const UPCGNode* Other : Graph->GetNodes())
		{
			if (Blocks(Other))
			{
				return true;
			}
		}
		return false;
	}

	/** First grid slot at or after StartIndex that nothing else occupies. */
	static void FindFreeSlot(const UPCGGraph* Graph, const UPCGNode* Exclude, int32 StartIndex, int32& OutX, int32& OutY)
	{
		// Bound the slot search; at the cap, allow shared slots rather than looping indefinitely.
		constexpr int32 MaxSlots = 512;
		for (int32 Slot = StartIndex; Slot < StartIndex + MaxSlots; ++Slot)
		{
			const int32 X = OriginX + StepX * (Slot / RowsPerColumn);
			const int32 Y = OriginY + StepY * (Slot % RowsPerColumn);
			if (!IsSlotTaken(Graph, Exclude, X, Y))
			{
				OutX = X;
				OutY = Y;
				return;
			}
		}

		OutX = OriginX + StepX * (StartIndex / RowsPerColumn);
		OutY = OriginY + StepY * (StartIndex % RowsPerColumn);
	}
} // namespace ClaireonPCGGraphHelpers_Placement

void ClaireonPCGGraphHelpers::AssignDefaultNodePosition(UPCGGraph* Graph, UPCGNode* Node)
{
	using namespace ClaireonPCGGraphHelpers_Placement;

	if (!IsValid(Graph) || !IsValid(Node))
	{
		return;
	}

#if WITH_EDITORONLY_DATA
	if (Node->PositionX != 0 || Node->PositionY != 0)
	{
		return;
	}

	int32 X = OriginX;
	int32 Y = OriginY;
	FindFreeSlot(Graph, Node, /*StartIndex=*/0, X, Y);
	Node->PositionX = X;
	Node->PositionY = Y;
#endif
}

void ClaireonPCGGraphHelpers::ComputeDisplayPosition(const UPCGGraph* Graph, const UPCGNode* Node, int32& OutX, int32& OutY)
{
	using namespace ClaireonPCGGraphHelpers_Placement;

	OutX = OriginX;
	OutY = OriginY;

	if (!IsValid(Graph) || !IsValid(Node))
	{
		return;
	}

#if WITH_EDITORONLY_DATA
	if (Node->PositionX != 0 || Node->PositionY != 0)
	{
		OutX = Node->PositionX;
		OutY = Node->PositionY;
		return;
	}

	const int32 Index = FMath::Max(0, Graph->GetNodes().IndexOfByKey(Node));
	FindFreeSlot(Graph, Node, Index, OutX, OutY);
#endif
}


TArray<FString> ClaireonPCGGraphHelpers::GetAvailableSettingsClasses()
{
	TArray<FString> Result;

	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(UPCGSettings::StaticClass(), DerivedClasses, true);

	for (const UClass* Class : DerivedClasses)
	{
		if (!IsValid(Class) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
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
