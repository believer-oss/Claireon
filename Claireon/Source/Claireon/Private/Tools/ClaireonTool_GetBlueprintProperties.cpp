// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/MemberReference.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "Animation/AnimBlueprint.h"
#include "WidgetBlueprint.h"
#include "UObject/CoreNetTypes.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

FString ClaireonTool_GetBlueprintProperties::GetCategory() const { return TEXT("blueprint"); }
FString ClaireonTool_GetBlueprintProperties::GetOperation() const { return TEXT("get_properties"); }

TArray<FString> ClaireonTool_GetBlueprintProperties::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("get"), TEXT("read"), TEXT("properties"), TEXT("variables"), TEXT("functions"), TEXT("interface")};
}

FString ClaireonTool_GetBlueprintProperties::GetDescription() const
{
	return TEXT(
		"Read a Blueprint asset's public interface: functions, variables, components, parent class, and implemented interfaces. "
		"Works with standard Blueprints, Animation Blueprints, Widget Blueprints, and Blueprint Function Libraries. "
		"By default, the components, variables, and functions arrays only include items declared on this Blueprint (SCS-only for components). "
		"Pass include_inherited=true to also include items inherited from ancestor Blueprints and (for actor-derived BPs) from native parent CDOs -- this matches what unreal.Actor.get_components_by_class(unreal.ActorComponent) returns when called on the editor CDO. "
		"Note that native subobjects guarded by WITH_EDITORONLY_DATA only appear in editor builds. "
		"Every entry in components, variables, and functions always carries is_inherited (bool) and source_class (short class name) fields regardless of include_inherited, providing a stable schema for callers.");
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

	// Short name of this BP's generated class. Used as source_class for
	// "this BP" entries on components/variables/functions to keep the schema
	// uniform whether or not include_inherited is set.
	const FString ThisGeneratedClassShortName = Blueprint->GeneratedClass
		? Blueprint->GeneratedClass->GetName()
		: FString(TEXT("Unknown"));

	// Track inherited counts so the summary line can report (N inherited).
	int32 InheritedVariableCount = 0;
	int32 InheritedFunctionCount = 0;
	int32 InheritedComponentCount = 0;

	// Build variables array
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	TSet<FName> EmittedVariableNames;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("variable_name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), FormatVariableType(Var.VarType));
		VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		VarObj->SetBoolField(TEXT("is_exposed"), (Var.PropertyFlags & CPF_BlueprintVisible) != 0);

		// Raw K2 pin reflection for fixture assertions (Gap 3).
		VarObj->SetStringField(TEXT("pin_category"), Var.VarType.PinCategory.ToString());
		VarObj->SetStringField(TEXT("pin_sub_category"), Var.VarType.PinSubCategory.ToString());
		if (UObject* SubObj = Var.VarType.PinSubCategoryObject.Get())
		{
			VarObj->SetStringField(TEXT("pin_sub_category_object"), SubObj->GetPathName());
		}

		// Delegate signature function (multicast + single-cast delegate variables
		// carry their UFunction signature in PinSubCategoryMemberReference, not in
		// PinSubCategoryObject). Emitting signature_function closes the round-trip
		// contract with blueprint_edit_graph[add_variable] on the target side; see
		// ClaireonBlueprintHelpers::ResolveSignatureFunction.
		if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ||
			Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
		{
			UFunction* SignatureFn = FMemberReference::ResolveSimpleMemberReference<UFunction>(
				Var.VarType.PinSubCategoryMemberReference, Blueprint->GeneratedClass);
			if (SignatureFn)
			{
				VarObj->SetStringField(TEXT("signature_function"), SignatureFn->GetPathName());
			}
			else
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("blueprint_get_properties: failed to resolve signature UFunction for delegate variable '%s' on Blueprint '%s'; omitting signature_function field"),
					*Var.VarName.ToString(), *Blueprint->GetPathName());
			}
		}

		const TCHAR* ContainerTypeStr = TEXT("None");
		switch (Var.VarType.ContainerType)
		{
			case EPinContainerType::Array: ContainerTypeStr = TEXT("Array"); break;
			case EPinContainerType::Set:   ContainerTypeStr = TEXT("Set"); break;
			case EPinContainerType::Map:   ContainerTypeStr = TEXT("Map"); break;
			default: break;
		}
		VarObj->SetStringField(TEXT("container_type"), ContainerTypeStr);

		// inner_pin_category mirrors pin_category for Array/Set so fixtures can
		// read the element kind without container-type branching.
		if (Var.VarType.ContainerType == EPinContainerType::Array ||
			Var.VarType.ContainerType == EPinContainerType::Set)
		{
			VarObj->SetStringField(TEXT("inner_pin_category"), Var.VarType.PinCategory.ToString());
		}

		// Map value side.
		if (Var.VarType.ContainerType == EPinContainerType::Map)
		{
			VarObj->SetStringField(TEXT("pin_value_category"), Var.VarType.PinValueType.TerminalCategory.ToString());
		}

		// Stable per-entry schema: is_inherited + source_class on every variable.
		VarObj->SetBoolField(TEXT("is_inherited"), false);
		VarObj->SetStringField(TEXT("source_class"), ThisGeneratedClassShortName);

		EmittedVariableNames.Add(Var.VarName);
		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	// Inherited variables (only when requested). Walk FProperty fields on the
	// generated class with super included; skip anything declared on this BP
	// (already emitted above). Filter to Blueprint-visible properties to match
	// the historical FormatVariables intent.
	if (bIncludeInherited && Blueprint->GeneratedClass)
	{
		for (TFieldIterator<FProperty> PropIt(Blueprint->GeneratedClass); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (!Property)
			{
				continue;
			}
			UClass* OwnerClass = Property->GetOwnerClass();
			if (!OwnerClass || OwnerClass == Blueprint->GeneratedClass)
			{
				continue;
			}
			if (!(Property->PropertyFlags & CPF_BlueprintVisible))
			{
				continue;
			}
			const FName PropertyName = Property->GetFName();
			if (EmittedVariableNames.Contains(PropertyName))
			{
				continue;
			}

			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			VarObj->SetStringField(TEXT("name"), Property->GetName());
			VarObj->SetStringField(TEXT("type"), Property->GetClass()->GetName());
			VarObj->SetBoolField(TEXT("is_exposed"), true);
			VarObj->SetBoolField(TEXT("is_inherited"), true);
			VarObj->SetStringField(TEXT("source_class"), OwnerClass->GetName());

			EmittedVariableNames.Add(PropertyName);
			VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
			++InheritedVariableCount;
		}
	}

	// Build functions array
	TArray<TSharedPtr<FJsonValue>> FunctionsArray;
	TSet<FName> EmittedFunctionNames;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), Graph->GetName());
		FuncObj->SetStringField(TEXT("function_name"), Graph->GetName());

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
		FuncObj->SetBoolField(TEXT("is_inherited"), false);
		FuncObj->SetStringField(TEXT("source_class"), ThisGeneratedClassShortName);

		EmittedFunctionNames.Add(FName(*Graph->GetName()));
		FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	// Inherited functions (only when requested). Walk UFunctions on the
	// generated class with super included; skip those declared on this BP
	// (already emitted above). Filter to Blueprint-visible callables/events.
	// Inherited entries omit rich return_type/parameters detail (parameters: [],
	// return_type: "void").
	if (bIncludeInherited && Blueprint->GeneratedClass)
	{
		for (TFieldIterator<UFunction> FuncIt(Blueprint->GeneratedClass); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (!Function)
			{
				continue;
			}
			UClass* OwnerClass = Function->GetOwnerClass();
			if (!OwnerClass || OwnerClass == Blueprint->GeneratedClass)
			{
				continue;
			}
			const bool bBlueprintCallable = (Function->FunctionFlags & FUNC_BlueprintCallable) != 0;
			const bool bBlueprintEvent = (Function->FunctionFlags & FUNC_BlueprintEvent) != 0;
			if (!bBlueprintCallable && !bBlueprintEvent)
			{
				continue;
			}
			const FName FunctionName = Function->GetFName();
			if (EmittedFunctionNames.Contains(FunctionName))
			{
				continue;
			}

			TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
			FuncObj->SetStringField(TEXT("name"), Function->GetName());
			FuncObj->SetStringField(TEXT("return_type"), TEXT("void"));
			FuncObj->SetArrayField(TEXT("parameters"), TArray<TSharedPtr<FJsonValue>>());
			FuncObj->SetBoolField(TEXT("is_pure"), (Function->FunctionFlags & FUNC_BlueprintPure) != 0);
			FuncObj->SetBoolField(TEXT("is_event"), bBlueprintEvent);
			FuncObj->SetBoolField(TEXT("is_inherited"), true);
			FuncObj->SetStringField(TEXT("source_class"), OwnerClass->GetName());

			EmittedFunctionNames.Add(FunctionName);
			FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
			++InheritedFunctionCount;
		}
	}

	// Build components array via a three-source merged walk:
	//   A) This BP's SCS roots (always)                       -> is_inherited=false
	//   B) Ancestor BP SCS chains (only when include_inherited) -> is_inherited=true
	//   C) Native inherited subobjects (actor-derived only)    -> is_inherited=true
	//
	// Dedupe by USCS_Node::GetVariableName() / UActorComponent::GetFName()
	// (FName-keyed). Source priority A > B > C: a child BP's SCS that shadows
	// a parent SCS or native subobject wins, and the child entry is the one
	// emitted (with is_inherited=false).
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	TSet<FName> EmittedComponentNames;

	auto EmitSCSNode = [&](USCS_Node* Node, USCS_Node* ParentNode, USCS_Node* DefaultRoot, bool bInherited, const FString& InSourceClass)
	{
		if (!Node)
		{
			return;
		}
		const FName VarName = Node->GetVariableName();
		if (EmittedComponentNames.Contains(VarName))
		{
			return;
		}

		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), VarName.ToString());
		CompObj->SetStringField(TEXT("component_name"), VarName.ToString());
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

		CompObj->SetBoolField(TEXT("is_inherited"), bInherited);
		CompObj->SetStringField(TEXT("source_class"), InSourceClass);

		EmittedComponentNames.Add(VarName);
		ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));

		if (bInherited)
		{
			++InheritedComponentCount;
		}
	};

	// Source A: this BP's SCS (always).
	if (Blueprint->SimpleConstructionScript)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode();

		TFunction<void(USCS_Node*, USCS_Node*)> CollectThisSCS = [&](USCS_Node* Node, USCS_Node* ParentNode)
		{
			if (!Node)
			{
				return;
			}
			EmitSCSNode(Node, ParentNode, DefaultRoot, /*bInherited=*/false, ThisGeneratedClassShortName);
			for (USCS_Node* ChildNode : Node->GetChildNodes())
			{
				CollectThisSCS(ChildNode, Node);
			}
		};

		for (USCS_Node* RootNode : SCS->GetRootNodes())
		{
			CollectThisSCS(RootNode, nullptr);
		}
	}

	if (bIncludeInherited)
	{
		// Source B: ancestor BP SCS chains. Walk the parent class upward, and
		// for each UBlueprintGeneratedClass with a non-null SCS, recurse its
		// roots. Stop at the first non-UBlueprintGeneratedClass ancestor (the
		// native boundary).
		UClass* AncestorClass = Blueprint->ParentClass;
		while (AncestorClass)
		{
			UBlueprintGeneratedClass* AncestorBPGC = Cast<UBlueprintGeneratedClass>(AncestorClass);
			if (!AncestorBPGC)
			{
				break;
			}
			USimpleConstructionScript* AncestorSCS = AncestorBPGC->SimpleConstructionScript;
			if (AncestorSCS)
			{
				const FString AncestorClassShortName = AncestorBPGC->GetName();
				USCS_Node* AncestorDefaultRoot = AncestorSCS->GetDefaultSceneRootNode();

				TFunction<void(USCS_Node*, USCS_Node*)> CollectAncestorSCS = [&](USCS_Node* Node, USCS_Node* ParentNode)
				{
					if (!Node)
					{
						return;
					}
					EmitSCSNode(Node, ParentNode, AncestorDefaultRoot, /*bInherited=*/true, AncestorClassShortName);
					for (USCS_Node* ChildNode : Node->GetChildNodes())
					{
						CollectAncestorSCS(ChildNode, Node);
					}
				};

				for (USCS_Node* RootNode : AncestorSCS->GetRootNodes())
				{
					CollectAncestorSCS(RootNode, nullptr);
				}
			}
			AncestorClass = AncestorBPGC->GetSuperClass();
		}

		// Source C: native inherited subobjects on actor-derived BPs. Cast the
		// editor CDO to AActor; if the BP isn't actor-derived (Anim, Widget,
		// FunctionLibrary, ...), the cast yields nullptr and we skip. For each
		// native subobject not already emitted, find the deepest native
		// ancestor whose CDO declares a subobject with the same FName -- that
		// is the source_class. Emit with is_root=false, no parent_component,
		// children=[] (no SCS structure to mirror).
		if (UClass* GeneratedClass = Blueprint->GeneratedClass)
		{
			if (AActor* CDOActor = Cast<AActor>(GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false)))
			{
				TArray<UActorComponent*> NativeComponents;
				CDOActor->GetComponents(NativeComponents);
				for (UActorComponent* Component : NativeComponents)
				{
					if (!Component)
					{
						continue;
					}
					const FName ComponentName = Component->GetFName();
					if (EmittedComponentNames.Contains(ComponentName))
					{
						continue;
					}

					// Walk the native class chain from the BP's first native
					// ancestor upward; the deepest class whose CDO has a
					// subobject with this FName is the declarer.
					FString NativeSourceClassName;
					UClass* NativeWalker = GeneratedClass;
					while (NativeWalker)
					{
						if (NativeWalker->IsNative())
						{
							UObject* NativeCDO = NativeWalker->GetDefaultObject(/*bCreateIfNeeded=*/false);
							if (NativeCDO)
							{
								if (NativeCDO->GetDefaultSubobjectByName(ComponentName) != nullptr)
								{
									NativeSourceClassName = NativeWalker->GetName();
								}
							}
						}
						NativeWalker = NativeWalker->GetSuperClass();
					}
					if (NativeSourceClassName.IsEmpty())
					{
						// Fall back to the component's owning class name if
						// the subobject lookup failed.
						NativeSourceClassName = Component->GetClass()->GetName();
					}

					TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
					CompObj->SetStringField(TEXT("name"), ComponentName.ToString());
					CompObj->SetStringField(TEXT("component_name"), ComponentName.ToString());
					CompObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					CompObj->SetBoolField(TEXT("is_root"), false);
					CompObj->SetArrayField(TEXT("children"), TArray<TSharedPtr<FJsonValue>>());
					CompObj->SetBoolField(TEXT("is_inherited"), true);
					CompObj->SetStringField(TEXT("source_class"), NativeSourceClassName);

					EmittedComponentNames.Add(ComponentName);
					ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
					++InheritedComponentCount;
				}
			}
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

	// Extract asset name for summary. When include_inherited=true and there is
	// at least one inherited entry of a given kind, append " (N inherited)" to
	// that count. Suffix is omitted when the count is zero.
	auto MakeCountFragment = [](int32 Total, int32 Inherited, const TCHAR* Label) -> FString
	{
		if (Inherited > 0)
		{
			return FString::Printf(TEXT("%d %s (%d inherited)"), Total, Label, Inherited);
		}
		return FString::Printf(TEXT("%d %s"), Total, Label);
	};

	const FString VariablesFragment = MakeCountFragment(VariablesArray.Num(), InheritedVariableCount, TEXT("variables"));
	const FString FunctionsFragment = MakeCountFragment(FunctionsArray.Num(), InheritedFunctionCount, TEXT("functions"));
	const FString ComponentsFragment = MakeCountFragment(ComponentsArray.Num(), InheritedComponentCount, TEXT("components"));

	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	FString Summary = FString::Printf(TEXT("%s: %s, %s, %s (parent: %s)"),
		*AssetName,
		*VariablesFragment,
		*FunctionsFragment,
		*ComponentsFragment,
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
