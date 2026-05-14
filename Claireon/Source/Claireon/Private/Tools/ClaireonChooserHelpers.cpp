// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "FileHelpers.h"
#include "UObject/SavePackage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "StructUtils/InstancedStruct.h"

// Chooser engine headers
#include "Chooser.h"
#include "IChooserColumn.h"
#include "IObjectChooser.h"
#include "ChooserPropertyAccess.h"
#include "IChooserParameterBase.h"

// Concrete column types
#include "GameplayTagColumn.h"
#include "BoolColumn.h"
#include "EnumColumn.h"
#include "FloatRangeColumn.h"
#include "OutputStructColumn.h"
#include "OutputObjectColumn.h"
#include "RandomizeColumn.h"
#include "FloatDistanceColumn.h"
#include "ObjectColumn.h"
#include "ObjectClassColumn.h"
#include "MultiEnumColumn.h"
#include "OutputBoolColumn.h"
#include "OutputFloatColumn.h"
#include "OutputEnumColumn.h"

// Concrete result/chooser types
#include "ObjectChooser_Asset.h"
#include "LookupProxy.h"
#include "ProxyAsset.h"

// Gameplay tags for tag container parsing
#include "GameplayTagContainer.h"

namespace ClaireonChooserHelpers
{

// ============================================================================
// Asset Loading & Saving
// ============================================================================

UChooserTable* LoadChooserTableAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UObject* LoadedObj = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UChooserTable* Chooser = Cast<UChooserTable>(LoadedObj);
	if (!Chooser)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a ChooserTable (actual type: %s)"),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return Chooser;
}

bool SaveChooserTable(UChooserTable* Chooser, FString& OutError)
{
	if (!Chooser)
	{
		OutError = TEXT("Chooser table is null");
		return false;
	}

	// Notify the engine of the change so dependent systems update
	Chooser->PostEditChange();
	Chooser->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Chooser->GetPackage());
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *Chooser->GetPackage()->GetName());
	}

	// Refresh the asset editor if it's open (ChooserTableEditor won't auto-refresh on external changes)
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Chooser);

	return bSaved;
}

// ============================================================================
// Result Type Helpers
// ============================================================================

FString ResultTypeToString(uint8 ResultType)
{
	switch (static_cast<EObjectChooserResultType>(ResultType))
	{
	case EObjectChooserResultType::ObjectResult: return TEXT("ObjectResult");
	case EObjectChooserResultType::ClassResult:  return TEXT("ClassResult");
	default: return TEXT("Unknown");
	}
}

// ============================================================================
// Context Data Serialization
// ============================================================================

TArray<TSharedPtr<FJsonValue>> SerializeContextData(const TArray<FInstancedStruct>& ContextData)
{
	TArray<TSharedPtr<FJsonValue>> Result;

	for (int32 i = 0; i < ContextData.Num(); ++i)
	{
		const FInstancedStruct& Entry = ContextData[i];
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetNumberField(TEXT("index"), i);

		if (!Entry.IsValid())
		{
			ParamObj->SetStringField(TEXT("type"), TEXT("Invalid"));
			Result.Add(MakeShared<FJsonValueObject>(ParamObj));
			continue;
		}

		const UScriptStruct* ScriptStruct = Entry.GetScriptStruct();

		// Try FContextObjectTypeStruct
		if (const FContextObjectTypeStruct* StructParam = Entry.GetPtr<FContextObjectTypeStruct>())
		{
			ParamObj->SetStringField(TEXT("param_type"), TEXT("Struct"));
			ParamObj->SetStringField(TEXT("struct_name"), StructParam->Struct ? StructParam->Struct->GetName() : TEXT("None"));
			if (StructParam->Struct)
			{
				ParamObj->SetStringField(TEXT("struct_path"), StructParam->Struct->GetPathName());
			}

			switch (StructParam->Direction)
			{
			case EContextObjectDirection::Read:      ParamObj->SetStringField(TEXT("direction"), TEXT("Input")); break;
			case EContextObjectDirection::Write:     ParamObj->SetStringField(TEXT("direction"), TEXT("Output")); break;
			case EContextObjectDirection::ReadWrite:  ParamObj->SetStringField(TEXT("direction"), TEXT("InputOutput")); break;
			}
		}
		// Try FContextObjectTypeClass
		else if (const FContextObjectTypeClass* ClassParam = Entry.GetPtr<FContextObjectTypeClass>())
		{
			ParamObj->SetStringField(TEXT("param_type"), TEXT("Class"));
			ParamObj->SetStringField(TEXT("class_name"), ClassParam->Class ? ClassParam->Class->GetName() : TEXT("None"));
			if (ClassParam->Class)
			{
				ParamObj->SetStringField(TEXT("class_path"), ClassParam->Class->GetPathName());
			}

			switch (ClassParam->Direction)
			{
			case EContextObjectDirection::Read:      ParamObj->SetStringField(TEXT("direction"), TEXT("Input")); break;
			case EContextObjectDirection::Write:     ParamObj->SetStringField(TEXT("direction"), TEXT("Output")); break;
			case EContextObjectDirection::ReadWrite:  ParamObj->SetStringField(TEXT("direction"), TEXT("InputOutput")); break;
			}
		}
		else
		{
			ParamObj->SetStringField(TEXT("param_type"), ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"));
		}

		Result.Add(MakeShared<FJsonValueObject>(ParamObj));
	}

	return Result;
}

// ============================================================================
// Property Binding Serialization
// ============================================================================

/** Resolve a property chain through a UStruct to find the final property type. */
static void ResolvePropertyChainType(const UStruct* StartStruct, const TArray<FName>& Chain, TSharedPtr<FJsonObject>& BindingObj)
{
	if (!StartStruct || Chain.IsEmpty())
	{
		return;
	}

	const UStruct* CurrentStruct = StartStruct;
	FString ResolvedPath;
	const FProperty* FinalProp = nullptr;

	for (int32 i = 0; i < Chain.Num(); ++i)
	{
		if (!CurrentStruct)
		{
			break;
		}

		const FProperty* Prop = CurrentStruct->FindPropertyByName(Chain[i]);
		if (!Prop)
		{
			// Try as a function
			if (const UClass* AsClass = Cast<UClass>(CurrentStruct))
			{
				UFunction* Func = AsClass->FindFunctionByName(Chain[i]);
				if (Func)
				{
					if (!ResolvedPath.IsEmpty()) ResolvedPath += TEXT(".");
					ResolvedPath += Chain[i].ToString() + TEXT("()");
					// Get return property for further chaining
					for (TFieldIterator<FProperty> It(Func); It; ++It)
					{
						if (It->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							FinalProp = *It;
							if (const FStructProperty* StructProp = CastField<FStructProperty>(FinalProp))
							{
								CurrentStruct = StructProp->Struct;
							}
							else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(FinalProp))
							{
								CurrentStruct = ObjProp->PropertyClass;
							}
							else
							{
								CurrentStruct = nullptr;
							}
							break;
						}
					}
					continue;
				}
			}
			break;
		}

		FinalProp = Prop;
		if (!ResolvedPath.IsEmpty()) ResolvedPath += TEXT(".");
		ResolvedPath += Chain[i].ToString();

		// Follow the chain into nested structs/objects
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			CurrentStruct = StructProp->Struct;
		}
		else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			CurrentStruct = ObjProp->PropertyClass;
		}
		else
		{
			CurrentStruct = nullptr;
		}
	}

	if (FinalProp)
	{
		BindingObj->SetStringField(TEXT("resolved_type"), FinalProp->GetCPPType());

		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(FinalProp))
		{
			if (const UEnum* Enum = EnumProp->GetEnum())
			{
				BindingObj->SetStringField(TEXT("enum_type"), Enum->GetName());
			}
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(FinalProp))
		{
			if (ByteProp->Enum)
			{
				BindingObj->SetStringField(TEXT("enum_type"), ByteProp->Enum->GetName());
			}
		}
		else if (const FStructProperty* StructProp = CastField<FStructProperty>(FinalProp))
		{
			BindingObj->SetStringField(TEXT("struct_type"), StructProp->Struct->GetName());
		}
		else if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(FinalProp))
		{
			BindingObj->SetStringField(TEXT("object_class"), ObjProp->PropertyClass->GetName());
		}
	}

	if (!ResolvedPath.IsEmpty())
	{
		BindingObj->SetStringField(TEXT("resolved_path"), ResolvedPath);
	}
}

TSharedPtr<FJsonObject> SerializePropertyBinding(const FInstancedStruct& ColumnStruct, const TArray<FInstancedStruct>* ContextData)
{
	TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();

	if (!ColumnStruct.IsValid())
	{
		return BindingObj;
	}

	// Get the column base to access GetInputValue
	const FChooserColumnBase* ColBase = ColumnStruct.GetPtr<FChooserColumnBase>();
	if (!ColBase)
	{
		return BindingObj;
	}

	// Access the InputValue field from the column struct
	const UScriptStruct* ColStruct = ColumnStruct.GetScriptStruct();
	if (ColStruct)
	{
		const FStructProperty* InputValueProp = CastField<FStructProperty>(ColStruct->FindPropertyByName(TEXT("InputValue")));
		if (InputValueProp && InputValueProp->Struct == TBaseStructure<FInstancedStruct>::Get())
		{
			const FInstancedStruct* InputValuePtr = InputValueProp->ContainerPtrToValuePtr<FInstancedStruct>(ColumnStruct.GetMemory());
			if (InputValuePtr && InputValuePtr->IsValid())
			{
				const UScriptStruct* ParamStruct = InputValuePtr->GetScriptStruct();

				if (ParamStruct)
				{
					const FProperty* BindingProp = ParamStruct->FindPropertyByName(TEXT("Binding"));
					if (BindingProp)
					{
						const uint8* ParamMemory = InputValuePtr->GetMemory();
						const FChooserPropertyBinding* Binding = reinterpret_cast<const FChooserPropertyBinding*>(
							ParamMemory + BindingProp->GetOffset_ForInternal());

						// Serialize the binding chain
						TArray<TSharedPtr<FJsonValue>> ChainArray;
						for (const FName& Name : Binding->PropertyBindingChain)
						{
							ChainArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
						}
						BindingObj->SetArrayField(TEXT("property_chain"), ChainArray);
						BindingObj->SetNumberField(TEXT("context_index"), Binding->ContextIndex);

#if WITH_EDITORONLY_DATA
						if (!Binding->DisplayName.IsEmpty())
						{
							BindingObj->SetStringField(TEXT("display_name"), Binding->DisplayName);
						}
#endif

						// Resolve the binding through the context data to find actual property types
						if (ContextData && ContextData->IsValidIndex(Binding->ContextIndex))
						{
							const FInstancedStruct& ContextEntry = (*ContextData)[Binding->ContextIndex];
							const UStruct* ContextStruct = nullptr;

							if (const FContextObjectTypeStruct* StructCtx = ContextEntry.GetPtr<FContextObjectTypeStruct>())
							{
								ContextStruct = StructCtx->Struct;
								BindingObj->SetStringField(TEXT("context_struct"), StructCtx->Struct ? StructCtx->Struct->GetName() : TEXT("None"));
							}
							else if (const FContextObjectTypeClass* ClassCtx = ContextEntry.GetPtr<FContextObjectTypeClass>())
							{
								ContextStruct = ClassCtx->Class;
								BindingObj->SetStringField(TEXT("context_class"), ClassCtx->Class ? ClassCtx->Class->GetName() : TEXT("None"));
							}

							if (ContextStruct)
							{
								ResolvePropertyChainType(ContextStruct, Binding->PropertyBindingChain, BindingObj);
							}
						}
					}
				}
			}
		}
	}

	return BindingObj;
}

// ============================================================================
// Column Serialization
// ============================================================================

TSharedPtr<FJsonObject> SerializeColumn(const FInstancedStruct& ColumnStruct, int32 ColumnIndex, const TArray<FInstancedStruct>* ContextData)
{
	TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
	ColObj->SetNumberField(TEXT("index"), ColumnIndex);

	if (!ColumnStruct.IsValid())
	{
		ColObj->SetStringField(TEXT("type"), TEXT("Invalid"));
		return ColObj;
	}

	const UScriptStruct* ScriptStruct = ColumnStruct.GetScriptStruct();
	ColObj->SetStringField(TEXT("type"), ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"));

	const FChooserColumnBase* ColBase = ColumnStruct.GetPtr<FChooserColumnBase>();
	if (!ColBase)
	{
		return ColObj;
	}

#if WITH_EDITORONLY_DATA
	ColObj->SetBoolField(TEXT("disabled"), ColBase->bDisabled);
#endif

	ColObj->SetBoolField(TEXT("has_filters"), ColBase->HasFilters());
	ColObj->SetBoolField(TEXT("has_outputs"), ColBase->HasOutputs());

	// Serialize property binding with type resolution
	TSharedPtr<FJsonObject> Binding = SerializePropertyBinding(ColumnStruct, ContextData);
	if (Binding->Values.Num() > 0)
	{
		ColObj->SetObjectField(TEXT("binding"), Binding);
	}

	// Type-specific settings
	if (const FGameplayTagColumn* TagCol = ColumnStruct.GetPtr<FGameplayTagColumn>())
	{
		ColObj->SetStringField(TEXT("tag_match_type"),
			TagCol->TagMatchType == EGameplayContainerMatchType::Any ? TEXT("Any") : TEXT("All"));

		FString MatchDir;
		switch (TagCol->TagMatchDirection)
		{
		case EGameplayTagMatchDirection::RowValueInInput: MatchDir = TEXT("RowValueInInput"); break;
		case EGameplayTagMatchDirection::InputInRowValue: MatchDir = TEXT("InputInRowValue"); break;
		}
		ColObj->SetStringField(TEXT("tag_match_direction"), MatchDir);
		ColObj->SetBoolField(TEXT("match_exact"), TagCol->bMatchExact);
		ColObj->SetBoolField(TEXT("invert_matching"), TagCol->bInvertMatchingLogic);
		ColObj->SetNumberField(TEXT("row_count"), TagCol->RowValues.Num());
	}
	else if (const FBoolColumn* BoolCol = ColumnStruct.GetPtr<FBoolColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), BoolCol->RowValuesWithAny.Num());
	}
	else if (const FEnumColumn* EnumCol = ColumnStruct.GetPtr<FEnumColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), EnumCol->RowValues.Num());
#if WITH_EDITOR
		const UEnum* Enum = EnumCol->GetEnum();
		if (Enum)
		{
			ColObj->SetStringField(TEXT("enum_type"), Enum->GetName());
		}
#endif
	}
	else if (const FMultiEnumColumn* MultiEnumCol = ColumnStruct.GetPtr<FMultiEnumColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), MultiEnumCol->RowValues.Num());
#if WITH_EDITOR
		const UEnum* Enum = MultiEnumCol->GetEnum();
		if (Enum)
		{
			ColObj->SetStringField(TEXT("enum_type"), Enum->GetName());
			// List all enum values for reference
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (int32 i = 0; i < Enum->NumEnums() - 1; ++i) // -1 to skip _MAX
			{
				TSharedPtr<FJsonObject> EV = MakeShared<FJsonObject>();
				EV->SetNumberField(TEXT("index"), i);
				EV->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(i));
				EV->SetNumberField(TEXT("bit"), 1 << i);
				EnumValues.Add(MakeShared<FJsonValueObject>(EV));
			}
			ColObj->SetArrayField(TEXT("enum_values"), EnumValues);
		}
#endif
	}
	else if (const FFloatRangeColumn* FloatCol = ColumnStruct.GetPtr<FFloatRangeColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), FloatCol->RowValues.Num());
		ColObj->SetBoolField(TEXT("wrap_input"), FloatCol->bWrapInput);
		if (FloatCol->bWrapInput)
		{
			ColObj->SetNumberField(TEXT("min_value"), FloatCol->MinValue);
			ColObj->SetNumberField(TEXT("max_value"), FloatCol->MaxValue);
		}
	}
	else if (const FObjectColumn* ObjCol = ColumnStruct.GetPtr<FObjectColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), ObjCol->RowValues.Num());
	}
	else if (const FOutputStructColumn* OutStructCol = ColumnStruct.GetPtr<FOutputStructColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), OutStructCol->RowValues.Num());
	}
	else if (const FOutputObjectColumn* OutObjCol = ColumnStruct.GetPtr<FOutputObjectColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), OutObjCol->RowValues.Num());
	}
	else if (const FOutputBoolColumn* OutBoolCol = ColumnStruct.GetPtr<FOutputBoolColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), OutBoolCol->RowValues.Num());
	}
	else if (const FOutputFloatColumn* OutFloatCol = ColumnStruct.GetPtr<FOutputFloatColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), OutFloatCol->RowValues.Num());
	}
	else if (const FOutputEnumColumn* OutEnumCol = ColumnStruct.GetPtr<FOutputEnumColumn>())
	{
		ColObj->SetNumberField(TEXT("row_count"), OutEnumCol->RowValues.Num());
#if WITH_EDITOR
		const UEnum* Enum = OutEnumCol->GetEnum();
		if (Enum)
		{
			ColObj->SetStringField(TEXT("enum_type"), Enum->GetName());
		}
#endif
	}

	return ColObj;
}

// ============================================================================
// Column Cell Value Serialization
// ============================================================================

TSharedPtr<FJsonValue> SerializeColumnCellValue(const FInstancedStruct& ColumnStruct, int32 RowIndex)
{
	if (!ColumnStruct.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}

	// GameplayTag column
	if (const FGameplayTagColumn* TagCol = ColumnStruct.GetPtr<FGameplayTagColumn>())
	{
		if (TagCol->RowValues.IsValidIndex(RowIndex))
		{
			const FGameplayTagContainer& Tags = TagCol->RowValues[RowIndex];
			TArray<TSharedPtr<FJsonValue>> TagArray;
			for (const FGameplayTag& Tag : Tags)
			{
				TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			return MakeShared<FJsonValueArray>(TagArray);
		}
	}
	// Bool column
	else if (const FBoolColumn* BoolCol = ColumnStruct.GetPtr<FBoolColumn>())
	{
		if (BoolCol->RowValuesWithAny.IsValidIndex(RowIndex))
		{
			switch (BoolCol->RowValuesWithAny[RowIndex])
			{
			case EBoolColumnCellValue::MatchFalse: return MakeShared<FJsonValueString>(TEXT("false"));
			case EBoolColumnCellValue::MatchTrue:  return MakeShared<FJsonValueString>(TEXT("true"));
			case EBoolColumnCellValue::MatchAny:   return MakeShared<FJsonValueString>(TEXT("any"));
			}
		}
	}
	// Enum column
	else if (const FEnumColumn* EnumCol = ColumnStruct.GetPtr<FEnumColumn>())
	{
		if (EnumCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserEnumRowData& Data = EnumCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> EnumObj = MakeShared<FJsonObject>();
			EnumObj->SetNumberField(TEXT("value"), Data.Value);
#if WITH_EDITORONLY_DATA
			if (!Data.ValueName.IsNone())
			{
				EnumObj->SetStringField(TEXT("value_name"), Data.ValueName.ToString());
			}
#endif
			FString CompStr;
			switch (Data.Comparison)
			{
			case EEnumColumnCellValueComparison::MatchEqual:    CompStr = TEXT("MatchEqual"); break;
			case EEnumColumnCellValueComparison::MatchNotEqual: CompStr = TEXT("MatchNotEqual"); break;
			case EEnumColumnCellValueComparison::MatchAny:      CompStr = TEXT("MatchAny"); break;
			default: CompStr = TEXT("Unknown"); break;
			}
			EnumObj->SetStringField(TEXT("comparison"), CompStr);
			return MakeShared<FJsonValueObject>(EnumObj);
		}
	}
	// Float range column
	else if (const FFloatRangeColumn* FloatCol = ColumnStruct.GetPtr<FFloatRangeColumn>())
	{
		if (FloatCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserFloatRangeRowData& Data = FloatCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
			RangeObj->SetNumberField(TEXT("min"), Data.Min);
			RangeObj->SetNumberField(TEXT("max"), Data.Max);
			RangeObj->SetBoolField(TEXT("no_min"), Data.bNoMin);
			RangeObj->SetBoolField(TEXT("no_max"), Data.bNoMax);
			return MakeShared<FJsonValueObject>(RangeObj);
		}
	}
	// Output struct column
	else if (const FOutputStructColumn* OutStructCol = ColumnStruct.GetPtr<FOutputStructColumn>())
	{
		if (OutStructCol->RowValues.IsValidIndex(RowIndex))
		{
			TSharedPtr<FJsonObject> StructObj = SerializeInstancedStructToJson(OutStructCol->RowValues[RowIndex]);
			return MakeShared<FJsonValueObject>(StructObj);
		}
	}
	// Output object column
	else if (const FOutputObjectColumn* OutObjCol = ColumnStruct.GetPtr<FOutputObjectColumn>())
	{
		if (OutObjCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserOutputObjectRowData& Data = OutObjCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> ObjResult = SerializeRowResult(Data.Value);
			return MakeShared<FJsonValueObject>(ObjResult);
		}
	}
	// MultiEnum column - bitmask where each bit is an enum value
	else if (const FMultiEnumColumn* MultiEnumCol = ColumnStruct.GetPtr<FMultiEnumColumn>())
	{
		if (MultiEnumCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserMultiEnumRowData& Data = MultiEnumCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> MultiObj = MakeShared<FJsonObject>();
			MultiObj->SetNumberField(TEXT("bitmask"), Data.Value);

			if (Data.Value == 0)
			{
				MultiObj->SetStringField(TEXT("match"), TEXT("Any"));
			}
			else
			{
				// Decode bitmask to matched enum value names
				TArray<TSharedPtr<FJsonValue>> MatchedValues;
#if WITH_EDITOR
				const UEnum* Enum = MultiEnumCol->GetEnum();
				if (Enum)
				{
					for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
					{
						if (Data.Value & (1 << i))
						{
							MatchedValues.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(i)));
						}
					}
				}
#endif
				MultiObj->SetArrayField(TEXT("matched_values"), MatchedValues);
			}
			return MakeShared<FJsonValueObject>(MultiObj);
		}
	}
	// Object column
	else if (const FObjectColumn* ObjCol = ColumnStruct.GetPtr<FObjectColumn>())
	{
		if (ObjCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserObjectRowData& Data = ObjCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> ObjData = MakeShared<FJsonObject>();
			ObjData->SetStringField(TEXT("value"), Data.Value.ToString());
			FString CompStr;
			switch (Data.Comparison)
			{
			case EObjectColumnCellValueComparison::MatchEqual:    CompStr = TEXT("MatchEqual"); break;
			case EObjectColumnCellValueComparison::MatchNotEqual: CompStr = TEXT("MatchNotEqual"); break;
			case EObjectColumnCellValueComparison::MatchAny:      CompStr = TEXT("MatchAny"); break;
			default: CompStr = TEXT("Unknown"); break;
			}
			ObjData->SetStringField(TEXT("comparison"), CompStr);
			return MakeShared<FJsonValueObject>(ObjData);
		}
	}
	// Output bool column
	else if (const FOutputBoolColumn* OutBoolCol = ColumnStruct.GetPtr<FOutputBoolColumn>())
	{
		if (OutBoolCol->RowValues.IsValidIndex(RowIndex))
		{
			return MakeShared<FJsonValueBoolean>(OutBoolCol->RowValues[RowIndex]);
		}
	}
	// Output float column
	else if (const FOutputFloatColumn* OutFloatCol = ColumnStruct.GetPtr<FOutputFloatColumn>())
	{
		if (OutFloatCol->RowValues.IsValidIndex(RowIndex))
		{
			return MakeShared<FJsonValueNumber>(OutFloatCol->RowValues[RowIndex]);
		}
	}
	// Output enum column
	else if (const FOutputEnumColumn* OutEnumCol = ColumnStruct.GetPtr<FOutputEnumColumn>())
	{
		if (OutEnumCol->RowValues.IsValidIndex(RowIndex))
		{
			const FChooserOutputEnumRowData& Data = OutEnumCol->RowValues[RowIndex];
			TSharedPtr<FJsonObject> EnumObj = MakeShared<FJsonObject>();
			EnumObj->SetNumberField(TEXT("value"), Data.Value);
#if WITH_EDITORONLY_DATA
			if (!Data.ValueName.IsNone())
			{
				EnumObj->SetStringField(TEXT("value_name"), Data.ValueName.ToString());
			}
#endif
			return MakeShared<FJsonValueObject>(EnumObj);
		}
	}

	return MakeShared<FJsonValueNull>();
}

// ============================================================================
// Row Result Serialization
// ============================================================================

TSharedPtr<FJsonObject> SerializeRowResult(const FInstancedStruct& ResultStruct)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!ResultStruct.IsValid())
	{
		Result->SetStringField(TEXT("type"), TEXT("None"));
		return Result;
	}

	const UScriptStruct* ScriptStruct = ResultStruct.GetScriptStruct();
	Result->SetStringField(TEXT("struct_type"), ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"));

	// FAssetChooser - hard asset reference
	if (const FAssetChooser* AssetChooser = ResultStruct.GetPtr<FAssetChooser>())
	{
		Result->SetStringField(TEXT("type"), TEXT("Asset"));
		if (AssetChooser->Asset)
		{
			Result->SetStringField(TEXT("asset_path"), AssetChooser->Asset->GetPathName());
			Result->SetStringField(TEXT("asset_name"), AssetChooser->Asset->GetName());
			Result->SetStringField(TEXT("asset_class"), AssetChooser->Asset->GetClass()->GetName());
		}
		else
		{
			Result->SetStringField(TEXT("asset_path"), TEXT("None"));
		}
	}
	// FSoftAssetChooser - soft reference
	else if (const FSoftAssetChooser* SoftChooser = ResultStruct.GetPtr<FSoftAssetChooser>())
	{
		Result->SetStringField(TEXT("type"), TEXT("SoftAsset"));
		Result->SetStringField(TEXT("asset_path"), SoftChooser->Asset.ToString());
	}
	// FEvaluateChooser - external chooser reference
	else if (const FEvaluateChooser* EvalChooser = ResultStruct.GetPtr<FEvaluateChooser>())
	{
		Result->SetStringField(TEXT("type"), TEXT("EvaluateChooser"));
		if (EvalChooser->Chooser)
		{
			Result->SetStringField(TEXT("chooser_path"), EvalChooser->Chooser->GetPathName());
			Result->SetStringField(TEXT("chooser_name"), EvalChooser->Chooser->GetName());
		}
		else
		{
			Result->SetStringField(TEXT("chooser_path"), TEXT("None"));
		}
	}
	// FNestedChooser - nested chooser within same asset
	else if (const FNestedChooser* NestedChooser = ResultStruct.GetPtr<FNestedChooser>())
	{
		Result->SetStringField(TEXT("type"), TEXT("NestedChooser"));
		if (NestedChooser->Chooser)
		{
			Result->SetStringField(TEXT("chooser_path"), NestedChooser->Chooser->GetPathName());
			Result->SetStringField(TEXT("chooser_name"), NestedChooser->Chooser->GetName());
		}
		else
		{
			Result->SetStringField(TEXT("chooser_path"), TEXT("None"));
		}
	}
	// FLookupProxy - proxy table lookup
	else if (const FLookupProxy* ProxyLookup = ResultStruct.GetPtr<FLookupProxy>())
	{
		Result->SetStringField(TEXT("type"), TEXT("LookupProxy"));
		if (ProxyLookup->Proxy)
		{
			Result->SetStringField(TEXT("proxy_name"), ProxyLookup->Proxy->GetName());
			Result->SetStringField(TEXT("proxy_path"), ProxyLookup->Proxy->GetPathName());
			Result->SetStringField(TEXT("proxy_guid"), ProxyLookup->Proxy->Guid.ToString());
		}
		else
		{
			Result->SetStringField(TEXT("proxy_name"), TEXT("None"));
		}
	}
	else
	{
		// Unknown result type - serialize generically
		Result->SetStringField(TEXT("type"), ScriptStruct ? ScriptStruct->GetName() : TEXT("Unknown"));
		TSharedPtr<FJsonObject> Fields = SerializeInstancedStructToJson(ResultStruct);
		Result->SetObjectField(TEXT("fields"), Fields);
	}

	return Result;
}

// ============================================================================
// Generic FInstancedStruct Serialization
// ============================================================================

TSharedPtr<FJsonObject> SerializeInstancedStructToJson(const FInstancedStruct& Struct)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!Struct.IsValid())
	{
		return Result;
	}

	const UScriptStruct* ScriptStruct = Struct.GetScriptStruct();
	if (!ScriptStruct)
	{
		return Result;
	}

	Result->SetStringField(TEXT("_struct_type"), ScriptStruct->GetName());

	const uint8* StructMemory = Struct.GetMemory();

	for (TFieldIterator<FProperty> It(ScriptStruct); It; ++It)
	{
		FProperty* Property = *It;
		const uint8* PropMem = StructMemory + Property->GetOffset_ForInternal();

		// Object / soft-object properties: ExportText's representation is
		// fragile and can emit "None" for valid refs. Go directly to the
		// pointer's path so downstream consumers get the real asset path
		// (load-bearing for chooser OutputStruct cells whose Anim field
		// is an FObjectPtr to an AnimSequence that the editor displays
		// as the visible row asset).
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(PropMem);
			Result->SetStringField(Property->GetName(),
				Obj ? Obj->GetPathName() : TEXT("None"));
			continue;
		}
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr& Soft = *reinterpret_cast<const FSoftObjectPtr*>(PropMem);
			Result->SetStringField(Property->GetName(),
				Soft.IsNull() ? TEXT("None") : Soft.ToString());
			continue;
		}

		FString ValueStr;
		Property->ExportText_Direct(ValueStr, PropMem, PropMem, nullptr, PPF_None);
		Result->SetStringField(Property->GetName(), ValueStr);
	}

	return Result;
}

// ============================================================================
// Column Cell Value Writing
// ============================================================================

bool SetColumnCellValue(FInstancedStruct& ColumnStruct, int32 RowIndex,
	const TSharedPtr<FJsonValue>& InValue, FString& OutError)
{
	if (!ColumnStruct.IsValid())
	{
		OutError = TEXT("Column struct is invalid");
		return false;
	}

	// If the value is a string that looks like JSON, try to parse it as a JSON object or array.
	// This handles the case where MCP delivers structured values as strings.
	TSharedPtr<FJsonValue> Value = InValue;
	FString StrVal;
	if (InValue->TryGetString(StrVal))
	{
		StrVal.TrimStartAndEndInline();
		if (StrVal.StartsWith(TEXT("{")) || StrVal.StartsWith(TEXT("[")))
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StrVal);
			TSharedPtr<FJsonValue> Parsed;
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				Value = Parsed;
			}
		}
	}

	// GameplayTag column
	if (FGameplayTagColumn* TagCol = ColumnStruct.GetMutablePtr<FGameplayTagColumn>())
	{
		if (!TagCol->RowValues.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, TagCol->RowValues.Num());
			return false;
		}

		FGameplayTagContainer NewTags;
		// Accept a string (comma-separated) or array of strings
		FString TagString;
		if (Value->TryGetString(TagString))
		{
			TArray<FString> TagNames;
			TagString.ParseIntoArray(TagNames, TEXT(","));
			for (FString& TagName : TagNames)
			{
				TagName.TrimStartAndEndInline();
				FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), false);
				if (Tag.IsValid())
				{
					NewTags.AddTag(Tag);
				}
			}
		}
		else if (const TArray<TSharedPtr<FJsonValue>>* ArrayValue; Value->TryGetArray(ArrayValue))
		{
			for (const auto& Elem : *ArrayValue)
			{
				FString TagName;
				if (Elem->TryGetString(TagName))
				{
					FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), false);
					if (Tag.IsValid())
					{
						NewTags.AddTag(Tag);
					}
				}
			}
		}
		TagCol->RowValues[RowIndex] = NewTags;
		return true;
	}

	// Bool column
	if (FBoolColumn* BoolCol = ColumnStruct.GetMutablePtr<FBoolColumn>())
	{
		if (!BoolCol->RowValuesWithAny.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, BoolCol->RowValuesWithAny.Num());
			return false;
		}

		FString BoolStr;
		if (Value->TryGetString(BoolStr))
		{
			BoolStr = BoolStr.ToLower();
			if (BoolStr == TEXT("true"))       BoolCol->RowValuesWithAny[RowIndex] = EBoolColumnCellValue::MatchTrue;
			else if (BoolStr == TEXT("false"))  BoolCol->RowValuesWithAny[RowIndex] = EBoolColumnCellValue::MatchFalse;
			else if (BoolStr == TEXT("any"))    BoolCol->RowValuesWithAny[RowIndex] = EBoolColumnCellValue::MatchAny;
			else
			{
				OutError = FString::Printf(TEXT("Invalid bool value '%s'. Expected 'true', 'false', or 'any'."), *BoolStr);
				return false;
			}
			return true;
		}
		OutError = TEXT("Bool column value must be a string ('true', 'false', or 'any')");
		return false;
	}

	// Enum column
	if (FEnumColumn* EnumCol = ColumnStruct.GetMutablePtr<FEnumColumn>())
	{
		if (!EnumCol->RowValues.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, EnumCol->RowValues.Num());
			return false;
		}

		const TSharedPtr<FJsonObject>* ObjValue;
		FString StrValue;
		if (Value->TryGetObject(ObjValue))
		{
			FString ValueName;
			(*ObjValue)->TryGetStringField(TEXT("value"), ValueName);
			FString CompStr;
			(*ObjValue)->TryGetStringField(TEXT("comparison"), CompStr);

			FChooserEnumRowData& Data = EnumCol->RowValues[RowIndex];
#if WITH_EDITORONLY_DATA
			Data.ValueName = FName(*ValueName);
#endif
			// Resolve enum value
#if WITH_EDITOR
			const UEnum* Enum = EnumCol->GetEnum();
			if (Enum)
			{
				int64 EnumValue = Enum->GetValueByNameString(ValueName);
				if (EnumValue != INDEX_NONE)
				{
					Data.Value = static_cast<uint8>(EnumValue);
				}
			}
#endif
			if (CompStr == TEXT("MatchNotEqual"))       Data.Comparison = EEnumColumnCellValueComparison::MatchNotEqual;
			else if (CompStr == TEXT("MatchAny"))        Data.Comparison = EEnumColumnCellValueComparison::MatchAny;
			else                                         Data.Comparison = EEnumColumnCellValueComparison::MatchEqual;

			return true;
		}
		else if (Value->TryGetString(StrValue))
		{
			// Simple string - just the value name with MatchEqual
			FChooserEnumRowData& Data = EnumCol->RowValues[RowIndex];
#if WITH_EDITORONLY_DATA
			Data.ValueName = FName(*StrValue);
#endif
#if WITH_EDITOR
			const UEnum* Enum = EnumCol->GetEnum();
			if (Enum)
			{
				int64 EnumValue = Enum->GetValueByNameString(StrValue);
				if (EnumValue != INDEX_NONE)
				{
					Data.Value = static_cast<uint8>(EnumValue);
				}
			}
#endif
			Data.Comparison = EEnumColumnCellValueComparison::MatchEqual;
			return true;
		}
		OutError = TEXT("Enum column value must be a string (value name) or object {\"value\": \"...\", \"comparison\": \"...\"}");
		return false;
	}

	// Float range column
	if (FFloatRangeColumn* FloatCol = ColumnStruct.GetMutablePtr<FFloatRangeColumn>())
	{
		if (!FloatCol->RowValues.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, FloatCol->RowValues.Num());
			return false;
		}

		const TSharedPtr<FJsonObject>* ObjValue;
		if (Value->TryGetObject(ObjValue))
		{
			FChooserFloatRangeRowData& Data = FloatCol->RowValues[RowIndex];
			double MinVal, MaxVal;
			if ((*ObjValue)->TryGetNumberField(TEXT("min"), MinVal)) Data.Min = static_cast<float>(MinVal);
			if ((*ObjValue)->TryGetNumberField(TEXT("max"), MaxVal)) Data.Max = static_cast<float>(MaxVal);
			(*ObjValue)->TryGetBoolField(TEXT("no_min"), Data.bNoMin);
			(*ObjValue)->TryGetBoolField(TEXT("no_max"), Data.bNoMax);
			return true;
		}
		OutError = TEXT("Float range column value must be an object {\"min\": N, \"max\": N, \"no_min\": bool, \"no_max\": bool}");
		return false;
	}

	// Output struct column - deserialize JSON fields into the struct
	if (FOutputStructColumn* OutStructCol = ColumnStruct.GetMutablePtr<FOutputStructColumn>())
	{
		if (!OutStructCol->RowValues.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, OutStructCol->RowValues.Num());
			return false;
		}

		const TSharedPtr<FJsonObject>* ObjValue;
		if (Value->TryGetObject(ObjValue))
		{
			FInstancedStruct& RowStruct = OutStructCol->RowValues[RowIndex];
			if (!RowStruct.IsValid())
			{
				OutError = TEXT("Output struct at this row is not initialized");
				return false;
			}

			const UScriptStruct* StructType = RowStruct.GetScriptStruct();
			uint8* StructMemory = RowStruct.GetMutableMemory();

			for (const auto& Pair : (*ObjValue)->Values)
			{
				FProperty* Property = StructType->FindPropertyByName(FName(*Pair.Key));
				if (!Property)
				{
					continue; // Skip unknown fields
				}

				FString ValueStr;
				if (Pair.Value->TryGetString(ValueStr))
				{
					Property->ImportText_Direct(*ValueStr, StructMemory + Property->GetOffset_ForInternal(), nullptr, PPF_None);
				}
			}
			return true;
		}
		OutError = TEXT("Output struct column value must be an object with field name/value pairs");
		return false;
	}

	// Output object column
	if (FOutputObjectColumn* OutObjCol = ColumnStruct.GetMutablePtr<FOutputObjectColumn>())
	{
		if (!OutObjCol->RowValues.IsValidIndex(RowIndex))
		{
			OutError = FString::Printf(TEXT("Row index %d out of bounds (column has %d rows)"), RowIndex, OutObjCol->RowValues.Num());
			return false;
		}

		FString AssetPath;
		if (Value->TryGetString(AssetPath))
		{
			FInstancedStruct NewResult;
			if (!MakeRowResult(TEXT("Asset"), AssetPath, NewResult, OutError))
			{
				return false;
			}
			OutObjCol->RowValues[RowIndex].Value = MoveTemp(NewResult);
			return true;
		}
		OutError = TEXT("Output object column value must be an asset path string");
		return false;
	}

	OutError = FString::Printf(TEXT("Unsupported column type for editing: %s"),
		ColumnStruct.GetScriptStruct() ? *ColumnStruct.GetScriptStruct()->GetName() : TEXT("Unknown"));
	return false;
}

// ============================================================================
// Row Result Construction
// ============================================================================

bool MakeRowResult(const FString& ResultType, const FString& ResultValue,
	FInstancedStruct& OutResult, FString& OutError)
{
	if (ResultType.Equals(TEXT("Asset"), ESearchCase::IgnoreCase))
	{
		UObject* Asset = nullptr;
		if (!ResultValue.IsEmpty())
		{
			auto ResolveResult = ClaireonPathResolver::Resolve(ResultValue);
			if (ResolveResult.bSuccess)
			{
				Asset = FSoftObjectPath(ResolveResult.ResolvedPath.Path).TryLoad();
			}
			if (!Asset)
			{
				OutError = FString::Printf(TEXT("Failed to load asset: %s"), *ResultValue);
				return false;
			}
		}
		OutResult.InitializeAs<FAssetChooser>();
		OutResult.GetMutable<FAssetChooser>().Asset = Asset;
		return true;
	}
	else if (ResultType.Equals(TEXT("SoftAsset"), ESearchCase::IgnoreCase))
	{
		OutResult.InitializeAs<FSoftAssetChooser>();
		OutResult.GetMutable<FSoftAssetChooser>().Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(ResultValue));
		return true;
	}
	else if (ResultType.Equals(TEXT("EvaluateChooser"), ESearchCase::IgnoreCase))
	{
		UChooserTable* Chooser = nullptr;
		if (!ResultValue.IsEmpty())
		{
			Chooser = LoadChooserTableAsset(ResultValue, OutError);
			if (!Chooser)
			{
				return false;
			}
		}
		OutResult.InitializeAs<FEvaluateChooser>();
		OutResult.GetMutable<FEvaluateChooser>().Chooser = Chooser;
		return true;
	}
	else if (ResultType.Equals(TEXT("LookupProxy"), ESearchCase::IgnoreCase))
	{
		UProxyAsset* Proxy = nullptr;
		if (!ResultValue.IsEmpty())
		{
			auto ResolveResult = ClaireonPathResolver::Resolve(ResultValue);
			if (ResolveResult.bSuccess)
			{
				UObject* LoadedObj = FSoftObjectPath(ResolveResult.ResolvedPath.Path).TryLoad();
				Proxy = Cast<UProxyAsset>(LoadedObj);
			}
			if (!Proxy)
			{
				OutError = FString::Printf(TEXT("Failed to load proxy asset: %s"), *ResultValue);
				return false;
			}
		}
		OutResult.InitializeAs<FLookupProxy>();
		OutResult.GetMutable<FLookupProxy>().Proxy = Proxy;
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown result type: '%s'. Expected: Asset, SoftAsset, EvaluateChooser, LookupProxy"), *ResultType);
	return false;
}

// ============================================================================
// Shared Utility Functions
// ============================================================================

bool ValidateNewAssetPath(const FString& InPath, FString& OutCanonPath, FString& OutAssetName, FString& OutError)
{
	OutCanonPath = FClaireonSessionManager::CanonicalizePath(InPath);
	if (OutCanonPath.IsEmpty())
	{
		OutError = TEXT("Invalid asset path. Must start with /Game/.");
		return false;
	}
	if (StaticFindObject(nullptr, nullptr, *OutCanonPath))
	{
		OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *OutCanonPath);
		return false;
	}
	OutAssetName = FPackageName::GetShortName(OutCanonPath);
	return true;
}

bool SaveNewAsset(UObject* Asset, FString& OutError)
{
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *PackageFileName, SaveArgs);
	if (Result.Result != ESavePackageResult::Success)
	{
		OutError = FString::Printf(TEXT("Failed to save package '%s'"), *Package->GetName());
		return false;
	}
	return true;
}

uint8 ParseDirection(const FString& Str)
{
	if (Str.Equals(TEXT("Output"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("Write"), ESearchCase::IgnoreCase))
		return static_cast<uint8>(EContextObjectDirection::Write);
	if (Str.Equals(TEXT("InputOutput"), ESearchCase::IgnoreCase) || Str.Equals(TEXT("ReadWrite"), ESearchCase::IgnoreCase))
		return static_cast<uint8>(EContextObjectDirection::ReadWrite);
	return static_cast<uint8>(EContextObjectDirection::Read);
}

// ============================================================================
// Context-data helpers (shared by chooser and proxyasset context edits)
// ============================================================================

bool AddContextParameter(
	TArray<FInstancedStruct>& ContextData,
	const FString& TypeString,
	const FString& NameString,
	const FString& DirectionString,
	bool& bContextChanged,
	FString& OutError)
{
	const EContextObjectDirection Dir =
		static_cast<EContextObjectDirection>(ParseDirection(DirectionString));

	if (TypeString.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
	{
		UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *NameString);
		if (!Struct) Struct = LoadObject<UScriptStruct>(nullptr, *NameString);
		if (!Struct)
		{
			OutError = FString::Printf(TEXT("Could not find struct: %s"), *NameString);
			return false;
		}

		FInstancedStruct NewParam;
		NewParam.InitializeAs<FContextObjectTypeStruct>();
		FContextObjectTypeStruct& StructParam = NewParam.GetMutable<FContextObjectTypeStruct>();
		StructParam.Struct = Struct;
		StructParam.Direction = Dir;
		ContextData.Add(MoveTemp(NewParam));
		bContextChanged = true;
		return true;
	}

	if (TypeString.Equals(TEXT("class"), ESearchCase::IgnoreCase))
	{
		UClass* Class = FindObject<UClass>(nullptr, *NameString);
		if (!Class) Class = LoadObject<UClass>(nullptr, *NameString);
		if (!Class)
		{
			OutError = FString::Printf(TEXT("Could not find class: %s"), *NameString);
			return false;
		}

		FInstancedStruct NewParam;
		NewParam.InitializeAs<FContextObjectTypeClass>();
		FContextObjectTypeClass& ClassParam = NewParam.GetMutable<FContextObjectTypeClass>();
		ClassParam.Class = Class;
		ClassParam.Direction = Dir;
		ContextData.Add(MoveTemp(NewParam));
		bContextChanged = true;
		return true;
	}

	OutError = TEXT("add_parameter.type must be 'struct' or 'class'");
	return false;
}

bool RemoveContextParameter(
	TArray<FInstancedStruct>& ContextData,
	int32 Index,
	bool& bContextChanged,
	FString& OutError)
{
	if (!ContextData.IsValidIndex(Index))
	{
		OutError = FString::Printf(TEXT("Parameter index %d out of bounds (count: %d)"),
			Index, ContextData.Num());
		return false;
	}
	ContextData.RemoveAt(Index);
	bContextChanged = true;
	return true;
}

bool SetContextParameterDirection(
	TArray<FInstancedStruct>& ContextData,
	int32 Index,
	const FString& DirectionString,
	bool& bContextChanged,
	FString& OutError)
{
	if (!ContextData.IsValidIndex(Index))
	{
		OutError = FString::Printf(TEXT("Parameter index %d out of bounds"), Index);
		return false;
	}

	FInstancedStruct& Param = ContextData[Index];
	FContextObjectTypeBase* Base = Param.GetMutablePtr<FContextObjectTypeBase>();
	if (!Base)
	{
		OutError = FString::Printf(TEXT("ContextData entry %d is not a FContextObjectTypeBase"), Index);
		return false;
	}

	Base->Direction = static_cast<EContextObjectDirection>(ParseDirection(DirectionString));
	bContextChanged = true;
	return true;
}

} // namespace ClaireonChooserHelpers
