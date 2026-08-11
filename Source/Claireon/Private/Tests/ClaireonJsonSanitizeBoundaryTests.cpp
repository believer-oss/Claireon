// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBridge.h"
#include "ClaireonOutputGate.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/IClaireonTool.h"

#include <limits>

// ---------------------------------------------------------------------------
// P0-1, result-boundary half.
//
// A real capture cannot be made to reliably contain an open frame, so the
// guard is tested at the two boundaries every tool result converges on:
//
//   1. FClaireonOutputGate::RouteResult -- the MCP HTTP wire, and (downstream
//      of the same call) spill files.
//   2. FClaireonBridge::BuildResultEnvelope -- the in-process Python envelope,
//      which deliberately bypasses the output gate.
//
// Guarding only one leaves the other exposed, which is why both are pinned
// here rather than trusting a single insertion point.
// ---------------------------------------------------------------------------
namespace ClaireonJsonSanitizeBoundaryTestsInternal
{

// File-local discriminator prefix (SanitizeBoundary_) per module convention.

double SanitizeBoundary_PosInf()
{
	return std::numeric_limits<double>::infinity();
}
double SanitizeBoundary_NegInf()
{
	return -std::numeric_limits<double>::infinity();
}
double SanitizeBoundary_NaN()
{
	return std::numeric_limits<double>::quiet_NaN();
}

/**
 * { "avg_ms": +inf, "healthy_ms": 16.6,
 *   "frames": [ {"duration_ms": 16.6}, {"duration_ms": -inf}, {"duration_ms": NaN} ] }
 *
 * Poison at the top level and nested inside an array, because the two take
 * different code paths (objects mutate in place, arrays are rebuilt).
 */
TSharedPtr<FJsonObject> SanitizeBoundary_MakePoisonedData()
{
	TArray<TSharedPtr<FJsonValue>> Frames;

	TSharedPtr<FJsonObject> Healthy = MakeShared<FJsonObject>();
	Healthy->SetNumberField(TEXT("duration_ms"), 16.6);
	Frames.Add(MakeShared<FJsonValueObject>(Healthy));

	TSharedPtr<FJsonObject> NegativeInfinite = MakeShared<FJsonObject>();
	NegativeInfinite->SetNumberField(TEXT("duration_ms"), SanitizeBoundary_NegInf());
	Frames.Add(MakeShared<FJsonValueObject>(NegativeInfinite));

	TSharedPtr<FJsonObject> NotANumber = MakeShared<FJsonObject>();
	NotANumber->SetNumberField(TEXT("duration_ms"), SanitizeBoundary_NaN());
	Frames.Add(MakeShared<FJsonValueObject>(NotANumber));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("avg_ms"), SanitizeBoundary_PosInf());
	Data->SetNumberField(TEXT("healthy_ms"), 16.6);
	Data->SetArrayField(TEXT("frames"), Frames);
	return Data;
}

IClaireonTool::FToolResult SanitizeBoundary_MakePoisonedResult()
{
	IClaireonTool::FToolResult Result;
	Result.Data = SanitizeBoundary_MakePoisonedData();
	Result.Summary = TEXT("frame stats");
	return Result;
}

bool SanitizeBoundary_IsNullField(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
{
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonValue>* Found = Object->Values.Find(FieldName);
	return Found != nullptr && Found->IsValid() && (*Found)->Type == EJson::Null;
}

FString SanitizeBoundary_Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Out;
	if (Object.IsValid())
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Out;
}

/** Round-trips the text back through the parser. Absence of an "inf" substring
 *  is not sufficient evidence on its own -- a field named `infantry_count`
 *  would false-positive -- so legality is proven by reparsing. */
bool SanitizeBoundary_ReparsesCleanly(const FString& Json)
{
	TSharedPtr<FJsonObject> Parsed;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid();
}

/** True when any warning mentions the given field path. */
bool SanitizeBoundary_WarnsAbout(const TArray<FString>& Warnings, const FString& FieldPath)
{
	for (const FString& Warning : Warnings)
	{
		if (Warning.Contains(FieldPath))
		{
			return true;
		}
	}
	return false;
}

bool SanitizeBoundary_EnvelopeWarnsAbout(const TSharedPtr<FJsonObject>& Envelope, const FString& FieldPath)
{
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	if (!Envelope.IsValid() || !Envelope->TryGetArrayField(TEXT("warnings"), Warnings))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
	{
		if (Warning.IsValid() && Warning->AsString().Contains(FieldPath))
		{
			return true;
		}
	}
	return false;
}

} // namespace ClaireonJsonSanitizeBoundaryTestsInternal

using namespace ClaireonJsonSanitizeBoundaryTestsInternal;

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, EnvelopeBoundarySanitizes, UNTEST_TIMEOUTMS(30000))
{
	const IClaireonTool::FToolResult Result = SanitizeBoundary_MakePoisonedResult();

	const TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(Result);
	UNTEST_ASSERT_TRUE(Envelope.IsValid());

	const TSharedPtr<FJsonObject>* Data = nullptr;
	UNTEST_ASSERT_TRUE(Envelope->TryGetObjectField(TEXT("data"), Data));

	UNTEST_EXPECT_TRUE(SanitizeBoundary_IsNullField(*Data, TEXT("avg_ms")));
	UNTEST_EXPECT_FALSE(SanitizeBoundary_IsNullField(*Data, TEXT("healthy_ms")));

	const FString Serialized = SanitizeBoundary_Serialize(Envelope);
	UNTEST_EXPECT_TRUE(SanitizeBoundary_ReparsesCleanly(Serialized));
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("inf")));
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("nan")));

	// Disclosure: a replaced value the caller is never told about is the same
	// class of failure as the bad value itself.
	UNTEST_EXPECT_TRUE(SanitizeBoundary_EnvelopeWarnsAbout(Envelope, TEXT("avg_ms")));
	UNTEST_EXPECT_TRUE(SanitizeBoundary_EnvelopeWarnsAbout(Envelope, TEXT("frames[1].duration_ms")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, RouteResultBoundarySanitizes, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult Result = SanitizeBoundary_MakePoisonedResult();

	Result = FClaireonOutputGate::RouteResult(
		MoveTemp(Result),
		TEXT("trace_get_frame_stats"),
		TEXT("json-sanitize-boundary"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_TRUE(SanitizeBoundary_IsNullField(Result.Data, TEXT("avg_ms")));
	UNTEST_EXPECT_FALSE(SanitizeBoundary_IsNullField(Result.Data, TEXT("healthy_ms")));

	const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("frames"), Frames));
	UNTEST_ASSERT_EQ(Frames->Num(), 3);
	UNTEST_EXPECT_FALSE(SanitizeBoundary_IsNullField((*Frames)[0]->AsObject(), TEXT("duration_ms")));
	UNTEST_EXPECT_TRUE(SanitizeBoundary_IsNullField((*Frames)[1]->AsObject(), TEXT("duration_ms")));
	UNTEST_EXPECT_TRUE(SanitizeBoundary_IsNullField((*Frames)[2]->AsObject(), TEXT("duration_ms")));

	UNTEST_EXPECT_TRUE(SanitizeBoundary_WarnsAbout(Result.Warnings, TEXT("avg_ms")));
	UNTEST_EXPECT_TRUE(SanitizeBoundary_WarnsAbout(Result.Warnings, TEXT("frames[2].duration_ms")));

	const FString Serialized = SanitizeBoundary_Serialize(Result.Data);
	UNTEST_EXPECT_TRUE(SanitizeBoundary_ReparsesCleanly(Serialized));
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("inf")));
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("nan")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, ForceInlineStillSanitizes, UNTEST_TIMEOUTMS(30000))
{
	// force_inline short-circuits the spill pipeline with an early return. The
	// guard must sit ABOVE that return, or every force-inlined result escapes
	// unsanitized -- the exact shape of hole this whole item is about.
	IClaireonTool::FToolResult Result = SanitizeBoundary_MakePoisonedResult();

	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	Arguments->SetBoolField(TEXT("force_inline"), true);

	Result = FClaireonOutputGate::RouteResult(
		MoveTemp(Result),
		TEXT("trace_get_frame_stats"),
		Arguments,
		TEXT("json-sanitize-boundary"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_TRUE(SanitizeBoundary_IsNullField(Result.Data, TEXT("avg_ms")));
	UNTEST_EXPECT_TRUE(SanitizeBoundary_WarnsAbout(Result.Warnings, TEXT("avg_ms")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, JsonSanitize, CleanResultIsUnchanged, UNTEST_TIMEOUTMS(30000))
{
	// The guard runs on every result of every tool, so the clean path must be
	// inert: no nulls introduced, no warnings invented.
	IClaireonTool::FToolResult Result;
	Result.Data = MakeShared<FJsonObject>();
	Result.Data->SetNumberField(TEXT("avg_ms"), 16.6);
	Result.Data->SetStringField(TEXT("thread"), TEXT("GameThread"));
	Result.Summary = TEXT("frame stats");

	Result = FClaireonOutputGate::RouteResult(
		MoveTemp(Result),
		TEXT("trace_get_frame_stats"),
		TEXT("json-sanitize-boundary"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_FALSE(SanitizeBoundary_IsNullField(Result.Data, TEXT("avg_ms")));
	UNTEST_EXPECT_EQ(Result.Warnings.Num(), 0);

	double AvgMs = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("avg_ms"), AvgMs));
	UNTEST_EXPECT_NEAR(AvgMs, 16.6, 0.0001);

	co_return;
}

#endif // WITH_UNTESTED
