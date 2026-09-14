// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonAdvisoryCoalesce.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ClaireonAdvisoryCoalesceInternal
{

	// U+001F unit separator: cannot appear in tool names, FName keys, asset paths, or
	// advisory text produced by our own emitters, so joined identity strings cannot
	// collide across field boundaries.
	static const TCHAR* ClAdvCo_Sep = TEXT("\x1F");

	static FString ClAdvCo_IdentityKey(const FClaireonAdvisory& Advisory)
	{
		FString Key;
		Key += FString::FromInt(static_cast<int32>(Advisory.Kind));
		Key += ClAdvCo_Sep;
		Key += Advisory.SourceTool;
		Key += ClAdvCo_Sep;
		Key += Advisory.Target;

		switch (Advisory.Kind)
		{
			case EClaireonAdvisoryKind::Hint:
				Key += ClAdvCo_Sep;
				Key += Advisory.HintKey.ToString();
				Key += ClAdvCo_Sep;
				Key += Advisory.Text;
				break;

			case EClaireonAdvisoryKind::Warning:
				Key += ClAdvCo_Sep;
				Key += Advisory.Text;
				break;

			case EClaireonAdvisoryKind::Summary:
				break;
		}
		return Key;
	}
}

TArray<FClaireonAdvisory> ClaireonAdvisoryCoalesce::Coalesce(const TArray<FClaireonAdvisory>& Raw)
{
	using namespace ClaireonAdvisoryCoalesceInternal;

	TArray<FClaireonAdvisory> Out;
	Out.Reserve(Raw.Num());
	TMap<FString, int32> IndexByIdentity;

	for (const FClaireonAdvisory& Advisory : Raw)
	{
		const FString Identity = ClAdvCo_IdentityKey(Advisory);
		if (int32* ExistingIndex = IndexByIdentity.Find(Identity))
		{
			FClaireonAdvisory& Existing = Out[*ExistingIndex];
			Existing.OccurrenceCount += Advisory.OccurrenceCount;

			if (Advisory.Kind == EClaireonAdvisoryKind::Summary)
			{
				// Last summary wins; its position remains at the first occurrence.
				Existing.Text = Advisory.Text;
				Existing.Payload = Advisory.Payload;
				Existing.Data = Advisory.Data;
			}
		}
		else
		{
			IndexByIdentity.Add(Identity, Out.Num());
			Out.Add(Advisory);
		}
	}

	return Out;
}

FString ClaireonAdvisoryCoalesce::RenderSummaryRollup(
	const TArray<FClaireonAdvisory>& Coalesced,
	int32 MaxBytes)
{
	FString Rollup;
	int32 Rendered = 0;
	int32 TotalSummaries = 0;
	bool bBudgetExhausted = false;

	// Reserve space for the elision line so appending it can never itself overflow.
	static const int32 ClAdvCo_ElisionReserveBytes = 32;

	for (const FClaireonAdvisory& Advisory : Coalesced)
	{
		// Tier 0 only: records carrying an aggregation spec are RenderTier1Rollup's rows.
		if (Advisory.Kind != EClaireonAdvisoryKind::Summary || Advisory.AggregationSpec.Num() > 0)
		{
			continue;
		}
		++TotalSummaries;
		if (bBudgetExhausted)
		{
			continue; // keep counting for the elision line
		}

		FString Line = TEXT("\n  ") + Advisory.SourceTool;
		if (Advisory.OccurrenceCount > 1)
		{
			Line += FString::Printf(TEXT(" x%d"), Advisory.OccurrenceCount);
		}
		if (!Advisory.Target.IsEmpty())
		{
			Line += TEXT(" on ") + Advisory.Target;
		}
		Line += TEXT(" -- ") + Advisory.Text;

		const int32 LineBytes = FTCHARToUTF8(*Line).Length();
		const int32 UsedBytes = FTCHARToUTF8(*Rollup).Length();
		if (UsedBytes + LineBytes > MaxBytes - ClAdvCo_ElisionReserveBytes)
		{
			bBudgetExhausted = true;
			continue;
		}

		Rollup += Line;
		++Rendered;
	}

	if (bBudgetExhausted && TotalSummaries > Rendered)
	{
		Rollup += FString::Printf(TEXT("\n  ... and %d more"), TotalSummaries - Rendered);
	}
	return Rollup;
}

namespace ClaireonAdvisoryCoalesceInternal
{
	/** Render a double as an integer when it is whole, else with two decimals. */
	static FString ClAdvCo_RenderNumber(double Value)
	{
		if (FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
		{
			return FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Value)));
		}
		return FString::Printf(TEXT("%.2f"), Value);
	}

	/** A field value as a comparable/renderable string; numbers normalized. */
	static bool ClAdvCo_FieldAsString(
		const TSharedPtr<FJsonObject>& Data, FName Field, FString& OutValue, double& OutNumber, bool& bOutIsNumber)
	{
		bOutIsNumber = false;
		OutNumber = 0.0;
		if (!Data.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonValue> Found = Data->TryGetField(Field.ToString());
		if (!Found.IsValid())
		{
			return false;
		}
		if (Found->Type == EJson::Number)
		{
			bOutIsNumber = true;
			OutNumber = Found->AsNumber();
			OutValue = ClAdvCo_RenderNumber(OutNumber);
			return true;
		}
		if (Found->Type == EJson::String)
		{
			OutValue = Found->AsString();
			return true;
		}
		if (Found->Type == EJson::Boolean)
		{
			OutValue = Found->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		}
		return false; // arrays/objects have no scalar merge semantics
	}
}

FString ClaireonAdvisoryCoalesce::RenderTier1Rollup(
	const TArray<FClaireonAdvisory>& Raw,
	int32 MaxBytes)
{
	using namespace ClaireonAdvisoryCoalesceInternal;

	// Group raw Tier 1 summary records by tool (stable first-occurrence order), then by
	// target within the tool.
	TArray<FString> ToolOrder;
	TMap<FString, TArray<const FClaireonAdvisory*>> ByTool;
	for (const FClaireonAdvisory& Advisory : Raw)
	{
		if (Advisory.Kind != EClaireonAdvisoryKind::Summary || Advisory.AggregationSpec.Num() == 0)
		{
			continue;
		}
		TArray<const FClaireonAdvisory*>& Rows = ByTool.FindOrAdd(Advisory.SourceTool);
		if (Rows.Num() == 0)
		{
			ToolOrder.Add(Advisory.SourceTool);
		}
		Rows.Add(&Advisory);
	}

	FString Rollup;
	for (const FString& ToolName : ToolOrder)
	{
		const TArray<const FClaireonAdvisory*>& Rows = ByTool[ToolName];
		const TArray<FClaireonFieldAggregation>& Spec = Rows[0]->AggregationSpec;

		// Distinct targets in first-occurrence order, and last row per target.
		TArray<FString> TargetOrder;
		TMap<FString, const FClaireonAdvisory*> LastPerTarget;
		for (const FClaireonAdvisory* Row : Rows)
		{
			if (!LastPerTarget.Contains(Row->Target))
			{
				TargetOrder.Add(Row->Target);
			}
			LastPerTarget.Add(Row->Target, Row);
		}

		TArray<FString> FieldRenders;
		for (const FClaireonFieldAggregation& FieldSpec : Spec)
		{
			const FString FieldName = FieldSpec.Field.ToString();
			switch (FieldSpec.Kind)
			{
				case EClaireonAggregationKind::Sum:
				{
					// Sum only the last observation of each target.
					double Total = 0.0;
					bool bAny = false;
					for (const FString& Target : TargetOrder)
					{
						FString Value; double Number; bool bIsNumber;
						if (ClAdvCo_FieldAsString(LastPerTarget[Target]->Data, FieldSpec.Field, Value, Number, bIsNumber) && bIsNumber)
						{
							Total += Number;
							bAny = true;
						}
					}
					if (bAny)
					{
						FieldRenders.Add(FieldName + TEXT(": ") + ClAdvCo_RenderNumber(Total));
					}
					break;
				}
				case EClaireonAggregationKind::Invariant:
				{
					// Per-target property: all of one target's records must agree.
					for (const FString& Target : TargetOrder)
					{
						FString FirstValue;
						bool bHaveFirst = false;
						bool bDiverged = false;
						FString DivergedTo;
						for (const FClaireonAdvisory* Row : Rows)
						{
							if (Row->Target != Target)
							{
								continue;
							}
							FString Value; double Number; bool bIsNumber;
							if (!ClAdvCo_FieldAsString(Row->Data, FieldSpec.Field, Value, Number, bIsNumber))
							{
								continue;
							}
							if (!bHaveFirst)
							{
								FirstValue = Value;
								bHaveFirst = true;
							}
							else if (Value != FirstValue)
							{
								bDiverged = true;
								DivergedTo = Value;
							}
						}
						if (bDiverged)
						{
							FieldRenders.Add(FString::Printf(TEXT("%s DIVERGED %s -> %s"),
								*FieldName, *FirstValue, *DivergedTo));
						}
						else if (bHaveFirst && TargetOrder.Num() == 1)
						{
							FieldRenders.Add(FieldName + TEXT(": ") + FirstValue);
						}
					}
					break;
				}
				case EClaireonAggregationKind::Histogram:
				{
					TArray<FString> ValueOrder;
					TMap<FString, int32> Counts;
					for (const FClaireonAdvisory* Row : Rows)
					{
						FString Value; double Number; bool bIsNumber;
						if (ClAdvCo_FieldAsString(Row->Data, FieldSpec.Field, Value, Number, bIsNumber))
						{
							int32& Count = Counts.FindOrAdd(Value);
							if (Count == 0)
							{
								ValueOrder.Add(Value);
							}
							++Count;
						}
					}
					if (ValueOrder.Num() > 0)
					{
						TArray<FString> Parts;
						for (const FString& Value : ValueOrder)
						{
							Parts.Add(FString::Printf(TEXT("%d %s"), Counts[Value], *Value));
						}
						FieldRenders.Add(FieldName + TEXT(": ") + FString::Join(Parts, TEXT("/")));
					}
					break;
				}
				case EClaireonAggregationKind::Last:
				{
					if (TargetOrder.Num() == 1)
					{
						FString Value; double Number; bool bIsNumber;
						if (ClAdvCo_FieldAsString(LastPerTarget[TargetOrder[0]]->Data, FieldSpec.Field, Value, Number, bIsNumber))
						{
							FieldRenders.Add(FieldName + TEXT(": ") + Value);
						}
					}
					break;
				}
				case EClaireonAggregationKind::DistinctCount:
				{
					// Distinct values across records; the field name "target" counts the
					// records' Target instead of a Data field.
					TSet<FString> Distinct;
					for (const FClaireonAdvisory* Row : Rows)
					{
						if (FieldSpec.Field == FName(TEXT("target")))
						{
							Distinct.Add(Row->Target);
						}
						else
						{
							FString Value; double Number; bool bIsNumber;
							if (ClAdvCo_FieldAsString(Row->Data, FieldSpec.Field, Value, Number, bIsNumber))
							{
								Distinct.Add(Value);
							}
						}
					}
					if (Distinct.Num() > 0)
					{
						FieldRenders.Add(FString::Printf(TEXT("%s: %d distinct"), *FieldName, Distinct.Num()));
					}
					break;
				}
			}
		}

		FString Line = FString::Printf(TEXT("\n  %s"), *ToolName);
		if (Rows.Num() > 1)
		{
			Line += FString::Printf(TEXT(" x%d"), Rows.Num());
		}
		if (TargetOrder.Num() == 1)
		{
			if (!TargetOrder[0].IsEmpty())
			{
				Line += TEXT(" on ") + TargetOrder[0];
			}
		}
		else
		{
			Line += FString::Printf(TEXT(" across %d targets"), TargetOrder.Num());
		}
		if (FieldRenders.Num() > 0)
		{
			Line += TEXT(" -- ") + FString::Join(FieldRenders, TEXT(", "));
		}
		else
		{
			// Use the last summary when Data has no mergeable scalar fields.
			Line += TEXT(" -- ") + Rows.Last()->Text;
		}

		const int32 UsedBytes = FTCHARToUTF8(*Rollup).Length();
		const int32 LineBytes = FTCHARToUTF8(*Line).Length();
		if (UsedBytes + LineBytes > MaxBytes)
		{
			break; // one line per tool; a tool set large enough to overflow is an emitter defect
		}
		Rollup += Line;
	}
	return Rollup;
}

int32 ClaireonAdvisoryCoalesce::MeasureWireAdvisoryBytes(const TArray<FClaireonAdvisory>& Advisories)
{
	int32 Bytes = 0;
	for (const FClaireonAdvisory& Advisory : Advisories)
	{
		Bytes += FTCHARToUTF8(*Advisory.SourceTool).Length();
		Bytes += FTCHARToUTF8(*Advisory.Target).Length();
		Bytes += FTCHARToUTF8(*Advisory.Text).Length();

		// Include the serialized hint payload in its wire cost.
		if (Advisory.Payload.IsValid())
		{
			FString PayloadJson;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
			FJsonSerializer::Serialize(Advisory.Payload.ToSharedRef(), Writer);
			Writer->Close();
			Bytes += FTCHARToUTF8(*PayloadJson).Length();
		}
	}
	return Bytes;
}

FString ClaireonAdvisoryCoalesce::ExtractTarget(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return FString();
	}

	FString AssetPath;
	FString GraphName;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	Arguments->TryGetStringField(TEXT("graph_name"), GraphName);

	if (AssetPath.IsEmpty())
	{
		// Keep session targets distinct when asset_path is absent. The bridge resolves open
		// sessions to asset paths; closed sessions retain this identity.
		FString SessionId;
		if (Arguments->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
		{
			AssetPath = TEXT("session:") + SessionId;
		}
	}

	if (!AssetPath.IsEmpty() && !GraphName.IsEmpty())
	{
		return AssetPath + TEXT(":") + GraphName;
	}
	return AssetPath.IsEmpty() ? GraphName : AssetPath;
}
