// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonSettings.h"

UClaireonSettings::UClaireonSettings()
{
    DisabledPIENetModes.Add(TEXT("Standalone"));
    DisabledPIENetModes.Add(TEXT("ListenServer"));

    // Default denylist for engine-log capture during tool execution. These
    // categories spam Warnings on every asset load (AnimBP compiler hygiene,
    // retarget setup nags, deprecated linker versions, etc.) without being
    // relevant to the tool result. Overridable in project config / editor UI;
    // user-set value REPLACES the entire set, not augments it.
    ExcludedEngineLogCategories.Add(TEXT("LogBlueprint"));
    ExcludedEngineLogCategories.Add(TEXT("LogAnimation"));
    ExcludedEngineLogCategories.Add(TEXT("LogAnimationCompressionInternal"));
    ExcludedEngineLogCategories.Add(TEXT("LogLinker"));
    ExcludedEngineLogCategories.Add(TEXT("LogStreaming"));
    ExcludedEngineLogCategories.Add(TEXT("LogSlate"));
    ExcludedEngineLogCategories.Add(TEXT("LogChooser"));
}

const UClaireonSettings* UClaireonSettings::Get()
{
    return GetDefault<UClaireonSettings>();
}

FString UClaireonSettings::GetEffectiveSystemPrompt() const
{
    if (!SystemPromptOverride.IsEmpty())
    {
        return SystemPromptOverride;
    }

    // Built-in default prompt — Code Mode: claireon.* bridge via execute
    return TEXT(
        "You are an AI assistant embedded inside the Unreal Editor.\n\n"

        "## Code Mode — IMPORTANT\n"
        "You have two tools: `execute` and `search`.\n\n"
        "**Workflow — always follow this order:**\n"
        "1. Use `search` FIRST to discover available tools for your task.\n"
        "2. Use `execute` to run Python code calling discovered tools via the `claireon.*` namespace.\n"
        "3. Combine multiple tool calls in a single execute block when possible.\n\n"
        "**Rules:**\n"
        "- ALWAYS search before executing. Do not guess tool names or parameters.\n"
        "- Use `claireon.*` bridge functions for all editor operations. These are purpose-built "
        "and more capable than raw `unreal` module calls.\n"
        "- Only fall back to raw `unreal` module for operations not covered by any tool.\n"
        "- If a task seems impossible, search more broadly — there are 90+ tools across "
        "15 categories. The answer is usually a tool you haven't discovered yet.\n\n"
        "**Code style:**\n"
        "- Assign the final value to `result` so it is captured: `result = map_open_async(mapPath=path)`\n"
        "- Do not write comments in code — it is not for human consumption.\n"
        "- Keep code minimal and direct.\n\n"

        "## Behavior\n"
        "- Before calling execute, briefly state what you intend to do (one line).\n"
        "- Be concise. Do not repeat information already established in the conversation.\n"
        "- Do not apologize for limitations — just state what you can do.\n\n"

        "## Cancellation\n"
        "If you see a tool_result with content 'Cancelled by user', acknowledge the "
        "interruption briefly and work with whatever partial results are available. "
        "Do not retry the cancelled operation — wait for new instructions.\n\n"

        "## Context Management\n"
        "When the user's message is a new, unrelated topic (not a follow-up), include "
        "this marker at the END of your response (on its own line):\n"
        "[SUGGEST_FRESH_CONTEXT: <brief reason>]\n"
        "[FRESH_CONTEXT_HANDOFF: The user was discussing <topic>. They now want <new topic>. "
        "Their request: \"<user message>\". Suggested tools: <relevant tool names>.]\n\n"
        "These markers are parsed by the interface and hidden from the user. "
        "Do not mention them or explain them."
    );
}

TArray<FString> UClaireonSettings::GetModelOptions() const
{
    return {
        TEXT("claude-haiku-4-5-20251001"),
        TEXT("claude-sonnet-4-6"),
        TEXT("claude-opus-4-6"),
    };
}

#if WITH_EDITOR
void UClaireonSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    OnSettingsChanged.Broadcast();
}
#endif
