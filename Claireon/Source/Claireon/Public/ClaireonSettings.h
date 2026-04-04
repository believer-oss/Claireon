// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ClaireonSettings.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnClaireonSettingsChanged);

/**
 * Live-editable settings for the MCP REPL.
 * All values are read fresh on each API call — change in Editor Preferences,
 * take effect immediately with no recompile or editor restart.
 *
 * Found at: Editor Preferences > Plugins > MCP REPL
 */
UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="MCP REPL"))
class UClaireonSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UClaireonSettings();

    /** Get the singleton. Safe to call from game thread. */
    static const UClaireonSettings* Get();

    // --- Server ---

    /** TCP port for the MCP HTTP server. Requires server restart to take effect. */
    UPROPERTY(Config, EditAnywhere, Category="Server",
        meta=(DisplayName="Server Port", ClampMin=1024, ClampMax=65535))
    uint32 ServerPort = 8017;

    // --- Connection ---

    /** Anthropic API key. Get one at https://console.anthropic.com/settings/keys */
    UPROPERTY(Config, EditAnywhere, Category="Connection",
        meta=(DisplayName="Anthropic API Key", PasswordField=true))
    FString AnthropicApiKey;

    /** API endpoint. Override for proxies or local testing. */
    UPROPERTY(Config, EditAnywhere, Category="Connection",
        meta=(DisplayName="API Endpoint URL"))
    FString ApiEndpointUrl = TEXT("https://api.anthropic.com/v1/messages");

    /** anthropic-version header value. */
    UPROPERTY(Config, EditAnywhere, Category="Connection",
        meta=(DisplayName="Anthropic-Version Header"))
    FString AnthropicVersion = TEXT("2023-06-01");

    // --- Model ---

    /**
     * Model ID to use. Default is Haiku for low cost.
     * Common options: claude-haiku-4-5, claude-sonnet-4-6, claude-opus-4-6
     */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Model ID", GetOptions="GetModelOptions"))
    FString ModelId = TEXT("claude-haiku-4-5-20251001");

    /** Maximum tokens per response. */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Max Tokens", ClampMin=256, ClampMax=16384))
    int32 MaxTokens = 4096;

    /**
     * Maximum tool-use loop iterations per user message.
     * Safety valve to prevent runaway tool chains.
     */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Tool-Use Depth Limit", ClampMin=1, ClampMax=20))
    int32 ToolUseDepthLimit = 10;

    // --- Prompt ---

    /**
     * System prompt sent with every API request.
     * Edit here to change Claude's behavior without recompiling.
     * Leave empty to use the built-in default prompt.
     */
    UPROPERTY(Config, EditAnywhere, Category="Prompt",
        meta=(DisplayName="System Prompt Override", MultiLine=true))
    FString SystemPromptOverride;

    // --- Display ---

    /** Render assistant messages with rich formatting (bold, code blocks, tables).
     *  When unchecked, messages display as plain text. */
    UPROPERTY(Config, EditAnywhere, Category="Display",
        meta=(DisplayName="Enable Rich Text Formatting"))
    bool bEnableRichText = true;

    // --- Logging ---

    /** Directory for JSONL conversation log files. Relative to project root. */
    UPROPERTY(Config, EditAnywhere, Category="Logging",
        meta=(DisplayName="Log Directory", RelativeToGameDir))
    FString LogDirectory = TEXT("Saved/Logs/MCPRepl");

    /** How often (seconds) to flush the log buffer to disk. */
    UPROPERTY(Config, EditAnywhere, Category="Logging",
        meta=(DisplayName="Log Flush Interval (seconds)", ClampMin=1, ClampMax=60))
    float LogFlushIntervalSeconds = 5.0f;

    /** Whether to write conversation logs at all. */
    UPROPERTY(Config, EditAnywhere, Category="Logging",
        meta=(DisplayName="Enable Conversation Logging"))
    bool bEnableLogging = true;

    // --- Emergency Stop ---

    /**
     * How long (seconds) the server remains in user-stop mode after Ctrl+.
     * Resets on each incoming tools/call, auto-clears when requests stop.
     */
    UPROPERTY(Config, EditAnywhere, Category="Emergency Stop",
        meta=(DisplayName="User Stop Cooldown (seconds)", ClampMin=1, ClampMax=30))
    float UserStopCooldownSeconds = 5.0f;

    // --- Reliability ---

    /** Maximum number of automatic retry attempts for transient API failures (429, 5xx, timeouts). Set to 0 to disable retries. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Max Retry Attempts", ClampMin=0, ClampMax=10))
    int32 MaxRetryAttempts = 10;

    /** Base delay in seconds for exponential backoff between retries. Actual delay: min(base * 2^attempt + jitter, max). */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Initial Retry Delay (seconds)", ClampMin=0.5, ClampMax=10.0))
    float InitialRetryDelaySeconds = 1.0f;

    /** Maximum delay in seconds between retries. Caps exponential growth and Retry-After header values. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Max Retry Delay (seconds)", ClampMin=5.0, ClampMax=120.0))
    float MaxRetryDelaySeconds = 30.0f;

    /** Minimum interval in seconds between outgoing API calls. Prevents burst traffic during tool-use loops. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Min Request Interval (seconds)", ClampMin=0.0, ClampMax=5.0))
    float MinRequestIntervalSeconds = 0.2f;

    /** HTTP request timeout in seconds. If the API does not respond within this time, the request is cancelled and retried. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Request Timeout (seconds)", ClampMin=10.0, ClampMax=300.0))
    float RequestTimeoutSeconds = 60.0f;

    /** When enabled, log full request/response bodies on API errors. Useful for debugging but produces large log entries. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Verbose Network Logging"))
    bool bVerboseNetworkLogging = false;

    // --- Search ---

    /** Timeout in seconds for Blueprint search (Find in Blueprints). Increase if FiB index is slow to build. */
    UPROPERTY(Config, EditAnywhere, Category="Search",
        meta=(DisplayName="Blueprint Search Timeout (seconds)", ClampMin=5.0, ClampMax=300.0))
    float BlueprintSearchTimeoutSeconds = 30.0f;

    // --- PIE ---

    /** Net modes to hide from the pie_start tool's netMode parameter.
     *  Any mode listed here cannot be selected by MCP callers.
     *  Valid values: "Standalone", "ListenServer", "DedicatedServer", "Client" */
    UPROPERTY(Config, EditAnywhere, Category="PIE",
        meta=(DisplayName="Disabled PIE Net Modes"))
    TSet<FString> DisabledPIENetModes;

    // --- Debug ---

    /** Log all REPL tool calls (name, arguments, results) to the Unreal output log.
     *  Also logs the full API request/response JSON for each turn. Very verbose. */
    UPROPERTY(Config, EditAnywhere, Category="Debug",
        meta=(DisplayName="Log All Tool Calls"))
    bool bLogAllToolCalls = false;

    // --- Helpers ---

    /** Returns the effective system prompt (override if set, else built-in default). */
    FString GetEffectiveSystemPrompt() const;

    /** Broadcast when any property changes in the editor UI. */
    FOnClaireonSettingsChanged OnSettingsChanged;

    /** Returns suggested model ID options for the property dropdown. */
    UFUNCTION()
    TArray<FString> GetModelOptions() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("MCP REPL"); }
};
