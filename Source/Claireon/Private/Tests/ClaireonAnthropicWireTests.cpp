// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonBridge.h"
#include "ClaireonOutputGate.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "SquidTasks/Task.h"
#include "Tools/IClaireonTool.h"

// ===========================================================================
// Anthropic wire tests (ClaireonAnthropicWireTests)
// ===========================================================================
// Cases 4a-4e per CLAIREON_DISK_RESULTS/test-plan.md section 4.
//
// FClaireonXmlFormatter::FormatExecuteResult is the rendering chokepoint used by
// the server path to build the XML envelope that ships as tool_result content
// text.  The REPL path builds a separate plain-text wire, but the <spilled-
// result> / <stream> / <path> / <size-bytes> / <preview> / <truncated> markers
// called out in the plan are emitted by FormatExecuteResult when the gate has
// flagged __mcp_spilled__ on Data.  These tests assert that contract end-to-end
// by routing a synthetic FToolResult through FClaireonOutputGate::RouteResult
// (exact same side that the server and REPL use) and then through
// FClaireonXmlFormatter::FormatExecuteResult.
// ===========================================================================

namespace ClaireonAnthropicWireTestsHelpers
{
	static FString MakeUniqueTestRoot(const TCHAR* Case)
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Short);
		return FPaths::ProjectIntermediateDir()
			/ TEXT("ClaireonTests")
			/ TEXT("AnthropicWire")
			/ FString(Case)
			/ ShortGuid;
	}

	struct FScopedTestRoot
	{
		FString Root;
		explicit FScopedTestRoot(const TCHAR* Case)
		{
			Root = MakeUniqueTestRoot(Case);
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FScopedTestRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists*/ false, /*Tree*/ true);
			}
		}
	};

	static IClaireonTool::FToolResult MakeGenericDataResult(int32 TargetBytes)
	{
		IClaireonTool::FToolResult R;
		R.Summary = TEXT("generic result");
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("payload"), FString::ChrN(FMath::Max(1, TargetBytes), TEXT('g')));
		R.Data = Data;
		return R;
	}

	static IClaireonTool::FToolResult MakePythonResult(const FString& Stdout, const FString& UELog)
	{
		IClaireonTool::FToolResult R;
		R.Summary = TEXT("python result");
		R.Logs = Stdout;
		R.UELog = UELog;
		return R;
	}
}

// ===========================================================================
// Case 4a: large generic-tool result spills; XML carries path/size/preview
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, AnthropicWire, GenericSpillRendersPathAndPreview, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("GenericSpillRendersPathAndPreview"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	IClaireonTool::FToolResult R = MakeGenericDataResult(Threshold * 2);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("asset_search"), TEXT("conv_4a"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<spilled-result>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"data\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<path>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<size-bytes>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<preview>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<truncated>true</truncated>")));

	// Does NOT contain the full padded payload inline. The <preview> element
	// legitimately echoes the first 1024 bytes of the spilled JSON (including
	// the "payload" key), so assert on the full body, not the key.
	const FString FullPayload = FString::ChrN(Threshold * 2, TEXT('g'));
	UNTEST_EXPECT_FALSE(Xml.Contains(FullPayload));

	co_return;
}

// ===========================================================================
// Case 4b: python_execute with only stdout spilled
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, AnthropicWire, PythonStdoutOnlySpillXml, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("PythonStdoutOnlySpillXml"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	const FString Big = FString::ChrN(Threshold * 2, TEXT('s'));
	const FString SmallUELog = TEXT("small uelog line");

	IClaireonTool::FToolResult R = MakePythonResult(Big, SmallUELog);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("python_execute"), TEXT("conv_4b"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"stdout\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"data\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"uelog\">"))); // uelog stayed inline

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<path>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<size-bytes>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<preview>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<truncated>true</truncated>")));

	// The full spilled stdout text (many 's' chars) must not appear in the XML.
	UNTEST_EXPECT_FALSE(Xml.Contains(Big));

	co_return;
}

// ===========================================================================
// Case 4c: python_execute with both stdout and uelog spilled
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, AnthropicWire, PythonBothStreamsSpillXml, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("PythonBothStreamsSpillXml"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	const FString BigStdout = FString::ChrN(Threshold * 2, TEXT('s'));
	const FString BigUELog  = FString::ChrN(Threshold * 2, TEXT('u'));

	IClaireonTool::FToolResult R = MakePythonResult(BigStdout, BigUELog);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("python_execute"), TEXT("conv_4c"),
		EClaireonSpillStreamSet::PythonStdoutAndUELog);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"stdout\">")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<stream name=\"uelog\">")));
	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<stream name=\"data\">")));

	UNTEST_EXPECT_FALSE(Xml.Contains(BigStdout));
	UNTEST_EXPECT_FALSE(Xml.Contains(BigUELog));

	co_return;
}

// ===========================================================================
// Case 4d: small inline result -- no <spilled-result> envelope appears
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, AnthropicWire, SmallPayloadStaysInline, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("SmallPayloadStaysInline"));

	IClaireonTool::FToolResult R = MakeGenericDataResult(/*TargetBytes=*/64);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("asset_search"), TEXT("conv_4d"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	UNTEST_EXPECT_FALSE(Xml.Contains(TEXT("<spilled-result>")));
	// Inline payload appears as today: <data> with the JSON body.
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<data>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("\"payload\"")));

	co_return;
}

// ===========================================================================
// Case 4e: truncation at 2048 chars still yields a well-formed summary + path
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, AnthropicWire, TruncatedDiagnosticsBodyContainsPath, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonAnthropicWireTestsHelpers;
	FScopedTestRoot Scope(TEXT("TruncatedDiagnosticsBodyContainsPath"));

	const UClaireonSettings* S = UClaireonSettings::Get();
	const int32 Threshold = S ? S->ResultSpillThresholdBytes : 8192;

	IClaireonTool::FToolResult R = MakeGenericDataResult(Threshold * 2);
	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("asset_search"), TEXT("conv_4e"),
		EClaireonSpillStreamSet::GenericData);

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Routed);

	// MCPDiagnosticsEntry::ResponseBody is truncated to 2048 today.  A spilled
	// envelope that renders <summary> and <path> inside the first 2048 bytes
	// proves that the truncation boundary does not cut a load-bearing element
	// off the top of the stream.
	const FString Head = Xml.Left(2048);
	UNTEST_EXPECT_TRUE(Head.Contains(TEXT("<summary>")));
	UNTEST_EXPECT_TRUE(Head.Contains(TEXT("<path>")));

	co_return;
}

// ===========================================================================
// FToolResult::Hint plumbed through the bridge wire envelope
// ===========================================================================
// FClaireonBridge::BuildResultEnvelope is the shared chokepoint exercised by the
// success-path inside MCPCallTool. The error path mirrors the same emit
// contract for the data + hint fields (see ClaireonBridge.cpp).
//
// These cases assert that:
//   1. A null Hint produces an envelope with NO "hint" field (back-compat).
//   2. A populated Hint on a success result emits "hint" verbatim.
//   3. A populated Hint on an error result emits "hint" verbatim (mirrors).
//
// All assertions JSON-roundtrip the envelope so we test the on-wire shape,
// not the in-memory object graph.
// ===========================================================================

namespace ClaireonAnthropicWireHintTestHelpers_Wave001
{
	static FString SerializeEnvelope(const TSharedPtr<FJsonObject>& Envelope)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
		Writer->Close();
		return Out;
	}

	static TSharedPtr<FJsonObject> ParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Parsed);
		return Parsed;
	}
}

UNTEST_UNIT(Claireon, AnthropicWire, HintFieldAbsentWhenNull)
{
	using namespace ClaireonAnthropicWireHintTestHelpers_Wave001;

	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("ok");
	// Result.Hint left default-constructed (null TSharedPtr).

	TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(Result);

	UNTEST_EXPECT_TRUE(Envelope.IsValid());
	UNTEST_EXPECT_FALSE(Envelope->HasField(TEXT("hint")));

	// JSON-roundtrip the envelope so the absence is asserted on the wire,
	// not just the in-memory graph.
	const FString Json = SerializeEnvelope(Envelope);
	TSharedPtr<FJsonObject> Parsed = ParseJson(Json);
	UNTEST_EXPECT_TRUE(Parsed.IsValid());
	UNTEST_EXPECT_FALSE(Parsed->HasField(TEXT("hint")));

	co_return;
}

UNTEST_UNIT(Claireon, AnthropicWire, HintFieldPresentOnSuccessWhenPopulated)
{
	using namespace ClaireonAnthropicWireHintTestHelpers_Wave001;

	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("ok");
	TSharedPtr<FJsonObject> HintObj = MakeShared<FJsonObject>();
	HintObj->SetStringField(TEXT("tool"), TEXT("tool_search"));
	HintObj->SetStringField(TEXT("reason"), TEXT("unknown tool 'bp_open'"));
	Result.Hint = HintObj;

	TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(Result);

	UNTEST_EXPECT_TRUE(Envelope.IsValid());
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("hint")));

	const FString Json = SerializeEnvelope(Envelope);
	TSharedPtr<FJsonObject> Parsed = ParseJson(Json);
	UNTEST_EXPECT_TRUE(Parsed.IsValid());
	UNTEST_EXPECT_TRUE(Parsed->HasField(TEXT("hint")));

	const TSharedPtr<FJsonObject>* HintOut = nullptr;
	const bool bGotHint = Parsed->TryGetObjectField(TEXT("hint"), HintOut);
	UNTEST_EXPECT_TRUE(bGotHint);
	if (bGotHint && HintOut && HintOut->IsValid())
	{
		FString ToolField;
		UNTEST_EXPECT_TRUE((*HintOut)->TryGetStringField(TEXT("tool"), ToolField));
		UNTEST_EXPECT_TRUE(ToolField == TEXT("tool_search"));

		FString ReasonField;
		UNTEST_EXPECT_TRUE((*HintOut)->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(TEXT("unknown tool 'bp_open'")));
	}

	co_return;
}

UNTEST_UNIT(Claireon, AnthropicWire, HintFieldPresentOnErrorWhenPopulated)
{
	using namespace ClaireonAnthropicWireHintTestHelpers_Wave001;

	// Error path: bIsError=true and a populated Hint -- BuildResultEnvelope
	// emits the same shape regardless of error state (the error-arm in
	// MCPCallTool replays the same field set).
	IClaireonTool::FToolResult Result;
	Result.bIsError = true;
	Result.ErrorMessage = TEXT("boom");
	Result.Summary = TEXT("error");
	TSharedPtr<FJsonObject> HintObj = MakeShared<FJsonObject>();
	HintObj->SetStringField(TEXT("tool"), TEXT("tool_search"));
	HintObj->SetStringField(TEXT("reason"), TEXT("signature mismatch on bp_compile"));
	Result.Hint = HintObj;

	TSharedPtr<FJsonObject> Envelope = FClaireonBridge::BuildResultEnvelope(Result);

	UNTEST_EXPECT_TRUE(Envelope.IsValid());
	UNTEST_EXPECT_TRUE(Envelope->HasField(TEXT("hint")));

	const FString Json = SerializeEnvelope(Envelope);
	TSharedPtr<FJsonObject> Parsed = ParseJson(Json);
	UNTEST_EXPECT_TRUE(Parsed.IsValid());
	UNTEST_EXPECT_TRUE(Parsed->HasField(TEXT("hint")));

	const TSharedPtr<FJsonObject>* HintOut = nullptr;
	const bool bGotHint = Parsed->TryGetObjectField(TEXT("hint"), HintOut);
	UNTEST_EXPECT_TRUE(bGotHint);
	if (bGotHint && HintOut && HintOut->IsValid())
	{
		FString ReasonField;
		UNTEST_EXPECT_TRUE((*HintOut)->TryGetStringField(TEXT("reason"), ReasonField));
		UNTEST_EXPECT_TRUE(ReasonField.Contains(TEXT("signature mismatch")));
	}

	co_return;
}

// ===========================================================================
// FToolResult::Hint rendered on the MCP XML transport
// ===========================================================================
// FormatExecuteResult is the chokepoint for the MCP HTTP tool_result content;
// without an explicit <hint> emit, Result.Hint is invisible to MCP clients
// (BuildResultEnvelope only serves the Python-side claireon.* envelope).
// ===========================================================================

UNTEST_UNIT(Claireon, AnthropicWire, HintRenderedInExecuteResultXml)
{
	IClaireonTool::FToolResult Result;
	Result.Summary = TEXT("ok");
	Result.Logs = TEXT("False");
	TSharedPtr<FJsonObject> HintObj = MakeShared<FJsonObject>();
	HintObj->SetStringField(TEXT("tool"), TEXT("python_execute"));
	HintObj->SetStringField(TEXT("reason"), TEXT("script uses get_editor_property; try claireon.uobject_inspect"));
	Result.Hint = HintObj;

	const FString Xml = FClaireonXmlFormatter::FormatExecuteResult(Result);

	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("<hint>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("</hint>")));
	UNTEST_EXPECT_TRUE(Xml.Contains(TEXT("uobject_inspect")));
	// The hint sits INSIDE the result envelope (before the final closing tag).
	UNTEST_EXPECT_TRUE(Xml.Find(TEXT("<hint>")) < Xml.Find(TEXT("</execute-result>"),
		ESearchCase::CaseSensitive, ESearchDir::FromEnd));

	co_return;
}

UNTEST_UNIT(Claireon, AnthropicWire, HintRenderedInErrorXmlAndAbsentWhenNull)
{
	// Error path carries the hint too.
	IClaireonTool::FToolResult ErrResult;
	ErrResult.bIsError = true;
	ErrResult.ErrorMessage = TEXT("NameError: name 'bp_opn' is not defined");
	TSharedPtr<FJsonObject> HintObj = MakeShared<FJsonObject>();
	HintObj->SetStringField(TEXT("tool"), TEXT("tool_search"));
	HintObj->SetStringField(TEXT("reason"), TEXT("unknown tool 'bp_opn'"));
	ErrResult.Hint = HintObj;

	const FString ErrXml = FClaireonXmlFormatter::FormatExecuteResult(ErrResult);
	UNTEST_EXPECT_TRUE(ErrXml.Contains(TEXT("status=\"error\"")));
	UNTEST_EXPECT_TRUE(ErrXml.Contains(TEXT("<hint>")));
	UNTEST_EXPECT_TRUE(ErrXml.Contains(TEXT("unknown tool 'bp_opn'")));

	// Null hint produces NO <hint> element (back-compat byte shape).
	IClaireonTool::FToolResult Plain;
	Plain.Summary = TEXT("ok");
	const FString PlainXml = FClaireonXmlFormatter::FormatExecuteResult(Plain);
	UNTEST_EXPECT_FALSE(PlainXml.Contains(TEXT("<hint>")));

	co_return;
}

#endif // WITH_UNTESTED
