// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AppendBlueprintCDOArrayInstanced.h"
#include "ClaireonPathResolver.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

FString ClaireonTool_AppendBlueprintCDOArrayInstanced::GetCategory() const
{
	return TEXT("blueprint");
}

FString ClaireonTool_AppendBlueprintCDOArrayInstanced::GetOperation() const
{
	return TEXT("append_cdo_array_instanced");
}

FString ClaireonTool_AppendBlueprintCDOArrayInstanced::GetDescription() const
{
	return TEXT("Append a new inline (UPROPERTY(Instanced)) sub-object to a TArray<UObject*> "
		"property on a Blueprint's Class Default Object. Constructs an instance of element_class "
		"and adds it to the array reached by array_property_path. Pairs with blueprint_set_cdo_property "
		"for then writing into the new element via `<array_property_path>[N].<sub_property>`. "
		"Returns the new index and array size.");
}

TSharedPtr<FJsonObject> ClaireonTool_AppendBlueprintCDOArrayInstanced::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Full Unreal asset path to the Blueprint (e.g. /Game/Path/To/BP_MyBlueprint)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> ArrayPathProp = MakeShared<FJsonObject>();
	ArrayPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ArrayPathProp->SetStringField(TEXT("description"),
		TEXT("Dot-separated path to the TArray<UObject*> UPROPERTY(Instanced) on the CDO "
		"(e.g. 'DefaultTargetingInstance.Collectors'). Intermediate segments may include "
		"`[N]` to index nested arrays."));
	Properties->SetObjectField(TEXT("array_property_path"), ArrayPathProp);

	TSharedPtr<FJsonObject> ElementClassProp = MakeShared<FJsonObject>();
	ElementClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ElementClassProp->SetStringField(TEXT("description"),
		TEXT("Class path of the element to construct "
		"(e.g. '/Script/FSTargeting.FSTC_CollisionSphere'). Must be a concrete subclass of "
		"the array's inner property class. Blueprint class paths resolve with implicit '_C' suffix."));
	Properties->SetObjectField(TEXT("element_class"), ElementClassProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("array_property_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("element_class")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AppendBlueprintCDOArrayInstanced::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString ArrayPropertyPath;
	if (!Arguments->TryGetStringField(TEXT("array_property_path"), ArrayPropertyPath))
	{
		return MakeErrorResult(TEXT("Missing required field: array_property_path"));
	}

	FString ElementClassPath;
	if (!Arguments->TryGetStringField(TEXT("element_class"), ElementClassPath))
	{
		return MakeErrorResult(TEXT("Missing required field: element_class"));
	}

	// Step 1: Resolve asset_path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Step 2: Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	if (!Blueprint->GeneratedClass)
	{
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass -- compile it first"));
	}

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
	}

	// Step 3: Resolve element_class. Try as-is first, then with the '_C' suffix
	// to support Blueprint-generated classes the caller may have spelled without it.
	UClass* ElementClass = LoadClass<UObject>(nullptr, *ElementClassPath);
	if (!ElementClass)
	{
		ElementClass = LoadClass<UObject>(nullptr, *(ElementClassPath + TEXT("_C")));
	}
	if (!ElementClass)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to load element class from '%s'"), *ElementClassPath));
	}

	// Step 4: Resolve the leaf array property so we can read the post-append size
	// without a second full path walk. ResolvePropertyByPath returns the FProperty
	// at the leaf -- for our purposes that is the FArrayProperty itself, with the
	// container pointer set to whatever struct/object holds the array.
	void* ArrayContainer = nullptr;
	FString ResolveError;
	FProperty* ResolvedLeaf = ClaireonPropertyUtils::ResolvePropertyByPath(
		CDO, ArrayPropertyPath, ArrayContainer, ResolveError);
	if (!ResolvedLeaf)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to resolve array_property_path '%s': %s"),
			*ArrayPropertyPath, *ResolveError));
	}
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(ResolvedLeaf);
	if (!ArrayProp)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("'%s' is not a TArray property"), *ArrayPropertyPath));
	}

	// Step 5: Open transaction + Modify() cluster. CreateInstancedArrayElement
	// explicitly does NOT do this (per its contract); the caller (us) is responsible.
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Append Instanced Array Element")));
	Blueprint->Modify();
	CDO->Modify();

	// Step 6: Append the new instanced sub-object via the shared helper. The helper
	// validates CPF_InstancedReference and class-compatibility internally, so we do
	// not duplicate those checks here.
	FString WriteError;
	UObject* NewElement = ClaireonPropertyUtils::CreateInstancedArrayElement(
		CDO, ElementClass, ArrayPropertyPath, WriteError);
	if (!NewElement)
	{
		return MakeErrorResult(WriteError);
	}

	// Step 7: Compute the new index/size. CreateInstancedArrayElement appends, so
	// the new element is always at NewArraySize - 1. Re-read the array length via
	// the already-resolved property/container pair we captured before the mutation.
	FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ArrayContainer));
	const int32 NewArraySize = Helper.Num();
	const int32 NewIndex = NewArraySize - 1;

	// Step 8: Mark the Blueprint package dirty so editor save/cook picks up the change.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Step 9: Build response JSON
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("array_property_path"), ArrayPropertyPath);
	Data->SetStringField(TEXT("element_class"), ElementClass->GetPathName());
	Data->SetNumberField(TEXT("new_index"), NewIndex);
	Data->SetNumberField(TEXT("new_array_size"), NewArraySize);
	Data->SetStringField(TEXT("new_element_path"), NewElement->GetPathName());

	const FString Summary = FString::Printf(
		TEXT("Appended %s to %s.%s at index %d (new size %d)"),
		*ElementClass->GetName(), *AssetPath, *ArrayPropertyPath, NewIndex, NewArraySize);

	return MakeSuccessResult(Data, Summary);
}
