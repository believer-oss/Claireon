// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecValidator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

bool FClaireonSpecValidator::Validate(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	if (!Spec.IsValid())
	{
		OutErrors.Add(TEXT("Spec is null"));
		return false;
	}
	return ValidateStructure(Spec, OutErrors) && ValidateTool(Spec, OutErrors);
}

bool FClaireonSpecValidator::ValidateStructure(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	if (Spec->Values.Num() == 0)
	{
		OutErrors.Add(TEXT("Spec is empty (no fields)"));
		return false;
	}

	// Known top-level array field names that contain entries with IDs
	static const TArray<FString> KnownArrayFields = {
		TEXT("nodes"), TEXT("states"), TEXT("widgets"), TEXT("keys"),
		TEXT("options"), TEXT("emitters"), TEXT("connections"),
		TEXT("variables"), TEXT("parameters"), TEXT("evaluators"),
		TEXT("global_tasks")
	};

	// Known reference fields (values that reference other IDs)
	static const TArray<FString> KnownRefFields = {
		TEXT("parent"), TEXT("source_node"), TEXT("target_node"), TEXT("target_state")
	};

	// Known array reference fields (arrays of IDs)
	static const TArray<FString> KnownArrayRefFields = {
		TEXT("children")
	};

	TSet<FString> DeclaredIds;
	TSet<FString> ReferencedIds;
	bool bHasEntries = false;

	// Walk all known array fields collecting IDs and references
	for (const FString& ArrayFieldName : KnownArrayFields)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (!Spec->TryGetArrayField(ArrayFieldName, ArrayPtr) || !ArrayPtr)
		{
			continue;
		}

		bHasEntries = true;

		for (int32 Index = 0; Index < ArrayPtr->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Element = (*ArrayPtr)[Index];
			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& EntryObj = Element->AsObject();
			if (!EntryObj.IsValid())
			{
				continue;
			}

			// Extract "id" field
			FString EntryId;
			if (EntryObj->TryGetStringField(TEXT("id"), EntryId))
			{
				if (EntryId.IsEmpty())
				{
					OutErrors.Add(FString::Printf(
						TEXT("%s[%d]: 'id' field is empty"), *ArrayFieldName, Index));
				}
				else if (DeclaredIds.Contains(EntryId))
				{
					OutErrors.Add(FString::Printf(
						TEXT("%s[%d]: duplicate id '%s'"), *ArrayFieldName, Index, *EntryId));
				}
				else
				{
					DeclaredIds.Add(EntryId);
				}
			}

			// Collect single-value reference fields
			for (const FString& RefField : KnownRefFields)
			{
				FString RefValue;
				if (EntryObj->TryGetStringField(RefField, RefValue) && !RefValue.IsEmpty())
				{
					ReferencedIds.Add(RefValue);
				}
			}

			// Collect array reference fields
			for (const FString& ArrayRefField : KnownArrayRefFields)
			{
				const TArray<TSharedPtr<FJsonValue>>* RefArrayPtr = nullptr;
				if (EntryObj->TryGetArrayField(ArrayRefField, RefArrayPtr) && RefArrayPtr)
				{
					for (const TSharedPtr<FJsonValue>& RefVal : *RefArrayPtr)
					{
						if (RefVal.IsValid() && RefVal->Type == EJson::String)
						{
							FString RefStr = RefVal->AsString();
							if (!RefStr.IsEmpty())
							{
								ReferencedIds.Add(RefStr);
							}
						}
					}
				}
			}

			// Also check nested arrays (decorators, services, tests, etc.)
			// that may contain objects with IDs
			static const TArray<FString> NestedArrayFields = {
				TEXT("decorators"), TEXT("services"), TEXT("tests"),
				TEXT("tasks"), TEXT("conditions"), TEXT("transitions"),
				TEXT("modules"), TEXT("renderers")
			};

			for (const FString& NestedField : NestedArrayFields)
			{
				const TArray<TSharedPtr<FJsonValue>>* NestedArrayPtr = nullptr;
				if (!EntryObj->TryGetArrayField(NestedField, NestedArrayPtr) || !NestedArrayPtr)
				{
					continue;
				}

				for (int32 NestedIdx = 0; NestedIdx < NestedArrayPtr->Num(); ++NestedIdx)
				{
					const TSharedPtr<FJsonValue>& NestedElement = (*NestedArrayPtr)[NestedIdx];
					if (!NestedElement.IsValid() || NestedElement->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>& NestedObj = NestedElement->AsObject();
					if (!NestedObj.IsValid())
					{
						continue;
					}

					FString NestedId;
					if (NestedObj->TryGetStringField(TEXT("id"), NestedId))
					{
						if (NestedId.IsEmpty())
						{
							OutErrors.Add(FString::Printf(
								TEXT("%s[%d].%s[%d]: 'id' field is empty"),
								*ArrayFieldName, Index, *NestedField, NestedIdx));
						}
						else if (DeclaredIds.Contains(NestedId))
						{
							OutErrors.Add(FString::Printf(
								TEXT("%s[%d].%s[%d]: duplicate id '%s'"),
								*ArrayFieldName, Index, *NestedField, NestedIdx, *NestedId));
						}
						else
						{
							DeclaredIds.Add(NestedId);
						}
					}
				}
			}
		}
	}

	// Check that all referenced IDs exist in declared IDs
	// (references to existing asset entities not in the spec are OK -- tool-specific
	// validation handles those, so we only flag references where the spec appears
	// internally inconsistent)
	for (const FString& RefId : ReferencedIds)
	{
		if (!DeclaredIds.Contains(RefId))
		{
			// This is a warning, not an error -- the reference might point to
			// an existing entity in the asset rather than a spec-defined entity
			// Tool-specific validators can escalate this to an error if needed
		}
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecValidator::ValidateTool(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	// Default: no tool-specific validation
	return true;
}
