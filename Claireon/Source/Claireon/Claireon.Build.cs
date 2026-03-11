// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

using UnrealBuildTool;
using System.IO;

public class Claireon : ModuleRules
{
	public Claireon(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Python3",           // CPython C API headers (Python.h, methodobject.h) for bridge
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"AssetRegistry",
			"InputCore",
			"PythonScriptPlugin",
			"WorkspaceMenuStructure",

			// Blueprint editing tools dependencies
			"BlueprintGraph",    // UK2Node, UEdGraphSchema_K2
			"Kismet",            // FBlueprintEditor, clipboard utilities
			"KismetWidgets",     // Blueprint editor widgets
			"GraphEditor",       // SGraphEditor, graph panel access
			"UMG",               // UWidgetBlueprint
			"UMGEditor",         // Widget editor support
			"MovieScene",        // UMovieScene, FMovieScenePossessable, channels
			"MovieSceneTracks",  // UMovieSceneFloatTrack, UMovieSceneColorTrack, UMovieSceneBoolTrack

			// PIE actor query and init state dependencies
			"ModularGameplay",   // UGameFrameworkComponentManager, IGameFrameworkInitStateInterface
			"GameplayTags",      // FGameplayTag for init state tags

			// Combat testing tools dependencies
			"GameplayAbilities", // UAbilitySystemComponent, UGameplayAbility, FGameplayAbilitySpec
			"AIModule",          // AAIController, UBlackboardComponent
			"BehaviorTreeEditor", // UBehaviorTreeGraph, UBehaviorTreeGraphNode (BT editing)
			"AIGraph",           // UAIGraphNode::AddSubNode/RemoveSubNode (decorator/service editing)

			// State Tree editing tools dependencies
			"StateTreeModule",       // UStateTree, FStateTreeStateHandle, node base classes
			"StateTreeEditorModule", // UStateTreeEditorData, UStateTreeState, FStateTreeCompiler

			// Blueprint editor library (RemoveUnusedNodes, RemoveUnusedVariables)
			"BlueprintEditorLibrary",

			// Animation Blueprint graph support (UAnimationGraph for list_graphs)
			"AnimGraph",

			// Asset management tools dependencies
			"AssetTools",        // IAssetTools, FAssetToolsModule (redirector fixup)

			// Test/parsing tools dependencies
			"XmlParser",         // FXmlFile for JUnit XML parsing

			// Trace analysis tools dependencies
			"TraceLog",          // UE::Trace::IInDataStream, FFileDataStream
			"TraceAnalysis",     // UE::Trace::IAnalyzer, FAnalysisContext
			"TraceServices",     // IAnalysisSession, ITimingProfilerProvider, IFrameProvider, IThreadProvider

			// REPL widget dependencies
			"HTTP",              // FHttpModule for REPL outbound API calls
			"Settings",          // ISettingsModule for settings gear deep-link
			"DeveloperSettings", // UDeveloperSettings base class for UClaireonSettings
			"ContentBrowser",    // Rich text asset path navigation
			"ApplicationCore",   // FPlatformApplicationMisc::ClipboardCopy

			// Niagara tools dependencies
			"Niagara",           // UNiagaraSystem, UNiagaraEmitter, UNiagaraRendererProperties
			"NiagaraCore",       // FNiagaraTypeDefinition, core Niagara type system

			// PCG Graph tools dependencies
			"PCG",               // UPCGGraph, UPCGNode, UPCGPin, UPCGEdge, UPCGSettings

			// Automation framework dependencies
			"AutomationController", // IAutomationControllerModule, IAutomationControllerManager (test runner)

		});

		// Untested dependencies (Claireon REPL unit tests) — optional
		if (Target.ProjectFile != null)
		{
			string UntestedPath = Path.Combine(Target.ProjectFile.Directory.FullName,
				"Plugins", "Untested", "Untested.uplugin");
			if (File.Exists(UntestedPath))
			{
				PrivateDependencyModuleNames.Add("Untested");
				PublicDefinitions.Add("WITH_UNTESTED=1");
			}
			else
			{
				PublicDefinitions.Add("WITH_UNTESTED=0");
			}
		}
		else
		{
			PublicDefinitions.Add("WITH_UNTESTED=0");
		}

		// Live Coding is Windows-only (matches PLATFORM_WINDOWS guard in ClaireonTool_LiveCodingReload.cpp)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// Conditional BlueprintAssist dependency
		if (Target.bBuildEditor)
		{
			string BAPluginPath = Path.Combine(Target.ProjectFile.Directory.FullName,
				"Plugins", "BlueprintAssist", "BlueprintAssist.uplugin");

			if (File.Exists(BAPluginPath))
			{
				PrivateDependencyModuleNames.Add("BlueprintAssist");
				PublicDefinitions.Add("WITH_BLUEPRINT_ASSIST=1");
			}
			else
			{
				PublicDefinitions.Add("WITH_BLUEPRINT_ASSIST=0");
			}
		}

		// Conditional LyraGame dependency (enables init state checking in PIE tools)
		if (Target.ProjectFile != null)
		{
			string LyraPath = Path.Combine(Target.ProjectFile.Directory.FullName, "Source", "LyraGame");
			if (Directory.Exists(LyraPath))
			{
				PrivateDependencyModuleNames.Add("LyraGame");
				PublicDefinitions.Add("WITH_LYRA_GAME=1");
			}
			else
			{
				PublicDefinitions.Add("WITH_LYRA_GAME=0");
			}
		}
		else
		{
			PublicDefinitions.Add("WITH_LYRA_GAME=0");
		}
	}
}
