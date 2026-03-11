// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

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

	/**
	 * Register an external tool with the server.
	 * Safe to call from other modules without including ClaireonServer.h.
	 * If the server is not yet running, the tool is queued and registered
	 * automatically when the server starts.
	 */
	void RegisterExternalTool(TSharedPtr<IClaireonTool> Tool);

private:
	/** Register toolbar button and menu entries */
	void RegisterMenus();

	/** Flush any pending external tools into the running server. */
	void FlushPendingExternalTools();

	/** The MCP server instance */
	TSharedPtr<FClaireonServer> Server;

	/** Tools registered before the server was started. */
	TArray<TSharedPtr<IClaireonTool>> PendingExternalTools;
};
