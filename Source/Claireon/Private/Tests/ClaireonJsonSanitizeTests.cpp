// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonJsonSanitize.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <limits>

// ---------------------------------------------------------------------------
// P0-1: trace tools emit the bare token `inf`, which is not legal JSON.
//
// The engine seeds every open frame with EndTime = +inf
// (TraceServices/Private/Model/Frames.cpp), and UE's JSON writer prints
// doubles with %.17g (Json/Public/Policies/JsonPrintPolicy.h), so a capture
// stopped mid-frame produces output no client can parse. Because trace_open
// returns the session_id every other trace_* tool requires, that takes out the
// whole family from its documented entry point.
//
// These drive ClaireonJsonSanitize::SanitizeNonFiniteNumbers directly. The
// result-boundary tests (formatter + envelope) live in
// ClaireonJsonSanitizeBoundaryTests.cpp.
// ---------------------------------------------------------------------------
namespace ClaireonJsonSanitizeTestsInternal
{

// File-local discriminator prefix (JsonSanitizeTests_) per module convention:
// anonymous namespaces are not isolation under unity batching.

double JsonSanitizeTests_PosInf()
{
	return std::numeric_limits<double>::infinity();
}
double JsonSanitizeTests_NegInf()
{
	return -std::numeric_limits<double>::infinity();
}
double JsonSanitizeTests_NaN()
{
	return std::numeric_limits<double>::quiet_NaN();
}

/** True when the field exists and is JSON null. A replaced field must remain
 *  present -- a silently dropped field is the same failure in a new costume. */
bool JsonSanitizeTests_IsNullField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* Found = Object->Values.Find(FieldName);
	return Found != nullptr && Found->IsValid() && (*Found)->Type == EJson::Null;
}

/** Builds { "frames": [ {...}, {...}, {...}, { "duration_ms": <Value> } ] } so
 *  the poisoned value sits at frames[3].duration_ms. */
TSharedPtr<FJsonObject> JsonSanitizeTests_MakeNestedFrames(double PoisonedValue)
{
	TArray<TSharedPtr<FJsonValue>> Frames;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
		Frame->SetNumberField(TEXT("index"), static_cast<double>(Index));
		Frame->SetNumberField(TEXT("duration_ms"), Index == 3 ? PoisonedValue : 16.6);
		Frames.Add(MakeShared<FJsonValueObject>(Frame));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("frames"), Frames);
	return Root;
}

/** Nests Depth objects under "child", with a poisoned "value" at depth 3. */
TSharedPtr<FJsonObject> JsonSanitizeTests_MakeDeepChain(int32 Depth)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Cursor = Root;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
		if (Index == 3)
		{
			Child->SetNumberField(TEXT("value"), JsonSanitizeTests_PosInf());
		}
		Cursor->SetObjectField(TEXT("child"), Child);
		Cursor = Child;
	}
	return Root;
}

} // namespace ClaireonJsonSanitizeTestsInternal

using namespace ClaireonJsonSanitizeTestsInternal;

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, PositiveInfinityBecomesNull, UNTEST_TIMEOUTMS(30000))
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("duration_ms"), JsonSanitizeTests_PosInf());

	TArray<FString> Poisoned;
	const bool bChanged = ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned);

	UNTEST_EXPECT_TRUE(bChanged);
	UNTEST_EXPECT_TRUE(JsonSanitizeTests_IsNullField(Root, TEXT("duration_ms")));
	UNTEST_ASSERT_EQ(Poisoned.Num(), 1);
	UNTEST_EXPECT_STREQ(*Poisoned[0], TEXT("duration_ms"));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, NegativeInfinityAndNaNBecomeNull, UNTEST_TIMEOUTMS(30000))
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("min_ms"), JsonSanitizeTests_NegInf());
	Root->SetNumberField(TEXT("avg_ms"), JsonSanitizeTests_NaN());
	Root->SetNumberField(TEXT("max_ms"), 16.7);

	TArray<FString> Poisoned;
	const bool bChanged = ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned);

	UNTEST_EXPECT_TRUE(bChanged);
	UNTEST_EXPECT_TRUE(JsonSanitizeTests_IsNullField(Root, TEXT("min_ms")));
	UNTEST_EXPECT_TRUE(JsonSanitizeTests_IsNullField(Root, TEXT("avg_ms")));
	UNTEST_EXPECT_FALSE(JsonSanitizeTests_IsNullField(Root, TEXT("max_ms")));
	UNTEST_EXPECT_EQ(Poisoned.Num(), 2);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, NestedArrayPathIsReported, UNTEST_TIMEOUTMS(30000))
{
	TSharedPtr<FJsonObject> Root = JsonSanitizeTests_MakeNestedFrames(JsonSanitizeTests_PosInf());

	TArray<FString> Poisoned;
	const bool bChanged = ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned);

	UNTEST_EXPECT_TRUE(bChanged);
	UNTEST_ASSERT_EQ(Poisoned.Num(), 1);
	// Array segments are [N], never .N -- a caller greps this path back out.
	UNTEST_EXPECT_STREQ(*Poisoned[0], TEXT("frames[3].duration_ms"));

	const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
	UNTEST_ASSERT_TRUE(Root->TryGetArrayField(TEXT("frames"), Frames));
	UNTEST_ASSERT_EQ(Frames->Num(), 4);
	UNTEST_EXPECT_TRUE(JsonSanitizeTests_IsNullField((*Frames)[3]->AsObject(), TEXT("duration_ms")));
	UNTEST_EXPECT_FALSE(JsonSanitizeTests_IsNullField((*Frames)[0]->AsObject(), TEXT("duration_ms")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, FiniteValuesUntouched, UNTEST_TIMEOUTMS(30000))
{
	TSharedPtr<FJsonObject> Root = JsonSanitizeTests_MakeNestedFrames(16.9);
	Root->SetStringField(TEXT("name"), TEXT("GameThread"));
	Root->SetBoolField(TEXT("truncated"), false);
	Root->SetNumberField(TEXT("zero"), 0.0);

	TArray<FString> Poisoned;
	const bool bChanged = ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned);

	UNTEST_EXPECT_FALSE(bChanged);
	UNTEST_EXPECT_EQ(Poisoned.Num(), 0);
	UNTEST_EXPECT_FALSE(JsonSanitizeTests_IsNullField(Root, TEXT("zero")));

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	UNTEST_EXPECT_TRUE(FJsonSerializer::Serialize(Root.ToSharedRef(), Writer));
	UNTEST_EXPECT_TRUE(Serialized.Contains(TEXT("GameThread")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, DeepNestingTerminates, UNTEST_TIMEOUTMS(30000))
{
	// Deeper than MaxRecursionDepth: the walk must bail out rather than
	// overflow the stack, and must still have found the shallow poison.
	TSharedPtr<FJsonObject> Root = JsonSanitizeTests_MakeDeepChain(ClaireonJsonSanitize::MaxRecursionDepth * 2);

	TArray<FString> Poisoned;
	const bool bChanged = ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned);

	UNTEST_EXPECT_TRUE(bChanged);
	UNTEST_ASSERT_EQ(Poisoned.Num(), 1);
	UNTEST_EXPECT_TRUE(Poisoned[0].EndsWith(TEXT("value")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, NullObjectIsSafeNoOp, UNTEST_TIMEOUTMS(30000))
{
	TSharedPtr<FJsonObject> Root;
	TArray<FString> Poisoned;

	UNTEST_EXPECT_FALSE(ClaireonJsonSanitize::SanitizeNonFiniteNumbers(Root, Poisoned));
	UNTEST_EXPECT_EQ(Poisoned.Num(), 0);

	co_return;
}

#endif // WITH_UNTESTED
