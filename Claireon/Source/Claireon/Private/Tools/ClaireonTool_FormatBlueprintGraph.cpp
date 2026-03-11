// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FormatBlueprintGraph.h"
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

// Constants for layout
static const float NODE_HORIZONTAL_SPACING = 400.0f;
static const float NODE_VERTICAL_SPACING = 100.0f;
static const float NODE_HEIGHT_ESTIMATE = 120.0f;

FString ClaireonTool_FormatBlueprintGraph::GetName() const
{
	return TEXT("editor.blueprint.formatGraph");
}

FString ClaireonTool_FormatBlueprintGraph::GetDescription() const
{
	return TEXT("Auto-format a Blueprint graph using Blueprint Assist or fallback formatter. Arranges nodes in a readable layout.");
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

	// formatter (optional)
	TSharedPtr<FJsonObject> FormatterProp = MakeShared<FJsonObject>();
	FormatterProp->SetStringField(TEXT("type"), TEXT("string"));
	FormatterProp->SetStringField(TEXT("description"), TEXT("Formatter to use: 'blueprint_assist' or 'fallback' (auto-detects if not specified)"));
	TArray<TSharedPtr<FJsonValue>> FormatterEnum;
	FormatterEnum.Add(MakeShared<FJsonValueString>(TEXT("blueprint_assist")));
	FormatterEnum.Add(MakeShared<FJsonValueString>(TEXT("fallback")));
	FormatterProp->SetArrayField(TEXT("enum"), FormatterEnum);
	Properties->SetObjectField(TEXT("formatter"), FormatterProp);

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

	FString FormatterType;
	Arguments->TryGetStringField(TEXT("formatter"), FormatterType);

	// Validate asset path
	FString Error;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load Blueprint
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

	// Determine which formatter to use
	bool bUseBlueprintAssist = false;
	bool bUseFallback = false;

	if (FormatterType.IsEmpty())
	{
		// Auto-detect
#if WITH_BLUEPRINT_ASSIST
		if (FModuleManager::Get().IsModuleLoaded(TEXT("BlueprintAssist")))
		{
			bUseBlueprintAssist = true;
		}
		else
#endif
		{
			bUseFallback = true;
		}
	}
	else if (FormatterType.Equals(TEXT("blueprint_assist"), ESearchCase::IgnoreCase))
	{
#if WITH_BLUEPRINT_ASSIST
		bUseBlueprintAssist = true;
#else
		return MakeErrorResult(TEXT("Blueprint Assist formatter requested but not available (WITH_BLUEPRINT_ASSIST=0)"));
#endif
	}
	else if (FormatterType.Equals(TEXT("fallback"), ESearchCase::IgnoreCase))
	{
		bUseFallback = true;
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid formatter type: %s (must be 'blueprint_assist' or 'fallback')"), *FormatterType));
	}

	// Format the graph
	FString FormatterUsed;
	bool bSuccess = false;

#if WITH_BLUEPRINT_ASSIST
	if (bUseBlueprintAssist)
	{
		bSuccess = FormatWithBlueprintAssist(Blueprint, Graph, Error);
		if (bSuccess)
		{
			FormatterUsed = TEXT("Blueprint Assist");
		}
		else if (FormatterType.IsEmpty())
		{
			// If auto-detect and Blueprint Assist failed, try fallback
			UE_LOG(LogClaireon, Warning, TEXT("[editor.blueprint.formatGraph] Blueprint Assist failed, falling back to simple formatter: %s"), *Error);
			bUseFallback = true;
		}
		else
		{
			// User explicitly requested Blueprint Assist, don't fall back
			return MakeErrorResult(FString::Printf(TEXT("Blueprint Assist formatting failed: %s"), *Error));
		}
	}
#endif

	if (bUseFallback)
	{
		bSuccess = FormatWithFallbackFormatter(Blueprint, Graph, Error);
		if (bSuccess)
		{
			FormatterUsed = TEXT("Fallback (Topological)");
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Fallback formatting failed: %s"), *Error));
		}
	}

	if (!bSuccess)
	{
		return MakeErrorResult(TEXT("Failed to format graph (unknown error)"));
	}

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

	// Create text result with the success message
	return MakeSuccessResult(nullptr, ResultMessage);
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

	UE_LOG(LogClaireon, Display, TEXT("[editor.blueprint.formatGraph] Formatted graph '%s' using Blueprint Assist"), *Graph->GetName());
	return true;
}
#endif

bool ClaireonTool_FormatBlueprintGraph::FormatWithFallbackFormatter(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutError)
{
	if (!Blueprint || !Graph)
	{
		OutError = TEXT("Invalid Blueprint or Graph");
		return false;
	}

	if (Graph->Nodes.Num() == 0)
	{
		OutError = TEXT("Graph has no nodes to format");
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("FormatBlueprintGraphFallback", "Format Blueprint Graph (Fallback)"));
	Blueprint->Modify();
	Graph->Modify();

	// Arrange nodes topologically
	ArrangeNodesTopologically(Graph);

	UE_LOG(LogClaireon, Display, TEXT("[editor.blueprint.formatGraph] Formatted graph '%s' using fallback formatter"), *Graph->GetName());
	return true;
}

void ClaireonTool_FormatBlueprintGraph::ArrangeNodesTopologically(UEdGraph* Graph)
{
	// Calculate depths for all nodes
	TMap<UEdGraphNode*, int32> NodeDepths = CalculateNodeDepths(Graph);

	// Group nodes by depth
	TMap<int32, TArray<UEdGraphNode*>> NodesByDepth;
	for (const auto& Pair : NodeDepths)
	{
		NodesByDepth.FindOrAdd(Pair.Value).Add(Pair.Key);
	}

	// Layout nodes column by column
	for (const auto& DepthPair : NodesByDepth)
	{
		int32 Depth = DepthPair.Key;
		const TArray<UEdGraphNode*>& NodesAtDepth = DepthPair.Value;

		float X = Depth * NODE_HORIZONTAL_SPACING;
		float Y = 0.0f;

		for (UEdGraphNode* Node : NodesAtDepth)
		{
			if (Node)
			{
				Node->NodePosX = static_cast<int32>(X);
				Node->NodePosY = static_cast<int32>(Y);
				Y += NODE_HEIGHT_ESTIMATE + NODE_VERTICAL_SPACING;
			}
		}
	}
}

TMap<UEdGraphNode*, int32> ClaireonTool_FormatBlueprintGraph::CalculateNodeDepths(UEdGraph* Graph)
{
	TMap<UEdGraphNode*, int32> NodeDepths;

	// Find root nodes
	TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Graph);

	// If no root nodes found, treat all nodes without exec inputs as roots
	if (RootNodes.Num() == 0)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !ClaireonBlueprintHelpers::HasExecInputPins(Node))
			{
				RootNodes.Add(Node);
			}
		}
	}

	// If still no root nodes, just use all nodes at depth 0
	if (RootNodes.Num() == 0)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				NodeDepths.Add(Node, 0);
			}
		}
		return NodeDepths;
	}

	// Perform BFS from each root node
	for (UEdGraphNode* RootNode : RootNodes)
	{
		BreadthFirstTraversal(RootNode, NodeDepths);
	}

	// Handle any remaining nodes not reached by BFS
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !NodeDepths.Contains(Node))
		{
			NodeDepths.Add(Node, 0);
		}
	}

	return NodeDepths;
}

void ClaireonTool_FormatBlueprintGraph::BreadthFirstTraversal(UEdGraphNode* RootNode, TMap<UEdGraphNode*, int32>& NodeDepths)
{
	if (!RootNode)
	{
		return;
	}

	TQueue<TPair<UEdGraphNode*, int32>> Queue;
	Queue.Enqueue(TPair<UEdGraphNode*, int32>(RootNode, 0));

	while (!Queue.IsEmpty())
	{
		TPair<UEdGraphNode*, int32> Current;
		Queue.Dequeue(Current);

		UEdGraphNode* CurrentNode = Current.Key;
		int32 CurrentDepth = Current.Value;

		// Skip if already processed with a shorter depth
		if (NodeDepths.Contains(CurrentNode) && NodeDepths[CurrentNode] <= CurrentDepth)
		{
			continue;
		}

		NodeDepths.Add(CurrentNode, CurrentDepth);

		// Follow execution pins to connected nodes
		TArray<UEdGraphPin*> ExecOutputPins = ClaireonBlueprintHelpers::GetExecPins(CurrentNode, false, true);
		for (UEdGraphPin* ExecPin : ExecOutputPins)
		{
			if (!ExecPin)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					UEdGraphNode* NextNode = LinkedPin->GetOwningNode();
					Queue.Enqueue(TPair<UEdGraphNode*, int32>(NextNode, CurrentDepth + 1));
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
