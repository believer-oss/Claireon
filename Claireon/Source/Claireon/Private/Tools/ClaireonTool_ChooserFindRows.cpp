// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserFindRows.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonChooserGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ObjectChooser_Asset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Compare two FJsonValue payloads by canonical-string equality. Cheap
	// and order-insensitive for primitives; for objects/arrays it is
	// position/key-order sensitive — fine for the v1 selectors here.
	bool JsonValueEquals(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		if (!A.IsValid() || !B.IsValid()) { return A.IsValid() == B.IsValid(); }
		FString StrA, StrB;
		auto WriterA = TJsonWriterFactory<>::Create(&StrA);
		auto WriterB = TJsonWriterFactory<>::Create(&StrB);
		FJsonSerializer::Serialize(A.ToSharedRef(), TEXT(""), WriterA);
		FJsonSerializer::Serialize(B.ToSharedRef(), TEXT(""), WriterB);
		return StrA.Equals(StrB);
	}

	struct FPredicate
	{
		bool bUseDisabled = false;
		bool bDisabledExpected = false;
		FString OutputAssetClass;
		FString OutputPathContains;
		FString OutputPathPrefix;

		bool bHasCellEquals = false;
		int32 CellEqColumn = INDEX_NONE;
		TSharedPtr<FJsonValue> CellEqValue;

		bool bHasEnumIncludes = false;
		int32 EnumIncColumn = INDEX_NONE;
		FString EnumIncName;
	};

	bool RowMatches(UChooserTable* Chooser, int32 RowIndex, const FPredicate& P)
	{
#if WITH_EDITORONLY_DATA
		if (P.bUseDisabled && Chooser->IsRowDisabled(RowIndex) != P.bDisabledExpected)
		{
			return false;
		}

		const bool bNeedResult =
			!P.OutputAssetClass.IsEmpty() ||
			!P.OutputPathContains.IsEmpty() ||
			!P.OutputPathPrefix.IsEmpty();

		if (bNeedResult)
		{
			if (!Chooser->ResultsStructs.IsValidIndex(RowIndex)) { return false; }
			const FInstancedStruct& Result = Chooser->ResultsStructs[RowIndex];
			FString AssetPath;
			FString AssetClass;
			if (const FAssetChooser* AC = Result.GetPtr<FAssetChooser>())
			{
				if (AC->Asset)
				{
					AssetPath = AC->Asset->GetPathName();
					AssetClass = AC->Asset->GetClass()->GetName();
				}
			}
			else if (const FSoftAssetChooser* SC = Result.GetPtr<FSoftAssetChooser>())
			{
				AssetPath = SC->Asset.ToString();
			}

			if (!P.OutputAssetClass.IsEmpty())
			{
				FString Want = P.OutputAssetClass;
				if (Want.EndsWith(TEXT("_C"))) { Want.LeftChopInline(2); }
				if (!AssetClass.Equals(Want)) { return false; }
			}
			if (!P.OutputPathContains.IsEmpty() && !AssetPath.Contains(P.OutputPathContains))
			{
				return false;
			}
			if (!P.OutputPathPrefix.IsEmpty() && !AssetPath.StartsWith(P.OutputPathPrefix))
			{
				return false;
			}
		}

		if (P.bHasCellEquals)
		{
			if (!Chooser->ColumnsStructs.IsValidIndex(P.CellEqColumn)) { return false; }
			TSharedPtr<FJsonValue> Cell = ClaireonChooserHelpers::SerializeColumnCellValue(
				Chooser->ColumnsStructs[P.CellEqColumn], RowIndex);
			if (!JsonValueEquals(Cell, P.CellEqValue)) { return false; }
		}

		if (P.bHasEnumIncludes)
		{
			if (!Chooser->ColumnsStructs.IsValidIndex(P.EnumIncColumn)) { return false; }
			TSharedPtr<FJsonValue> Cell = ClaireonChooserHelpers::SerializeColumnCellValue(
				Chooser->ColumnsStructs[P.EnumIncColumn], RowIndex);
			// Look at matched_entries (MultiEnum) or display_name/value_name (Enum).
			const TSharedPtr<FJsonObject>* CellObj = nullptr;
			if (!Cell->TryGetObject(CellObj) || !CellObj->IsValid()) { return false; }
			bool bFound = false;
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if ((*CellObj)->TryGetArrayField(TEXT("matched_entries"), Entries))
			{
				for (const TSharedPtr<FJsonValue>& E : *Entries)
				{
					const TSharedPtr<FJsonObject>* EO = nullptr;
					if (!E->TryGetObject(EO)) { continue; }
					FString Name;
					if ((*EO)->TryGetStringField(TEXT("display_name"), Name) && Name.Equals(P.EnumIncName)) { bFound = true; break; }
					if ((*EO)->TryGetStringField(TEXT("name"), Name) && Name.Equals(P.EnumIncName)) { bFound = true; break; }
				}
			}
			else
			{
				FString DName;
				if ((*CellObj)->TryGetStringField(TEXT("display_name"), DName) && DName.Equals(P.EnumIncName)) { bFound = true; }
				if (!bFound)
				{
					FString VName;
					if ((*CellObj)->TryGetStringField(TEXT("value_name"), VName) && VName.Equals(P.EnumIncName)) { bFound = true; }
				}
			}
			if (!bFound) { return false; }
		}
#endif
		return true;
	}
}

FString ClaireonTool_ChooserFindRows::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserFindRows::GetOperation() const { return TEXT("find_rows"); }

FString ClaireonTool_ChooserFindRows::GetDescription() const
{
	return TEXT("Search a chooser tree for rows matching a predicate. Selectors (AND-joined): "
		"disabled (bool), output_asset_class (string), output_path_contains (string), "
		"output_path_prefix (string), column_cell_equals ({column_index, value}), "
		"enum_value_includes ({column_index, enum_value_name}). Recurses into NestedChoosers and "
		"FNestedChooser/FEvaluateChooser row-result references when recursive=true (default).");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserFindRows::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("root"), TEXT("Root ChooserTable asset path"), true);
	S.AddBoolean(TEXT("recursive"), TEXT("Recurse into sub-choosers (default true)"));
	S.AddObject(TEXT("where"), TEXT("Predicate selectors (AND-joined). See description for shape."), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserFindRows::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString RootPath;
	if (!Arguments->TryGetStringField(TEXT("root"), RootPath) || RootPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: root"));
	}

	bool bRecursive = true;
	Arguments->TryGetBoolField(TEXT("recursive"), bRecursive);

	const TSharedPtr<FJsonObject>* WhereObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("where"), WhereObj) || !WhereObj->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: where"));
	}

	FPredicate Pred;
	bool bDisabledTmp;
	if ((*WhereObj)->TryGetBoolField(TEXT("disabled"), bDisabledTmp))
	{
		Pred.bUseDisabled = true;
		Pred.bDisabledExpected = bDisabledTmp;
	}
	(*WhereObj)->TryGetStringField(TEXT("output_asset_class"), Pred.OutputAssetClass);
	(*WhereObj)->TryGetStringField(TEXT("output_path_contains"), Pred.OutputPathContains);
	(*WhereObj)->TryGetStringField(TEXT("output_path_prefix"), Pred.OutputPathPrefix);

	const TSharedPtr<FJsonObject>* CellEqObj = nullptr;
	if ((*WhereObj)->TryGetObjectField(TEXT("column_cell_equals"), CellEqObj) && CellEqObj->IsValid())
	{
		double Col;
		if ((*CellEqObj)->TryGetNumberField(TEXT("column_index"), Col))
		{
			Pred.bHasCellEquals = true;
			Pred.CellEqColumn = static_cast<int32>(Col);
			Pred.CellEqValue = (*CellEqObj)->TryGetField(TEXT("value"));
		}
	}

	const TSharedPtr<FJsonObject>* EnumIncObj = nullptr;
	if ((*WhereObj)->TryGetObjectField(TEXT("enum_value_includes"), EnumIncObj) && EnumIncObj->IsValid())
	{
		double Col;
		if ((*EnumIncObj)->TryGetNumberField(TEXT("column_index"), Col)
			&& (*EnumIncObj)->TryGetStringField(TEXT("enum_value_name"), Pred.EnumIncName))
		{
			Pred.bHasEnumIncludes = true;
			Pred.EnumIncColumn = static_cast<int32>(Col);
		}
	}

	FString Error;
	UChooserTable* Root = ClaireonChooserHelpers::LoadChooserTableAsset(RootPath, Error);
	if (!Root)
	{
		return MakeErrorResult(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Matches;
	TSet<FString> SearchedPaths;

	auto VisitChooser = [&](UChooserTable* Cur, int32 /*Depth*/, const FString& /*ParentPath*/, int32 /*ParentRowIndex*/) -> bool
	{
		if (!Cur) { return true; }
		SearchedPaths.Add(Cur->GetPathName());
#if WITH_EDITORONLY_DATA
		const int32 RowCount = Cur->ResultsStructs.Num();
		for (int32 i = 0; i < RowCount; ++i)
		{
			if (!RowMatches(Cur, i, Pred)) { continue; }
			TSharedPtr<FJsonObject> Match = MakeShared<FJsonObject>();
			Match->SetStringField(TEXT("chooser_path"), Cur->GetPathName());
			Match->SetStringField(TEXT("chooser_name"), Cur->GetName());
			Match->SetNumberField(TEXT("row_index"), i);
			Match->SetBoolField(TEXT("disabled"), Cur->IsRowDisabled(i));
			if (Cur->ResultsStructs.IsValidIndex(i))
			{
				Match->SetObjectField(TEXT("result"),
					ClaireonChooserHelpers::SerializeRowResult(Cur->ResultsStructs[i]));
			}
			Matches.Add(MakeShared<FJsonValueObject>(Match));
		}
#endif
		return true;
	};

	if (bRecursive)
	{
		ClaireonChooserGraphHelpers::EnumerateChoosersBFS(Root, VisitChooser, /*MaxDepth*/ -1);
	}
	else
	{
		VisitChooser(Root, 0, FString(), INDEX_NONE);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("root"), Root->GetPathName());
	Data->SetNumberField(TEXT("total_matches"), Matches.Num());
	Data->SetNumberField(TEXT("choosers_searched"), SearchedPaths.Num());
	Data->SetArrayField(TEXT("matches"), Matches);

	const FString Summary = FString::Printf(TEXT("Found %d row(s) across %d chooser(s)"),
		Matches.Num(), SearchedPaths.Num());
	return MakeSuccessResult(Data, Summary);
}
