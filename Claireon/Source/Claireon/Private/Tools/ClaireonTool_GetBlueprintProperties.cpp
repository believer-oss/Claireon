// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "Animation/AnimBlueprint.h"
#include "WidgetBlueprint.h"
#include "UObject/CoreNetTypes.h"

FString ClaireonTool_GetBlueprintProperties::GetName() const
{
	return TEXT("claireon.blueprint_get_properties");
}

FString ClaireonTool_GetBlueprintProperties::GetDescription() const
{
	return TEXT("Read a Blueprint asset's public interface: functions, variables, components, parent class, and implemented interfaces. Works with standard Blueprints, Animation Blueprints, Widget Blueprints, and Blueprint Function Libraries. Use this to understand what a Blueprint exposes before inspecting its internal graph structure.");
}

TSharedPtr<FJsonObject> ClaireonTool_GetBlueprintProperties::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"), TEXT("Unreal content path of the Blueprint asset (e.g., /Game/Characters/BP_PlayerCharacter). Must start with /Game/."));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	// include_inherited - optional
	TSharedPtr<FJsonObject> InheritedProp = MakeShared<FJsonObject>();
	InheritedProp->SetStringField(TEXT("type"), TEXT("boolean"));
	InheritedProp->SetStringField(TEXT("description"), TEXT("Include inherited properties from parent classes. Default: false."));
	Properties->SetObjectField(TEXT("include_inherited"), InheritedProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GetBlueprintProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Validate asset_path
	if (!Arguments->HasField(TEXT("asset_path")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path. Use find_assets to locate Blueprint paths."));
	}

	FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("%s Use find_assets to locate valid Blueprint paths."), *ResolveResult.Error));
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	bool bIncludeInherited = false;
	if (Arguments->HasField(TEXT("include_inherited")))
	{
		bIncludeInherited = Arguments->GetBoolField(TEXT("include_inherited"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath, LoadError);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("%s Use find_assets to locate valid Blueprint paths."), *LoadError));
	}

	// Gather parent class info
	FString ParentClassName = TEXT("None");
	if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetSuperClass())
	{
		ParentClassName = Blueprint->GeneratedClass->GetSuperClass()->GetName();
	}

	FString BlueprintType = GetBlueprintTypeName(Blueprint);

	// Build variables array
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), FormatVariableType(Var.VarType));
		VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarObj->SetBoolField(TEXT("is_exposed"), (Var.PropertyFlags & CPF_BlueprintVisible) != 0);
		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	// Build functions array
	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Graph->GetName());

		// Find function entry node for signature details
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (EntryNode)
			{
				break;
			}
		}

		bool bIsPure = false;
		bool bIsEvent = false;
		FString ReturnType = TEXT("void");
		TArray<TSharedPtr<FJsonValue>> ParamsArray;

		if (EntryNode)
		{
			bIsPure = (EntryNode->GetFunctionFlags() & FUNC_BlueprintPure) != 0;

			// Collect parameters (output pins on entry node, excluding exec)
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Pin->GetName());
					ParamObj->SetStringField(TEXT("type"), FormatVariableType(Pin->PinType));
					ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
			}

			// Check for return type (input pins on entry node, excluding exec)
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					ReturnType = FormatVariableType(Pin->PinType);
					break;
				}
			}
		}

		FuncObj->SetStringField(TEXT("return_type"), ReturnType);
		FuncObj->SetArrayField(TEXT("parameters"), ParamsArray);
		FuncObj->SetBoolField(TEXT("is_pure"), bIsPure);
		FuncObj->SetBoolField(TEXT("is_event"), bIsEvent);
		FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	// Build components array
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (Blueprint->SimpleConstructionScript)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();

		TFunction<void(USCS_Node*, USCS_Node*)> CollectComponents = [&](USCS_Node* Node, USCS_Node* ParentNode)
		{
			if (!Node)
			{
				return;
			}

			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
			CompObj->SetBoolField(TEXT("is_root"), Node == DefaultRoot);

			if (ParentNode)
			{
				CompObj->SetStringField(TEXT("parent_component"), ParentNode->GetVariableName().ToString());
			}

			TArray<TSharedPtr<FJsonValue>> ChildrenArray;
			for (USCS_Node* ChildNode : Node->GetChildNodes())
			{
				ChildrenArray.Add(MakeShared<FJsonValueString>(ChildNode->GetVariableName().ToString()));
			}
			CompObj->SetArrayField(TEXT("children"), ChildrenArray);

			ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));

			for (USCS_Node* ChildNode : Node->GetChildNodes())
			{
				CollectComponents(ChildNode, Node);
			}
		};

		for (USCS_Node* RootNode : SCS->GetRootNodes())
		{
			CollectComponents(RootNode, nullptr);
		}
	}

	// Build interfaces array
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			InterfacesArray.Add(MakeShared<FJsonValueString>(Interface.Interface->GetName()));
		}
	}

	// Build result data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("parent_class"), ParentClassName);
	Data->SetStringField(TEXT("blueprint_type"), BlueprintType);
	Data->SetArrayField(TEXT("variables"), VariablesArray);
	Data->SetArrayField(TEXT("functions"), FunctionsArray);
	Data->SetArrayField(TEXT("components"), ComponentsArray);
	Data->SetArrayField(TEXT("interfaces"), InterfacesArray);

	// Build metadata object (UBlueprint-level properties, not CDO)
	TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
	MetadataObj->SetStringField(TEXT("namespace"), Blueprint->BlueprintNamespace);
	MetadataObj->SetStringField(TEXT("display_name"), Blueprint->BlueprintDisplayName);
	MetadataObj->SetStringField(TEXT("description"), Blueprint->BlueprintDescription);
	MetadataObj->SetStringField(TEXT("category"), Blueprint->BlueprintCategory);

	// hide_categories as JSON array
	TArray<TSharedPtr<FJsonValue>> HideCategoriesArray;
	for (const FString& Cat : Blueprint->HideCategories)
	{
		HideCategoriesArray.Add(MakeShared<FJsonValueString>(Cat));
	}
	MetadataObj->SetArrayField(TEXT("hide_categories"), HideCategoriesArray);

	MetadataObj->SetBoolField(TEXT("is_abstract"), Blueprint->bGenerateAbstractClass != 0);
	MetadataObj->SetBoolField(TEXT("is_const"), Blueprint->bGenerateConstClass != 0);
	MetadataObj->SetBoolField(TEXT("is_deprecated"), Blueprint->bDeprecate != 0);

	// CompileMode enum to string
	FString CompileModeStr = TEXT("Default");
	if (const UEnum* CompileModeEnum = StaticEnum<EBlueprintCompileMode>())
	{
		CompileModeStr = CompileModeEnum->GetNameStringByValue(static_cast<int64>(Blueprint->CompileMode));
	}
	MetadataObj->SetStringField(TEXT("compile_mode"), CompileModeStr);

	// num_replicated_properties from BlueprintGeneratedClass (read-only)
	int32 NumReplicatedProps = 0;
	if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
	{
		NumReplicatedProps = BPGC->NumReplicatedProperties;
	}
	MetadataObj->SetNumberField(TEXT("num_replicated_properties"), NumReplicatedProps);

	Data->SetObjectField(TEXT("metadata"), MetadataObj);

	// Extract asset name for summary
	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	FString Summary = FString::Printf(TEXT("%s: %d variables, %d functions, %d components (parent: %s)"),
		*AssetName,
		VariablesArray.Num(),
		FunctionsArray.Num(),
		ComponentsArray.Num(),
		*ParentClassName);

	return MakeSuccessResult(Data, Summary);
}

UBlueprint* ClaireonTool_GetBlueprintProperties::LoadBlueprintFromPath(const FString& AssetPath, FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint at path: %s"), *AssetPath);
		return nullptr;
	}

	return Blueprint;
}


FString ClaireonTool_GetBlueprintProperties::FormatVariables(const UBlueprint* Blueprint, bool bIncludeInherited)
{
	if (!Blueprint)
	{
		return FString();
	}

	TArray<FString> VariableLines;

	// User-defined variables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		FString VarLine = FString::Printf(TEXT("- %s (%s)"),
			*Var.VarName.ToString(),
			*FormatVariableType(Var.VarType));

		// Add flags
		TArray<FString> Flags = ClaireonBlueprintHelpers::FormatPropertyFlags(Var.PropertyFlags);

		if (Flags.Num() > 0)
		{
			VarLine += TEXT(" [");
			VarLine += FString::Join(Flags, TEXT(", "));
			VarLine += TEXT("]");
		}

		// Add default value if available
		if (!Var.DefaultValue.IsEmpty())
		{
			VarLine += FString::Printf(TEXT(" Default: %s"), *Var.DefaultValue);
		}

		// Add category if not empty or "Default"
		FString CategoryStr = Var.Category.ToString();
		if (!CategoryStr.IsEmpty() && CategoryStr != TEXT("Default"))
		{
			VarLine += FString::Printf(TEXT(" Category: %s"), *CategoryStr);
		}

		// Add replication info
		if (Var.PropertyFlags & CPF_Net)
		{
			if (Var.PropertyFlags & CPF_RepNotify)
			{
				VarLine += TEXT(" Replication: RepNotify");
				if (Var.RepNotifyFunc != NAME_None)
				{
					VarLine += FString::Printf(TEXT(" RepNotifyFunc: %s"), *Var.RepNotifyFunc.ToString());
				}
			}
			else
			{
				VarLine += TEXT(" Replication: Replicated");
			}

			if (Var.ReplicationCondition != COND_None)
			{
				const UEnum* CondEnum = StaticEnum<ELifetimeCondition>();
				if (CondEnum)
				{
					FString CondName = CondEnum->GetNameStringByValue(static_cast<int64>(Var.ReplicationCondition));
					if (!CondName.IsEmpty())
					{
						VarLine += FString::Printf(TEXT(" ReplicationCondition: %s"), *CondName);
					}
				}
			}
		}

		// Add metadata entries
		if (Var.MetaDataArray.Num() > 0)
		{
			TArray<FString> MetaEntries;
			for (const FBPVariableMetaDataEntry& MetaEntry : Var.MetaDataArray)
			{
				MetaEntries.Add(FString::Printf(TEXT("%s=%s"), *MetaEntry.DataKey.ToString(), *MetaEntry.DataValue));
			}
			VarLine += FString::Printf(TEXT(" Metadata: {%s}"), *FString::Join(MetaEntries, TEXT(", ")));
		}

		VariableLines.Add(VarLine);
	}

	// Inherited variables (if requested)
	if (bIncludeInherited && Blueprint->GeneratedClass)
	{
		UClass* ParentClass = Blueprint->GeneratedClass->GetSuperClass();
		if (ParentClass && ParentClass != UObject::StaticClass())
		{
			for (TFieldIterator<FProperty> PropIt(ParentClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property)
				{
					continue;
				}

				// Only show properties that are Blueprint-visible
				if (!(Property->PropertyFlags & CPF_BlueprintVisible))
				{
					continue;
				}

				FString VarLine = FString::Printf(TEXT("- %s (%s) [Inherited]"),
					*Property->GetName(),
					*Property->GetClass()->GetName());

				VariableLines.Add(VarLine);
			}
		}
	}

	if (VariableLines.Num() == 0)
	{
		return FString();
	}

	FString Output = FString::Printf(TEXT("## Variables (%d)\n"), VariableLines.Num());
	for (const FString& Line : VariableLines)
	{
		Output += Line + TEXT("\n");
	}

	return Output;
}

FString ClaireonTool_GetBlueprintProperties::FormatFunctions(const UBlueprint* Blueprint, bool bIncludeInherited)
{
	if (!Blueprint)
	{
		return FString();
	}

	TArray<FString> FunctionLines;

	// User-defined functions from function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		// Find function entry node to get signature
		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			if (EntryNode)
			{
				break;
			}
		}

		FString FuncLine = FString::Printf(TEXT("- %s("), *Graph->GetName());

		// Add parameters
		TArray<FString> Params;
		if (EntryNode)
		{
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					FString ParamStr = FString::Printf(TEXT("%s %s"),
						*FormatVariableType(Pin->PinType),
						*Pin->GetName());
					Params.Add(ParamStr);
				}
			}
		}

		FuncLine += FString::Join(Params, TEXT(", "));
		FuncLine += TEXT(")");

		// Add return type
		if (EntryNode)
		{
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					FuncLine += FString::Printf(TEXT(" -> %s"), *FormatVariableType(Pin->PinType));
					break;
				}
			}
		}

		// Add flags
		TArray<FString> Flags;
		if (EntryNode)
		{
			if (EntryNode->MetaData.bCallInEditor)
			{
				Flags.Add(TEXT("CallInEditor"));
			}
			if (EntryNode->GetFunctionFlags() & FUNC_BlueprintPure)
			{
				Flags.Add(TEXT("Pure"));
			}
			if (EntryNode->GetFunctionFlags() & FUNC_Const)
			{
				Flags.Add(TEXT("Const"));
			}
		}

		if (Flags.Num() > 0)
		{
			FuncLine += TEXT(" [");
			FuncLine += FString::Join(Flags, TEXT(", "));
			FuncLine += TEXT("]");
		}

		FunctionLines.Add(FuncLine);
	}

	// Inherited functions (if requested)
	if (bIncludeInherited && Blueprint->GeneratedClass)
	{
		UClass* ParentClass = Blueprint->GeneratedClass->GetSuperClass();
		if (ParentClass && ParentClass != UObject::StaticClass())
		{
			for (TFieldIterator<UFunction> FuncIt(ParentClass, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
			{
				UFunction* Function = *FuncIt;
				if (!Function)
				{
					continue;
				}

				// Only show Blueprint-accessible functions
				if (!(Function->FunctionFlags & FUNC_BlueprintCallable) &&
					!(Function->FunctionFlags & FUNC_BlueprintPure))
				{
					continue;
				}

				FString FuncLine = FString::Printf(TEXT("- %s() [Inherited]"), *Function->GetName());
				FunctionLines.Add(FuncLine);
			}
		}
	}

	if (FunctionLines.Num() == 0)
	{
		return FString();
	}

	FString Output = FString::Printf(TEXT("## Functions (%d)\n"), FunctionLines.Num());
	for (const FString& Line : FunctionLines)
	{
		Output += Line + TEXT("\n");
	}

	return Output;
}

FString ClaireonTool_GetBlueprintProperties::FormatComponents(const UBlueprint* Blueprint)
{
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return FString();
	}

	TArray<FString> ComponentLines;
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	// Helper lambda to recursively format component hierarchy
	TFunction<void(USCS_Node*, int32)> FormatComponentNode = [&](USCS_Node* Node, int32 Depth)
	{
		if (!Node)
		{
			return;
		}

		FString Indent;
		for (int32 i = 0; i < Depth; ++i)
		{
			Indent += TEXT("  ");
		}

		FString CompLine = Indent;
		CompLine += FString::Printf(TEXT("- %s (%s)"),
			*Node->GetVariableName().ToString(),
			Node->ComponentClass ? *Node->ComponentClass->GetName() : TEXT("Unknown"));

		// Mark root components
		if (Node == SCS->GetDefaultSceneRootNode())
		{
			CompLine += TEXT(" [Root]");
		}

		ComponentLines.Add(CompLine);

		// Recurse to children
		for (USCS_Node* ChildNode : Node->GetChildNodes())
		{
			FormatComponentNode(ChildNode, Depth + 1);
		}
	};

	// Start with root nodes
	for (USCS_Node* RootNode : SCS->GetRootNodes())
	{
		FormatComponentNode(RootNode, 0);
	}

	if (ComponentLines.Num() == 0)
	{
		return FString();
	}

	FString Output = FString::Printf(TEXT("## Components (%d)\n"), ComponentLines.Num());
	for (const FString& Line : ComponentLines)
	{
		Output += Line + TEXT("\n");
	}

	return Output;
}

FString ClaireonTool_GetBlueprintProperties::FormatInterfaces(const UBlueprint* Blueprint)
{
	if (!Blueprint || Blueprint->ImplementedInterfaces.Num() == 0)
	{
		return FString();
	}

	TArray<FString> InterfaceLines;

	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			InterfaceLines.Add(FString::Printf(TEXT("- %s"), *Interface.Interface->GetName()));
		}
	}

	if (InterfaceLines.Num() == 0)
	{
		return FString();
	}

	FString Output = FString::Printf(TEXT("## Interfaces (%d)\n"), InterfaceLines.Num());
	for (const FString& Line : InterfaceLines)
	{
		Output += Line + TEXT("\n");
	}

	return Output;
}

FString ClaireonTool_GetBlueprintProperties::FormatGraphSummary(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return FString();
	}

	TArray<FString> GraphLines;

	// Event graphs
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [Event Graph]"),
				*Graph->GetName(),
				Graph->Nodes.Num()));
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [Function]"),
				*Graph->GetName(),
				Graph->Nodes.Num()));
		}
	}

	// AnimBlueprint-specific graphs
	if (const UAnimBlueprint* AnimBP = Cast<const UAnimBlueprint>(Blueprint))
	{
		// AnimGraph is typically in FunctionGraphs, but we can check for it specifically
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Contains(TEXT("AnimGraph")))
			{
				GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [AnimGraph]"),
					*Graph->GetName(),
					Graph->Nodes.Num()));
			}
		}
	}

	// WidgetBlueprint-specific info
	if (const UWidgetBlueprint* WidgetBP = Cast<const UWidgetBlueprint>(Blueprint))
	{
		if (WidgetBP->WidgetTree)
		{
			GraphLines.Add(TEXT("- Widget Hierarchy [Widget Tree]"));
		}
	}

	if (GraphLines.Num() == 0)
	{
		return FString();
	}

	FString Output = FString::Printf(TEXT("## Graphs (%d)\n"), GraphLines.Num());
	for (const FString& Line : GraphLines)
	{
		Output += Line + TEXT("\n");
	}

	return Output;
}

FString ClaireonTool_GetBlueprintProperties::GetBlueprintTypeName(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return TEXT("Unknown");
	}

	if (Blueprint->IsA<UAnimBlueprint>())
	{
		return TEXT("AnimBlueprint");
	}
	else if (Blueprint->IsA<UWidgetBlueprint>())
	{
		return TEXT("WidgetBlueprint");
	}
	else if (Blueprint->BlueprintType == BPTYPE_FunctionLibrary)
	{
		return TEXT("FunctionLibrary");
	}
	else if (Blueprint->BlueprintType == BPTYPE_MacroLibrary)
	{
		return TEXT("MacroLibrary");
	}
	else if (Blueprint->BlueprintType == BPTYPE_Interface)
	{
		return TEXT("Interface");
	}
	else
	{
		return TEXT("Normal");
	}
}

FString ClaireonTool_GetBlueprintProperties::FormatVariableType(const FEdGraphPinType& PinType)
{
	FString TypeStr;

	// Container type
	if (PinType.ContainerType == EPinContainerType::Array)
	{
		TypeStr += TEXT("Array<");
	}
	else if (PinType.ContainerType == EPinContainerType::Set)
	{
		TypeStr += TEXT("Set<");
	}
	else if (PinType.ContainerType == EPinContainerType::Map)
	{
		TypeStr += TEXT("Map<");
	}

	// Base type
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		TypeStr += TEXT("Boolean");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		TypeStr += TEXT("Byte");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		TypeStr += TEXT("Int");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		TypeStr += TEXT("Int64");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float)
		{
			TypeStr += TEXT("Float");
		}
		else if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double)
		{
			TypeStr += TEXT("Double");
		}
		else
		{
			TypeStr += TEXT("Real");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		TypeStr += TEXT("String");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		TypeStr += TEXT("Name");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		TypeStr += TEXT("Text");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeStr += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			TypeStr += TEXT("Object");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeStr += FString::Printf(TEXT("Class<%s>"), *PinType.PinSubCategoryObject->GetName());
		}
		else
		{
			TypeStr += TEXT("Class");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeStr += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			TypeStr += TEXT("Struct");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeStr += PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			TypeStr += TEXT("Enum");
		}
	}
	else
	{
		TypeStr += PinType.PinCategory.ToString();
	}

	// Close container type
	if (PinType.ContainerType != EPinContainerType::None)
	{
		TypeStr += TEXT(">");
	}

	return TypeStr;
}
