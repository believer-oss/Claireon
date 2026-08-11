// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceOpen.h"
#include "Tools/ClaireonTool_TraceClose.h"
#include "Tools/ClaireonTool_TraceGetSessionInfo.h"
#include "Tools/ClaireonTool_TraceGetFrameStats.h"
#include "Tools/ClaireonTool_TraceGetTopScopes.h"
#include "Tools/ClaireonTool_TraceGetScopeDetails.h"
#include "Tools/ClaireonTool_TraceGetThreads.h"
#include "ClaireonTraceSession.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

namespace TraceTestHelpers
{
	/**
	 * Generate a small .utrace file for testing by capturing a brief trace
	 * via console commands. Returns the file path, or empty string on failure.
	 */
	static FString GenerateTestTrace(FAutomationTestBase& Test)
	{
		// Absolute, deliberately. ProjectSavedDir() is relative
		// ("../../../../<worktree>/Saved/"), and Trace.File does not resolve a relative path the
		// way IFileManager does: FTraceAuxiliary runs it through
		// ConvertToAbsolutePathForExternalAppForWrite, which landed this capture in
		// D:/<worktree>/Saved/Profiling instead of D:/git/<worktree>/Saved/Profiling -- outside the
		// repo. Two bugs fell out of that single mismatch. The capture littered a 29 MB
		// .utrace in a stray directory, and because Delete() below resolved the path
		// project-relative it never removed the real file, so every run after the first hit
		// FTraceAuxiliary's "Trace file already exists" refusal and this test could not pass
		// twice. Absolutizing once makes delete, write and FileExists agree.
		const FString OutputPath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Profiling") / TEXT("MCPTestTrace.utrace"));

		// Clean up any previous test trace
		IFileManager::Get().Delete(*OutputPath, false, true);

		// Trace.File is refused while another trace destination is already active (for example
		// when the editor was launched with -trace), which is one of the ways this capture used
		// to silently produce nothing. Drop any existing destination first.
		GEngine->Exec(nullptr, TEXT("Trace.Stop"));

		// Start trace capture via console command
		const FString StartCmd = FString::Printf(TEXT("Trace.File %s -Channels=Cpu,Frame"), *OutputPath);
		GEngine->Exec(nullptr, *StartCmd);

		// Capture across real frame boundaries.
		//
		// This used to be a flat FPlatformProcess::Sleep(0.5f). That works interactively --
		// the engine keeps ticking around the automation test, so the Frame channel records
		// frames -- but under the UntestRunTests commandlet the game thread IS this test, so
		// a sleep blocks the only thread that emits frame boundaries. The capture then
		// contained Cpu events and zero frames, and "default run should return at least one
		// frame" failed with nothing actually wrong in trace_frame_stats.
		//
		// Bracket each slice the way FEngineLoop::Tick does, so the fixture emits genuine
		// Frame-channel events in both contexts rather than depending on who else is ticking.
		for (int32 FrameIndex = 0; FrameIndex < 10; ++FrameIndex)
		{
			TRACE_BEGIN_FRAME(TraceFrameType_Game);
			FPlatformProcess::Sleep(0.05f);
			TRACE_END_FRAME(TraceFrameType_Game);
		}

		// Stop trace capture
		GEngine->Exec(nullptr, TEXT("Trace.Stop"));

		// Small delay to let the trace file finish flushing
		FPlatformProcess::Sleep(0.2f);

		if (!IFileManager::Get().FileExists(*OutputPath))
		{
			// This is an AddError, NOT an AddWarning-and-skip. The whole trace-dependent surface
			// hangs off this file, so "no fixture" means "zero assertions ran" -- reporting that
			// as a pass is how ~150 assertions in this file stayed dead without anyone noticing.
			// A missing capture is a broken precondition of the test itself, so it must be red.
			Test.AddError(FString::Printf(
				TEXT("Trace capture produced no file at %s. Trace-dependent coverage cannot run. ")
				TEXT("Check that the Cpu and Frame trace channels are available and that no other ")
				TEXT("trace destination is active (Trace.Status)."),
				*OutputPath));
			return FString();
		}

		return OutputPath;
	}

	/** Open a trace and return session ID. Returns empty string on failure. */
	static FString OpenTestSession(FAutomationTestBase& Test, const FString& TracePath)
	{
		ClaireonTool_TraceOpen OpenTool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("filePath"), TracePath);

		auto Result = OpenTool.Execute(Args);
		if (Result.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("Failed to open trace: %s"), *Result.GetContentAsString()));
			return FString();
		}

		// The session id lives in the structured Data payload
		// (ClaireonTool_TraceOpen.cpp: Data->SetStringField("session_id", ...)).
		//
		// This used to scrape GetContentAsString() for a line starting with "sessionId: ".
		// GetContentAsString() returns bIsError ? ErrorMessage : Summary (IClaireonTool.h), and
		// trace_open's Summary is only "Opened trace: %d frames, %.0fms" -- that token can never
		// appear there. So this helper returned empty every single run, the caller bailed out,
		// and every trace-dependent assertion below it was unreachable in both branches.
		// Do NOT re-point this at the content channel.
		FString SessionId;
		if (!Result.Data.IsValid()
			|| !Result.Data->TryGetStringField(TEXT("session_id"), SessionId)
			|| SessionId.IsEmpty())
		{
			Test.AddError(TEXT("trace_open reported success but Data.session_id was missing or empty"));
			return FString();
		}

		return SessionId;
	}

	/** Close a session. Does not report failure. */
	static void CloseTestSession(const FString& SessionId)
	{
		ClaireonTool_TraceClose CloseTool;

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), SessionId);
		CloseTool.Execute(Args);
	}

	// -------------------------------------------------------------------------
	// Data-channel accessors.
	//
	// Every tool in the trace family now puts its payload in Data and reduces
	// Summary to a single human line.
	//
	// This used to be split: trace_get_session_info and trace_get_threads passed
	// nullptr for Data and dumped "key: value" prose into Summary instead. That
	// was P0-6d -- a null Data becomes {} in the Python envelope, so the obvious
	// result["data"][...] access was a KeyError and callers had to re-parse a
	// human-formatted string to get at structured values. Both were converted.
	//
	// Substring assertions against a one-line Summary for structured fields are
	// the class of dead assertion this file was full of; use these helpers.
	// -------------------------------------------------------------------------

	static bool TryGetDataString(const IClaireonTool::FToolResult& Result, const FString& Field, FString& Out)
	{
		return Result.Data.IsValid() && Result.Data->TryGetStringField(Field, Out);
	}

	static bool TryGetDataNumber(const IClaireonTool::FToolResult& Result, const FString& Field, double& Out)
	{
		return Result.Data.IsValid() && Result.Data->TryGetNumberField(Field, Out);
	}

	static const TArray<TSharedPtr<FJsonValue>>* TryGetDataArray(
		const IClaireonTool::FToolResult& Result, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(Field, Out))
		{
			return Out;
		}
		return nullptr;
	}

	static bool TryGetElemNumber(const TSharedPtr<FJsonValue>& Elem, const FString& Field, double& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		return Elem.IsValid() && Elem->TryGetObject(Obj) && (*Obj)->TryGetNumberField(Field, Out);
	}

	static bool TryGetElemString(const TSharedPtr<FJsonValue>& Elem, const FString& Field, FString& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		return Elem.IsValid() && Elem->TryGetObject(Obj) && (*Obj)->TryGetStringField(Field, Out);
	}

	// -------------------------------------------------------------------------
	// Finiteness invariant (P0-1).
	//
	// UE's JSON writer prints doubles with %.17g, so a non-finite number reaches
	// the wire as the bare token `inf` / `nan` -- not legal JSON, and fatal to
	// the whole result rather than one field. The engine seeds every open frame
	// with EndTime = +inf, so a capture stopped mid-frame produces exactly that.
	//
	// A healthy capture should never contain one. This is cheap to assert and
	// fails loudly if a real capture ever carries an open frame -- which is the
	// condition the result-boundary guard exists for.
	// -------------------------------------------------------------------------

	static void CollectNonFinitePaths(const TSharedPtr<FJsonValue>& Value, const FString& Path,
		int32 Depth, TArray<FString>& OutPaths);

	static void CollectNonFinitePathsInObject(const TSharedPtr<FJsonObject>& Object, const FString& Path,
		int32 Depth, TArray<FString>& OutPaths)
	{
		if (!Object.IsValid() || Depth > 64)
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FString FieldPath = Path.IsEmpty() ? Pair.Key : Path + TEXT(".") + Pair.Key;
			CollectNonFinitePaths(Pair.Value, FieldPath, Depth + 1, OutPaths);
		}
	}

	static void CollectNonFinitePaths(const TSharedPtr<FJsonValue>& Value, const FString& Path,
		int32 Depth, TArray<FString>& OutPaths)
	{
		if (!Value.IsValid() || Depth > 64)
		{
			return;
		}

		switch (Value->Type)
		{
		case EJson::Number:
			if (!FMath::IsFinite(Value->AsNumber()))
			{
				OutPaths.Add(Path);
			}
			break;

		case EJson::Object:
			CollectNonFinitePathsInObject(Value->AsObject(), Path, Depth + 1, OutPaths);
			break;

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Elements = Value->AsArray();
			for (int32 Index = 0; Index < Elements.Num(); ++Index)
			{
				CollectNonFinitePaths(Elements[Index],
					FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1, OutPaths);
			}
			break;
		}

		default:
			break;
		}
	}

	/** Fails the test naming every offending field path, not just a count --
	 *  "some number was infinite" is not actionable. */
	static void ExpectAllNumbersFinite(FAutomationTestBase& Test, const FString& ToolName,
		const IClaireonTool::FToolResult& Result)
	{
		TArray<FString> NonFinitePaths;
		CollectNonFinitePathsInObject(Result.Data, FString(), 0, NonFinitePaths);

		if (NonFinitePaths.Num() > 0)
		{
			Test.AddError(FString::Printf(
				TEXT("%s returned %d non-finite number(s) on a healthy capture: %s"),
				*ToolName, NonFinitePaths.Num(), *FString::Join(NonFinitePaths, TEXT(", "))));
		}
	}
}

// =============================================================================
// Error Handling -- does not require a trace file (fast)
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_ErrorHandling,
	"Claireon.Trace.ErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTraceToolTest_ErrorHandling::RunTest(const FString& Parameters)
{
	// --- TraceOpen: missing filePath ---
	{
		ClaireonTool_TraceOpen Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("Open with no filePath should error"), Result.bIsError);
		TestTrue(TEXT("Error should mention filePath"),
			Result.GetContentAsString().Contains(TEXT("filePath")));
	}

	// --- TraceOpen: empty filePath ---
	{
		ClaireonTool_TraceOpen Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("filePath"), TEXT(""));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("Open with empty filePath should error"), Result.bIsError);
	}

	// --- TraceOpen: non-existent file ---
	{
		ClaireonTool_TraceOpen Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("filePath"), TEXT("D:/nonexistent_file_12345.utrace"));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("Open with non-existent file should error"), Result.bIsError);
		TestTrue(TEXT("Error should mention file not found"),
			Result.GetContentAsString().Contains(TEXT("not found")));
	}

	// --- TraceOpen: wrong extension (use a file that exists but isn't .utrace) ---
	{
		const FString NonTracePath = FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini");
		if (IFileManager::Get().FileExists(*NonTracePath))
		{
			ClaireonTool_TraceOpen Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("filePath"), NonTracePath);

			auto Result = Tool.Execute(Args);
			TestTrue(TEXT("Open with wrong extension should error"), Result.bIsError);
			TestTrue(TEXT("Error should mention .utrace"),
				Result.GetContentAsString().Contains(TEXT("utrace")));
		}
	}

	// --- TraceClose: missing sessionId ---
	{
		ClaireonTool_TraceClose Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("Close with no sessionId should error"), Result.bIsError);
		TestTrue(TEXT("Error should mention sessionId"),
			Result.GetContentAsString().Contains(TEXT("sessionId")));
	}

	// --- TraceClose: invalid sessionId ---
	{
		ClaireonTool_TraceClose Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), TEXT("nonexistent_session"));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("Close with invalid sessionId should error"), Result.bIsError);
		TestTrue(TEXT("Error should mention session not found"),
			Result.GetContentAsString().Contains(TEXT("not found")));
	}

	// --- GetSessionInfo: invalid session ---
	{
		ClaireonTool_TraceGetSessionInfo Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), TEXT("nonexistent_session"));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("GetSessionInfo with invalid session should error"), Result.bIsError);
	}

	// --- GetFrameStats: missing sessionId ---
	{
		ClaireonTool_TraceGetFrameStats Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetFrameStats with no sessionId should error"), Result.bIsError);
	}

	// --- GetFrameStats: invalid sessionId ---
	{
		ClaireonTool_TraceGetFrameStats Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), TEXT("nonexistent_session"));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("GetFrameStats with invalid session should error"), Result.bIsError);
	}

	// --- GetTopScopes: missing sessionId ---
	{
		ClaireonTool_TraceGetTopScopes Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetTopScopes with no sessionId should error"), Result.bIsError);
	}

	// --- GetScopeDetails: missing sessionId ---
	{
		ClaireonTool_TraceGetScopeDetails Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetScopeDetails with no sessionId should error"), Result.bIsError);
	}

	// --- GetScopeDetails: missing scopeName ---
	{
		ClaireonTool_TraceGetScopeDetails Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), TEXT("some_session"));

		auto Result = Tool.Execute(Args);
		TestTrue(TEXT("GetScopeDetails with no scopeName should error"), Result.bIsError);
		TestTrue(TEXT("Error should mention scopeName"),
			Result.GetContentAsString().Contains(TEXT("scopeName")));
	}

	// --- GetThreads: missing sessionId ---
	{
		ClaireonTool_TraceGetThreads Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());
		TestTrue(TEXT("GetThreads with no sessionId should error"), Result.bIsError);
	}

	// --- Null arguments ---
	{
		ClaireonTool_TraceOpen OpenTool;
		auto Result = OpenTool.Execute(nullptr);
		TestTrue(TEXT("Open with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceClose CloseTool;
		auto Result = CloseTool.Execute(nullptr);
		TestTrue(TEXT("Close with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceGetSessionInfo InfoTool;
		auto Result = InfoTool.Execute(nullptr);
		TestTrue(TEXT("GetSessionInfo with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceGetFrameStats FrameTool;
		auto Result = FrameTool.Execute(nullptr);
		TestTrue(TEXT("GetFrameStats with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceGetTopScopes ScopesTool;
		auto Result = ScopesTool.Execute(nullptr);
		TestTrue(TEXT("GetTopScopes with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceGetScopeDetails DetailsTool;
		auto Result = DetailsTool.Execute(nullptr);
		TestTrue(TEXT("GetScopeDetails with null args should error"), Result.bIsError);
	}
	{
		ClaireonTool_TraceGetThreads ThreadsTool;
		auto Result = ThreadsTool.Execute(nullptr);
		TestTrue(TEXT("GetThreads with null args should error"), Result.bIsError);
	}

	return true;
}

// =============================================================================
// Tool Metadata -- does not require a trace file (fast)
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_ToolMetadata,
	"Claireon.Trace.ToolMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTraceToolTest_ToolMetadata::RunTest(const FString& Parameters)
{
	auto VerifyTool = [this](IClaireonTool& Tool, const FString& ExpectedName)
	{
		const FString Name = Tool.GetName();
		TestEqual(FString::Printf(TEXT("%s GetName"), *ExpectedName), Name, ExpectedName);

		const FString Description = Tool.GetDescription();
		TestFalse(FString::Printf(TEXT("%s GetDescription should not be empty"), *ExpectedName),
			Description.IsEmpty());

		TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
		TestTrue(FString::Printf(TEXT("%s GetInputSchema should return valid object"), *ExpectedName),
			Schema.IsValid());

		if (Schema.IsValid())
		{
			FString Type;
			TestTrue(FString::Printf(TEXT("%s schema should have type=object"), *ExpectedName),
				Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object"));

			TestTrue(FString::Printf(TEXT("%s schema should have properties"), *ExpectedName),
				Schema->HasField(TEXT("properties")));

			TestTrue(FString::Printf(TEXT("%s schema should have required array"), *ExpectedName),
				Schema->HasField(TEXT("required")));
		}
	};

	ClaireonTool_TraceOpen OpenTool;
	VerifyTool(OpenTool, TEXT("trace_open"));

	ClaireonTool_TraceClose CloseTool;
	VerifyTool(CloseTool, TEXT("trace_close"));

	ClaireonTool_TraceGetSessionInfo SessionInfoTool;
	VerifyTool(SessionInfoTool, TEXT("trace_get_session_info"));

	ClaireonTool_TraceGetFrameStats FrameStatsTool;
	VerifyTool(FrameStatsTool, TEXT("trace_get_frame_stats"));

	ClaireonTool_TraceGetTopScopes TopScopesTool;
	VerifyTool(TopScopesTool, TEXT("trace_get_top_scopes"));

	ClaireonTool_TraceGetScopeDetails ScopeDetailsTool;
	VerifyTool(ScopeDetailsTool, TEXT("trace_get_scope_details"));

	ClaireonTool_TraceGetThreads ThreadsTool;
	VerifyTool(ThreadsTool, TEXT("trace_get_threads"));

	// Verify specific schema properties for TraceOpen
	{
		TSharedPtr<FJsonObject> Schema = OpenTool.GetInputSchema();
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), Props))
		{
			TestTrue(TEXT("TraceOpen should have filePath property"),
				(*Props)->HasField(TEXT("filePath")));
		}
	}

	// Verify specific schema for GetFrameStats
	{
		TSharedPtr<FJsonObject> Schema = FrameStatsTool.GetInputSchema();
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), Props))
		{
			TestTrue(TEXT("GetFrameStats should have sessionId"), (*Props)->HasField(TEXT("sessionId")));
			TestTrue(TEXT("GetFrameStats should have frameType"), (*Props)->HasField(TEXT("frameType")));
			TestTrue(TEXT("GetFrameStats should have hitchThresholdMs"), (*Props)->HasField(TEXT("hitchThresholdMs")));
			TestTrue(TEXT("GetFrameStats should have onlyHitches"), (*Props)->HasField(TEXT("onlyHitches")));
			TestTrue(TEXT("GetFrameStats should have maxResults"), (*Props)->HasField(TEXT("maxResults")));
		}
	}

	// Verify specific schema for GetTopScopes
	{
		TSharedPtr<FJsonObject> Schema = TopScopesTool.GetInputSchema();
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), Props))
		{
			TestTrue(TEXT("GetTopScopes should have threadFilter"), (*Props)->HasField(TEXT("threadFilter")));
			TestTrue(TEXT("GetTopScopes should have sortBy"), (*Props)->HasField(TEXT("sortBy")));
			TestTrue(TEXT("GetTopScopes should have includeGpu"), (*Props)->HasField(TEXT("includeGpu")));
		}
	}

	// Verify specific schema for GetScopeDetails
	{
		TSharedPtr<FJsonObject> Schema = ScopeDetailsTool.GetInputSchema();
		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (Schema->TryGetArrayField(TEXT("required"), Required))
		{
			bool bHasSessionId = false;
			bool bHasScopeName = false;
			for (const TSharedPtr<FJsonValue>& Val : *Required)
			{
				FString Str;
				if (Val->TryGetString(Str))
				{
					if (Str == TEXT("sessionId")) bHasSessionId = true;
					if (Str == TEXT("scopeName")) bHasScopeName = true;
				}
			}
			TestTrue(TEXT("GetScopeDetails should require sessionId"), bHasSessionId);
			TestTrue(TEXT("GetScopeDetails should require scopeName"), bHasScopeName);
		}
	}

	AddInfo(TEXT("Tool metadata test passed"));
	return true;
}

// =============================================================================
// WithTraceFile -- All trace-dependent tests in one test to avoid repeated
// trace analysis (each open takes ~22s on the game thread).
//
// Opens the trace ONCE, runs all tool tests, then tests session lifecycle
// (close, double-close) at the end.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_WithTraceFile,
	"Claireon.Trace.WithTraceFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FTraceToolTest_WithTraceFile::RunTest(const FString& Parameters)
{
	using namespace TraceTestHelpers;

	const FString TracePath = GenerateTestTrace(*this);
	if (TracePath.IsEmpty())
	{
		// GenerateTestTrace already AddError'd. Returning false (not true) because a capture
		// failure means zero trace coverage ran; the old `return true` made "skipped" report as
		// the only passing outcome this test had.
		return false;
	}

	AddInfo(FString::Printf(TEXT("Using trace file: %s"), *TracePath));

	// =========================================================================
	// Open session (the only heavyweight open for all single-session tests)
	// =========================================================================
	FString SessionId = TraceTestHelpers::OpenTestSession(*this, TracePath);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	TestTrue(TEXT("Session ID should start with 'trace_'"), SessionId.StartsWith(TEXT("trace_")));
	AddInfo(FString::Printf(TEXT("Opened session: %s"), *SessionId));

	// Verify FindSession works
	{
		FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId);
		TestNotNull(TEXT("FindSession should return valid session"), Session);
		if (Session)
		{
			TestEqual(TEXT("Session ID should match"), Session->SessionId, SessionId);
			TestTrue(TEXT("AnalysisSession should be valid"), Session->AnalysisSession.IsValid());
			TestFalse(TEXT("Session should not be expired"), Session->IsExpired());
		}
	}

	// =========================================================================
	// GetSessionInfo
	//
	// These assertions used to read the content channel, correctly, because
	// trace_get_session_info returned MakeSuccessResult(nullptr, <prose dump>).
	// That was P0-6d: a null Data becomes {} in the Python envelope, so a caller
	// doing result["data"]["thread_count"] got a KeyError and the only way to
	// reach any of these fields was to re-parse a human-formatted string.
	//
	// Data is now the primary channel and these are re-pointed onto it. Do NOT
	// convert them back to Summary substring checks -- the summary is one line.
	// =========================================================================
	{
		ClaireonTool_TraceGetSessionInfo Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), SessionId);

		auto Result = Tool.Execute(Args);
		TestFalse(TEXT("GetSessionInfo should succeed"), Result.bIsError);
		TestNotNull(TEXT("SessionInfo: Data must not be null"), Result.Data.Get());

		FString EchoedId;
		TestTrue(TEXT("SessionInfo: Data echoes session_id"),
			TryGetDataString(Result, TEXT("session_id"), EchoedId) && EchoedId == SessionId);

		FString FilePath;
		TestTrue(TEXT("SessionInfo: Data carries file_path naming the trace"),
			TryGetDataString(Result, TEXT("file_path"), FilePath)
				&& FilePath.Contains(FPaths::GetCleanFilename(TracePath)));

		double DurationSeconds = -1.0;
		TestTrue(TEXT("SessionInfo: Data carries duration_seconds"),
			TryGetDataNumber(Result, TEXT("duration_seconds"), DurationSeconds));

		bool bAnalysisComplete = false;
		TestTrue(TEXT("SessionInfo: Data carries analysis_complete"),
			Result.Data.IsValid()
				&& Result.Data->TryGetBoolField(TEXT("analysis_complete"), bAnalysisComplete));

		double GameFrameCount = -1.0;
		TestTrue(TEXT("SessionInfo: Data carries game_frame_count"),
			TryGetDataNumber(Result, TEXT("game_frame_count"), GameFrameCount));

		double ThreadCount = -1.0;
		TestTrue(TEXT("SessionInfo: Data carries thread_count"),
			TryGetDataNumber(Result, TEXT("thread_count"), ThreadCount));

		// P0-6c: the manifest must be reachable without re-opening the trace.
		const TSharedPtr<FJsonObject>* Manifest = nullptr;
		TestTrue(TEXT("SessionInfo: Data carries capture_manifest"),
			Result.Data.IsValid() && Result.Data->TryGetObjectField(TEXT("capture_manifest"), Manifest));

		AddInfo(TEXT("GetSessionInfo passed"));
	}

	// =========================================================================
	// GetThreads
	//
	// Same P0-6d rewrite as GetSessionInfo above: this tool also returned
	// MakeSuccessResult(nullptr, <prose dump>) and now populates Data.
	// =========================================================================
	{
		ClaireonTool_TraceGetThreads Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), SessionId);

		auto Result = Tool.Execute(Args);
		TestFalse(TEXT("GetThreads should succeed"), Result.bIsError);
		TestNotNull(TEXT("Threads: Data must not be null"), Result.Data.Get());

		const TArray<TSharedPtr<FJsonValue>>* Threads = TryGetDataArray(Result, TEXT("threads"));
		TestNotNull(TEXT("Threads: Data carries a threads array"), Threads);

		double ThreadCount = -1.0;
		TestTrue(TEXT("Threads: Data carries thread_count"),
			TryGetDataNumber(Result, TEXT("thread_count"), ThreadCount));

		if (Threads)
		{
			TestTrue(TEXT("Threads: array should be non-empty"), Threads->Num() > 0);
			// Derived from the emitted array, so the two can never disagree.
			TestEqual(TEXT("Threads: thread_count matches threads.Num()"),
				(int32)ThreadCount, Threads->Num());

			bool bFoundGameThread = false;
			bool bEveryEntryWellFormed = true;
			for (const TSharedPtr<FJsonValue>& Elem : *Threads)
			{
				double Id = -1.0;
				FString Name, GroupName;
				const bool bShapeOk = TryGetElemNumber(Elem, TEXT("id"), Id)
					&& TryGetElemString(Elem, TEXT("name"), Name)
					&& TryGetElemString(Elem, TEXT("group_name"), GroupName);
				bEveryEntryWellFormed = bEveryEntryWellFormed && bShapeOk;
				if (bShapeOk && Name.Contains(TEXT("GameThread")))
				{
					bFoundGameThread = true;
				}
			}
			TestTrue(TEXT("Threads: every entry has id/name/group_name"), bEveryEntryWellFormed);
			TestTrue(TEXT("Threads: a GameThread entry is present"), bFoundGameThread);
		}

		AddInfo(TEXT("GetThreads passed"));
	}

	// =========================================================================
	// GetFrameStats -- various parameter combinations
	//
	// EVERY assertion in this section used to read GetContentAsString(), i.e. the
	// Summary. trace_get_frame_stats reduces its Summary to one line
	// ("Frames %d-%d: avg %.1fms, min %.1fms, max %.1fms",
	// ClaireonTool_TraceGetFrameStats.cpp:231) and puts the real payload in Data
	// (session_id, frames[{frame_index, duration_ms, game_thread_ms}], avg_ms,
	// min_ms, max_ms). None of the tokens that were asserted here -- "--- Summary ---",
	// "frameType:", "p50FrameTimeMs:", "hitchCount:", "frameRange:",
	// "hitchThresholdMs:", "frame[0].isHitch" -- exist on any channel this tool emits.
	// All re-pointed onto Data. Do not convert these back to Summary substring checks.
	// =========================================================================
	bool bHasGameFrames = false;
	{
		// Default parameters
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats defaults should succeed"), Result.bIsError);

			// The one thing the content channel really does carry.
			TestTrue(TEXT("FrameStats: summary reports an average"),
				Result.GetContentAsString().Contains(TEXT("avg ")));

			FString EchoedId;
			TestTrue(TEXT("FrameStats: Data echoes session_id"),
				TryGetDataString(Result, TEXT("session_id"), EchoedId) && EchoedId == SessionId);

			const TArray<TSharedPtr<FJsonValue>>* Frames = TryGetDataArray(Result, TEXT("frames"));
			TestNotNull(TEXT("FrameStats: Data should carry a frames array"), Frames);

			double AvgMs = -1.0, MinMs = -1.0, MaxMs = -1.0;
			TestTrue(TEXT("FrameStats: Data should carry avg_ms"),
				TryGetDataNumber(Result, TEXT("avg_ms"), AvgMs));
			TestTrue(TEXT("FrameStats: Data should carry min_ms"),
				TryGetDataNumber(Result, TEXT("min_ms"), MinMs));
			TestTrue(TEXT("FrameStats: Data should carry max_ms"),
				TryGetDataNumber(Result, TEXT("max_ms"), MaxMs));
			TestTrue(TEXT("FrameStats: min_ms <= avg_ms"), MinMs <= AvgMs + KINDA_SMALL_NUMBER);
			TestTrue(TEXT("FrameStats: avg_ms <= max_ms"), AvgMs <= MaxMs + KINDA_SMALL_NUMBER);

			if (Frames)
			{
				bHasGameFrames = Frames->Num() > 0;
				TestTrue(TEXT("FrameStats: default run should return at least one frame"), bHasGameFrames);
				TestTrue(TEXT("FrameStats: default maxResults caps at 100"), Frames->Num() <= 100);

				// Per-frame shape plus an independent recomputation of the reported aggregates.
				double RecomputedSum = 0.0;
				double RecomputedMax = 0.0;
				double RecomputedMin = TNumericLimits<double>::Max();
				bool bAllFieldsPresent = true;
				bool bIndicesAscending = true;
				double PreviousIndex = -1.0;

				for (const TSharedPtr<FJsonValue>& Elem : *Frames)
				{
					double FrameIndex = 0.0, DurationMs = 0.0, GameThreadMs = 0.0;
					const bool bOk = TryGetElemNumber(Elem, TEXT("frame_index"), FrameIndex)
						&& TryGetElemNumber(Elem, TEXT("duration_ms"), DurationMs)
						&& TryGetElemNumber(Elem, TEXT("game_thread_ms"), GameThreadMs);
					if (!bOk)
					{
						bAllFieldsPresent = false;
						continue;
					}
					if (FrameIndex <= PreviousIndex)
					{
						bIndicesAscending = false;
					}
					PreviousIndex = FrameIndex;
					RecomputedSum += DurationMs;
					RecomputedMax = FMath::Max(RecomputedMax, DurationMs);
					RecomputedMin = FMath::Min(RecomputedMin, DurationMs);
				}

				TestTrue(TEXT("FrameStats: every frame has frame_index/duration_ms/game_thread_ms"),
					bAllFieldsPresent);
				TestTrue(TEXT("FrameStats: frame_index is strictly ascending"), bIndicesAscending);

				if (bHasGameFrames && bAllFieldsPresent)
				{
					const double RecomputedAvg = RecomputedSum / (double)Frames->Num();
					TestTrue(TEXT("FrameStats: avg_ms matches the returned frames"),
						FMath::IsNearlyEqual(AvgMs, RecomputedAvg, 0.01));
					TestTrue(TEXT("FrameStats: max_ms matches the returned frames"),
						FMath::IsNearlyEqual(MaxMs, RecomputedMax, 0.01));
					TestTrue(TEXT("FrameStats: min_ms matches the returned frames"),
						FMath::IsNearlyEqual(MinMs, RecomputedMin, 0.01));
				}
			}
		}

		// onlyHitches mode.
		//
		// The old form branched on Output.Contains("frame[0].isHitch: true") -- a token the
		// one-line Summary cannot contain, so that block never ran -- and then on
		// !Output.Contains("hitchCount: 0"), which is constant-true for the same reason, so its
		// body always ran and always failed against "--- Hitch Categories", a string no channel
		// carries. There is no hitch metadata in Data either (no hitch_count, isHitch,
		// hitchCause, or per-frame scope breakdown), so the only honest check is that the
		// filter itself worked: with onlyHitches, every returned frame must be over threshold.
		{
			const double ThresholdMs = 33.3;

			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetBoolField(TEXT("onlyHitches"), true);
			Args->SetNumberField(TEXT("hitchThresholdMs"), ThresholdMs);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats onlyHitches should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Hitches = TryGetDataArray(Result, TEXT("frames"));
			TestNotNull(TEXT("FrameStats onlyHitches: Data should carry a frames array"), Hitches);

			if (Hitches)
			{
				const int32 HitchCount = Hitches->Num();
				if (HitchCount > 0)
				{
					bool bAllOverThreshold = true;
					for (const TSharedPtr<FJsonValue>& Elem : *Hitches)
					{
						double DurationMs = 0.0;
						if (!TryGetElemNumber(Elem, TEXT("duration_ms"), DurationMs)
							|| DurationMs < ThresholdMs)
						{
							bAllOverThreshold = false;
						}
					}
					TestTrue(TEXT("FrameStats onlyHitches: every returned frame is at or over the threshold"),
						bAllOverThreshold);

					double MaxMs = 0.0;
					TestTrue(TEXT("FrameStats onlyHitches: max_ms is at or over the threshold"),
						TryGetDataNumber(Result, TEXT("max_ms"), MaxMs) && MaxMs >= ThresholdMs);
				}
				else
				{
					// A 0.5s idle-editor capture usually has no hitches. That is a coverage gap,
					// not a product failure, so warn instead of asserting into thin air.
					AddWarning(TEXT("FrameStats onlyHitches: capture contained no frames over 33.3ms, ")
						TEXT("so the hitch-filter path was exercised only for the empty case."));
				}
			}
		}

		// Frame range filtering. Old form asserted "frameRange: 0 - 10" in the Summary; the
		// range is not echoed on any channel, so verify it through the returned indices.
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 10);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats frame range should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Frames = TryGetDataArray(Result, TEXT("frames"));
			TestNotNull(TEXT("FrameStats frame range: Data should carry a frames array"), Frames);
			if (Frames)
			{
				TestTrue(TEXT("FrameStats frame range: maxResults=5 is honored"), Frames->Num() <= 5);

				bool bAllInRange = true;
				for (const TSharedPtr<FJsonValue>& Elem : *Frames)
				{
					double FrameIndex = -1.0;
					if (!TryGetElemNumber(Elem, TEXT("frame_index"), FrameIndex)
						|| FrameIndex < 0.0 || FrameIndex > 10.0)
					{
						bAllInRange = false;
					}
				}
				TestTrue(TEXT("FrameStats frame range: every frame_index is within [0, 10]"), bAllInRange);
			}
		}

		// Custom hitch threshold. Old form asserted "hitchThresholdMs: 100.0" in the Summary,
		// which never appears; assert the filter instead.
		{
			const double ThresholdMs = 100.0;

			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("hitchThresholdMs"), ThresholdMs);
			Args->SetBoolField(TEXT("onlyHitches"), true);
			Args->SetNumberField(TEXT("maxResults"), 3);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats high threshold should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Frames = TryGetDataArray(Result, TEXT("frames"));
			TestNotNull(TEXT("FrameStats high threshold: Data should carry a frames array"), Frames);
			if (Frames)
			{
				TestTrue(TEXT("FrameStats high threshold: maxResults=3 is honored"), Frames->Num() <= 3);

				bool bAllOverThreshold = true;
				for (const TSharedPtr<FJsonValue>& Elem : *Frames)
				{
					double DurationMs = 0.0;
					if (!TryGetElemNumber(Elem, TEXT("duration_ms"), DurationMs)
						|| DurationMs < ThresholdMs)
					{
						bAllOverThreshold = false;
					}
				}
				TestTrue(TEXT("FrameStats high threshold: every returned frame is at or over 100ms"),
					bAllOverThreshold);
			}
		}

		// Render frame type. Old form asserted "frameType: render" in the Summary; frameType is
		// not echoed on any channel, so only the successful traversal of the render frame
		// provider can be asserted here.
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("frameType"), TEXT("render"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats render type should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Frames = TryGetDataArray(Result, TEXT("frames"));
			TestNotNull(TEXT("FrameStats render type: Data should carry a frames array"), Frames);
			if (Frames)
			{
				TestTrue(TEXT("FrameStats render type: maxResults=5 is honored"), Frames->Num() <= 5);
			}
		}

		AddInfo(TEXT("GetFrameStats passed"));
	}

	// =========================================================================
	// GetTopScopes -- aggregation with various parameters
	//
	// trace_get_top_scopes emits Data{session_id, scopes[{name, total_ms, avg_ms,
	// call_count}]} and a one-line Summary ("Top %d scopes: %s (%.1fms avg)",
	// ClaireonTool_TraceGetTopScopes.cpp:280). The former assertions here read the
	// content channel for "timeRange:", "sortBy: ...", "totalScopes:", "showing:",
	// "scope[0].totalInclusiveMs:", "threadFilter: GameThread" and so on -- an older
	// text-dump format that this tool has never produced. All re-pointed onto Data,
	// and the echo-only checks replaced with checks on the effect of each parameter.
	// =========================================================================
	FString TopScopeName;
	{
		// Default parameters
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes defaults should succeed"), Result.bIsError);

			FString EchoedId;
			TestTrue(TEXT("TopScopes: Data echoes session_id"),
				TryGetDataString(Result, TEXT("session_id"), EchoedId) && EchoedId == SessionId);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes: Data should carry a scopes array"), Scopes);

			if (Scopes)
			{
				TestTrue(TEXT("TopScopes: a Cpu-channel trace should aggregate at least one scope"),
					Scopes->Num() > 0);
				TestTrue(TEXT("TopScopes: default maxResults caps at 50"), Scopes->Num() <= 50);

				bool bAllFieldsPresent = true;
				bool bSortedByTotalDescending = true;
				bool bAllCountsPositive = true;
				double PreviousTotalMs = TNumericLimits<double>::Max();

				for (const TSharedPtr<FJsonValue>& Elem : *Scopes)
				{
					FString Name;
					double TotalMs = 0.0, AvgMs = 0.0, CallCount = 0.0;
					const bool bOk = TryGetElemString(Elem, TEXT("name"), Name)
						&& TryGetElemNumber(Elem, TEXT("total_ms"), TotalMs)
						&& TryGetElemNumber(Elem, TEXT("avg_ms"), AvgMs)
						&& TryGetElemNumber(Elem, TEXT("call_count"), CallCount);
					if (!bOk || Name.IsEmpty())
					{
						bAllFieldsPresent = false;
						continue;
					}
					if (TotalMs > PreviousTotalMs + KINDA_SMALL_NUMBER)
					{
						bSortedByTotalDescending = false;
					}
					PreviousTotalMs = TotalMs;
					if (CallCount <= 0.0)
					{
						bAllCountsPositive = false;
					}
				}

				TestTrue(TEXT("TopScopes: every scope has name/total_ms/avg_ms/call_count"),
					bAllFieldsPresent);
				TestTrue(TEXT("TopScopes: default sort is descending by total_ms"),
					bSortedByTotalDescending);
				TestTrue(TEXT("TopScopes: zero-instance scopes are filtered out"), bAllCountsPositive);

				if (Scopes->Num() > 0)
				{
					TryGetElemString((*Scopes)[0], TEXT("name"), TopScopeName);
					TestTrue(TEXT("TopScopes: Summary names the top scope"),
						Result.GetContentAsString().Contains(TopScopeName));
				}
			}
		}

		// GameThread filter. The old form only checked that the filter string was echoed back,
		// which is not a function of filtering; assert the filter actually narrowed the set
		// instead. GameThread must survive a Cpu-channel capture.
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("threadFilter"), TEXT("GameThread"));

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes GameThread filter should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes GameThread filter: Data should carry a scopes array"), Scopes);
			if (Scopes)
			{
				TestTrue(TEXT("TopScopes GameThread filter: should still match scopes"),
					Scopes->Num() > 0);
			}
		}

		// Nonexistent thread filter.
		//
		// The old form asserted bIsError and an error string "No threads matching". The tool does
		// neither: an unmatched CpuThreadFilter simply yields an empty aggregation and
		// MakeSuccessResult (GetTopScopes Execute has no no-match branch at all). Assert the
		// behavior that exists -- success with an empty scopes array. See report: the missing
		// no-match error is a product gap, not a test bug.
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("threadFilter"), TEXT("NonExistentThread12345"));

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes unmatched filter should not error"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes unmatched filter: Data should carry a scopes array"), Scopes);
			if (Scopes)
			{
				TestEqual(TEXT("TopScopes unmatched filter: scopes should be empty"), Scopes->Num(), 0);
			}
		}

		// Sort by count. This is the one alternate sort key the tool actually implements
		// (Execute special-cases "count"; every other value falls through to total_ms), so it
		// gets a real ordering assertion.
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("sortBy"), TEXT("count"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes sortBy count should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes sortBy count: Data should carry a scopes array"), Scopes);
			if (Scopes)
			{
				TestTrue(TEXT("TopScopes sortBy count: maxResults=5 is honored"), Scopes->Num() <= 5);

				bool bSortedByCountDescending = true;
				double PreviousCount = TNumericLimits<double>::Max();
				for (const TSharedPtr<FJsonValue>& Elem : *Scopes)
				{
					double CallCount = 0.0;
					if (!TryGetElemNumber(Elem, TEXT("call_count"), CallCount)
						|| CallCount > PreviousCount)
					{
						bSortedByCountDescending = false;
					}
					PreviousCount = CallCount;
				}
				TestTrue(TEXT("TopScopes sortBy count: result is descending by call_count"),
					bSortedByCountDescending);
			}
		}

		// sortBy totalExclusive / maxInclusive.
		//
		// C4 hardening (product-defects item 4): the tool used to accept these two schema-legal
		// values and then silently sort by totalInclusive instead, since it tracks no exclusive-time
		// or max-inclusive column at all. That is now a clean input-validation error instead of a
		// wrong-order success -- see the guard at the top of
		// ClaireonTool_TraceGetTopScopes::Execute(). Update this block (and re-add an ordering
		// assertion) if the tool ever grows real totalExclusive/maxInclusive aggregation.
		{
			const TCHAR* UnimplementedSortKeys[] = { TEXT("totalExclusive"), TEXT("maxInclusive") };
			for (const TCHAR* SortKey : UnimplementedSortKeys)
			{
				ClaireonTool_TraceGetTopScopes Tool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("sessionId"), SessionId);
				Args->SetStringField(TEXT("sortBy"), SortKey);
				Args->SetNumberField(TEXT("maxResults"), 5);

				auto Result = Tool.Execute(Args);
				TestTrue(FString::Printf(TEXT("TopScopes sortBy %s should now be a clean input-validation error"), SortKey),
					Result.bIsError);
				TestTrue(FString::Printf(TEXT("TopScopes sortBy %s: error should name a supported key"), SortKey),
					Result.GetContentAsString().Contains(TEXT("totalInclusive")));
			}
			AddInfo(TEXT("TopScopes: sortBy totalExclusive/maxInclusive are now rejected (C4 hardening) ")
				TEXT("instead of silently sorting by totalInclusive."));
		}

		// Frame range. Old form asserted nothing but success. A narrower interval cannot
		// aggregate more time than the full trace, so assert that relation.
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 0);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto NarrowResult = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes frame range should succeed"), NarrowResult.bIsError);

			ClaireonTool_TraceGetTopScopes FullTool;
			TSharedPtr<FJsonObject> FullArgs = MakeShared<FJsonObject>();
			FullArgs->SetStringField(TEXT("sessionId"), SessionId);
			auto FullResult = FullTool.Execute(FullArgs);

			const TArray<TSharedPtr<FJsonValue>>* NarrowScopes = TryGetDataArray(NarrowResult, TEXT("scopes"));
			const TArray<TSharedPtr<FJsonValue>>* FullScopes = TryGetDataArray(FullResult, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes frame range: Data should carry a scopes array"), NarrowScopes);

			if (bHasGameFrames && NarrowScopes && FullScopes)
			{
				auto SumTotalMs = [](const TArray<TSharedPtr<FJsonValue>>* Array) -> double
				{
					double Sum = 0.0;
					for (const TSharedPtr<FJsonValue>& Elem : *Array)
					{
						double TotalMs = 0.0;
						if (TryGetElemNumber(Elem, TEXT("total_ms"), TotalMs))
						{
							Sum += TotalMs;
						}
					}
					return Sum;
				};
				TestTrue(TEXT("TopScopes frame range: a single-frame window aggregates no more than the full trace"),
					SumTotalMs(NarrowScopes) <= SumTotalMs(FullScopes) + KINDA_SMALL_NUMBER);
			}
		}

		// Time range
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("startTime"), 0.0);
			Args->SetNumberField(TEXT("endTime"), 10.0);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes time range should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes time range: Data should carry a scopes array"), Scopes);
			if (Scopes)
			{
				TestTrue(TEXT("TopScopes time range: maxResults=5 is honored"), Scopes->Num() <= 5);
			}
		}

		// maxResults clamping. Old form asserted "showing: 3" / absence of "scope[3].name:" in
		// the Summary; neither token exists. The array length is the clamp.
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("maxResults"), 3);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes maxResults=3 should succeed"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Scopes = TryGetDataArray(Result, TEXT("scopes"));
			TestNotNull(TEXT("TopScopes maxResults=3: Data should carry a scopes array"), Scopes);
			if (Scopes)
			{
				TestTrue(TEXT("TopScopes maxResults=3: at most 3 scopes are returned"),
					Scopes->Num() <= 3);
			}
		}

		AddInfo(TEXT("GetTopScopes passed"));
	}

	// =========================================================================
	// GetScopeDetails
	//
	// trace_get_scope_details emits Data{session_id, scope_name, total_ms, avg_ms,
	// call_count, matches[{name, total_ms, avg_ms, max_ms, call_count}], callers[],
	// callees[]} and a one-line Summary (ClaireonTool_TraceGetScopeDetails.cpp:219).
	// The old assertions here read the Summary for "scopeFilter:", "matchingTimers:",
	// "totalOccurrences:" and "occurrence[N].*" -- none of which exist on any channel.
	// Note especially that there is NO per-occurrence data anywhere in the response
	// despite the tool's description; matches[] is aggregated per matching timer. The
	// occurrence[0].startTimeMs / .durationMs / .depth assertions therefore had no
	// possible target and are replaced with assertions on matches[].
	// =========================================================================
	{
		// TopScopeName was resolved above from the top-scopes Data payload (scopes[0].name).
		// The old code re-ran trace_get_top_scopes and line-scraped "scope[0].name:" out of the
		// Summary, which that tool never prints, so FoundScopeName was always empty and the whole
		// positive path fell into the AddInfo skip.
		double FullRangeTotalMs = 0.0;
		double FullRangeCallCount = 0.0;
		bool bHaveFullRangeBaseline = false;

		if (!TopScopeName.IsEmpty())
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TopScopeName);
			Args->SetNumberField(TEXT("maxResults"), 10);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("ScopeDetails for known scope should succeed"), Result.bIsError);

			FString EchoedId, EchoedScope;
			TestTrue(TEXT("ScopeDetails: Data echoes session_id"),
				TryGetDataString(Result, TEXT("session_id"), EchoedId) && EchoedId == SessionId);
			TestTrue(TEXT("ScopeDetails: Data echoes scope_name"),
				TryGetDataString(Result, TEXT("scope_name"), EchoedScope) && EchoedScope == TopScopeName);

			double TotalMs = -1.0, AvgMs = -1.0, CallCount = -1.0;
			TestTrue(TEXT("ScopeDetails: Data should carry total_ms"),
				TryGetDataNumber(Result, TEXT("total_ms"), TotalMs));
			TestTrue(TEXT("ScopeDetails: Data should carry avg_ms"),
				TryGetDataNumber(Result, TEXT("avg_ms"), AvgMs));
			TestTrue(TEXT("ScopeDetails: Data should carry call_count"),
				TryGetDataNumber(Result, TEXT("call_count"), CallCount));

			const TArray<TSharedPtr<FJsonValue>>* Matches = TryGetDataArray(Result, TEXT("matches"));
			TestNotNull(TEXT("ScopeDetails: Data should carry a matches array"), Matches);
			TestNotNull(TEXT("ScopeDetails: Data should carry a callers array"),
				TryGetDataArray(Result, TEXT("callers")));
			TestNotNull(TEXT("ScopeDetails: Data should carry a callees array"),
				TryGetDataArray(Result, TEXT("callees")));

			if (Matches)
			{
				TestTrue(TEXT("ScopeDetails: a name taken from top scopes must match at least one timer"),
					Matches->Num() > 0);
				TestTrue(TEXT("ScopeDetails: maxResults=10 is honored"), Matches->Num() <= 10);

				bool bAllFieldsPresent = true;
				bool bAllNamesContainQuery = true;
				bool bSortedByTotalDescending = true;
				double PreviousTotalMs = TNumericLimits<double>::Max();
				double SummedTotalMs = 0.0;
				double SummedCallCount = 0.0;

				for (const TSharedPtr<FJsonValue>& Elem : *Matches)
				{
					FString Name;
					double MatchTotalMs = 0.0, MatchAvgMs = 0.0, MatchMaxMs = 0.0, MatchCallCount = 0.0;
					const bool bOk = TryGetElemString(Elem, TEXT("name"), Name)
						&& TryGetElemNumber(Elem, TEXT("total_ms"), MatchTotalMs)
						&& TryGetElemNumber(Elem, TEXT("avg_ms"), MatchAvgMs)
						&& TryGetElemNumber(Elem, TEXT("max_ms"), MatchMaxMs)
						&& TryGetElemNumber(Elem, TEXT("call_count"), MatchCallCount);
					if (!bOk)
					{
						bAllFieldsPresent = false;
						continue;
					}
					if (!Name.Contains(TopScopeName, ESearchCase::IgnoreCase))
					{
						bAllNamesContainQuery = false;
					}
					if (MatchTotalMs > PreviousTotalMs + KINDA_SMALL_NUMBER)
					{
						bSortedByTotalDescending = false;
					}
					PreviousTotalMs = MatchTotalMs;
					SummedTotalMs += MatchTotalMs;
					SummedCallCount += MatchCallCount;
				}

				TestTrue(TEXT("ScopeDetails: every match has name/total_ms/avg_ms/max_ms/call_count"),
					bAllFieldsPresent);
				TestTrue(TEXT("ScopeDetails: every match name contains the requested substring"),
					bAllNamesContainQuery);
				TestTrue(TEXT("ScopeDetails: matches are descending by total_ms"),
					bSortedByTotalDescending);

				if (bAllFieldsPresent && Matches->Num() > 0)
				{
					TestTrue(TEXT("ScopeDetails: total_ms equals the sum over matches"),
						FMath::IsNearlyEqual(TotalMs, SummedTotalMs, 0.01));
					TestTrue(TEXT("ScopeDetails: call_count equals the sum over matches"),
						FMath::IsNearlyEqual(CallCount, SummedCallCount, 0.5));
					TestTrue(TEXT("ScopeDetails: avg_ms equals total_ms / call_count"),
						CallCount > 0.0 && FMath::IsNearlyEqual(AvgMs, TotalMs / CallCount, 0.01));

					FullRangeTotalMs = TotalMs;
					FullRangeCallCount = CallCount;
					bHaveFullRangeBaseline = true;
				}
			}
		}
		else
		{
			AddError(TEXT("ScopeDetails: no scope name was resolved from trace_get_top_scopes, so the ")
				TEXT("scope-details positive path could not run."));
		}

		// Nonexistent scope.
		//
		// The old form asserted bIsError plus an error string "No timers matching". The tool has
		// no no-match branch: an empty Matches array still goes through MakeSuccessResult, so
		// this reported a failure that the test then papered over by never running. Assert the
		// real contract -- success with an empty result set. See report: the absent no-match
		// error is a product gap.
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TEXT("ThisScopeDoesNotExist12345"));

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("ScopeDetails nonexistent scope should not error"), Result.bIsError);

			const TArray<TSharedPtr<FJsonValue>>* Matches = TryGetDataArray(Result, TEXT("matches"));
			TestNotNull(TEXT("ScopeDetails nonexistent: Data should carry a matches array"), Matches);
			if (Matches)
			{
				TestEqual(TEXT("ScopeDetails nonexistent: matches should be empty"), Matches->Num(), 0);
			}

			double CallCount = -1.0;
			TestTrue(TEXT("ScopeDetails nonexistent: call_count should be 0"),
				TryGetDataNumber(Result, TEXT("call_count"), CallCount) && CallCount == 0.0);
		}

		// Empty scopeName
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TEXT(""));

			auto Result = Tool.Execute(Args);
			TestTrue(TEXT("ScopeDetails empty scopeName should error"), Result.bIsError);
			TestTrue(TEXT("ScopeDetails empty scopeName: error names the field"),
				Result.GetContentAsString().Contains(TEXT("scopeName")));
		}

		// Frame range.
		//
		// The old form queried the literal name "Tick", commented "may error -- that's ok", left
		// the error case unasserted, and then checked for "scopeFilter:" in the Summary. Three
		// problems: the guard was the thing under test, scopeFilter is a pure echo of the request
		// argument (never a function of frame filtering), and neither token exists on any channel.
		//
		// Replaced with a real narrowing check against the already-confirmed TopScopeName. Note
		// that no timestamps are emitted anywhere in the response, so comparing occurrence times
		// against the requested window (the ideal assertion) is impossible; the monotonic relation
		// between a single-frame window and the full trace is the strongest available substitute.
		if (bHaveFullRangeBaseline && bHasGameFrames)
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TopScopeName);
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 0);
			Args->SetNumberField(TEXT("maxResults"), 10);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("ScopeDetails frame range should succeed"), Result.bIsError);

			double NarrowTotalMs = -1.0, NarrowCallCount = -1.0;
			TestTrue(TEXT("ScopeDetails frame range: Data should carry total_ms"),
				TryGetDataNumber(Result, TEXT("total_ms"), NarrowTotalMs));
			TestTrue(TEXT("ScopeDetails frame range: Data should carry call_count"),
				TryGetDataNumber(Result, TEXT("call_count"), NarrowCallCount));
			TestTrue(TEXT("ScopeDetails frame range: single-frame total_ms <= full-trace total_ms"),
				NarrowTotalMs <= FullRangeTotalMs + KINDA_SMALL_NUMBER);
			TestTrue(TEXT("ScopeDetails frame range: single-frame call_count <= full-trace call_count"),
				NarrowCallCount <= FullRangeCallCount);
		}

		AddInfo(TEXT("GetScopeDetails passed"));
	}

	// =========================================================================
	// Multiple Sessions -- open a 2nd session, verify isolation
	// =========================================================================
	{
		FString SessionId2 = TraceTestHelpers::OpenTestSession(*this, TracePath);
		if (!SessionId2.IsEmpty())
		{
			TestNotEqual(TEXT("Sessions should have different IDs"), SessionId, SessionId2);

			// Both should be queryable
			{
				ClaireonTool_TraceGetSessionInfo Tool;

				TSharedPtr<FJsonObject> Args1 = MakeShared<FJsonObject>();
				Args1->SetStringField(TEXT("sessionId"), SessionId);
				auto Result1 = Tool.Execute(Args1);
				TestFalse(TEXT("Session 1 should be queryable"), Result1.bIsError);

				TSharedPtr<FJsonObject> Args2 = MakeShared<FJsonObject>();
				Args2->SetStringField(TEXT("sessionId"), SessionId2);
				auto Result2 = Tool.Execute(Args2);
				TestFalse(TEXT("Session 2 should be queryable"), Result2.bIsError);
			}

			// Close session 2 -- session 1 should still work
			TraceTestHelpers::CloseTestSession(SessionId2);

			{
				ClaireonTool_TraceGetSessionInfo Tool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("sessionId"), SessionId);

				auto Result = Tool.Execute(Args);
				TestFalse(TEXT("Session 1 should still work after closing session 2"), Result.bIsError);
			}

			// Session 2 should be gone
			{
				FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId2);
				TestNull(TEXT("Session 2 should be null after close"), Session);
			}

			AddInfo(TEXT("MultipleSessions passed"));
		}
		// No else-warning: OpenTestSession now AddError's on every failure path, so a downgrade
		// to a warning here would only mask the error it already reported. This whole branch used
		// to be the outcome (the helper could never succeed), which is why the isolation and
		// lifecycle blocks below never ran.
	}

	// =========================================================================
	// Capture manifest, GPU presence, and truncation disclosure (P0-6b/c).
	//
	// The fixture captures with -Channels=Cpu,Frame, which makes all of this
	// directly assertable: the capture provably has no GPU data, so
	// includeGpu=true must produce identical rows AND say so. Before the fix
	// the two were byte-identical with no disclosure, which reads as "GPU work
	// is free" rather than "this capture contains no GPU data".
	// =========================================================================
	{
		// -- Manifest on trace_open's session, via get_session_info ----------
		{
			ClaireonTool_TraceGetSessionInfo Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("Manifest: get_session_info should succeed"), Result.bIsError);

			const TSharedPtr<FJsonObject>* Manifest = nullptr;
			const bool bHasManifest = Result.Data.IsValid()
				&& Result.Data->TryGetObjectField(TEXT("capture_manifest"), Manifest)
				&& Manifest && Manifest->IsValid();
			TestTrue(TEXT("Manifest: capture_manifest is present"), bHasManifest);

			if (bHasManifest)
			{
				// Channels: the fixture asked for Cpu,Frame and no Gpu.
				const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
				if ((*Manifest)->TryGetArrayField(TEXT("channels"), Channels) && Channels)
				{
					bool bHasCpu = false, bHasFrame = false;
					for (const TSharedPtr<FJsonValue>& Elem : *Channels)
					{
						FString ChannelName;
						bool bEnabled = false;
						const TSharedPtr<FJsonObject>* Obj = nullptr;
						if (Elem.IsValid() && Elem->TryGetObject(Obj) && Obj
							&& (*Obj)->TryGetStringField(TEXT("name"), ChannelName))
						{
							(*Obj)->TryGetBoolField(TEXT("enabled"), bEnabled);
							if (!bEnabled) { continue; }
							if (ChannelName.Contains(TEXT("Cpu"))) { bHasCpu = true; }
							if (ChannelName.Contains(TEXT("Frame"))) { bHasFrame = true; }
						}
					}
					TestTrue(TEXT("Manifest: an enabled Cpu channel is reported"), bHasCpu);
					TestTrue(TEXT("Manifest: an enabled Frame channel is reported"), bHasFrame);

					// Deliberately NOT asserting the absence of an enabled Gpu channel.
					//
					// Measured on this fixture (captured with -Channels=Cpu,Frame): the
					// channel provider reports Gpu as ENABLED while the capture contains
					// zero GPU events. Channel state reflects what the traced process had
					// registered, not what actually landed in the file.
					//
					// That is the whole reason gpu_timelines_present is derived from
					// Timeline::GetEventCount() rather than from channel state or from
					// GetGpuTimelineIndex (which unconditionally returns true). Anything
					// that looks like a GPU-presence signal except the event count is a
					// trap, and this test would have been the third one.
				}

				// Named events is a tri-state, never a bare bool: absence from the
				// command line does not prove the flag was off, because it can be
				// forced on in-process (which pie_trace_start now does).
				FString NamedEvents;
				const bool bHasNamedEvents = (*Manifest)->TryGetStringField(TEXT("stat_named_events"), NamedEvents);
				TestTrue(TEXT("Manifest: stat_named_events is reported"), bHasNamedEvents);
				if (bHasNamedEvents)
				{
					TestTrue(TEXT("Manifest: stat_named_events is one of enabled/disabled/unknown"),
						NamedEvents == TEXT("enabled") || NamedEvents == TEXT("disabled")
							|| NamedEvents == TEXT("unknown"));
				}

				bool bGpuPresent = true;
				TestTrue(TEXT("Manifest: gpu_timelines_present is reported"),
					(*Manifest)->TryGetBoolField(TEXT("gpu_timelines_present"), bGpuPresent));
				TestFalse(TEXT("Manifest: fixture capture has no GPU timelines"), bGpuPresent);
			}
		}

		// -- includeGpu is disclosed, not silently identical ------------------
		{
			ClaireonTool_TraceGetTopScopes ToolWithout;
			TSharedPtr<FJsonObject> ArgsWithout = MakeShared<FJsonObject>();
			ArgsWithout->SetStringField(TEXT("sessionId"), SessionId);
			ArgsWithout->SetBoolField(TEXT("includeGpu"), false);
			auto ResultWithout = ToolWithout.Execute(ArgsWithout);

			ClaireonTool_TraceGetTopScopes ToolWith;
			TSharedPtr<FJsonObject> ArgsWith = MakeShared<FJsonObject>();
			ArgsWith->SetStringField(TEXT("sessionId"), SessionId);
			ArgsWith->SetBoolField(TEXT("includeGpu"), true);
			auto ResultWith = ToolWith.Execute(ArgsWith);

			TestFalse(TEXT("GPU: includeGpu=false should succeed"), ResultWithout.bIsError);
			TestFalse(TEXT("GPU: includeGpu=true should succeed"), ResultWith.bIsError);

			bool bGpuPresent = true;
			TestTrue(TEXT("GPU: top_scopes reports gpu_timelines_present"),
				ResultWith.Data.IsValid()
					&& ResultWith.Data->TryGetBoolField(TEXT("gpu_timelines_present"), bGpuPresent));
			TestFalse(TEXT("GPU: fixture capture has no GPU timelines"), bGpuPresent);

			const TArray<TSharedPtr<FJsonValue>>* ScopesWithout = TryGetDataArray(ResultWithout, TEXT("scopes"));
			const TArray<TSharedPtr<FJsonValue>>* ScopesWith = TryGetDataArray(ResultWith, TEXT("scopes"));
			if (ScopesWithout && ScopesWith)
			{
				// Identical is the CORRECT outcome here -- the point is that it is
				// now accompanied by a warning saying why.
				TestEqual(TEXT("GPU: includeGpu changes nothing on a GPU-less capture"),
					ScopesWith->Num(), ScopesWithout->Num());
			}

			bool bDisclosed = false;
			for (const FString& Warning : ResultWith.Warnings)
			{
				if (Warning.Contains(TEXT("no GPU timeline events")))
				{
					bDisclosed = true;
				}
			}
			TestTrue(TEXT("GPU: includeGpu=true on a GPU-less capture warns rather than staying silent"),
				bDisclosed);
		}

		// -- Truncation is disclosed ------------------------------------------
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("maxResults"), 1);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("Truncation: maxResults=1 should succeed"), Result.bIsError);

			bool bTruncated = false;
			TestTrue(TEXT("Truncation: truncated flag is reported"),
				Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("truncated"), bTruncated));
			TestTrue(TEXT("Truncation: maxResults=1 truncates a multi-scope capture"), bTruncated);

			double TotalBefore = 0.0;
			TestTrue(TEXT("Truncation: total_scopes_before_truncation is reported"),
				TryGetDataNumber(Result, TEXT("total_scopes_before_truncation"), TotalBefore));
			TestTrue(TEXT("Truncation: total exceeds the returned page"), TotalBefore > 1.0);
		}

		AddInfo(TEXT("CaptureManifestAndDisclosure passed"));
	}

	// =========================================================================
	// Finiteness invariant (P0-1) -- every number every trace tool emits must
	// be finite on a healthy capture.
	//
	// Cheap, and it is the tripwire for the defect that took out the whole
	// family: one +inf duration makes the entire result unparseable, and
	// because trace_open hands out the session_id every other trace_* tool
	// needs, the blast radius is every tool here. Runs while the session is
	// still open, so it must stay above the lifecycle block below.
	// =========================================================================
	{
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			auto Result = Tool.Execute(Args);
			if (!Result.bIsError)
			{
				ExpectAllNumbersFinite(*this, TEXT("trace_get_frame_stats"), Result);
			}
		}

		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			auto Result = Tool.Execute(Args);
			if (!Result.bIsError)
			{
				ExpectAllNumbersFinite(*this, TEXT("trace_get_top_scopes"), Result);
			}
		}

		{
			ClaireonTool_TraceGetThreads Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			auto Result = Tool.Execute(Args);
			if (!Result.bIsError)
			{
				ExpectAllNumbersFinite(*this, TEXT("trace_get_threads"), Result);
			}
		}

		{
			ClaireonTool_TraceGetSessionInfo Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			auto Result = Tool.Execute(Args);
			if (!Result.bIsError)
			{
				ExpectAllNumbersFinite(*this, TEXT("trace_get_session_info"), Result);
			}
		}

		AddInfo(TEXT("FinitenessInvariant passed"));
	}

	// =========================================================================
	// Session Lifecycle -- close, double-close, tools on closed session
	// (Must be last since it closes the main session)
	// =========================================================================
	{
		// Close session
		{
			ClaireonTool_TraceClose CloseTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = CloseTool.Execute(Args);
			TestFalse(TEXT("Close should succeed"), Result.bIsError);
			// trace_close's Summary really is "Closed trace session <id>", so this content-channel
			// assertion is legitimate; it also mirrors the id into Data.
			TestTrue(TEXT("Close result should mention session"),
				Result.GetContentAsString().Contains(SessionId));

			FString ClosedId;
			TestTrue(TEXT("Close: Data echoes session_id"),
				TryGetDataString(Result, TEXT("session_id"), ClosedId) && ClosedId == SessionId);
			bool bClosedFlag = false;
			TestTrue(TEXT("Close: Data.closed should be true"),
				Result.Data.IsValid() && Result.Data->TryGetBoolField(TEXT("closed"), bClosedFlag) && bClosedFlag);
		}

		// FindSession should fail after close
		{
			FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId);
			TestNull(TEXT("FindSession should return null after close"), Session);
		}

		// Double-close should error
		{
			ClaireonTool_TraceClose CloseTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = CloseTool.Execute(Args);
			TestTrue(TEXT("Double-close should error"), Result.bIsError);
		}

		// Tools should error on closed session
		{
			ClaireonTool_TraceGetSessionInfo InfoTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = InfoTool.Execute(Args);
			TestTrue(TEXT("GetSessionInfo on closed session should error"), Result.bIsError);
		}

		AddInfo(TEXT("SessionLifecycle passed"));
	}

	// Clean up the generated test trace file
	IFileManager::Get().Delete(*TracePath, false, true);

	return true;
}
