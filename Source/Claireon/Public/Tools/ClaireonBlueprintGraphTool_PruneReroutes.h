// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class UBlueprint;
class UEdGraph;

/** Reroute pruning shared by the tool and headless graph tests. */
namespace ClaireonPruneReroutes
{
	/** Removal counts and convergence status from one pruning call. */
	struct FPruneResult
	{
		int32 RemovedOrphanComponent = 0;
		int32 RemovedDanglingNoInput = 0;
		int32 RemovedDanglingNoOutput = 0;
		int32 SkippedNoInputWithDefault = 0;
		int32 Passes = 0;
		bool bConverged = false;

		int32 Total() const
		{
			return RemovedOrphanComponent + RemovedDanglingNoInput + RemovedDanglingNoOutput;
		}
	};

	/** Delete every orphaned or dangling reroute in Graph, iterating to a fixpoint. See the .cpp. */
	FPruneResult PruneOrphanReroutesInGraph(UBlueprint* Blueprint, UEdGraph* Graph);

	/** Render Result as the single caller-facing status line (session status and stateless summary). */
	FString BuildStatusMessage(const FPruneResult& Result);
}

DECLARE_BPGRAPH_TOOL(ClaireonBlueprintGraphTool_PruneReroutes);
