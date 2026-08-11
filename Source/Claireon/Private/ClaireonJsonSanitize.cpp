// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonJsonSanitize.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonJsonSanitizeInternal
{

// File-local discriminator prefix (JsonSanitize_) per module convention:
// anonymous namespaces are not isolation under unity batching.

bool JsonSanitize_ObjectHasNonFinite(const TSharedPtr<FJsonObject>& Object, int32 Depth);
bool JsonSanitize_SanitizeObject(const TSharedPtr<FJsonObject>& Object, const FString& Path, int32 Depth,
	TArray<FString>& OutPoisonedFieldPaths);

/**
 * Read-only pre-pass.
 *
 * This walk runs on every result of every tool, so the common case -- nothing
 * to fix -- must not allocate. Path strings and array rebuilds are paid for
 * only once a non-finite value is known to exist.
 */
bool JsonSanitize_ValueHasNonFinite(const TSharedPtr<FJsonValue>& Value, int32 Depth)
{
	if (!Value.IsValid() || Depth > ClaireonJsonSanitize::MaxRecursionDepth)
	{
		return false;
	}

	switch (Value->Type)
	{
		case EJson::Number:
			return !FMath::IsFinite(Value->AsNumber());

		case EJson::Object:
			return JsonSanitize_ObjectHasNonFinite(Value->AsObject(), Depth + 1);

		case EJson::Array:
			for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
			{
				if (JsonSanitize_ValueHasNonFinite(Element, Depth + 1))
				{
					return true;
				}
			}
			return false;

		default:
			return false;
	}
}

bool JsonSanitize_ObjectHasNonFinite(const TSharedPtr<FJsonObject>& Object, int32 Depth)
{
	if (!Object.IsValid() || Depth > ClaireonJsonSanitize::MaxRecursionDepth)
	{
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		if (JsonSanitize_ValueHasNonFinite(Pair.Value, Depth + 1))
		{
			return true;
		}
	}
	return false;
}

/** "frames" + [3] -> "frames[3]"; "frames[3]" + duration_ms -> "frames[3].duration_ms". */
FString JsonSanitize_JoinField(const FString& Path, const FString& Key)
{
	return Path.IsEmpty() ? Key : Path + TEXT(".") + Key;
}

/**
 * Mutating walk. Slot is the owning container's pointer, so a poisoned number
 * can be replaced with null in place.
 *
 * Replaced, never removed: a silently missing field is the same confident-wrong
 * -data failure this guard exists to remove, just harder to notice.
 */
bool JsonSanitize_SanitizeValue(TSharedPtr<FJsonValue>& Slot, const FString& Path, int32 Depth,
	TArray<FString>& OutPoisonedFieldPaths)
{
	if (!Slot.IsValid() || Depth > ClaireonJsonSanitize::MaxRecursionDepth)
	{
		return false;
	}

	switch (Slot->Type)
	{
		case EJson::Number:
		{
			if (FMath::IsFinite(Slot->AsNumber()))
			{
				return false;
			}
			Slot = MakeShared<FJsonValueNull>();
			OutPoisonedFieldPaths.Add(Path);
			return true;
		}

		case EJson::Object:
			// Objects are shared, so recursion mutates the same instance the parent
			// points at -- no write-back needed.
			return JsonSanitize_SanitizeObject(Slot->AsObject(), Path, Depth + 1, OutPoisonedFieldPaths);

		case EJson::Array:
		{
			// FJsonValue::AsArray() is const, so sanitize a copy and write it back.
			// Only reached once the pre-pass proved a replacement is coming.
			TArray<TSharedPtr<FJsonValue>> Elements = Slot->AsArray();
			bool bChanged = false;
			for (int32 Index = 0; Index < Elements.Num(); ++Index)
			{
				const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
				bChanged |= JsonSanitize_SanitizeValue(Elements[Index], ElementPath, Depth + 1, OutPoisonedFieldPaths);
			}
			if (bChanged)
			{
				Slot = MakeShared<FJsonValueArray>(Elements);
			}
			return bChanged;
		}

		default:
			return false;
	}
}

bool JsonSanitize_SanitizeObject(const TSharedPtr<FJsonObject>& Object, const FString& Path, int32 Depth,
	TArray<FString>& OutPoisonedFieldPaths)
{
	if (!Object.IsValid() || Depth > ClaireonJsonSanitize::MaxRecursionDepth)
	{
		return false;
	}

	bool bChanged = false;
	// auto&, and FString(*Pair.Key), because FJsonObject's key type is not FString on
	// every engine version: 5.8 keys the map with UE::FSharedString unless
	// UE_JSONOBJECT_LEGACY_STRING_KEYS=1. Note that a spelled-out
	// `const TPair<FString, TSharedPtr<FJsonValue>>&` DOES still compile on 5.8 --
	// TTuple's converting constructor makes a temporary and a const reference binds to
	// it -- so do not carry that idiom into a loop that writes: an assignment through
	// the temporary's TSharedPtr would be dropped. auto& keeps the reference on the
	// real element, which is what this loop needs.
	// `*Key` is `const TCHAR*` for both FString and FSharedString, so the FString
	// round-trip is one spelling that works on all four engines.
	for (auto& Pair : Object->Values)
	{
		const FString FieldPath = JsonSanitize_JoinField(Path, FString(*Pair.Key));
		bChanged |= JsonSanitize_SanitizeValue(Pair.Value, FieldPath, Depth + 1, OutPoisonedFieldPaths);
	}
	return bChanged;
}

} // namespace ClaireonJsonSanitizeInternal

namespace ClaireonJsonSanitize
{

bool SanitizeNonFiniteNumbers(const TSharedPtr<FJsonObject>& InOut, TArray<FString>& OutPoisonedFieldPaths)
{
	using namespace ClaireonJsonSanitizeInternal;

	if (!InOut.IsValid())
	{
		return false;
	}

	// Cheap const pre-pass keeps the common (clean) case allocation-free.
	if (!JsonSanitize_ObjectHasNonFinite(InOut, 0))
	{
		return false;
	}

	return JsonSanitize_SanitizeObject(InOut, FString(), 0, OutPoisonedFieldPaths);
}

FString DescribeReplacement(const FString& FieldPath)
{
	return FString::Printf(TEXT("Non-finite number replaced with null: %s"), *FieldPath);
}

} // namespace ClaireonJsonSanitize
