// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ClaireonSettings.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnClaireonSettingsChanged);

/**
 * Live-editable settings for Claireon.
 * All values are read fresh on each API call — change in Editor Preferences,
 * take effect immediately with no recompile or editor restart.
 *
 * Found at: Editor Preferences > Plugins > Claireon
 */
UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="Claireon"))
class UClaireonSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UClaireonSettings();

    /** Get the singleton. Safe to call from game thread. */
    static const UClaireonSettings* Get();

    // --- Connection ---
    // Server > Server Port is no longer exposed: the editor's MCP listener
    // binds the per-worktree SHA port computed by
    // Claireon::DeriveDefaultMcpPort; there is no editor-facing port setting.

    /** Anthropic API key. Stored via the platform credential store, not in config. */
    UPROPERTY(Transient, EditAnywhere, Category="Connection",
        meta=(DisplayName="Anthropic API Key", PasswordField=true,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    FString AnthropicApiKey;

    /** API endpoint. Override for proxies or local testing. */
    UPROPERTY(Config, EditAnywhere, Category="Connection",
        meta=(DisplayName="API Endpoint URL",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    FString ApiEndpointUrl = TEXT("https://api.anthropic.com/v1/messages");

    /** anthropic-version header value. */
    UPROPERTY(Config, EditAnywhere, Category="Connection",
        meta=(DisplayName="Anthropic-Version Header",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    FString AnthropicVersion = TEXT("2023-06-01");

    // --- Model ---

    /**
     * Model ID to use. Default is Haiku for low cost.
     * Common options: claude-haiku-4-5, claude-sonnet-4-6, claude-opus-4-6
     */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Model ID", GetOptions="GetModelOptions",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    FString ModelId = TEXT("claude-haiku-4-5-20251001");

    /** Maximum tokens per response. */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Max Tokens", ClampMin=256, ClampMax=16384,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    int32 MaxTokens = 4096;

    /**
     * Maximum tool-use loop iterations per user message.
     * Safety valve to prevent runaway tool chains.
     */
    UPROPERTY(Config, EditAnywhere, Category="Model",
        meta=(DisplayName="Tool-Use Depth Limit", ClampMin=1, ClampMax=20,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    int32 ToolUseDepthLimit = 10;

    // --- Prompt ---

    /**
     * System prompt sent with every API request.
     * Edit here to change Claude's behavior without recompiling.
     * Leave empty to use the built-in default prompt.
     */
    UPROPERTY(Config, EditAnywhere, Category="Prompt",
        meta=(DisplayName="System Prompt Override", MultiLine=true,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    FString SystemPromptOverride;

    // --- Display ---

    /** Render assistant messages with rich formatting (bold, code blocks, tables).
     *  When unchecked, messages display as plain text. */
    UPROPERTY(Config, EditAnywhere, Category="Display",
        meta=(DisplayName="Enable Rich Text Formatting",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    bool bEnableRichText = true;

    // --- Logging ---

    /** Directory for JSONL conversation log files. Relative to project root. Fixed at Saved/Claireon/Logs. */
    UPROPERTY(Config, VisibleAnywhere, Category="Logging",
        meta=(DisplayName="Log Directory"))
    FString LogDirectory = TEXT("Saved/Claireon/Logs");

    /** How often (seconds) to flush the log buffer to disk. */
    UPROPERTY(Config, EditAnywhere, Category="Logging",
        meta=(DisplayName="Log Flush Interval (seconds)", ClampMin=1, ClampMax=60,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    float LogFlushIntervalSeconds = 5.0f;

    /** Whether to write conversation logs at all. */
    UPROPERTY(Config, EditAnywhere, Category="Logging",
        meta=(DisplayName="Enable Conversation Logging",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
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
        meta=(DisplayName="Max Retry Attempts", ClampMin=0, ClampMax=10,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    int32 MaxRetryAttempts = 10;

    /** Base delay in seconds for exponential backoff between retries. Actual delay: min(base * 2^attempt + jitter, max). */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Initial Retry Delay (seconds)", ClampMin=0.5, ClampMax=10.0,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    float InitialRetryDelaySeconds = 1.0f;

    /** Maximum delay in seconds between retries. Caps exponential growth and Retry-After header values. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Max Retry Delay (seconds)", ClampMin=5.0, ClampMax=120.0,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    float MaxRetryDelaySeconds = 30.0f;

    /** Minimum interval in seconds between outgoing API calls. Prevents burst traffic during tool-use loops. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Min Request Interval (seconds)", ClampMin=0.0, ClampMax=5.0,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    float MinRequestIntervalSeconds = 0.2f;

    /** HTTP request timeout in seconds. If the API does not respond within this time, the request is cancelled and retried. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Request Timeout (seconds)", ClampMin=10.0, ClampMax=300.0,
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    float RequestTimeoutSeconds = 60.0f;

    /** When enabled, log full request/response bodies on API errors. Useful for debugging but produces large log entries. */
    UPROPERTY(Config, EditAnywhere, Category="Reliability",
        meta=(DisplayName="Verbose Network Logging",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    bool bVerboseNetworkLogging = false;

    // --- Python Execution ---

    /**
     * Watchdog timeout for a single python_execute call (seconds).
     * After this many seconds the watchdog injects a TimeoutError into the
     * Python interpreter; the game thread unblocks once Python processes it.
     *
     * Limitations:
     *   - The injection fires between Python bytecodes only. A blocking C
     *     extension call (e.g. a slow claireon.* tool) will not be interrupted
     *     mid-call; the error surfaces when that call returns to Python.
     *   - Set to 0 to disable the watchdog entirely (not recommended -- a
     *     hung script will freeze the editor until it is killed).
     *
     * Must be less than the proxy's HEARTBEAT_STALENESS_SECONDS (180 s) so the
     * session is not evicted before the watchdog fires.
     */
    UPROPERTY(Config, EditAnywhere, Category="Python Execution",
        meta=(DisplayName="Python Execution Timeout (seconds)", ClampMin=0.0, ClampMax=120.0))
    float PythonExecutionTimeoutSeconds = 60.0f;

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

    // --- Auto-Save ---

    /** Master toggle for automatic pre-mutation saves.
     *  When enabled, dirty packages are saved before crash-risk operations. */
    UPROPERTY(Config, EditAnywhere, Category="Auto-Save",
        meta=(DisplayName="Enable Auto-Save"))
    bool bEnableAutoSave = true;

    /** Save dirty packages before each Python execution. Setting this to true will attempt to save assets nearly every time the MCP server is used. Defaulting to false as of April 9, 2026 since this would be perhaps surprising new behavior rather than a helpful default. If this changes let's switch it! */
    UPROPERTY(Config, EditAnywhere, Category="Auto-Save",
        meta=(DisplayName="Auto-Save Before Python Execution",
              EditCondition="bEnableAutoSave"))
    bool bAutoSaveBeforePythonExecution = false;

    /** Save dirty packages before deferred world-transition actions (map load, PIE start, etc.). */
    UPROPERTY(Config, EditAnywhere, Category="Auto-Save",
        meta=(DisplayName="Auto-Save Before Deferred Actions",
              EditCondition="bEnableAutoSave"))
    bool bAutoSaveBeforeDeferredActions = true;

    /** Minimum seconds between auto-saves (debounce). Prevents save-spam during rapid tool calls. */
    UPROPERTY(Config, EditAnywhere, Category="Auto-Save",
        meta=(DisplayName="Auto-Save Debounce (seconds)",
              EditCondition="bEnableAutoSave",
              ClampMin=0.0, ClampMax=60.0))
    float AutoSaveDebounceSeconds = 5.0f;

    // --- Large Results ---

    /** Per-stream threshold (bytes) above which tool result streams spill to disk under
     *  <ProjectSavedDir>/Claireon/Results/.  Streams below this threshold stay inline. */
    UPROPERTY(Config, EditAnywhere, Category="Large Results",
        meta=(DisplayName="Result Spill Threshold (bytes)", ClampMin=1024, ClampMax=10485760))
    int32 ResultSpillThresholdBytes = 8192;

    /** Per-stream hard ceiling (bytes).  Streams above this size are truncated to this limit
     *  on disk with bOverCeiling flagged on the envelope.  Clamp floor is threshold+1. */
    UPROPERTY(Config, EditAnywhere, Category="Large Results",
        meta=(DisplayName="Result Spill Ceiling (bytes)", ClampMin=1025, ClampMax=1073741824))
    int32 ResultSpillMaxBytes = 52428800;

    /** When true, spill directories are never swept on connect and accumulate indefinitely.
     *  When false, spills older than ResultSpillRetentionDays are deleted on connect.
     *  Default is true -- keep everything unless explicitly opted out. */
    UPROPERTY(Config, EditAnywhere, Category="Large Results",
        meta=(DisplayName="Keep Result Spills"))
    bool bKeepResultSpills = true;

    /** Age threshold (days) above which spill subdirectories are swept on connect.
     *  0 = never delete (same effect as enabling Keep Result Spills above).
     *  Only active when Keep Result Spills is unchecked. */
    UPROPERTY(Config, EditAnywhere, Category="Large Results",
        meta=(DisplayName="Result Spill Retention (days)",
              EditCondition="!bKeepResultSpills",
              ClampMin=0, ClampMax=365))
    int32 ResultSpillRetentionDays = 7;

    // --- Debug ---

    /** Log all REPL tool calls (name, arguments, results) to the Unreal output log.
     *  Also logs the full API request/response JSON for each turn. Very verbose. */
    UPROPERTY(Config, EditAnywhere, Category="Debug",
        meta=(DisplayName="Log All Tool Calls",
              EditCondition="bEnableREPLChat", HideEditConditionToggle))
    bool bLogAllToolCalls = false;

    // --- Engine Diagnostics ---

    /** Log categories to exclude from engine diagnostics capture during tool execution.
     *  Categories listed here are skipped by FClaireonLogCapture.
     *  Use this to suppress known-noisy categories (e.g., LogSlate, LogStreaming). */
    UPROPERTY(Config, EditAnywhere, Category="Engine Diagnostics",
        meta=(DisplayName="Excluded Engine Log Categories"))
    TSet<FName> ExcludedEngineLogCategories;

    // --- Claude Code Launch ---

    /** Initial prompt sent to Claude Code on launch (typically a slash command).
     *  Empty = no initial prompt; engineer types their own.
     *  Example: "/mcp-connect-claireon" to auto-fire the worktree-init skill. */
    UPROPERTY(Config, EditAnywhere, Category="Claude Code Launch",
        meta=(DisplayName="Initial Prompt"))
    FString LaunchInitialPrompt;

    /** When true, the launch button passes --dangerously-skip-permissions to Claude Code,
     *  auto-approving every tool call for the entire session. Convenient for engineer
     *  workflows where the initial-prompt skill needs to run unattended (e.g., worktree
     *  init via Bash). All security caveats of skipping permissions apply. */
    UPROPERTY(Config, EditAnywhere, Category="Claude Code Launch",
        meta=(DisplayName="Skip Permission Prompts on Launch"))
    bool bLaunchSkipPermissions = false;

    /** Show the [ Claude Code ] launch button in the Claireon panel status strip.
     *  Disable if you prefer to launch Claude Code another way. */
    UPROPERTY(Config, EditAnywhere, Category="Claude Code Launch",
        meta=(DisplayName="Show Claude Code Button"))
    bool bShowClaudeCodeButton = true;

    // --- MCP Proxy ---

    /** Whether to front this editor with the always-on MCP proxy.
     *  Default is false: the editor runs in direct-connect mode (Claude talks
     *  directly to the editor's MCP port, no proxy) unless explicitly opted in.
     *  Opting in spawns-or-attaches to the proxy and registers; the proxy
     *  becomes the sole ingress for MCP traffic.
     *  Command-line override: `-EnableMCPProxy` forces this to true regardless of the setting.
     *
     *  Note: even with bEnableProxy=false, the editor auto-promotes into
     *  proxy-attached mode when the SHA port is already held by a Claireon
     *  proxy on 43017. This keeps the toolbar button transparent across
     *  modes; this setting only controls whether StartServer proactively
     *  spawns the proxy. */
    UPROPERTY(Config, EditAnywhere, Category="MCP Proxy",
        meta=(DisplayName="Enable MCP Proxy"))
    bool bEnableProxy = false;

    // ProxyIdleTimeoutSeconds is no longer exposed: the singleton proxy runs
    // until SIGINT/SIGTERM, so the setting has no consumer.

    /** Extra arguments appended to the proxy spawn command line (whitespace-separated). */
    UPROPERTY(Config, EditAnywhere, Category="MCP Proxy",
        meta=(DisplayName="Proxy Extra Python Args"))
    FString ProxyExtraPythonArgs;

    /** HTTP forward timeout (seconds) the plugin uses when sending MCP traffic to the proxy. */
    UPROPERTY(Config, EditAnywhere, Category="MCP Proxy",
        meta=(DisplayName="Proxy Forward Timeout (seconds)", ClampMin=5, ClampMax=3600))
    int32 ProxyForwardTimeoutSeconds = 600;

    // --- Launch Agent ---

    /**
     * Command passed to the Launch Agent toolbar button. The default "claude"
     * launches the Claude Code CLI. Override with a custom path or wrapper if
     * needed. The command is launched in a terminal at the project root.
     */
    UPROPERTY(Config, EditAnywhere, Category="Launch Agent",
        meta=(DisplayName="Agent Launch Command"))
    FString AgentLaunchCommand = TEXT("claude");

    // --- Advanced / REPL ---

    /**
     * Show the in-editor REPL Chat tab inside the Claireon panel.
     * Disabled by default -- the primary interaction path is via the
     * Launch Agent toolbar button (Claude Code CLI). Requires an
     * Anthropic API key to be set in the Connection category above.
     */
    UPROPERTY(Config, EditAnywhere, Category="Advanced",
        meta=(DisplayName="Enable In-Editor REPL Chat"))
    bool bEnableREPLChat = false;

    // --- Helpers ---

    /** Returns the effective system prompt (override if set, else built-in default). */
    FString GetEffectiveSystemPrompt() const;

    /** Read the Anthropic API key from platform storage. */
    FString GetAnthropicApiKey() const;

    /** True when an API key is available in platform storage. */
    bool HasAnthropicApiKey() const;

    /** Save or clear the Anthropic API key in platform storage. */
    void SetAnthropicApiKey(const FString& NewApiKey);

    /** Broadcast when any property changes in the editor UI. */
    FOnClaireonSettingsChanged OnSettingsChanged;

    /** Returns suggested model ID options for the property dropdown. */
    UFUNCTION()
    TArray<FString> GetModelOptions() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("Claireon"); }
};
