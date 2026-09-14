// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// Test structured error transport through the Python bridge, MCP XML, and REPL, including spill and bounds.
// Untest assertion macros co_return, so helpers must be plain functions rather than lambdas.

#include "Untest.h"
#include "ClaireonAnthropicClient.h"
#include "ClaireonBridge.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSettings.h"
#include "ClaireonXmlFormatter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SquidTasks/Task.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/IClaireonTool.h"

#include <limits>

namespace ClaireonErrorTransportTestsInternal
{
	/** Per-test unique spill root so concurrent cases cannot see each other's files. */
	static FString ClErrTr_MakeUniqueTestRoot(const TCHAR* Case)
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Short);
		return FPaths::ProjectIntermediateDir()
			/ TEXT("ClaireonTests")
			/ TEXT("ErrorTransport")
			/ FString(Case)
			/ ShortGuid;
	}

	/** RAII spill-root override. */
	struct FClErrTrScopedTestRoot
	{
		FString Root;
		explicit FClErrTrScopedTestRoot(const TCHAR* Case)
		{
			Root = ClErrTr_MakeUniqueTestRoot(Case);
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FClErrTrScopedTestRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists*/ false, /*Tree*/ true);
			}
		}
	};

	static int32 ClErrTr_SpillThreshold()
	{
		const UClaireonSettings* S = UClaireonSettings::Get();
		return IsValid(S) ? S->ResultSpillThresholdBytes : 8192;
	}

	static int32 ClErrTr_Utf8Len(const FString& In)
	{
		FTCHARToUTF8 Converter(*In);
		return Converter.Length();
	}

	static FString ClErrTr_Serialize(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		if (!Obj.IsValid())
		{
			return Out;
		}
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		Writer->Close();
		return Out;
	}

	/** Populate every inline mutation scalar to test complete spill survival. */
	static FClaireonBPMutationResult ClErrTr_MakeFullMutationResult()
	{
		FClaireonBPMutationResult M;
		M.MutationState = EClaireonMutationState::AppliedOperationFailed;
		M.bMutationRetained = true;
		M.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Wiring);
		M.LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse);
		M.FailedIslandGuid = TEXT("6E1B4A0C9F5D4E7A8B2C3D4E5F607182");
		M.EngineCompileStatus = EClaireonEngineCompileStatus::Succeeded;
		M.bRollbackAvailable = true;
		M.bRollbackGroupSafe = false;
		M.bUndoRecordAvailable = true;
		M.AssetPath = TEXT("/Game/ClErrTr/BP_TransportProbe.BP_TransportProbe");
		M.SessionId = TEXT("bp_sess_clerrtr_0001");
		return M;
	}

	/** Inline scalar names for field-specific assertions. */
	static bool ClErrTr_HasAllElevenScalars(const TSharedPtr<FJsonObject>& Obj, FString& OutMissing)
	{
		OutMissing.Reset();
		if (!Obj.IsValid())
		{
			OutMissing = TEXT("<null object>");
			return false;
		}
		for (const TCHAR* FieldName : ClaireonBPMutation::GetInlineScalarFieldNames())
		{
			if (!Obj->TryGetField(FieldName).IsValid())
			{
				OutMissing = FieldName;
				return false;
			}
		}
		return true;
	}

	/** A hint that passes IClaireonTool::ValidateHint, so no transport drops it. */
	static TSharedPtr<FJsonObject> ClErrTr_MakeRecoveryHint()
	{
		return IClaireonTool::MakeGuidanceHint(
			TEXT("transaction_end_group"),
			TEXT("The group is retained; transaction_end_group is safe and recommended. "
				 "transaction_rollback_group is unavailable until BA settlement can be proven."));
	}

	/** Locate one stream manifest by name on a post-spill Data object. */
	static TSharedPtr<FJsonObject> ClErrTr_FindStream(
		const TSharedPtr<FJsonObject>& Manifest, const FString& Name)
	{
		if (!Manifest.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("spilled_streams"), Arr) || !Arr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			FString StreamName;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), StreamName) && StreamName == Name)
			{
				return Obj;
			}
		}
		return nullptr;
	}

	/** Read the spilled "data" stream back off disk and parse it. */
	static TSharedPtr<FJsonObject> ClErrTr_ReadSpilledData(const TSharedPtr<FJsonObject>& Manifest)
	{
		const TSharedPtr<FJsonObject> Stream = ClErrTr_FindStream(Manifest, TEXT("data"));
		FString AbsolutePath;
		if (!Stream.IsValid() || !Stream->TryGetStringField(TEXT("absolute_path"), AbsolutePath)
			|| AbsolutePath.IsEmpty())
		{
			return nullptr;
		}
		FString FileText;
		if (!FFileHelper::LoadFileToString(FileText, *AbsolutePath))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(FileText);
		if (!FJsonSerializer::Deserialize(Reader, Parsed))
		{
			return nullptr;
		}
		return Parsed;
	}

	/** Use a nested delta that can only be recovered from the spill file. */
	static TSharedPtr<FJsonObject> ClErrTr_MakeOperationDelta(int32 EntryCount)
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Reserve(EntryCount);
		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("node_guid"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Entry->SetStringField(TEXT("change"), TEXT("node_added"));
			Entry->SetNumberField(TEXT("ordinal"), Index);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Delta = MakeShared<FJsonObject>();
		Delta->SetArrayField(TEXT("entries"), Entries);
		Delta->SetNumberField(TEXT("entry_count"), EntryCount);
		return Delta;
	}

	/** Count non-overlapping occurrences (case-sensitive). */
	static int32 ClErrTr_CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		if (Needle.IsEmpty())
		{
			return 0;
		}
		int32 Count = 0;
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 FoundAt = Haystack.Find(
				Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (FoundAt < 0)
			{
				break;
			}
			++Count;
			SearchFrom = FoundAt + Needle.Len();
		}
		return Count;
	}
}

using namespace ClaireonErrorTransportTestsInternal;

// MCP error fields.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, XmlErrorBranchCarriesDataSummaryWarnings, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed: gateway pin count mismatch");
	R.Summary = TEXT("extraction failed with the mutation retained");
	R.Warnings.Add(TEXT("BlueprintAssist may still be formatting this graph"));
	R.AddHint(ClErrTr_MakeRecoveryHint());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetObjectField(TEXT("operation_delta"), ClErrTr_MakeOperationDelta(4));
	R.Data = Data;

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(R);

	UNTEST_ASSERT_TRUE(Xml.Contains(TEXT("<execute-result status=\"error\">")));

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<summary>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("extraction failed with the mutation retained")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("operation_delta")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<warning>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("BlueprintAssist may still be formatting this graph")));

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<error code=")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<suggestion>")));

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("transaction_end_group")));

	// Scalars travel in data or the spill manifest, without a separate result-state element.
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<result-state>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("mutation_state")));

	co_return;
}

// Render valid inline data on errors; suppress data after spill, as on success.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, XmlErrorRendersDataWhenNotSpilled, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("Failed to load asset");
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), TEXT("/Game/Whatever/DA_Thing.DA_Thing"));
	Data->SetStringField(TEXT("session_id"), TEXT("sess_0007"));
	Data->SetStringField(TEXT("failed_phase"), TEXT("validate"));
	R.Data = Data;

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(R);

	UNTEST_ASSERT_TRUE(Xml.Contains(TEXT("<execute-result status=\"error\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("/Game/Whatever/DA_Thing.DA_Thing")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("sess_0007")));

	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<result-state>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<failed_phase>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<asset_path>")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, XmlErrorRendersNoDataWhenSpilled, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("XmlErrorRendersNoDataWhenSpilled"));

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("apply failed");
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), TEXT("/Game/Whatever/DA_Thing.DA_Thing"));
	Data->SetStringField(TEXT("padding"),
		FString::ChrN(FMath::Max(1, ClErrTr_SpillThreshold() * 2), TEXT('q')));
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("eqs_apply_delta"), TEXT("conv_clerrtr_nodata"),
		EClaireonSpillStreamSet::GenericData);

	bool bSpilled = false;
	UNTEST_ASSERT_TRUE(Routed.Data.IsValid());
	Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);
	UNTEST_ASSERT_TRUE(bSpilled);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	// Recover spilled payloads through spilled-result.
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<path>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<result-state>")));

	const FString ReplText = FClaireonAnthropicClient::BuildREPLResultText(Routed);
	UNTEST_EXPECT_FALSE(ReplText.Contains(TEXT("__mcp_spilled__")));
	UNTEST_EXPECT_FALSE(ReplText.Contains(TEXT("Result state: ")));

	co_return;
}

// MCP spilled errors.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, SpilledErrorKeepsScalarsHintAndWarnings, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("SpilledErrorKeepsScalarsHintAndWarnings"));

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed: gateway pin count mismatch");
	R.Summary = TEXT("extraction failed with the mutation retained");
	R.Warnings.Add(TEXT("undo and history cannot attribute interleaved plugin entries"));
	R.AddHint(ClErrTr_MakeRecoveryHint());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetObjectField(TEXT("operation_delta"), ClErrTr_MakeOperationDelta(400));
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("bp_extract_function"), TEXT("conv_clerrtr_spill"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(Routed.bIsError);
	UNTEST_ASSERT_TRUE(Routed.Data.IsValid());
	bool bSpilled = false;
	Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);
	UNTEST_ASSERT_TRUE(bSpilled);

	// The gate copies bounded top-level scalars onto the manifest.
	FString Missing;
	for (const TCHAR* FieldName : ClaireonBPMutation::GetInlineScalarFieldNames())
	{
		UE_LOG(LogTemp, Log, TEXT("[ErrorTransport] post-spill manifest: %s = %s"),
			FieldName,
			Routed.Data->TryGetField(FieldName).IsValid() ? TEXT("PRESENT") : TEXT("MISSING"));
	}
	const bool bManifestHasAll = ClErrTr_HasAllElevenScalars(Routed.Data, Missing);
	if (!bManifestHasAll)
	{
		UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] spill dropped inline scalar: %s"), *Missing);
	}
	UNTEST_ASSERT_TRUE(bManifestHasAll);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_ASSERT_TRUE(Xml.Contains(TEXT("<execute-result status=\"error\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"data\">")));

	// Recover spilled errors through summary and spilled-result.
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<result-state>")));

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("transaction_end_group")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<warning>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<summary>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("[SPILLED -> ")));

	UNTEST_EXPECT_EQ(ClErrTr_CountOccurrences(Xml, TEXT("__mcp_spilled__")), 0);

	// Recover the nested operation_delta from disk.
	const TSharedPtr<FJsonObject> FromDisk = ClErrTr_ReadSpilledData(Routed.Data);
	UNTEST_ASSERT_TRUE(FromDisk.IsValid());
	const TSharedPtr<FJsonObject>* DeltaOnDisk = nullptr;
	UNTEST_ASSERT_TRUE(FromDisk->TryGetObjectField(TEXT("operation_delta"), DeltaOnDisk));
	const TArray<TSharedPtr<FJsonValue>>* EntriesOnDisk = nullptr;
	UNTEST_ASSERT_TRUE((*DeltaOnDisk)->TryGetArrayField(TEXT("entries"), EntriesOnDisk));
	UNTEST_EXPECT_EQ(EntriesOnDisk->Num(), 400);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, XmlSpilledSuccessCarriesWarnings, UNTEST_TIMEOUTMS(30000))
{
	// Preserve sanitizer warnings on spilled results.
	FClErrTrScopedTestRoot Scope(TEXT("XmlSpilledSuccessCarriesWarnings"));

	IClaireonTool::FToolResult R;
	R.Summary = TEXT("frame stats");
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("padding"),
		FString::ChrN(FMath::Max(1, ClErrTr_SpillThreshold() * 2), TEXT('p')));
	// A non-finite number the gate must replace AND disclose.
	Data->SetNumberField(TEXT("avg_ms"), std::numeric_limits<double>::infinity());
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("trace_get_frame_stats"), TEXT("conv_clerrtr_warn"),
		EClaireonSpillStreamSet::GenericData);

	UNTEST_ASSERT_TRUE(Routed.Warnings.Num() > 0);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);
	UNTEST_ASSERT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<warning>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("avg_ms")));

	co_return;
}

// The Python bridge preserves inline data and sanitizes non-finite values.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, BridgeErrorEnvelopeIsComplete, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed");
	R.Summary = TEXT("extraction failed with the mutation retained");
	R.Warnings.Add(TEXT("operation warning"));
	R.Logs = TEXT("stdout line\n");
	R.UELog = TEXT("[Warning] LogClaireon: something\n");
	R.AddHint(ClErrTr_MakeRecoveryHint());
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetObjectField(TEXT("operation_delta"), ClErrTr_MakeOperationDelta(3));
	R.Data = Data;

	const TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(R);
	UNTEST_ASSERT_TRUE(Envelope.IsValid());

	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("data")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("summary")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("warnings")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("hint")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("logs")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("ue_log")));

	const TSharedPtr<FJsonObject>* EnvData = nullptr;
	UNTEST_ASSERT_TRUE(Envelope->TryGetObjectField(TEXT("data"), EnvData));
	FString Missing;
	const bool bAll = ClErrTr_HasAllElevenScalars(*EnvData, Missing);
	if (!bAll)
	{
		UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] bridge envelope dropped scalar: %s"), *Missing);
	}
	UNTEST_EXPECT_TRUE(bAll);

	UNTEST_EXPECT_FALSE((*EnvData)->HasField(TEXT("__mcp_spilled__")));
	const TSharedPtr<FJsonObject>* DeltaInline = nullptr;
	UNTEST_EXPECT_TRUE((*EnvData)->TryGetObjectField(TEXT("operation_delta"), DeltaInline));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, BridgeErrorEnvelopeSanitizesNonFinite, UNTEST_TIMEOUTMS(30000))
{
	// UE's bare inf/nan tokens are invalid for json.loads; sanitize without losing structured error fields.
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("compile validation failed");
	R.Summary = TEXT("applied_validation_failed");
	R.Warnings.Add(TEXT("pre-existing operation warning"));
	R.AddHint(ClErrTr_MakeRecoveryHint());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetNumberField(TEXT("avg_ms"), std::numeric_limits<double>::infinity());
	Data->SetNumberField(TEXT("worst_ms"), -std::numeric_limits<double>::infinity());
	Data->SetNumberField(TEXT("ratio"), std::numeric_limits<double>::quiet_NaN());
	Data->SetNumberField(TEXT("healthy_ms"), 16.0);
	R.Data = Data;

	const TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(R);
	UNTEST_ASSERT_TRUE(Envelope.IsValid());

	const TSharedPtr<FJsonObject>* EnvData = nullptr;
	UNTEST_ASSERT_TRUE(Envelope->TryGetObjectField(TEXT("data"), EnvData));
	const TSharedPtr<FJsonValue> Avg = (*EnvData)->TryGetField(TEXT("avg_ms"));
	UNTEST_ASSERT_TRUE(Avg.IsValid());
	UNTEST_EXPECT_TRUE(Avg->Type == EJson::Null);
	const TSharedPtr<FJsonValue> Healthy = (*EnvData)->TryGetField(TEXT("healthy_ms"));
	UNTEST_ASSERT_TRUE(Healthy.IsValid());
	UNTEST_EXPECT_TRUE(Healthy->Type == EJson::Number);

	const FString Serialized = ClErrTr_Serialize(Envelope);
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("inf")));
	UNTEST_EXPECT_FALSE(Serialized.Contains(TEXT("nan")));

	const TArray<TSharedPtr<FJsonValue>>* WarningsOut = nullptr;
	UNTEST_ASSERT_TRUE(Envelope->TryGetArrayField(TEXT("warnings"), WarningsOut));
	bool bDisclosed = false;
	bool bKeptOriginal = false;
	for (const TSharedPtr<FJsonValue>& W : *WarningsOut)
	{
		const FString Text = W.IsValid() ? W->AsString() : FString();
		bDisclosed = bDisclosed || Text.Contains(TEXT("avg_ms"));
		bKeptOriginal = bKeptOriginal || Text.Contains(TEXT("pre-existing operation warning"));
	}
	UNTEST_EXPECT_TRUE(bDisclosed);
	UNTEST_EXPECT_TRUE(bKeptOriginal);

	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("data")));
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("hint")));
	FString SummaryOut;
	UNTEST_ASSERT_TRUE(Envelope->TryGetStringField(TEXT("summary"), SummaryOut));
	UNTEST_EXPECT_STREQ(SummaryOut, TEXT("applied_validation_failed"));
	FString Missing;
	const bool bAll = ClErrTr_HasAllElevenScalars(*EnvData, Missing);
	if (!bAll)
	{
		UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] sanitizer dropped scalar: %s"), *Missing);
	}
	UNTEST_EXPECT_TRUE(bAll);

	co_return;
}

// REPL error text.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, REPLErrorTextCarriesSummaryDataAndHint, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed: gateway pin count mismatch");
	R.Summary = TEXT("extraction failed with the mutation retained");
	R.Warnings.Add(TEXT("BA may still be formatting"));
	R.Logs = TEXT("bridge stdout\n");
	R.AddHint(ClErrTr_MakeRecoveryHint());
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	R.Data = Data;

	const FString Text = FClaireonAnthropicClient::BuildREPLResultText(R);

	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("extract_wiring failed")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("extraction failed with the mutation retained")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("mutation_state")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("applied_operation_failed")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Warning: BA may still be formatting")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Logs:")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Hint: ")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("transaction_end_group")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, REPLSpilledErrorCarriesSpillMarkerAndHint, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("REPLSpilledErrorCarriesSpillMarkerAndHint"));

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed");
	R.Summary = TEXT("extraction failed with the mutation retained");
	R.Warnings.Add(TEXT("undo cannot attribute interleaved plugin entries"));
	R.AddHint(ClErrTr_MakeRecoveryHint());
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetObjectField(TEXT("operation_delta"), ClErrTr_MakeOperationDelta(400));
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("bp_extract_function"), TEXT("conv_clerrtr_repl"),
		EClaireonSpillStreamSet::GenericData);
	UNTEST_ASSERT_TRUE(Routed.bIsError);

	const FString Text = FClaireonAnthropicClient::BuildREPLResultText(Routed);

	// REPL recovery uses the summary's spill-path marker; it does not serialize the manifest.
	UNTEST_EXPECT_FALSE(Text.Contains(TEXT("__mcp_spilled__")));
	UNTEST_EXPECT_FALSE(Text.Contains(TEXT("Result state: ")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("[SPILLED -> ")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("extract_wiring failed")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Warning: ")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Hint: ")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("transaction_end_group")));

	co_return;
}

// Summary and warning bounds.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, BoundsTruncateSummaryAndWarningsVisibly, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("BoundsTruncateSummaryAndWarningsVisibly"));

	constexpr int32 SummaryMaxBytes = 2048;
	constexpr int32 WarningsMaxEntries = 32;
	constexpr int32 WarningsMaxTotalBytes = 8192;

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("compile validation failed");
	R.Summary = FString::ChrN(6000, TEXT('S'));
	for (int32 Index = 0; Index < 200; ++Index)
	{
		R.Warnings.Add(FString::Printf(TEXT("warning %03d: %s"), Index, *FString::ChrN(300, TEXT('w'))));
	}
	R.AddHint(ClErrTr_MakeRecoveryHint());
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("bp_extract_function"), TEXT("conv_clerrtr_bounds"),
		EClaireonSpillStreamSet::GenericData);

	// A 54-byte truncation marker leaves 1994 payload bytes within the 2048-byte summary bound.
	const int32 SummaryBytes = ClErrTr_Utf8Len(Routed.Summary);
	UNTEST_EXPECT_EQ(SummaryBytes, SummaryMaxBytes);
	UNTEST_EXPECT_TRUE(Routed.Summary.Contains(
		TEXT("... [TRUNCATED: summary was 6000 bytes, bound is 2048]")));
	UNTEST_EXPECT_TRUE(Routed.Summary.EndsWith(TEXT("bound is 2048]")));

	int32 WarningBytes = 0;
	for (const FString& W : Routed.Warnings)
	{
		WarningBytes += ClErrTr_Utf8Len(W);
	}
	// The byte limit admits 26 warnings of 313 bytes; reserve an additional entry for the marker.
	UNTEST_EXPECT_TRUE(Routed.Warnings.Num() <= WarningsMaxEntries);
	UNTEST_EXPECT_EQ(Routed.Warnings.Num(), 27);
	UNTEST_ASSERT_TRUE(Routed.Warnings.Num() > 0);
	UNTEST_EXPECT_TRUE(Routed.Warnings.Last().Contains(
		TEXT("[TRUNCATED: 174 of 200 warnings omitted; bounds are 32 entries / 8192 bytes]")));

	FString Missing;
	const bool bAll = ClErrTr_HasAllElevenScalars(Routed.Data, Missing);
	if (!bAll)
	{
		UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] bounds dropped scalar: %s"), *Missing);
	}
	UNTEST_EXPECT_TRUE(bAll);
	UNTEST_EXPECT_TRUE(Routed.Hints.Num() > 0);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);
	UNTEST_EXPECT_EQ(ClErrTr_CountOccurrences(Xml, TEXT("<warning>")), Routed.Warnings.Num());
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("[TRUNCATED: summary was")));

	const FString ReplText = FClaireonAnthropicClient::BuildREPLResultText(Routed);
	UNTEST_EXPECT_EQ(ClErrTr_CountOccurrences(ReplText, TEXT("\n\nWarning: ")), Routed.Warnings.Num());

	UNTEST_EXPECT_EQ(WarningBytes - ClErrTr_Utf8Len(Routed.Warnings.Last()), 8138);
	UNTEST_EXPECT_TRUE(WarningBytes <= WarningsMaxTotalBytes + ClErrTr_Utf8Len(Routed.Warnings.Last()));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, BoundsApplyOnEveryRouteResultExit, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("BoundsApplyOnEveryRouteResultExit"));

	// force_inline bypasses spilling but retains metadata bounds.
	{
		IClaireonTool::FToolResult R;
		R.Summary = FString::ChrN(6000, TEXT('S'));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetBoolField(TEXT("force_inline"), true);
		IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
			MoveTemp(R), TEXT("asset_search"), Args, TEXT("conv_clerrtr_fi"),
			EClaireonSpillStreamSet::GenericData);
		UNTEST_EXPECT_TRUE(ClErrTr_Utf8Len(Routed.Summary) <= 2048);
		UNTEST_EXPECT_TRUE(Routed.Summary.Contains(TEXT("[TRUNCATED: summary was")));
	}

	{
		IClaireonTool::FToolResult R;
		R.Summary = FString::ChrN(6000, TEXT('S'));
		IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
			MoveTemp(R), TEXT("asset_search"), TEXT("conv_clerrtr_nospill"),
			EClaireonSpillStreamSet::GenericData);
		UNTEST_EXPECT_TRUE(ClErrTr_Utf8Len(Routed.Summary) <= 2048);
		UNTEST_EXPECT_TRUE(Routed.Summary.Contains(TEXT("[TRUNCATED: summary was")));
	}

	// Tail truncation must preserve the spill-path prefix.
	{
		IClaireonTool::FToolResult R;
		R.Summary = FString::ChrN(6000, TEXT('S'));
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("padding"),
			FString::ChrN(FMath::Max(1, ClErrTr_SpillThreshold() * 2), TEXT('p')));
		R.Data = Data;
		IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
			MoveTemp(R), TEXT("asset_search"), TEXT("conv_clerrtr_spilled"),
			EClaireonSpillStreamSet::GenericData);
		UNTEST_EXPECT_TRUE(ClErrTr_Utf8Len(Routed.Summary) <= 2048);
		UNTEST_EXPECT_TRUE(Routed.Summary.StartsWith(TEXT("[SPILLED -> ")));
		UNTEST_EXPECT_TRUE(Routed.Summary.Contains(TEXT("[TRUNCATED: summary was")));
	}

	co_return;
}

// Top-level strings above the inline-scalar limit are omitted after spill.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, OverLongTopLevelScalarIsDroppedBySpill, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("OverLongTopLevelScalarIsDroppedBySpill"));

	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("extract_wiring failed");
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Data);
	Data->SetStringField(TEXT("exactly_512"), FString::ChrN(kClaireonInlineScalarMaxChars, TEXT('a')));
	Data->SetStringField(TEXT("over_512"), FString::ChrN(kClaireonInlineScalarMaxChars + 1, TEXT('b')));
	Data->SetObjectField(TEXT("operation_delta"), ClErrTr_MakeOperationDelta(400));
	R.Data = Data;

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("bp_extract_function"), TEXT("conv_clerrtr_512"),
		EClaireonSpillStreamSet::GenericData);

	bool bSpilled = false;
	UNTEST_ASSERT_TRUE(Routed.Data.IsValid());
	Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);
	UNTEST_ASSERT_TRUE(bSpilled);

	UNTEST_EXPECT_TRUE(Routed.Data->HasField(TEXT("exactly_512")));
	UNTEST_EXPECT_FALSE(Routed.Data->HasField(TEXT("over_512")));

	// Require every recovery string to fit the inline bound.
	TSharedPtr<FJsonObject> Written = MakeShared<FJsonObject>();
	ClErrTr_MakeFullMutationResult().WriteInlineScalars(*Written);
	for (const TCHAR* FieldName : ClaireonBPMutation::GetInlineScalarFieldNames())
	{
		FString Value;
		if (Written->TryGetStringField(FieldName, Value))
		{
			if (Value.Len() > kClaireonInlineScalarMaxChars)
			{
				UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] inline scalar '%s' is %d chars, over the %d bound"),
					FieldName, Value.Len(), kClaireonInlineScalarMaxChars);
			}
			UNTEST_EXPECT_TRUE(Value.Len() <= kClaireonInlineScalarMaxChars);
		}
	}
	FString Missing;
	const bool bAll = ClErrTr_HasAllElevenScalars(Routed.Data, Missing);
	if (!bAll)
	{
		UE_LOG(LogTemp, Error, TEXT("[ErrorTransport] 512-bound test lost scalar: %s"), *Missing);
	}
	UNTEST_EXPECT_TRUE(bAll);

	// Hints live outside Data and its scalar bound.
	co_return;
}

// Preserve existing result fields and error polarity.

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, PlainErrorEnvelopeUnchanged, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R;
	R.bIsError = true;
	R.ErrorMessage = TEXT("Failed to load Blueprint: /Game/Nope/BP_X.BP_X");
	R.Logs = TEXT("stdout tail\n");
	R.UELog = TEXT("[Warning] LogUObjectGlobals: Failed to find object\n");

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(R);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<execute-result status=\"error\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<error code=")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<suggestion>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<logs>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<ue_log>")));

	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<summary>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<warning>")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<result-state>")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ErrorTransport, RouteResultNeverFlipsErrorPolarity, UNTEST_TIMEOUTMS(30000))
{
	FClErrTrScopedTestRoot Scope(TEXT("RouteResultNeverFlipsErrorPolarity"));

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bError = (Pass == 1);
		IClaireonTool::FToolResult R;
		R.bIsError = bError;
		R.ErrorMessage = bError ? TEXT("boom") : FString();
		R.Summary = TEXT("summary");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("padding"),
			FString::ChrN(FMath::Max(1, ClErrTr_SpillThreshold() * 2), TEXT('z')));
		R.Data = Data;

		IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
			MoveTemp(R), TEXT("asset_search"), TEXT("conv_clerrtr_polarity"),
			EClaireonSpillStreamSet::GenericData);

		UNTEST_EXPECT_TRUE(Routed.bIsError == bError);

		const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);
		UNTEST_EXPECT_TRUE(Xml.Contains(bError
			? TEXT("<execute-result status=\"error\">")
			: TEXT("<execute-result status=\"success\">")));
	}

	co_return;
}

#endif // WITH_UNTESTED
