// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UPCGGraph;
class UPCGNode;
class UPCGPin;
class UPCGSettings;

/**
 * Shared utility functions for PCG Graph MCP tools.
 * Provides asset loading, node lookup, formatting, and property manipulation.
 */
namespace ClaireonPCGGraphHelpers
{
	/** Load and validate a PCG Graph asset from an asset path. */
	UPCGGraph* LoadPCGGraphAsset(const FString& AssetPath, FString& OutError);

	/** Find a node by identifier (numeric index, node title, or settings class name). */
	UPCGNode* FindNodeByIdentifier(UPCGGraph* Graph, const FString& Identifier, int32& OutIndex);

	/** Get the display name for a node (title if set, otherwise settings class name). */
	FString GetNodeDisplayName(const UPCGNode* Node);

	/** Get a short name for a settings class (strips "PCG" prefix and "Settings" suffix). */
	FString GetSettingsShortName(const UPCGSettings* Settings);

	/** Resolve a settings class name to a UClass*. Accepts full, partial, or short names. */
	UClass* ResolveSettingsClass(const FString& ClassName, FString& OutError);

	/** Format the full graph structure as structured text. */
	FString FormatGraphStructure(const UPCGGraph* Graph, const FString& DetailLevel);

	/** Format a single node with its pins, connections, and optionally properties. */
	FString FormatNodeDetail(const UPCGGraph* Graph, const UPCGNode* Node, int32 NodeIndex, bool bIncludeProperties);

	/** Format pin info including connections. */
	FString FormatPinConnections(const UPCGGraph* Graph, const UPCGPin* Pin);

	/** Read all editable properties of a node's settings as formatted text. */
	FString ReadNodeProperties(const UPCGNode* Node);

	/** Set a property value on a node's settings via ImportText. */
	bool SetNodeProperty(UPCGNode* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError);

	/** Notify the graph that its structure has changed (triggers editor refresh). */
	void NotifyGraphChanged(UPCGGraph* Graph);

	/** List all available PCG settings class names. */
	TArray<FString> GetAvailableSettingsClasses();
} // namespace ClaireonPCGGraphHelpers
