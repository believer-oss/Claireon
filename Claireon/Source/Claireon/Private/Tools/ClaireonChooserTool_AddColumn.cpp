// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_AddColumn.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "IChooserColumn.h"
#include "ChooserPropertyAccess.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
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

FString ClaireonTool_ChooserAddColumn::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserAddColumn::GetOperation() const { return TEXT("add_column"); }

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

	// Set up the property binding when either a path or a non-default context_index is provided.
	// A non-default context_index alone is meaningful for OutputStruct columns that write the
	// whole output context parameter (root binding, empty PropertyChain).
	const bool bExplicitContextIndex = Arguments->HasTypedField<EJson::Number>(TEXT("context_index"));
	if (PropertyChain.Num() > 0 || bExplicitContextIndex)
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
