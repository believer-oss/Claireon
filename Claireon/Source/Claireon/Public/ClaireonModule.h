// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "IClaireonToolProvider.h"

class FClaireonServer;
class FClaireonProxyClient;
class IClaireonTool;

/**
 * Stage 010 (D4 + direct-connect): which way the editor's MCP traffic is
 * flowing. DirectConnect means the editor itself owns the per-worktree SHA
 * port and Claude talks to it directly. ProxyAttached means the singleton
 * proxy owns the SHA port and the editor binds an ephemeral local listener
 * that the proxy forwards to.
 */
enum class EClaireonMcpMode : uint8
{
	/** StartServer has not yet decided which mode to enter. */
	Unstarted,
	/** Editor owns the SHA port; no proxy in the path. */
	DirectConnect,
	/**
	 * Proxy owns the SHA port; editor binds an ephemeral local listener and
	 * registers via /editor/register. Reached either by opt-in
	 * (bEnableProxy=true / -EnableMCPProxy) or by auto-promote when the
	 * editor's TryStart on the SHA port fails because the proxy is already
	 * holding it.
	 */
	ProxyAttached,
};

/**
 * Editor module that hosts an MCP server inside the Unreal Editor.
 * Exposes editor functionality to external AI tools via Streamable HTTP.
 */
class CLAIREON_API FClaireonModule final : public IModuleInterface, private FNoncopyable
{
public:
	FClaireonModule();
	virtual ~FClaireonModule();

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Start the MCP server */
	void StartServer();

	/** Stop the MCP server */
	void StopServer();

	/** Whether the MCP server is currently running */
	bool IsServerRunning() const;

	/** Get the server instance (may be null if not running) */
	FClaireonServer* GetServer() const { return Server.Get(); }

	/**
	 * Get the proxy client (may be null when proxy wiring is disabled, i.e.
	 * direct-connect mode). The diagnostics widget reads ProxyState through
	 * this and exposes a Reconnect button when state is Failed (D4).
	 */
	FClaireonProxyClient* GetProxyClient() const { return ProxyClient.Get(); }

	/** Stage 010: current ingress mode for diagnostics-tab display. */
	EClaireonMcpMode GetMcpMode() const { return CurrentMcpMode; }

	/** Get the module instance */
	static FClaireonModule& Get();

private:
	/** Register toolbar button and menu entries */
	void RegisterMenus();

	/** Collect tools from all registered IClaireonToolProvider implementations. */
	void CollectToolsFromProviders();

	/** Collect tools from a single provider and register them with the server. */
	void CollectToolsFromProvider(IClaireonToolProvider* Provider);

	/** Called when a modular feature is registered. */
	void OnModularFeatureRegistered(const FName& Type, IModularFeature* ModularFeature);

	/** Called when a modular feature is unregistered. */
	void OnModularFeatureUnregistered(const FName& Type, IModularFeature* ModularFeature);

	/** The MCP server instance */
	TSharedPtr<FClaireonServer> Server;

	/**
	 * Always-on MCP proxy client (stage-06). Null when proxy wiring is
	 * disabled (the default). Opt in with the `-EnableMCPProxy` command-line
	 * flag or by setting bEnableProxy=true in Editor Preferences. When non-null,
	 * the proxy is the sole ingress for MCP traffic; the server runs token-gated.
	 */
	TUniquePtr<FClaireonProxyClient> ProxyClient;

	/** Built-in tool provider (owns the ~99 built-in tool definitions) */
	TUniquePtr<IClaireonToolProvider> BuiltinToolProvider;

	/** Handle for UClaireonSettings::OnSettingsChanged subscription. */
	FDelegateHandle SettingsChangedHandle;

	/** True if -MCPServerPort= was passed on the command line; suppresses settings-change restart. */
	bool bPortOverriddenByCommandLine = false;

	/** Stage 010: which path StartServer settled on (DirectConnect / ProxyAttached). */
	EClaireonMcpMode CurrentMcpMode = EClaireonMcpMode::Unstarted;

	/** Handle for IPythonScriptPlugin::OnPythonInitialized subscription.
	 *  Bound in StartupModule when Python is not yet initialised; released in ShutdownModule. */
	FDelegateHandle PythonInitHandle;
};
