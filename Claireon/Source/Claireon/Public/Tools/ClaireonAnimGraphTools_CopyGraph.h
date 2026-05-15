// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: duplicate a single graph's contents from one Animation Blueprint
 * into another. Uses FEdGraphUtilities::ExportNodesToText / ImportNodesFromText so
 * nested state-machine state graphs and transition rule graphs ride along as T3D
 * subobjects.
 *
 * The destination graph must already exist in the destination Animation Blueprint.
 * For newly-created AnimBPs this covers the default "AnimGraph" without extra work;
 * custom function / state-machine graphs require creating the graph first via other
 * tools or running this copy against an ABP that already has the target graph.
 *
 * Dependency replication (copy_functions=true, default):
 *   - All source BP variables are mirrored onto the destination (via the dedicated flag).
 *   - All source BP FunctionGraphs are duplicated onto the destination (needed by state
 *     machine OnBecomeRelevant/OnUpdate bindings, transition helper functions, and any
 *     CallFunction nodes in the copied graph that point at self-context functions).
 *   - All source BP UbergraphPages (event graphs) are duplicated.
 *   - All source BP MacroGraphs are duplicated.
 *   - Interfaces, timelines, and components are NOT copied — a warning is emitted if
 *     the source uses them.
 */
class CLAIREON_API ClaireonAnimGraphTool_CopyGraph : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("animgraph"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
