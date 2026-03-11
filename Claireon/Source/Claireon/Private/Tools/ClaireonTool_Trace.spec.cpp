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
		const FString OutputPath = FPaths::ProjectSavedDir() / TEXT("Profiling") / TEXT("MCPTestTrace.utrace");

		// Clean up any previous test trace
		IFileManager::Get().Delete(*OutputPath, false, true);

		// Start trace capture via console command
		const FString StartCmd = FString::Printf(TEXT("Trace.File %s -Channels=Cpu,Frame"), *OutputPath);
		GEngine->Exec(nullptr, *StartCmd);

		// Let it capture briefly
		FPlatformProcess::Sleep(0.5f);

		// Stop trace capture
		GEngine->Exec(nullptr, TEXT("Trace.Stop"));

		// Small delay to let the trace file finish flushing
		FPlatformProcess::Sleep(0.2f);

		if (!IFileManager::Get().FileExists(*OutputPath))
		{
			Test.AddWarning(TEXT("Trace file was not created. Skipping trace-dependent tests."));
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

		FString ResultText = Result.GetContentAsString();
		FString SessionId;
		TArray<FString> Lines;
		ResultText.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("sessionId: ")))
			{
				SessionId = Line.Mid(11).TrimStartAndEnd();
				break;
			}
		}

		if (SessionId.IsEmpty())
		{
			Test.AddError(TEXT("Failed to extract sessionId from open result"));
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
}

// =============================================================================
// Error Handling — does not require a trace file (fast)
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_ErrorHandling,
	"Claireon.Trace.ErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
// Tool Metadata — does not require a trace file (fast)
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_ToolMetadata,
	"Claireon.Trace.ToolMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
	VerifyTool(OpenTool, TEXT("editor.trace.open"));

	ClaireonTool_TraceClose CloseTool;
	VerifyTool(CloseTool, TEXT("editor.trace.close"));

	ClaireonTool_TraceGetSessionInfo SessionInfoTool;
	VerifyTool(SessionInfoTool, TEXT("editor.trace.getSessionInfo"));

	ClaireonTool_TraceGetFrameStats FrameStatsTool;
	VerifyTool(FrameStatsTool, TEXT("editor.trace.getFrameStats"));

	ClaireonTool_TraceGetTopScopes TopScopesTool;
	VerifyTool(TopScopesTool, TEXT("editor.trace.getTopScopes"));

	ClaireonTool_TraceGetScopeDetails ScopeDetailsTool;
	VerifyTool(ScopeDetailsTool, TEXT("editor.trace.getScopeDetails"));

	ClaireonTool_TraceGetThreads ThreadsTool;
	VerifyTool(ThreadsTool, TEXT("editor.trace.getThreads"));

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
// WithTraceFile — All trace-dependent tests in one test to avoid repeated
// trace analysis (each open takes ~22s on the game thread).
//
// Opens the trace ONCE, runs all tool tests, then tests session lifecycle
// (close, double-close) at the end.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTraceToolTest_WithTraceFile,
	"Claireon.Trace.WithTraceFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTraceToolTest_WithTraceFile::RunTest(const FString& Parameters)
{
	const FString TracePath = TraceTestHelpers::GenerateTestTrace(*this);
	if (TracePath.IsEmpty())
	{
		return true; // Warning already added by GenerateTestTrace
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
	// =========================================================================
	{
		ClaireonTool_TraceGetSessionInfo Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), SessionId);

		auto Result = Tool.Execute(Args);
		TestFalse(TEXT("GetSessionInfo should succeed"), Result.bIsError);

		FString Output = Result.GetContentAsString();
		TestTrue(TEXT("SessionInfo: should contain sessionId"), Output.Contains(TEXT("sessionId:")));
		TestTrue(TEXT("SessionInfo: should contain filePath"), Output.Contains(TEXT("filePath:")));
		TestTrue(TEXT("SessionInfo: should contain durationSeconds"), Output.Contains(TEXT("durationSeconds:")));
		TestTrue(TEXT("SessionInfo: should contain analysisComplete"), Output.Contains(TEXT("analysisComplete:")));
		TestTrue(TEXT("SessionInfo: should contain gameFrameCount"), Output.Contains(TEXT("gameFrameCount:")));
		TestTrue(TEXT("SessionInfo: should contain threadCount"), Output.Contains(TEXT("threadCount:")));
		TestTrue(TEXT("SessionInfo: should contain our session ID"), Output.Contains(SessionId));
		TestTrue(TEXT("SessionInfo: should contain trace file name"),
			Output.Contains(FPaths::GetCleanFilename(TracePath)));

		AddInfo(TEXT("GetSessionInfo passed"));
	}

	// =========================================================================
	// GetThreads
	// =========================================================================
	{
		ClaireonTool_TraceGetThreads Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("sessionId"), SessionId);

		auto Result = Tool.Execute(Args);
		TestFalse(TEXT("GetThreads should succeed"), Result.bIsError);

		FString Output = Result.GetContentAsString();
		TestTrue(TEXT("Threads: should contain threadCount"), Output.Contains(TEXT("threadCount:")));
		TestTrue(TEXT("Threads: should have thread[0].id"), Output.Contains(TEXT("thread[0].id:")));
		TestTrue(TEXT("Threads: should have thread[0].name"), Output.Contains(TEXT("thread[0].name:")));
		TestTrue(TEXT("Threads: should mention GameThread"), Output.Contains(TEXT("GameThread")));

		AddInfo(TEXT("GetThreads passed"));
	}

	// =========================================================================
	// GetFrameStats — various parameter combinations
	// =========================================================================
	{
		// Default parameters
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats defaults should succeed"), Result.bIsError);

			FString Output = Result.GetContentAsString();
			TestTrue(TEXT("FrameStats: summary header"), Output.Contains(TEXT("--- Summary ---")));
			TestTrue(TEXT("FrameStats: frameType"), Output.Contains(TEXT("frameType: game")));
			TestTrue(TEXT("FrameStats: totalFrames"), Output.Contains(TEXT("totalFrames:")));
			TestTrue(TEXT("FrameStats: avgFrameTimeMs"), Output.Contains(TEXT("avgFrameTimeMs:")));
			TestTrue(TEXT("FrameStats: p50"), Output.Contains(TEXT("p50FrameTimeMs:")));
			TestTrue(TEXT("FrameStats: p95"), Output.Contains(TEXT("p95FrameTimeMs:")));
			TestTrue(TEXT("FrameStats: p99"), Output.Contains(TEXT("p99FrameTimeMs:")));
			TestTrue(TEXT("FrameStats: maxFrameTimeMs"), Output.Contains(TEXT("maxFrameTimeMs:")));
			TestTrue(TEXT("FrameStats: hitchCount"), Output.Contains(TEXT("hitchCount:")));
			TestTrue(TEXT("FrameStats: Frames section"), Output.Contains(TEXT("--- Frames")));
		}

		// onlyHitches mode
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetBoolField(TEXT("onlyHitches"), true);
			Args->SetNumberField(TEXT("hitchThresholdMs"), 33.3);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats onlyHitches should succeed"), Result.bIsError);

			FString Output = Result.GetContentAsString();
			TestTrue(TEXT("FrameStats: hitchThresholdMs"), Output.Contains(TEXT("hitchThresholdMs: 33.3")));

			if (Output.Contains(TEXT("frame[0].isHitch: true")))
			{
				TestTrue(TEXT("FrameStats: hitch should have hitchCause"),
					Output.Contains(TEXT("frame[0].hitchCause:")));
				TestTrue(TEXT("FrameStats: hitch should have gameThread scopes"),
					Output.Contains(TEXT("frame[0].gameThread[0]:")));
			}

			if (!Output.Contains(TEXT("hitchCount: 0")))
			{
				TestTrue(TEXT("FrameStats: should have Hitch Categories"),
					Output.Contains(TEXT("--- Hitch Categories")));
			}
		}

		// Frame range filtering
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 10);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats frame range should succeed"), Result.bIsError);
			TestTrue(TEXT("FrameStats: frameRange"),
				Result.GetContentAsString().Contains(TEXT("frameRange: 0 - 10")));
		}

		// Custom hitch threshold
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("hitchThresholdMs"), 100.0);
			Args->SetBoolField(TEXT("onlyHitches"), true);
			Args->SetNumberField(TEXT("maxResults"), 3);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats high threshold should succeed"), Result.bIsError);
			TestTrue(TEXT("FrameStats: custom threshold"),
				Result.GetContentAsString().Contains(TEXT("hitchThresholdMs: 100.0")));
		}

		// Render frame type
		{
			ClaireonTool_TraceGetFrameStats Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("frameType"), TEXT("render"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("FrameStats render type should succeed"), Result.bIsError);
			TestTrue(TEXT("FrameStats: render frameType"),
				Result.GetContentAsString().Contains(TEXT("frameType: render")));
		}

		AddInfo(TEXT("GetFrameStats passed"));
	}

	// =========================================================================
	// GetTopScopes — aggregation with various parameters
	// =========================================================================
	{
		// Default parameters
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes defaults should succeed"), Result.bIsError);

			FString Output = Result.GetContentAsString();
			TestTrue(TEXT("TopScopes: timeRange"), Output.Contains(TEXT("timeRange:")));
			TestTrue(TEXT("TopScopes: sortBy"), Output.Contains(TEXT("sortBy: totalInclusive")));
			TestTrue(TEXT("TopScopes: totalScopes"), Output.Contains(TEXT("totalScopes:")));
			TestTrue(TEXT("TopScopes: showing"), Output.Contains(TEXT("showing:")));
			TestTrue(TEXT("TopScopes: scope[0].name"), Output.Contains(TEXT("scope[0].name:")));
			TestTrue(TEXT("TopScopes: totalInclusiveMs"), Output.Contains(TEXT("scope[0].totalInclusiveMs:")));
			TestTrue(TEXT("TopScopes: totalExclusiveMs"), Output.Contains(TEXT("scope[0].totalExclusiveMs:")));
			TestTrue(TEXT("TopScopes: count"), Output.Contains(TEXT("scope[0].count:")));
		}

		// GameThread filter
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("threadFilter"), TEXT("GameThread"));

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes GameThread filter should succeed"), Result.bIsError);

			FString Output = Result.GetContentAsString();
			TestTrue(TEXT("TopScopes: threadFilter"), Output.Contains(TEXT("threadFilter: GameThread")));
			TestTrue(TEXT("TopScopes: matched"), Output.Contains(TEXT("matched")));
		}

		// Invalid thread filter
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("threadFilter"), TEXT("NonExistentThread12345"));

			auto Result = Tool.Execute(Args);
			TestTrue(TEXT("TopScopes invalid filter should error"), Result.bIsError);
			TestTrue(TEXT("TopScopes: no matching threads"),
				Result.GetContentAsString().Contains(TEXT("No threads matching")));
		}

		// Sort by totalExclusive
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("sortBy"), TEXT("totalExclusive"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes sortBy totalExclusive should succeed"), Result.bIsError);
			TestTrue(TEXT("TopScopes: totalExclusive sort"),
				Result.GetContentAsString().Contains(TEXT("sortBy: totalExclusive")));
		}

		// Sort by maxInclusive
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("sortBy"), TEXT("maxInclusive"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes sortBy maxInclusive should succeed"), Result.bIsError);
			TestTrue(TEXT("TopScopes: maxInclusive sort"),
				Result.GetContentAsString().Contains(TEXT("sortBy: maxInclusive")));
		}

		// Sort by count
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("sortBy"), TEXT("count"));
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes sortBy count should succeed"), Result.bIsError);
			TestTrue(TEXT("TopScopes: count sort"),
				Result.GetContentAsString().Contains(TEXT("sortBy: count")));
		}

		// Frame range
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 100);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes frame range should succeed"), Result.bIsError);
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
		}

		// maxResults clamping
		{
			ClaireonTool_TraceGetTopScopes Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetNumberField(TEXT("maxResults"), 3);

			auto Result = Tool.Execute(Args);
			TestFalse(TEXT("TopScopes maxResults=3 should succeed"), Result.bIsError);

			FString Output = Result.GetContentAsString();
			TestTrue(TEXT("TopScopes: showing 3"), Output.Contains(TEXT("showing: 3")));
			TestFalse(TEXT("TopScopes: no scope[3]"), Output.Contains(TEXT("scope[3].name:")));
		}

		AddInfo(TEXT("GetTopScopes passed"));
	}

	// =========================================================================
	// GetScopeDetails — per-occurrence timing
	// =========================================================================
	{
		// Search for a scope — try broad names likely present in any trace
		{
			// First get top scopes to find a real scope name we can query
			FString FoundScopeName;
			{
				ClaireonTool_TraceGetTopScopes TopTool;
				TSharedPtr<FJsonObject> TopArgs = MakeShared<FJsonObject>();
				TopArgs->SetStringField(TEXT("sessionId"), SessionId);
				TopArgs->SetNumberField(TEXT("maxResults"), 1);

				auto TopResult = TopTool.Execute(TopArgs);
				if (!TopResult.bIsError)
				{
					// Extract scope[0].name from output
					FString TopOutput = TopResult.GetContentAsString();
					TArray<FString> TopLines;
					TopOutput.ParseIntoArrayLines(TopLines);
					for (const FString& Line : TopLines)
					{
						if (Line.Contains(TEXT("scope[0].name:")))
						{
							FoundScopeName = Line.Mid(Line.Find(TEXT(":")) + 2).TrimStartAndEnd();
							break;
						}
					}
				}
			}

			if (!FoundScopeName.IsEmpty())
			{
				ClaireonTool_TraceGetScopeDetails Tool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("sessionId"), SessionId);
				Args->SetStringField(TEXT("scopeName"), FoundScopeName);
				Args->SetNumberField(TEXT("maxResults"), 10);

				auto Result = Tool.Execute(Args);
				TestFalse(TEXT("ScopeDetails for known scope should succeed"), Result.bIsError);

				FString Output = Result.GetContentAsString();
				TestTrue(TEXT("ScopeDetails: scopeFilter"), Output.Contains(TEXT("scopeFilter:")));
				TestTrue(TEXT("ScopeDetails: matchingTimers"), Output.Contains(TEXT("matchingTimers:")));
				TestTrue(TEXT("ScopeDetails: totalOccurrences"), Output.Contains(TEXT("totalOccurrences:")));

				if (!Output.Contains(TEXT("totalOccurrences: 0")))
				{
					TestTrue(TEXT("ScopeDetails: occurrence timer"),
						Output.Contains(TEXT("occurrence[0].timer:")));
					TestTrue(TEXT("ScopeDetails: startTimeMs"),
						Output.Contains(TEXT("occurrence[0].startTimeMs:")));
					TestTrue(TEXT("ScopeDetails: durationMs"),
						Output.Contains(TEXT("occurrence[0].durationMs:")));
					TestTrue(TEXT("ScopeDetails: depth"),
						Output.Contains(TEXT("occurrence[0].depth:")));
				}
			}
			else
			{
				AddInfo(TEXT("No scopes found in small trace — skipping scope details positive test"));
			}
		}

		// Non-existent scope
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TEXT("ThisScopeDoesNotExist12345"));

			auto Result = Tool.Execute(Args);
			TestTrue(TEXT("ScopeDetails non-existent should error"), Result.bIsError);
			TestTrue(TEXT("ScopeDetails: no matching timers"),
				Result.GetContentAsString().Contains(TEXT("No timers matching")));
		}

		// Empty scopeName
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TEXT(""));

			auto Result = Tool.Execute(Args);
			TestTrue(TEXT("ScopeDetails empty scopeName should error"), Result.bIsError);
		}

		// With frame range (use a broad search that should match something)
		{
			ClaireonTool_TraceGetScopeDetails Tool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("sessionId"), SessionId);
			Args->SetStringField(TEXT("scopeName"), TEXT("Tick"));
			Args->SetNumberField(TEXT("startFrame"), 0);
			Args->SetNumberField(TEXT("endFrame"), 50);
			Args->SetNumberField(TEXT("maxResults"), 5);

			auto Result = Tool.Execute(Args);
			// May error if "Tick" doesn't exist in a small trace — that's ok
			if (!Result.bIsError)
			{
				TestTrue(TEXT("ScopeDetails frame range: should have scopeFilter"),
					Result.GetContentAsString().Contains(TEXT("scopeFilter:")));
			}
		}

		AddInfo(TEXT("GetScopeDetails passed"));
	}

	// =========================================================================
	// Multiple Sessions — open a 2nd session, verify isolation
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

			// Close session 2 — session 1 should still work
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
		else
		{
			AddWarning(TEXT("Could not open second session for multi-session test"));
		}
	}

	// =========================================================================
	// Session Lifecycle — close, double-close, tools on closed session
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
			TestTrue(TEXT("Close result should mention session"),
				Result.GetContentAsString().Contains(SessionId));
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
