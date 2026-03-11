// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonREPLLogger.h"
#include "ClaireonSettings.h"
#include "ClaireonLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"

FClaireonREPLLogger::FClaireonREPLLogger()
{
}

FClaireonREPLLogger::~FClaireonREPLLogger()
{
    if (bInitialized)
    {
        Shutdown();
    }
}

void FClaireonREPLLogger::Initialize()
{
    if (bInitialized)
    {
        return;
    }

    const UClaireonSettings* Settings = UClaireonSettings::Get();
    if (!Settings || !Settings->bEnableLogging)
    {
        return;
    }

    // Build log file path: Saved/Logs/MCPRepl/repl_YYYY-MM-DD_HHMMSS.jsonl
    FString LogDir = FPaths::Combine(FPaths::ProjectDir(), Settings->LogDirectory);
    FPaths::NormalizeDirectoryName(LogDir);
    IFileManager::Get().MakeDirectory(*LogDir, /*Tree=*/true);

    FDateTime Now = FDateTime::Now();
    FString FileName = FString::Printf(TEXT("repl_%04d-%02d-%02d_%02d%02d%02d.jsonl"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(),
        Now.GetHour(), Now.GetMinute(), Now.GetSecond());
    LogFilePath = FPaths::Combine(LogDir, FileName);

    bInitialized = true;
    UE_LOG(LogClaireon, Log, TEXT("[REPLLogger] Logging to: %s"), *LogFilePath);

    // Start flush ticker on the core ticker (works in non-UObject classes)
    float Interval = Settings->LogFlushIntervalSeconds;
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FClaireonREPLLogger::OnFlushTick),
        Interval);
}

void FClaireonREPLLogger::Shutdown()
{
    if (!bInitialized)
    {
        return;
    }

    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }

    // Synchronous final flush on shutdown
    TArray<FString> LinesToWrite;
    {
        FScopeLock Lock(&PendingLock);
        LinesToWrite = MoveTemp(PendingLines);
    }

    if (LinesToWrite.Num() > 0 && !LogFilePath.IsEmpty())
    {
        FString Combined;
        for (const FString& Line : LinesToWrite)
        {
            Combined += Line + TEXT("\n");
        }
        FFileHelper::SaveStringToFile(Combined, *LogFilePath,
            FFileHelper::EEncodingOptions::AutoDetect,
            &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
    }

    bInitialized = false;
}

void FClaireonREPLLogger::LogSessionStart(const FString& ModelId,
    const FString& SystemPrompt, int32 ToolCount)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("session_start"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("model"), ModelId);
    Fields.Add(TEXT("tool_count"), FString::FromInt(ToolCount));
    // Truncate system prompt to avoid huge lines
    Fields.Add(TEXT("system_prompt_len"), FString::FromInt(SystemPrompt.Len()));
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogUserMessage(const FString& Content,
    const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("user_message"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("content"), Content.Left(4096));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogAssistantMessage(const FString& ContentJson,
    const FString& StopReason, int32 InputTokens, int32 OutputTokens,
    double DurationMs, const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("assistant_message"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("stop_reason"), StopReason);
    Fields.Add(TEXT("input_tokens"), FString::FromInt(InputTokens));
    Fields.Add(TEXT("output_tokens"), FString::FromInt(OutputTokens));
    Fields.Add(TEXT("duration_ms"), FString::Printf(TEXT("%.1f"), DurationMs));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    // Content stored separately to avoid JSON-in-JSON escaping issues
    Fields.Add(TEXT("content_len"), FString::FromInt(ContentJson.Len()));
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogToolResult(const FString& ToolName,
    const FString& ToolUseId, const FString& Content,
    double DurationMs, bool bIsError, const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("tool_result"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("tool_name"), ToolName);
    Fields.Add(TEXT("tool_use_id"), ToolUseId);
    Fields.Add(TEXT("content"), Content.Left(2048));
    Fields.Add(TEXT("duration_ms"), FString::Printf(TEXT("%.1f"), DurationMs));
    Fields.Add(TEXT("is_error"), bIsError ? TEXT("true") : TEXT("false"));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogCancelled(const FString& Phase,
    const TArray<FString>& ToolsCompleted, const TArray<FString>& ToolsSkipped,
    const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("cancelled"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("phase"), Phase);
    Fields.Add(TEXT("tools_completed"), FString::Join(ToolsCompleted, TEXT(",")));
    Fields.Add(TEXT("tools_skipped"), FString::Join(ToolsSkipped, TEXT(",")));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogEmergencyStop(const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("emergency_stop"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("source"), TEXT("ctrl_period"));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogFreshContext(const FString& OldConversationId,
    const FString& NewConversationId, const FString& Trigger,
    const FString& HandoffSummary)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("fresh_context"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("old_conversation_id"), OldConversationId);
    Fields.Add(TEXT("new_conversation_id"), NewConversationId);
    Fields.Add(TEXT("trigger"), Trigger);
    Fields.Add(TEXT("handoff_summary"), HandoffSummary.Left(1024));
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogSettingsChanged(const TArray<FString>& ChangedFields)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("settings_changed"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("changed_fields"), FString::Join(ChangedFields, TEXT(",")));
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogError(const FString& ErrorType,
    const FString& Message, const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("error"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("error_type"), ErrorType);
    Fields.Add(TEXT("message"), Message.Left(1024));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogError(const FString& ErrorType, const FString& Message,
    int32 HttpStatusCode, const FString& ResponseBody,
    const FString& FailureReason, int32 RetryAttempt,
    double RequestDurationMs, const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("error"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("error_type"), ErrorType);
    Fields.Add(TEXT("message"), Message.Left(1024));
    Fields.Add(TEXT("http_status"), FString::FromInt(HttpStatusCode));
    Fields.Add(TEXT("response_body"), ResponseBody.Left(2048));
    Fields.Add(TEXT("failure_reason"), FailureReason);
    Fields.Add(TEXT("retry_attempt"), FString::FromInt(RetryAttempt));
    Fields.Add(TEXT("duration_ms"), FString::Printf(TEXT("%.1f"), RequestDurationMs));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::LogRetryAttempt(const FString& ErrorCategory,
    int32 HttpStatusCode, int32 AttemptNumber, float DelaySeconds,
    const FString& ConversationId)
{
    TMap<FString, FString> Fields;
    Fields.Add(TEXT("type"), TEXT("retry_attempt"));
    Fields.Add(TEXT("timestamp"), NowISO8601());
    Fields.Add(TEXT("error_category"), ErrorCategory);
    Fields.Add(TEXT("http_status"), FString::FromInt(HttpStatusCode));
    Fields.Add(TEXT("attempt"), FString::FromInt(AttemptNumber));
    Fields.Add(TEXT("delay_seconds"), FString::Printf(TEXT("%.1f"), DelaySeconds));
    Fields.Add(TEXT("conversation_id"), ConversationId);
    EnqueueLine(MakeJsonLine(Fields));
}

void FClaireonREPLLogger::ForceFlush()
{
    OnFlushTick(0.0f);
}

void FClaireonREPLLogger::EnqueueLine(FString&& JsonLine)
{
    if (!bInitialized || LogFilePath.IsEmpty())
    {
        return;
    }
    FScopeLock Lock(&PendingLock);
    PendingLines.Add(MoveTemp(JsonLine));
}

bool FClaireonREPLLogger::OnFlushTick(float DeltaTime)
{
    TArray<FString> LinesToWrite;
    {
        // Swap out pending lines under lock — game thread blocked for microseconds only
        FScopeLock Lock(&PendingLock);
        if (PendingLines.Num() == 0)
        {
            return true; // keep ticking
        }
        LinesToWrite = MoveTemp(PendingLines);
    }

    // Capture path for the lambda
    FString FilePath = LogFilePath;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [LinesToWrite = MoveTemp(LinesToWrite), FilePath]()
        {
            FString Combined;
            Combined.Reserve(LinesToWrite.Num() * 256);
            for (const FString& Line : LinesToWrite)
            {
                Combined += Line + TEXT("\n");
            }
            FFileHelper::SaveStringToFile(Combined, *FilePath,
                FFileHelper::EEncodingOptions::AutoDetect,
                &IFileManager::Get(), EFileWrite::FILEWRITE_Append);
        });

    return true; // keep ticking
}

FString FClaireonREPLLogger::MakeJsonLine(const TMap<FString, FString>& Fields)
{
    // Simple key-value JSON serializer (no nesting needed for log lines)
    FString Json = TEXT("{");
    bool bFirst = true;
    for (const auto& Pair : Fields)
    {
        if (!bFirst) Json += TEXT(",");
        bFirst = false;
        // Escape the value: replace backslashes and quotes
        FString Val = Pair.Value;
        Val.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Val.ReplaceInline(TEXT("\""), TEXT("\\\""));
        Val.ReplaceInline(TEXT("\n"), TEXT("\\n"));
        Val.ReplaceInline(TEXT("\r"), TEXT(""));
        Json += FString::Printf(TEXT("\"%s\":\"%s\""), *Pair.Key, *Val);
    }
    Json += TEXT("}");
    return Json;
}

FString FClaireonREPLLogger::NowISO8601()
{
    FDateTime Now = FDateTime::UtcNow();
    return FString::Printf(TEXT("%04d-%02d-%02dT%02d:%02d:%02dZ"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(),
        Now.GetHour(), Now.GetMinute(), Now.GetSecond());
}
