// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_CloseAll.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_CloseAll::GetOperation() const { return TEXT("close_all"); }

TArray<FString> ClaireonBlueprintGraphTool_CloseAll::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("blueprint"), TEXT("close"), TEXT("all"), TEXT("save"), TEXT("flush"),
            TEXT("session"), TEXT("release"), TEXT("unblock")};
}

FString ClaireonBlueprintGraphTool_CloseAll::GetDescription() const
{
    return TEXT("Compile and save every open Blueprint editing session, then close them all. Use when you are done "
                "editing Blueprints, or to clear the session locks that block editor-wide tools (see "
                "bp_open's blocking_scope). Unlike bp_close, this DOES compile and save. Takes no parameters.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_CloseAll::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_CloseAll::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // Every bp_* tool opens its session under ToolName "bp", so this filter is exactly
    // the set of Blueprint sessions.
    const TArray<FMCPSession> BPSessions = FClaireonSessionManager::Get().ListSessions(TEXT("bp"));

    TArray<TSharedPtr<FJsonValue>> ClosedSessions;
    int32 SavedCount = 0;
    TArray<FString> Warnings;

    for (const FMCPSession& Session : BPSessions)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("session_id"), Session.SessionId);
        Entry->SetStringField(TEXT("asset_path"), Session.AssetPath);

        FBlueprintEditToolData* Data = FindToolData(Session.SessionId);
        if (!Data)
        {
            Entry->SetBoolField(TEXT("saved"), false);
            Entry->SetStringField(TEXT("error"), TEXT("tool data missing for session"));
        }
        else
        {
            FString SavedPathOrError;
            TArray<FString> SessionWarnings;
            const bool bSaved = CompileAndSaveSession(Data, SavedPathOrError, SessionWarnings);
            Entry->SetBoolField(TEXT("saved"), bSaved);
            if (bSaved)
            {
                ++SavedCount;
            }
            else
            {
                Entry->SetStringField(TEXT("error"), SavedPathOrError);
            }
            for (const FString& W : SessionWarnings)
            {
                Warnings.Add(FString::Printf(TEXT("[%s] %s"), *Session.AssetPath, *W));
            }
        }

        // Close regardless of save outcome -- a failed save must not leak the lock,
        // which is the whole reason a caller reaches for close_all.
        FClaireonSessionManager::Get().CloseSession(Session.SessionId);
        ClosedSessions.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("closed_sessions"), ClosedSessions);
    Data->SetNumberField(TEXT("closed_count"), SavedCount);
    Data->SetNumberField(TEXT("total_count"), BPSessions.Num());

    FToolResult Result = MakeSuccessResult(Data, FString::Printf(
        TEXT("Closed %d/%d bp session(s) (compiled+saved)."), SavedCount, BPSessions.Num()));
    Result.Warnings.Append(Warnings);
    return Result;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_CloseAll::GetFullDescription() const
{
    return TEXT(
        "Compiles, saves, and closes every open Blueprint editing session in one call.\n\n"
        "Contrast with bp_close, which releases a single session and does NOT save.\n\n"
        "Per session it scrubs trashed pin links, compiles, and saves the package, then "
        "closes the session. A session whose save fails is still closed -- a failed save "
        "must not leak the lock.\n\n"
        "Response: closed_sessions[] carries one {session_id, asset_path, saved, error?} "
        "entry per session found; closed_count is how many saved successfully and "
        "total_count how many were open.\n\n"
        "Main use is unblocking the editor-wide tools (asset_resave, "
        "blueprint_compile_batch, gameplay_tags_*, level_set_actor_property, "
        "asset_fixup_redirectors), which refuse to run while any bp session holds a lock. "
        "bp_open reports that roster as blocking_scope.\n\n"
        "To drop sessions WITHOUT saving, use session_release(force_all=true) instead.");
}

FString ClaireonBlueprintGraphTool_CloseAll::GetExampleUsage() const
{
    return TEXT("bp_close_all");
}

#undef LOCTEXT_NAMESPACE
