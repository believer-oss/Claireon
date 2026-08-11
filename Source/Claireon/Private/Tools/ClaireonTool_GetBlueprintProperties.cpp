// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/MemberReference.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Animation/AnimBlueprint.h"
#include "WidgetBlueprint.h"
#include "UObject/CoreNetTypes.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

FString ClaireonTool_GetBlueprintProperties::GetCategory() const { return kBPCategory; }
FString ClaireonTool_GetBlueprintProperties::GetOperation() const { return TEXT("get_properties"); }

TArray<FString> ClaireonTool_GetBlueprintProperties::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("get"), TEXT("read"), TEXT("properties"), TEXT("variables"), TEXT("functions"), TEXT("interface")};
}

FString ClaireonTool_GetBlueprintProperties::GetDescription() const
{
	return TEXT(
		"Read a Blueprint's public interface: functions, variables, components, parent class, and implemented "
		"interfaces. Lists only members declared on this Blueprint (SCS-only for components) unless "
		"include_inherited=true, which adds ancestors and native parent CDOs. Entries carry is_inherited and "
		"source_class; include_cdo=true adds CDO field values under data.cdo_fields. Read-only / non-session.");
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

	// include_cdo - optional (B32)
	TSharedPtr<FJsonObject> CdoProp = MakeShared<FJsonObject>();
	CdoProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CdoProp->SetStringField(TEXT("description"),
		TEXT("When true, also serialize the Blueprint CDO's reflected field values into a flat map under data.cdo_fields. Default: false."));
	Properties->SetObjectField(TEXT("include_cdo"), CdoProp);

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

	bool bIncludeCdo = false;
	if (Arguments->HasField(TEXT("include_cdo")))
	{
		bIncludeCdo = Arguments->GetBoolField(TEXT("include_cdo"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath, LoadError);
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(FString::Printf(TEXT("%s Use find_assets to locate valid Blueprint paths."), *LoadError));
	}

	// Gather parent class info
	FString ParentClassName = TEXT("None");
	if (IsValid(Blueprint->GeneratedClass) && IsValid(Blueprint->GeneratedClass->GetSuperClass()))
	{
		ParentClassName = Blueprint->GeneratedClass->GetSuperClass()->GetName();
	}

	FString BlueprintType = GetBlueprintTypeName(Blueprint);

	// Short name of this BP's generated class. Used as source_class for
	// "this BP" entries on components/variables/functions to keep the schema
	// uniform whether or not include_inherited is set.
	const FString ThisGeneratedClassShortName = IsValid(Blueprint->GeneratedClass)
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

		// Raw K2 pin reflection for fixture assertions.
		VarObj->SetStringField(TEXT("pin_category"), Var.VarType.PinCategory.ToString());
		VarObj->SetStringField(TEXT("pin_sub_category"), Var.VarType.PinSubCategory.ToString());
		if (UObject* SubObj = Var.VarType.PinSubCategoryObject.Get(); IsValid(SubObj))
		{
			VarObj->SetStringField(TEXT("pin_sub_category_object"), SubObj->GetPathName());
		}

		// Delegate signature function (multicast + single-cast delegate variables
		// carry their UFunction signature in PinSubCategoryMemberReference, not in
		// PinSubCategoryObject). Emitting signature_function closes the round-trip
		// contract with bp_add_variable on the target side; see
		// ClaireonBlueprintHelpers::ResolveSignatureFunction.
		if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ||
			Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
		{
			UFunction* SignatureFn = FMemberReference::ResolveSimpleMemberReference<UFunction>(
				Var.VarType.PinSubCategoryMemberReference, Blueprint->GeneratedClass);

			// BP-authored event dispatchers commonly carry an empty/self member
			// reference (the editor's own dispatcher creation leaves it default and
			// the compiler synthesizes '<Var>__DelegateSignature' from the signature
			// graph). Fall back to that generated function by name on the generated
			// and skeleton classes before giving up.
			if (!IsValid(SignatureFn))
			{
				const FString GeneratedSigName = Var.VarName.ToString() + TEXT("__DelegateSignature");
				const FName MemberSigName = Var.VarType.PinSubCategoryMemberReference.MemberName;
				UClass* const CandidateClasses[] = { Blueprint->GeneratedClass.Get(), Blueprint->SkeletonGeneratedClass.Get() };
				for (UClass* Candidate : CandidateClasses)
				{
					if (!IsValid(Candidate)) continue;
					SignatureFn = Candidate->FindFunctionByName(FName(*GeneratedSigName));
					if (!IsValid(SignatureFn) && !MemberSigName.IsNone())
					{
						SignatureFn = Candidate->FindFunctionByName(MemberSigName);
						if (!IsValid(SignatureFn))
						{
							SignatureFn = Candidate->FindFunctionByName(
								FName(*(MemberSigName.ToString() + TEXT("__DelegateSignature"))));
						}
					}
					if (IsValid(SignatureFn)) break;
				}
			}

			if (IsValid(SignatureFn))
			{
				VarObj->SetStringField(TEXT("signature_function"), SignatureFn->GetPathName());
			}
			else
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("blueprint_get_properties: failed to resolve signature UFunction for delegate variable '%s' on Blueprint '%s'; omitting signature_function field"),
					*Var.VarName.ToString(), *Blueprint->GetPathName());
			}

			// Dispatcher usability flags: CallDelegate/AddDelegate nodes require these.
			VarObj->SetBoolField(TEXT("is_blueprint_assignable"), (Var.PropertyFlags & CPF_BlueprintAssignable) != 0);
			VarObj->SetBoolField(TEXT("is_blueprint_callable"), (Var.PropertyFlags & CPF_BlueprintCallable) != 0);
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
	if (bIncludeInherited && IsValid(Blueprint->GeneratedClass))
	{
		for (TFieldIterator<FProperty> PropIt(Blueprint->GeneratedClass); PropIt; ++PropIt)
		{
			FProperty* Property = *PropIt;
			if (!Property)
			{
				continue;
			}
			UClass* OwnerClass = Property->GetOwnerClass();
			if (!IsValid(OwnerClass) || OwnerClass == Blueprint->GeneratedClass)
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
		if (!IsValid(Graph))
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
			if (IsValid(EntryNode))
			{
				break;
			}
		}

		bool bIsPure = false;
		bool bIsEvent = false;
		FString ReturnType = TEXT("void");
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		TArray<TSharedPtr<FJsonValue>> OutputsArray;

		if (IsValid(EntryNode))
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

			// Output parameters (incl. the return value) live as INPUT pins on the
			// K2Node_FunctionResult node -- NOT on the entry node, whose input pins
			// are exec only. The old entry-node scan left return_type at "void" for
			// every function, so replayed copies lost their return pins entirely.
			UK2Node_FunctionResult* ResultNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
				if (IsValid(ResultNode))
				{
					break;
				}
			}
			if (IsValid(ResultNode))
			{
				for (UEdGraphPin* Pin : ResultNode->Pins)
				{
					if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
						OutObj->SetStringField(TEXT("name"), Pin->GetName());
						OutObj->SetStringField(TEXT("type"), FormatVariableType(Pin->PinType));
						OutputsArray.Add(MakeShared<FJsonValueObject>(OutObj));
					}
				}
				if (OutputsArray.Num() > 0)
				{
					const TSharedPtr<FJsonObject>* FirstOut = nullptr;
					if (OutputsArray[0]->TryGetObject(FirstOut) && FirstOut)
					{
						ReturnType = (*FirstOut)->GetStringField(TEXT("type"));
					}
				}
			}

			// Function-local variables: replay needs these to re-declare locals via
			// bp_add_local_variable and to bind member_scope VariableGet/Set nodes.
			if (EntryNode->LocalVariables.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> LocalsArray;
				for (const FBPVariableDescription& Local : EntryNode->LocalVariables)
				{
					TSharedPtr<FJsonObject> LocalObj = MakeShared<FJsonObject>();
					LocalObj->SetStringField(TEXT("name"), Local.VarName.ToString());
					LocalObj->SetStringField(TEXT("type"), FormatVariableType(Local.VarType));
					LocalObj->SetStringField(TEXT("pin_category"), Local.VarType.PinCategory.ToString());
					if (UObject* SubObj = Local.VarType.PinSubCategoryObject.Get(); IsValid(SubObj))
					{
						LocalObj->SetStringField(TEXT("pin_sub_category_object"), SubObj->GetPathName());
					}
					LocalObj->SetStringField(TEXT("default_value"), Local.DefaultValue);
					LocalsArray.Add(MakeShared<FJsonValueObject>(LocalObj));
				}
				FuncObj->SetArrayField(TEXT("local_variables"), LocalsArray);
			}
		}

		FuncObj->SetStringField(TEXT("return_type"), ReturnType);
		FuncObj->SetArrayField(TEXT("parameters"), ParamsArray);
		FuncObj->SetArrayField(TEXT("outputs"), OutputsArray);
		FuncObj->SetBoolField(TEXT("is_pure"), bIsPure);
		FuncObj->SetBoolField(TEXT("is_event"), bIsEvent);
		FuncObj->SetBoolField(TEXT("is_inherited"), false);
		FuncObj->SetStringField(TEXT("source_class"), ThisGeneratedClassShortName);

		// Override detection: replay must route parent-declared functions through
		// add_function_override (re-creating one as a plain function collides with
		// the parent signature). A function graph is an override when the parent
		// class already declares a UFunction of the same name.
		UFunction* OverriddenFunc = IsValid(Blueprint->ParentClass)
			? Blueprint->ParentClass->FindFunctionByName(FName(*Graph->GetName()))
			: nullptr;
		FuncObj->SetBoolField(TEXT("is_override"), OverriddenFunc != nullptr);
		if (IsValid(OverriddenFunc))
		{
			UClass* DeclaringClass = OverriddenFunc->GetOwnerClass();
			FuncObj->SetStringField(TEXT("override_source_class"),
				IsValid(DeclaringClass) ? DeclaringClass->GetName() : FString());
			FuncObj->SetBoolField(TEXT("override_is_native_event"),
				OverriddenFunc->HasAllFunctionFlags(FUNC_Native | FUNC_BlueprintEvent));
			FuncObj->SetBoolField(TEXT("override_has_return"),
				OverriddenFunc->GetReturnProperty() != nullptr);
		}

		EmittedFunctionNames.Add(FName(*Graph->GetName()));
		FunctionsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	// Inherited functions (only when requested). Walk UFunctions on the
	// generated class with super included; skip those declared on this BP
	// (already emitted above). Filter to Blueprint-visible callables/events.
	// Inherited entries omit rich return_type/parameters detail (parameters: [],
	// return_type: "void").
	if (bIncludeInherited && IsValid(Blueprint->GeneratedClass))
	{
		for (TFieldIterator<UFunction> FuncIt(Blueprint->GeneratedClass); FuncIt; ++FuncIt)
		{
			UFunction* Function = *FuncIt;
			if (!IsValid(Function))
			{
				continue;
			}
			UClass* OwnerClass = Function->GetOwnerClass();
			if (!IsValid(OwnerClass) || OwnerClass == Blueprint->GeneratedClass)
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
		if (!IsValid(Node))
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
		// Full path: BP-class short names ('BPC_MyComponent_C') only resolve on
		// a warm editor; a cold replay needs the loadable object path.
		if (Node->ComponentClass)
		{
			CompObj->SetStringField(TEXT("class_path"), Node->ComponentClass->GetPathName());
		}
		CompObj->SetBoolField(TEXT("is_root"), Node == DefaultRoot);

		if (IsValid(ParentNode))
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
			if (!IsValid(Node))
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
		while (IsValid(AncestorClass))
		{
			UBlueprintGeneratedClass* AncestorBPGC = Cast<UBlueprintGeneratedClass>(AncestorClass);
			if (!IsValid(AncestorBPGC))
			{
				break;
			}
			USimpleConstructionScript* AncestorSCS = AncestorBPGC->SimpleConstructionScript;
			if (IsValid(AncestorSCS))
			{
				const FString AncestorClassShortName = AncestorBPGC->GetName();
				USCS_Node* AncestorDefaultRoot = AncestorSCS->GetDefaultSceneRootNode();

				TFunction<void(USCS_Node*, USCS_Node*)> CollectAncestorSCS = [&](USCS_Node* Node, USCS_Node* ParentNode)
				{
					if (!IsValid(Node))
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
		if (UClass* GeneratedClass = Blueprint->GeneratedClass; IsValid(GeneratedClass))
		{
			if (AActor* CDOActor = Cast<AActor>(GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false)); IsValid(CDOActor))
			{
				TArray<UActorComponent*> NativeComponents;
				CDOActor->GetComponents(NativeComponents);
				for (UActorComponent* Component : NativeComponents)
				{
					if (!IsValid(Component))
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
					while (IsValid(NativeWalker))
					{
						if (NativeWalker->IsNative())
						{
							UObject* NativeCDO = NativeWalker->GetDefaultObject(/*bCreateIfNeeded=*/false);
							if (IsValid(NativeCDO))
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

	// Build interfaces array. Objects carry the loadable path alongside the
	// short name: BP-interface short names ('BPI_Footstep_C') only resolve on a
	// warm editor, so replay against a cold editor needs the object path.
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (IsValid(Interface.Interface))
		{
			TSharedPtr<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
			IfaceObj->SetStringField(TEXT("name"), Interface.Interface->GetName());
			IfaceObj->SetStringField(TEXT("path"), Interface.Interface->GetPathName());
			InterfacesArray.Add(MakeShared<FJsonValueObject>(IfaceObj));
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
	if (const UEnum* CompileModeEnum = StaticEnum<EBlueprintCompileMode>(); IsValid(CompileModeEnum))
	{
		CompileModeStr = CompileModeEnum->GetNameStringByValue(static_cast<int64>(Blueprint->CompileMode));
	}
	MetadataObj->SetStringField(TEXT("compile_mode"), CompileModeStr);

	// num_replicated_properties from BlueprintGeneratedClass (read-only)
	int32 NumReplicatedProps = 0;
	if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass); IsValid(BPGC))
	{
		NumReplicatedProps = BPGC->NumReplicatedProperties;
	}
	MetadataObj->SetNumberField(TEXT("num_replicated_properties"), NumReplicatedProps);

	Data->SetObjectField(TEXT("metadata"), MetadataObj);

	// when requested, serialize the CDO's reflected field values into a
	// flat map. Uses the same ClaireonPropertyUtils::PropertyToJsonValue helper
	// that uobject_inspect uses, so the two tools cannot drift. Walks the full
	// reflected chain (super included) so callers can read inherited fields
	// like bAllowGlobalHost / ActivationMode / StopBehavior in a single call.
	if (bIncludeCdo && IsValid(Blueprint->GeneratedClass))
	{
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false);
		if (IsValid(CDO))
		{
			TSharedPtr<FJsonObject> CdoFields = MakeShared<FJsonObject>();
			constexpr int32 CdoSerializationDepth = 2;
			for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property)
				{
					continue;
				}
				// Mirror uobject_inspect: do not filter by Transient/Deprecated;
				// the goal is to surface every reflected field so callers can
				// read CDO-state-only fields like bAllowGlobalHost.
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
				TSharedPtr<FJsonValue> SerializedValue =
					ClaireonPropertyUtils::PropertyToJsonValue(Property, ValuePtr, CDO, CdoSerializationDepth);
				if (SerializedValue.IsValid())
				{
					CdoFields->SetField(Property->GetName(), SerializedValue);
				}
			}
			Data->SetObjectField(TEXT("cdo_fields"), CdoFields);

			// cdo_fields_text: ImportText-form values for exactly the fields whose
			// value DIFFERS from the parent class default. This is the replayable
			// CDO diff -- feed each entry to bp_set_cdo_property(property_name,
			// value) on a copy to reproduce the source's CDO state (cdo_fields'
			// depth-limited JSON is lossy for arrays/structs and cannot round-trip).
			UObject* ParentCDO = IsValid(Blueprint->ParentClass)
				? Blueprint->ParentClass->GetDefaultObject(/*bCreateIfNeeded=*/false)
				: nullptr;

			// BP-added component variables: the generated class carries one
			// FObjectProperty per SCS node, but its CDO value is always None
			// (assigned at construction) and the CDO writer resolves the bare
			// name to the component template, so these fields can never replay.
			TSet<FName> ScsVariableNames;
			if (Blueprint->SimpleConstructionScript)
			{
				for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
				{
					if (IsValid(Node))
					{
						ScsVariableNames.Add(Node->GetVariableName());
					}
				}
			}

			// Export a property's value on Obj and diff it against the same
			// property on ParentObj. Returns false when the value matches the
			// parent (nothing to emit). "(INVALID)" exports (stale enum bytes
			// the source asset carries) can never import; treated as no-emit.
			auto ExportDiffedField = [](FProperty* Prop, UObject* Obj, UObject* ParentObj, FString& OutText) -> bool
			{
				Prop->ExportTextItem_Direct(OutText,
					Prop->ContainerPtrToValuePtr<void>(Obj), nullptr, Obj, PPF_None);
				if (OutText == TEXT("(INVALID)"))
				{
					return false;
				}
				if (IsValid(ParentObj) && IsValid(Prop->GetOwnerClass()) && ParentObj->IsA(Prop->GetOwnerClass()))
				{
					FString ParentText;
					Prop->ExportTextItem_Direct(ParentText,
						Prop->ContainerPtrToValuePtr<void>(ParentObj), nullptr, ParentObj, PPF_None);
					if (ParentText == OutText)
					{
						return false;
					}
				}
				return true;
			};

			TSharedPtr<FJsonObject> CdoFieldsText = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> PropIt(CDO->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Property = *PropIt;
				if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				// Delegate bindings reference the emitting class's own functions
				// ("Unable to find function Default__<Source>_C.X" on import) and
				// are re-established by the copy's graph nodes at runtime.
				if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>())
				{
					continue;
				}
				if (ScsVariableNames.Contains(Property->GetFName()))
				{
					continue;
				}
				// Default-subobject / instanced component pointers: exporting the
				// pointer bakes a source-asset path that can never import on a
				// copy (and always text-diffs against the parent because the CDO
				// names differ). Emit the subobject's parent-diffed fields as
				// "<SubobjectName>.<Field>" entries instead -- the CDO writer
				// resolves that prefix to the same-named subobject on the copy.
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
				{
					UObject* SubObj = ObjProp->GetObjectPropertyValue_InContainer(CDO);
					if (IsValid(SubObj) && SubObj->IsIn(CDO))
					{
						UObject* ParentSub = IsValid(ParentCDO)
							? ParentCDO->GetDefaultSubobjectByName(SubObj->GetFName())
							: nullptr;
						for (TFieldIterator<FProperty> SubIt(SubObj->GetClass()); SubIt; ++SubIt)
						{
							FProperty* SubProp = *SubIt;
							if (!SubProp || SubProp->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
							{
								continue;
							}
							if (SubProp->IsA<FDelegateProperty>() || SubProp->IsA<FMulticastDelegateProperty>())
							{
								continue;
							}
							// Nested subobject pointers have the same
							// unreplayable-path problem; one level of recursion
							// covers every case the replay loop has hit.
							if (FObjectProperty* SubObjProp = CastField<FObjectProperty>(SubProp))
							{
								UObject* Nested = SubObjProp->GetObjectPropertyValue_InContainer(SubObj);
								if (IsValid(Nested) && Nested->IsIn(CDO))
								{
									continue;
								}
							}
							FString SubText;
							if (ExportDiffedField(SubProp, SubObj, ParentSub, SubText))
							{
								CdoFieldsText->SetStringField(
									SubObj->GetName() + TEXT(".") + SubProp->GetName(), SubText);
							}
						}
						continue;
					}
				}
				FString ValueText;
				if (ExportDiffedField(Property, CDO, ParentCDO, ValueText))
				{
					CdoFieldsText->SetStringField(Property->GetName(), ValueText);
				}
			}
			Data->SetObjectField(TEXT("cdo_fields_text"), CdoFieldsText);
		}
	}

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
	if (!IsValid(Blueprint))
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint at path: %s"), *AssetPath);
		return nullptr;
	}

	return Blueprint;
}


FString ClaireonTool_GetBlueprintProperties::FormatComponents(const UBlueprint* Blueprint)
{
	if (!IsValid(Blueprint) || !Blueprint->SimpleConstructionScript)
	{
		return FString();
	}

	TArray<FString> ComponentLines;
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	// Helper lambda to recursively format component hierarchy
	TFunction<void(USCS_Node*, int32)> FormatComponentNode = [&](USCS_Node* Node, int32 Depth)
	{
		if (!IsValid(Node))
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
	if (!IsValid(Blueprint) || Blueprint->ImplementedInterfaces.Num() == 0)
	{
		return FString();
	}

	TArray<FString> InterfaceLines;

	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (IsValid(Interface.Interface))
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
	if (!IsValid(Blueprint))
	{
		return FString();
	}

	TArray<FString> GraphLines;

	// Event graphs
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (IsValid(Graph))
		{
			GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [Event Graph]"),
				*Graph->GetName(),
				Graph->Nodes.Num()));
		}
	}

	// Function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (IsValid(Graph))
		{
			GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [Function]"),
				*Graph->GetName(),
				Graph->Nodes.Num()));
		}
	}

	// AnimBlueprint-specific graphs
	if (const UAnimBlueprint* AnimBP = Cast<const UAnimBlueprint>(Blueprint); IsValid(AnimBP))
	{
		// AnimGraph is typically in FunctionGraphs, but we can check for it specifically
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (IsValid(Graph) && Graph->GetName().Contains(TEXT("AnimGraph")))
			{
				GraphLines.Add(FString::Printf(TEXT("- %s (%d nodes) [AnimGraph]"),
					*Graph->GetName(),
					Graph->Nodes.Num()));
			}
		}
	}

	// WidgetBlueprint-specific info
	if (const UWidgetBlueprint* WidgetBP = Cast<const UWidgetBlueprint>(Blueprint); IsValid(WidgetBP))
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
	if (!IsValid(Blueprint))
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
	// Format one terminal (category, sub-category, sub-object) triple. Used for the
	// pin's own type and, for maps, the value terminal -- both must round-trip
	// through ClaireonBlueprintHelpers::ParseVariableTypeChecked.
	auto FormatBase = [](const FName& Category, const FName& SubCategory, const UObject* SubObj) -> FString
	{
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			return TEXT("Boolean");
		}
		if (Category == UEdGraphSchema_K2::PC_Byte)
		{
			// A byte pin with a bound UEnum is an enum variable; emit the enum name
			// so the round trip restores the enum binding instead of a raw byte.
			return IsValid(SubObj) ? SubObj->GetName() : TEXT("Byte");
		}
		if (Category == UEdGraphSchema_K2::PC_Int)
		{
			return TEXT("Int");
		}
		if (Category == UEdGraphSchema_K2::PC_Int64)
		{
			return TEXT("Int64");
		}
		if (Category == UEdGraphSchema_K2::PC_Real)
		{
			if (SubCategory == UEdGraphSchema_K2::PC_Float)
			{
				return TEXT("Float");
			}
			if (SubCategory == UEdGraphSchema_K2::PC_Double)
			{
				return TEXT("Double");
			}
			return TEXT("Real");
		}
		if (Category == UEdGraphSchema_K2::PC_String)
		{
			return TEXT("String");
		}
		if (Category == UEdGraphSchema_K2::PC_Name)
		{
			return TEXT("Name");
		}
		if (Category == UEdGraphSchema_K2::PC_Text)
		{
			return TEXT("Text");
		}
		if (Category == UEdGraphSchema_K2::PC_Object)
		{
			return IsValid(SubObj) ? SubObj->GetName() : TEXT("Object");
		}
		if (Category == UEdGraphSchema_K2::PC_Class)
		{
			return IsValid(SubObj) ? FString::Printf(TEXT("Class<%s>"), *SubObj->GetName()) : TEXT("Class");
		}
		if (Category == UEdGraphSchema_K2::PC_SoftObject)
		{
			return IsValid(SubObj) ? FString::Printf(TEXT("SoftObject<%s>"), *SubObj->GetName()) : TEXT("SoftObject");
		}
		if (Category == UEdGraphSchema_K2::PC_SoftClass)
		{
			return IsValid(SubObj) ? FString::Printf(TEXT("SoftClass<%s>"), *SubObj->GetName()) : TEXT("SoftClass");
		}
		if (Category == UEdGraphSchema_K2::PC_Struct || Category == UEdGraphSchema_K2::PC_Enum)
		{
			return IsValid(SubObj) ? SubObj->GetName() : Category.ToString();
		}
		return Category.ToString();
	};

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

	TypeStr += FormatBase(PinType.PinCategory, PinType.PinSubCategory, PinType.PinSubCategoryObject.Get());

	// Map value terminal -- without it the emitted type is not reconstructable
	// and map variables fail to replay.
	if (PinType.ContainerType == EPinContainerType::Map)
	{
		TypeStr += TEXT(",");
		TypeStr += FormatBase(PinType.PinValueType.TerminalCategory,
			PinType.PinValueType.TerminalSubCategory,
			PinType.PinValueType.TerminalSubCategoryObject.Get());
	}

	// Close container type
	if (PinType.ContainerType != EPinContainerType::None)
	{
		TypeStr += TEXT(">");
	}

	return TypeStr;
}
