// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTools_Edit.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Chooser.h"
#include "IChooserColumn.h"
#include "ChooserPropertyAccess.h"
#include "ObjectChooser_Asset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"

// Column types for add_column
#include "GameplayTagColumn.h"
#include "BoolColumn.h"
#include "EnumColumn.h"
#include "MultiEnumColumn.h"
#include "FloatRangeColumn.h"
#include "OutputStructColumn.h"
#include "OutputObjectColumn.h"
#include "OutputBoolColumn.h"
#include "OutputFloatColumn.h"
#include "OutputEnumColumn.h"
#include "ObjectColumn.h"
#include "RandomizeColumn.h"

// ============================================================================
// claireon.chooser_edit — Edit structural properties of a ChooserTable
// ============================================================================

FString ClaireonTool_ChooserEdit::GetName() const { return TEXT("claireon.chooser_edit"); }

FString ClaireonTool_ChooserEdit::GetDescription() const
{
	return TEXT("Edit structural properties of a ChooserTable: result type, output class, "
		"context data parameters (add/remove input/output structs and classes), and fallback result. "
		"Only provided fields are modified; omitted fields are left unchanged.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserEdit::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Set result type"),
		{TEXT("ObjectResult"), TEXT("ClassResult")});
	S.AddString(TEXT("output_class"), TEXT("Class path for OutputObjectType"));
	S.AddObject(TEXT("add_parameter"), TEXT("Add context parameter: {\"type\": \"struct\"/\"class\", \"name\": \"StructOrClassName\", \"direction\": \"Input\"/\"Output\"/\"InputOutput\"}"));
	S.AddInteger(TEXT("remove_parameter"), TEXT("Remove context parameter at this index"));
	S.AddObject(TEXT("set_parameter_direction"), TEXT("Change parameter direction: {\"index\": N, \"direction\": \"Input\"/\"Output\"/\"InputOutput\"}"));
	S.AddString(TEXT("fallback_result_type"), TEXT("Set fallback result type: Asset, SoftAsset, EvaluateChooser, LookupProxy"));
	S.AddString(TEXT("fallback_result_value"), TEXT("Set fallback result value (asset/chooser path)"));
	return S.Build();
}


IClaireonTool::FToolResult ClaireonTool_ChooserEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Edit ChooserTable")));
	Chooser->Modify();
	bool bChanged = false;
	bool bContextDataChanged = false;

	// Result type
	FString ResultTypeStr;
	if (Arguments->TryGetStringField(TEXT("result_type"), ResultTypeStr))
	{
		if (ResultTypeStr == TEXT("ClassResult"))
			Chooser->ResultType = EObjectChooserResultType::ClassResult;
		else
			Chooser->ResultType = EObjectChooserResultType::ObjectResult;
		bChanged = true;
	}

	// Output class
	FString OutputClassStr;
	if (Arguments->TryGetStringField(TEXT("output_class"), OutputClassStr) && !OutputClassStr.IsEmpty())
	{
		UClass* OutputClass = FindObject<UClass>(nullptr, *OutputClassStr);
		if (!OutputClass) OutputClass = LoadObject<UClass>(nullptr, *OutputClassStr);
		if (OutputClass)
		{
			Chooser->OutputObjectType = OutputClass;
			bChanged = true;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *OutputClassStr));
		}
	}

	// Add parameter
	const TSharedPtr<FJsonObject>* AddParamObj;
	if (Arguments->TryGetObjectField(TEXT("add_parameter"), AddParamObj))
	{
		FString ParamType, ParamName, DirStr;
		(*AddParamObj)->TryGetStringField(TEXT("type"), ParamType);
		(*AddParamObj)->TryGetStringField(TEXT("name"), ParamName);
		(*AddParamObj)->TryGetStringField(TEXT("direction"), DirStr);
		EContextObjectDirection Dir = static_cast<EContextObjectDirection>(ClaireonChooserHelpers::ParseDirection(DirStr));

		UChooserTable* ContextOwner = Chooser->GetRootChooser();

		if (ParamType.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *ParamName);
			if (!Struct) Struct = LoadObject<UScriptStruct>(nullptr, *ParamName);
			if (!Struct)
			{
				return MakeErrorResult(FString::Printf(TEXT("Could not find struct: %s"), *ParamName));
			}

			FInstancedStruct NewParam;
			NewParam.InitializeAs<FContextObjectTypeStruct>();
			FContextObjectTypeStruct& StructParam = NewParam.GetMutable<FContextObjectTypeStruct>();
			StructParam.Struct = Struct;
			StructParam.Direction = Dir;
			ContextOwner->ContextData.Add(MoveTemp(NewParam));
			bChanged = true;
			bContextDataChanged = true;
		}
		else if (ParamType.Equals(TEXT("class"), ESearchCase::IgnoreCase))
		{
			UClass* Class = FindObject<UClass>(nullptr, *ParamName);
			if (!Class) Class = LoadObject<UClass>(nullptr, *ParamName);
			if (!Class)
			{
				return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *ParamName));
			}

			FInstancedStruct NewParam;
			NewParam.InitializeAs<FContextObjectTypeClass>();
			FContextObjectTypeClass& ClassParam = NewParam.GetMutable<FContextObjectTypeClass>();
			ClassParam.Class = Class;
			ClassParam.Direction = Dir;
			ContextOwner->ContextData.Add(MoveTemp(NewParam));
			bChanged = true;
			bContextDataChanged = true;
		}
		else
		{
			return MakeErrorResult(TEXT("add_parameter.type must be 'struct' or 'class'"));
		}
	}

	// Remove parameter
	double RemoveIdx;
	if (Arguments->TryGetNumberField(TEXT("remove_parameter"), RemoveIdx))
	{
		int32 Idx = static_cast<int32>(RemoveIdx);
		UChooserTable* ContextOwner = Chooser->GetRootChooser();
		if (!ContextOwner->ContextData.IsValidIndex(Idx))
		{
			return MakeErrorResult(FString::Printf(TEXT("Parameter index %d out of bounds (count: %d)"),
				Idx, ContextOwner->ContextData.Num()));
		}
		ContextOwner->ContextData.RemoveAt(Idx);
		bChanged = true;
		bContextDataChanged = true;
	}

	// Set parameter direction
	const TSharedPtr<FJsonObject>* SetDirObj;
	if (Arguments->TryGetObjectField(TEXT("set_parameter_direction"), SetDirObj))
	{
		double IdxVal;
		FString DirStr;
		(*SetDirObj)->TryGetNumberField(TEXT("index"), IdxVal);
		(*SetDirObj)->TryGetStringField(TEXT("direction"), DirStr);
		int32 Idx = static_cast<int32>(IdxVal);

		UChooserTable* ContextOwner = Chooser->GetRootChooser();
		if (!ContextOwner->ContextData.IsValidIndex(Idx))
		{
			return MakeErrorResult(FString::Printf(TEXT("Parameter index %d out of bounds"), Idx));
		}

		FInstancedStruct& Param = ContextOwner->ContextData[Idx];
		if (FContextObjectTypeBase* Base = Param.GetMutablePtr<FContextObjectTypeBase>())
		{
			Base->Direction = static_cast<EContextObjectDirection>(ClaireonChooserHelpers::ParseDirection(DirStr));
			bChanged = true;
			bContextDataChanged = true;
		}
	}

	// Fallback result
	FString FallbackType, FallbackValue;
	bool bHasFallbackType = Arguments->TryGetStringField(TEXT("fallback_result_type"), FallbackType);
	Arguments->TryGetStringField(TEXT("fallback_result_value"), FallbackValue);
	if (bHasFallbackType)
	{
		FInstancedStruct NewFallback;
		if (!ClaireonChooserHelpers::MakeRowResult(FallbackType, FallbackValue, NewFallback, Error))
		{
			return MakeErrorResult(Error);
		}
		Chooser->FallbackResult = MoveTemp(NewFallback);
		bChanged = true;
	}

	if (bChanged)
	{
		// Only recompile bindings when context data changes — avoids triggering
		// compile errors on unbound columns during unrelated property edits
		if (bContextDataChanged)
		{
			Chooser->Compile(true);
		}
		if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
		{
			return MakeErrorResult(Error);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetStringField(TEXT("result_type"), ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));
	Data->SetNumberField(TEXT("parameter_count"), Chooser->GetRootChooser()->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Edited ChooserTable '%s'"), *Chooser->GetName()));
}

// ============================================================================
// claireon.chooser_add_row
// ============================================================================

FString ClaireonTool_ChooserAddRow::GetName() const { return TEXT("claireon.chooser_add_row"); }

FString ClaireonTool_ChooserAddRow::GetDescription() const
{
	return TEXT("Add a new row to a ChooserTable. Optionally set the result and insert position.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserAddRow::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Row result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")});
	S.AddString(TEXT("result_value"), TEXT("Asset/chooser/proxy path for the result"));
	S.AddInteger(TEXT("insert_index"), TEXT("Insert position (default: append at end)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserAddRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ChooserTable Row")));
	Chooser->Modify();

	int32 CurrentRowCount = Chooser->ResultsStructs.Num();
	int32 InsertIndex = CurrentRowCount; // default: append

	double InsertIdxDouble;
	if (Arguments->TryGetNumberField(TEXT("insert_index"), InsertIdxDouble))
	{
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIdxDouble), 0, CurrentRowCount);
	}

	// Build the result FInstancedStruct
	FInstancedStruct NewResult;
	FString ResultType, ResultValue;
	Arguments->TryGetStringField(TEXT("result_type"), ResultType);
	Arguments->TryGetStringField(TEXT("result_value"), ResultValue);

	if (!ResultType.IsEmpty())
	{
		if (!ClaireonChooserHelpers::MakeRowResult(ResultType, ResultValue, NewResult, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else
	{
		// Default: empty asset chooser
		NewResult.InitializeAs<FAssetChooser>();
	}

	// Insert into ResultsStructs
	if (InsertIndex >= CurrentRowCount)
	{
		Chooser->ResultsStructs.Add(MoveTemp(NewResult));
	}
	else
	{
		Chooser->ResultsStructs.Insert(MoveTemp(NewResult), InsertIndex);
	}

	// Update DisabledRows
	if (InsertIndex >= Chooser->DisabledRows.Num())
	{
		Chooser->DisabledRows.Add(false);
	}
	else
	{
		Chooser->DisabledRows.Insert(false, InsertIndex);
	}

	// Update each column's RowValues via InsertRows
	for (FInstancedStruct& ColStruct : Chooser->ColumnsStructs)
	{
		if (ColStruct.IsValid())
		{
			FChooserColumnBase* Col = ColStruct.GetMutablePtr<FChooserColumnBase>();
			if (Col)
			{
				Col->InsertRows(InsertIndex, 1);
			}
		}
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), InsertIndex);
	Data->SetNumberField(TEXT("row_count"), Chooser->ResultsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added row at index %d (total: %d)"),
		InsertIndex, Chooser->ResultsStructs.Num()));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}

// ============================================================================
// claireon.chooser_remove_row
// ============================================================================

FString ClaireonTool_ChooserRemoveRow::GetName() const { return TEXT("claireon.chooser_remove_row"); }

FString ClaireonTool_ChooserRemoveRow::GetDescription() const
{
	return TEXT("Remove a row from a ChooserTable by index.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRemoveRow::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Index of the row to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRemoveRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	int32 RowIndex = static_cast<int32>(RowIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	if (!Chooser->ResultsStructs.IsValidIndex(RowIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Row index %d out of bounds (row count: %d)"),
			RowIndex, Chooser->ResultsStructs.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ChooserTable Row")));
	Chooser->Modify();

	// Remove from ResultsStructs
	Chooser->ResultsStructs.RemoveAt(RowIndex);

	// Remove from DisabledRows
	if (Chooser->DisabledRows.IsValidIndex(RowIndex))
	{
		Chooser->DisabledRows.RemoveAt(RowIndex);
	}

	// Remove from each column's RowValues
	TArray<uint32> RowIndices;
	RowIndices.Add(static_cast<uint32>(RowIndex));
	for (FInstancedStruct& ColStruct : Chooser->ColumnsStructs)
	{
		if (ColStruct.IsValid())
		{
			FChooserColumnBase* Col = ColStruct.GetMutablePtr<FChooserColumnBase>();
			if (Col)
			{
				Col->DeleteRows(RowIndices);
			}
		}
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("removed_index"), RowIndex);
	Data->SetNumberField(TEXT("row_count"), Chooser->ResultsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed row %d (remaining: %d)"),
		RowIndex, Chooser->ResultsStructs.Num()));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}

// ============================================================================
// claireon.chooser_set_row_result
// ============================================================================

FString ClaireonTool_ChooserSetRowResult::GetName() const { return TEXT("claireon.chooser_set_row_result"); }

FString ClaireonTool_ChooserSetRowResult::GetDescription() const
{
	return TEXT("Set or change the result for a specific row in a ChooserTable.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetRowResult::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Index of the row to modify"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")}, true);
	S.AddString(TEXT("result_value"), TEXT("Asset/chooser/proxy path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetRowResult::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	int32 RowIndex = static_cast<int32>(RowIdxDouble);

	FString ResultType, ResultValue;
	if (!Arguments->TryGetStringField(TEXT("result_type"), ResultType) || ResultType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: result_type"));
	}
	if (!Arguments->TryGetStringField(TEXT("result_value"), ResultValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: result_value"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	if (!Chooser->ResultsStructs.IsValidIndex(RowIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Row index %d out of bounds (row count: %d)"),
			RowIndex, Chooser->ResultsStructs.Num()));
	}

	FInstancedStruct NewResult;
	if (!ClaireonChooserHelpers::MakeRowResult(ResultType, ResultValue, NewResult, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Row Result")));
	Chooser->Modify();

	Chooser->ResultsStructs[RowIndex] = MoveTemp(NewResult);

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetStringField(TEXT("result_type"), ResultType);
	Data->SetStringField(TEXT("result_value"), ResultValue);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set row %d result to %s: %s"),
		RowIndex, *ResultType, *ResultValue));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}

// ============================================================================
// claireon.chooser_set_column_value
// ============================================================================

FString ClaireonTool_ChooserSetColumnValue::GetName() const { return TEXT("claireon.chooser_set_column_value"); }

FString ClaireonTool_ChooserSetColumnValue::GetDescription() const
{
	return TEXT("Set a column cell value for a specific row in a ChooserTable. "
		"The value format depends on the column type: "
		"GameplayTag: comma-separated tags or array; "
		"Bool: 'true'/'false'/'any'; "
		"Enum: {\"value\": \"Name\", \"comparison\": \"MatchEqual\"} or just the value name; "
		"FloatRange: {\"min\": N, \"max\": N, \"no_min\": bool, \"no_max\": bool}; "
		"OutputStruct: {field_name: value, ...}; "
		"OutputObject: asset path string.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetColumnValue::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Row index"), true);
	S.AddInteger(TEXT("column_index"), TEXT("Column index"), true);
	S.AddString(TEXT("value"), TEXT("Value to set (format depends on column type; use JSON string for structured values)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetColumnValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxDouble, ColIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	if (!Arguments->TryGetNumberField(TEXT("column_index"), ColIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: column_index"));
	}

	int32 RowIndex = static_cast<int32>(RowIdxDouble);
	int32 ColIndex = static_cast<int32>(ColIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	if (!Chooser->ColumnsStructs.IsValidIndex(ColIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Column index %d out of bounds (column count: %d)"),
			ColIndex, Chooser->ColumnsStructs.Num()));
	}

	// Get the value field - could be string, object, array, etc.
	TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Column Value")));
	Chooser->Modify();

	if (!ClaireonChooserHelpers::SetColumnCellValue(Chooser->ColumnsStructs[ColIndex], RowIndex, ValueField, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetNumberField(TEXT("column_index"), ColIndex);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set column %d value at row %d"), ColIndex, RowIndex));
}

// ============================================================================
// claireon.chooser_add_column
// ============================================================================

FString ClaireonTool_ChooserAddColumn::GetName() const { return TEXT("claireon.chooser_add_column"); }

FString ClaireonTool_ChooserAddColumn::GetDescription() const
{
	return TEXT("Add a new column to a ChooserTable. Specify the column type and optional property binding. "
		"Filter column types: GameplayTag, Bool, Enum, MultiEnum, FloatRange, Object. "
		"Output column types: OutputStruct, OutputBool, OutputFloat, OutputEnum, OutputObject. "
		"Special: Randomize.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserAddColumn::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("column_type"), TEXT("Column type to add"), {
		TEXT("GameplayTag"), TEXT("Bool"), TEXT("Enum"), TEXT("MultiEnum"), TEXT("FloatRange"),
		TEXT("Object"), TEXT("OutputStruct"), TEXT("OutputBool"), TEXT("OutputFloat"),
		TEXT("OutputEnum"), TEXT("OutputObject"), TEXT("Randomize")
	}, true);
	S.AddString(TEXT("property_path"), TEXT("Property binding path (e.g. 'Gait' or 'MyStruct.MyField'). Separate nested properties with '.'"));
	S.AddInteger(TEXT("context_index"), TEXT("Context parameter index to bind to (default: 0)"));
	S.AddInteger(TEXT("insert_index"), TEXT("Column position to insert at (default: append)"));
	return S.Build();
}

namespace
{
	/** Set up the property binding on a column's InputValue. */
	void SetupColumnBinding(FInstancedStruct& ColumnStruct, const TArray<FName>& PropertyChain, int32 ContextIndex)
	{
		const UScriptStruct* ColStructType = ColumnStruct.GetScriptStruct();
		if (!ColStructType) return;

		const FStructProperty* InputValueProp = CastField<FStructProperty>(ColStructType->FindPropertyByName(TEXT("InputValue")));
		if (!InputValueProp || InputValueProp->Struct != TBaseStructure<FInstancedStruct>::Get()) return;

		FInstancedStruct* InputValuePtr = InputValueProp->ContainerPtrToValuePtr<FInstancedStruct>(ColumnStruct.GetMutableMemory());
		if (!InputValuePtr || !InputValuePtr->IsValid()) return;

		const UScriptStruct* ParamStruct = InputValuePtr->GetScriptStruct();
		if (!ParamStruct) return;

		const FProperty* BindingProp = ParamStruct->FindPropertyByName(TEXT("Binding"));
		if (!BindingProp) return;

		uint8* ParamMemory = InputValuePtr->GetMutableMemory();
		FChooserPropertyBinding* Binding = reinterpret_cast<FChooserPropertyBinding*>(
			ParamMemory + BindingProp->GetOffset_ForInternal());

		Binding->PropertyBindingChain = PropertyChain;
		Binding->ContextIndex = ContextIndex;
	}
}

IClaireonTool::FToolResult ClaireonTool_ChooserAddColumn::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString ColumnType;
	if (!Arguments->TryGetStringField(TEXT("column_type"), ColumnType) || ColumnType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: column_type"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	// Parse property path
	TArray<FName> PropertyChain;
	FString PropertyPath;
	if (Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) && !PropertyPath.IsEmpty())
	{
		TArray<FString> Parts;
		PropertyPath.ParseIntoArray(Parts, TEXT("."));
		for (const FString& Part : Parts)
		{
			PropertyChain.Add(FName(*Part));
		}
	}

	int32 ContextIndex = 0;
	double CtxIdxDouble;
	if (Arguments->TryGetNumberField(TEXT("context_index"), CtxIdxDouble))
	{
		ContextIndex = static_cast<int32>(CtxIdxDouble);
	}

	int32 InsertIndex = Chooser->ColumnsStructs.Num();
	double InsertIdxDouble;
	if (Arguments->TryGetNumberField(TEXT("insert_index"), InsertIdxDouble))
	{
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIdxDouble), 0, Chooser->ColumnsStructs.Num());
	}

	// Create the column FInstancedStruct
	FInstancedStruct NewColumn;

	if (ColumnType.Equals(TEXT("GameplayTag"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FGameplayTagColumn>();
	}
	else if (ColumnType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FBoolColumn>();
	}
	else if (ColumnType.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FEnumColumn>();
	}
	else if (ColumnType.Equals(TEXT("MultiEnum"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FMultiEnumColumn>();
	}
	else if (ColumnType.Equals(TEXT("FloatRange"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FFloatRangeColumn>();
	}
	else if (ColumnType.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FObjectColumn>();
	}
	else if (ColumnType.Equals(TEXT("OutputStruct"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FOutputStructColumn>();
	}
	else if (ColumnType.Equals(TEXT("OutputBool"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FOutputBoolColumn>();
	}
	else if (ColumnType.Equals(TEXT("OutputFloat"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FOutputFloatColumn>();
	}
	else if (ColumnType.Equals(TEXT("OutputEnum"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FOutputEnumColumn>();
	}
	else if (ColumnType.Equals(TEXT("OutputObject"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FOutputObjectColumn>();
	}
	else if (ColumnType.Equals(TEXT("Randomize"), ESearchCase::IgnoreCase))
	{
		NewColumn.InitializeAs<FRandomizeColumn>();
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown column type: '%s'"), *ColumnType));
	}

	// Set up the property binding if provided
	if (PropertyChain.Num() > 0)
	{
		SetupColumnBinding(NewColumn, PropertyChain, ContextIndex);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ChooserTable Column")));
	Chooser->Modify();

	// Insert the column
	if (InsertIndex >= Chooser->ColumnsStructs.Num())
	{
		Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));
	}
	else
	{
		Chooser->ColumnsStructs.Insert(MoveTemp(NewColumn), InsertIndex);
	}

	// Initialize RowValues to match existing row count
	int32 RowCount = Chooser->ResultsStructs.Num();
	if (RowCount > 0)
	{
		FChooserColumnBase* Col = Chooser->ColumnsStructs[InsertIndex].GetMutablePtr<FChooserColumnBase>();
		if (Col)
		{
			Col->SetNumRows(RowCount);
		}
	}

	// Only compile if a binding was set — otherwise leave in clean "unbound" state
	if (PropertyChain.Num() > 0)
	{
		Chooser->Compile(true);
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("column_index"), InsertIndex);
	Data->SetNumberField(TEXT("column_count"), Chooser->ColumnsStructs.Num());
	Data->SetStringField(TEXT("column_type"), ColumnType);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added %s column at index %d (total: %d)"),
		*ColumnType, InsertIndex, Chooser->ColumnsStructs.Num()));
#else
	return MakeErrorResult(TEXT("Column editing requires editor data"));
#endif
}

// ============================================================================
// claireon.chooser_remove_column
// ============================================================================

FString ClaireonTool_ChooserRemoveColumn::GetName() const { return TEXT("claireon.chooser_remove_column"); }

FString ClaireonTool_ChooserRemoveColumn::GetDescription() const
{
	return TEXT("Remove a column from a ChooserTable by index.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRemoveColumn::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("column_index"), TEXT("Index of the column to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRemoveColumn::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double ColIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("column_index"), ColIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: column_index"));
	}
	int32 ColIndex = static_cast<int32>(ColIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	if (!Chooser->ColumnsStructs.IsValidIndex(ColIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Column index %d out of bounds (column count: %d)"),
			ColIndex, Chooser->ColumnsStructs.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ChooserTable Column")));
	Chooser->Modify();

	Chooser->ColumnsStructs.RemoveAt(ColIndex);

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("removed_index"), ColIndex);
	Data->SetNumberField(TEXT("column_count"), Chooser->ColumnsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed column %d (remaining: %d)"),
		ColIndex, Chooser->ColumnsStructs.Num()));
}
