// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FormatBlueprintGraph.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "BlueprintEditor.h"
#include "Framework/Application/SlateApplication.h"

#if WITH_BLUEPRINT_ASSIST
	#include "BlueprintAssistGraphHandler.h"
	#include "BlueprintAssistModule.h"
	#include "BlueprintAssistTabHandler.h"
	#include "Widgets/Docking/SDockTab.h"
#endif

#define LOCTEXT_NAMESPACE "Claireon"

FString ClaireonTool_FormatBlueprintGraph::GetName() const
{
	return TEXT("claireon.blueprint_format_graph");
}

TArray<FString> ClaireonTool_FormatBlueprintGraph::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("format"), TEXT("graph"), TEXT("layout"), TEXT("arrange"), TEXT("pretty")};
}

FString ClaireonTool_FormatBlueprintGraph::GetDescription() const
{
	return TEXT("Auto-format a Blueprint graph using Blueprint Assist. Arranges nodes in a readable layout.");
}

TSharedPtr<FJsonObject> ClaireonTool_FormatBlueprintGraph::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path (required)
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Path to the Blueprint asset (must start with /Game/)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// graph_name (optional)
	TSharedPtr<FJsonObject> GraphNameProp = MakeShared<FJsonObject>();
	GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
	GraphNameProp->SetStringField(TEXT("description"), TEXT("Name of the graph to format (defaults to EventGraph)"));
	Properties->SetObjectField(TEXT("graph_name"), GraphNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_FormatBlueprintGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Extract arguments
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString GraphName = TEXT("EventGraph");
	Arguments->TryGetStringField(TEXT("graph_name"), GraphName);

	// Resolve asset path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Load Blueprint
	FString Error;
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath, Error);
	if (!Blueprint)
	{
		return MakeErrorResult(Error);
	}

	// Find graph
	UEdGraph* Graph = FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Format Blueprint Graph")));
#if WITH_BLUEPRINT_ASSIST
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("BlueprintAssist")))
	{
		return MakeErrorResult(TEXT("Blueprint Assist module is not loaded. This tool requires the Blueprint Assist plugin."));
	}

	bool bSuccess = FormatWithBlueprintAssist(Blueprint, Graph, Error);
	if (!bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("Blueprint Assist formatting failed: %s"), *Error));
	}

	FString FormatterUsed = TEXT("Blueprint Assist");
#else
	return MakeErrorResult(TEXT("Blueprint Assist is not available (WITH_BLUEPRINT_ASSIST=0). This tool requires the Blueprint Assist plugin."));
	FString FormatterUsed = TEXT("None");  // unreachable, satisfies compiler
#endif

	// Mark Blueprint as modified and compile
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Build success message
	int32 NodeCount = Graph->Nodes.Num();
	FString ResultMessage = FString::Printf(
		TEXT("Successfully formatted graph '%s' using %s formatter.\nNodes formatted: %d"),
		*GraphName,
		*FormatterUsed,
		NodeCount
	);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("graph_name"), GraphName);
	ResultJson->SetStringField(TEXT("formatter_used"), FormatterUsed);
	ResultJson->SetNumberField(TEXT("node_count"), NodeCount);

	return MakeSuccessResult(ResultJson, ResultMessage);
}

UBlueprint* ClaireonTool_FormatBlueprintGraph::LoadBlueprintFromPath(const FString& AssetPath, FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath);
		return nullptr;
	}
	return Blueprint;
}

UEdGraph* ClaireonTool_FormatBlueprintGraph::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// Check event graphs (UbergraphPages)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check function graphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	// Check macro graphs
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	return nullptr;
}

#if WITH_BLUEPRINT_ASSIST
bool ClaireonTool_FormatBlueprintGraph::FormatWithBlueprintAssist(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutError)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("BlueprintAssist")))
	{
		OutError = TEXT("Blueprint Assist module is not loaded");
		return false;
	}

	// Open editor (creates SGraphEditor widgets needed by BA)
	FScopedBlueprintEditor ScopedEditor(Blueprint, /*bSilent=*/false, /*bCloseOnDestroy=*/true);
	if (!ScopedEditor.IsValid())
	{
		OutError = TEXT("Failed to open Blueprint editor");
		return false;
	}

	// Bring the target graph to front so BA's tab handler detects it
	ScopedEditor.GetBlueprintEditor()->OpenGraphAndBringToFront(Graph);

	// Tick Slate to allow widget creation from OpenGraphAndBringToFront
	FSlateApplication::Get().Tick();

	// BA's normal ProcessTab path uses SetTimerForNextTick which can't fire from MCP context.
	// Use ProcessTabImmediate to synchronously detect the SGraphEditor and create the handler.
	TSharedPtr<SDockTab> ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
	if (ActiveTab.IsValid())
	{
		FBATabHandler::Get().ProcessTabImmediate(ActiveTab);
	}

	TSharedPtr<FBAGraphHandler> Handler = FBATabHandler::Get().GetActiveGraphHandler();
	if (!Handler.IsValid())
	{
		// The graph tab widget may not be fully built yet. Tick Slate and retry.
		constexpr float HandlerTickDelta = 1.0f / 60.0f;
		constexpr float HandlerMaxWaitSeconds = 5.0f;
		float HandlerElapsed = 0.0f;

		while (HandlerElapsed < HandlerMaxWaitSeconds)
		{
			FSlateApplication::Get().Tick();
			HandlerElapsed += HandlerTickDelta;

			ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
			if (ActiveTab.IsValid())
			{
				FBATabHandler::Get().ProcessTabImmediate(ActiveTab);
			}

			Handler = FBATabHandler::Get().GetActiveGraphHandler();
			if (Handler.IsValid())
			{
				break;
			}
		}
	}

	if (!Handler.IsValid())
	{
		OutError = TEXT("Blueprint Assist could not find active graph handler");
		return false;
	}

	// Pre-format: scatter nodes that share exact positions to prevent BA stacked-node bug.
	// BA's knot track creation breaks when multiple nodes occupy the same coordinates.
	{
		TMap<FIntPoint, TArray<UEdGraphNode*>> PositionBuckets;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				FIntPoint Pos(Node->NodePosX, Node->NodePosY);
				PositionBuckets.FindOrAdd(Pos).Add(Node);
			}
		}

		for (auto& [Pos, Nodes] : PositionBuckets)
		{
			if (Nodes.Num() > 1)
			{
				UE_LOG(LogClaireon, Verbose, TEXT("[editor.blueprint.formatGraph] Scattering %d nodes at position (%d, %d)"),
					Nodes.Num(), Pos.X, Pos.Y);
				for (int32 i = 1; i < Nodes.Num(); ++i)
				{
					// Deterministic offset: 300 units apart horizontally per node.
					// No randomness -- deterministic behavior is easier to debug.
					Nodes[i]->Modify();
					Nodes[i]->NodePosX += i * 300;
					Nodes[i]->NodePosY += i * 100;
				}
			}
		}
	}

	// Kick off formatting (categorizes nodes into columns, starts node size caching)
	Handler->FormatAllEvents();

	// Drive the async formatting to completion via Slate ticks.
	// BA needs: size caching -> format -> post-format, all via Tick()
	constexpr float TickDelta = 1.0f / 60.0f;
	constexpr float MaxWaitSeconds = 60.0f;
	float Elapsed = 0.0f;

	while (Elapsed < MaxWaitSeconds)
	{
		FSlateApplication::Get().Tick();
		Handler->Tick(TickDelta);
		Elapsed += TickDelta;

		// Done when no pending sizes and no pending formatting
		if (!Handler->IsCalculatingNodeSize() &&
			Handler->GetNumberOfPendingNodesToCache() == 0)
		{
			break;
		}
	}

	if (Elapsed >= MaxWaitSeconds)
	{
		OutError = TEXT("Blueprint Assist formatting timed out waiting for node sizes");
		return false;
	}

	// Post-format validation: check for overlapping nodes
	{
		int32 OverlapCount = 0;
		TArray<UEdGraphNode*> FormattedNodes = Graph->Nodes;
		for (int32 i = 0; i < FormattedNodes.Num(); ++i)
		{
			if (!FormattedNodes[i]) continue;
			FSlateRect BoundsA = Handler->GetCachedNodeBounds(FormattedNodes[i]);
			if (BoundsA.GetSize().IsZero()) continue; // Skip nodes with no cached bounds

			for (int32 j = i + 1; j < FormattedNodes.Num(); ++j)
			{
				if (!FormattedNodes[j]) continue;
				FSlateRect BoundsB = Handler->GetCachedNodeBounds(FormattedNodes[j]);
				if (BoundsB.GetSize().IsZero()) continue;

				if (FSlateRect::DoRectanglesIntersect(BoundsA, BoundsB))
				{
					++OverlapCount;
				}
			}
		}

		if (OverlapCount > 0)
		{
			UE_LOG(LogClaireon, Warning, TEXT("[editor.blueprint.formatGraph] Post-format validation: %d node pair overlaps detected in graph '%s'"),
				OverlapCount, *Graph->GetName());
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[editor.blueprint.formatGraph] Formatted graph '%s' using Blueprint Assist"), *Graph->GetName());
	return true;
}
#endif

#undef LOCTEXT_NAMESPACE
