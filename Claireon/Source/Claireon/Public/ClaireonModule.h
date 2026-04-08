// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "IClaireonToolProvider.h"

class FClaireonServer;
class IClaireonTool;

/**
 * Editor module that hosts an MCP server inside the Unreal Editor.
 * Exposes editor functionality to external AI tools via Streamable HTTP.
 */
class CLAIREON_API FClaireonModule final : public IModuleInterface, private FNoncopyable
{
public:
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

	/** Built-in tool provider (owns the ~99 built-in tool definitions) */
	TUniquePtr<IClaireonToolProvider> BuiltinToolProvider;

	/** Handle for UClaireonSettings::OnSettingsChanged subscription. */
	FDelegateHandle SettingsChangedHandle;

	/** True if -MCPServerPort= was passed on the command line; suppresses settings-change restart. */
	bool bPortOverriddenByCommandLine = false;
};
