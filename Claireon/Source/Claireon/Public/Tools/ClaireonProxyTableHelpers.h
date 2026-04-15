// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UProxyTable;
class UProxyAsset;
struct FProxyEntry;
struct FProxyStructOutput;

/**
 * Shared utility functions for Proxy Table and Proxy Asset MCP tools.
 * Provides asset loading, entry introspection, and save utilities.
 */
namespace ClaireonProxyTableHelpers
{
	/** Load and validate a UProxyTable from an asset path. */
	UProxyTable* LoadProxyTableAsset(const FString& AssetPath, FString& OutError);

	/** Load and validate a UProxyAsset from an asset path. */
	UProxyAsset* LoadProxyAsset(const FString& AssetPath, FString& OutError);

	/** Save a modified proxy table to disk. */
	bool SaveProxyTable(UProxyTable* ProxyTable, FString& OutError);

	/** Save a modified proxy asset to disk. */
	bool SaveProxyAsset(UProxyAsset* ProxyAsset, FString& OutError);

	/** Serialize a proxy table entry to JSON. */
	TSharedPtr<FJsonObject> SerializeProxyEntry(const FProxyEntry& Entry, int32 Index);

	/** Serialize proxy output struct data to JSON array. */
	TArray<TSharedPtr<FJsonValue>> SerializeProxyStructOutputs(const TArray<FProxyStructOutput>& Outputs);

	/** Serialize a proxy asset's metadata to JSON. */
	TSharedPtr<FJsonObject> SerializeProxyAssetInfo(const UProxyAsset* ProxyAsset);

} // namespace ClaireonProxyTableHelpers
