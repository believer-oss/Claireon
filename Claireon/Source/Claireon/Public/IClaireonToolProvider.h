// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Features/IModularFeatures.h"

class IClaireonTool; // forward-declare; callers include IClaireonTool.h for TSharedPtr usage

/**
 * Modular-feature interface for registering MCP tools with the Claireon server.
 *
 * Modules implement this interface to provide tools, then register via:
 *   IModularFeatures::Get().RegisterModularFeature(IClaireonToolProvider::FeatureName, this);
 *
 * The Claireon module discovers all registered providers and collects their tools
 * at server start, and dynamically when providers register/unregister at runtime.
 */
class CLAIREON_API IClaireonToolProvider : public IModularFeature
{
public:
	static const FName FeatureName;

	virtual ~IClaireonToolProvider() = default;

	/** Return all tools this provider offers. Called each time tools are collected. */
	virtual TArray<TSharedPtr<IClaireonTool>> GetTools() const = 0;

	/** Human-readable name for diagnostics (e.g. "MyPlugin", "Claireon Built-in"). */
	virtual FName GetProviderName() const = 0;
};
