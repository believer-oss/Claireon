// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test PCG authoring contracts with transient graphs and scoped in-memory packages.

#if WITH_UNTESTED

#include "Untest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Elements/PCGUserParameterGet.h"
#include "Elements/PCGSelfPruning.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "MeshSelectors/PCGMeshSelectorWeighted.h"
#include "PCGGraph.h"
#include "PCGSettings.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSubgraph.h"
#include "StructUtils/PropertyBag.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/TriggerVolume.h"
#include "Engine/World.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "Tools/ClaireonPCGEditorSync.h"
#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/ClaireonPCGGraphTool_AddUserParameter.h"
#include "Tools/ClaireonPCGGraphTool_Create.h"
#include "Tools/ClaireonPCGGraphTool_InspectData.h"
#include "Tools/ClaireonPCGGraphTool_Generate.h"
#include "Tools/ClaireonPCGGraphTool_ApplySpec.h"
#include "Tools/ClaireonPCGGraphTool_Connect.h"
#include "Tools/ClaireonPCGGraphTool_ListNodeTypes.h"
#include "Tools/ClaireonPCGGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonPCGGraphTool_SetSubgraph.h"
#include "Tools/ClaireonPCGGraphTool_SetSubgraphOverride.h"
#include "Tools/ClaireonTool_LevelBuildBrush.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Misc/EngineVersionComparison.h"
#include "PCGComponent.h"
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Data/PCGPointArrayData.h"
#endif
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace ClaireonPCGToolGapTests_anon
{
	/** A transient PCG graph and registered edit session. */
	struct FPCGGapFixture
	{
		TStrongObjectPtr<UPCGGraph> Graph;
		FString SessionId;

		explicit FPCGGapFixture(const TCHAR* UniqueName)
		{
			Graph.Reset(NewObject<UPCGGraph>(GetTransientPackage(), NAME_None, RF_Transient));

			// Synthetic paths exercise session lookup without disk assets.
			const FString AssetPath = FString::Printf(TEXT("/Game/__ClaireonTransient/%s.%s"), UniqueName, UniqueName);
			ClaireonPCGGraphEditToolBase::EnsureDelegateRegistered();
			const FMCPOpenSessionResult Opened = FClaireonSessionManager::Get().OpenSession(
				AssetPath, ClaireonPCGGraphEditToolBase::PCGSessionToolName);
			SessionId = Opened.SessionId;

			FPCGGraphEditToolData Data;
			Data.PCGGraph = Graph.Get();
			Data.LastOperationStatus = TEXT("test fixture");
			ClaireonPCGGraphEditToolBase::ToolData.Add(SessionId, MoveTemp(Data));
		}

		~FPCGGapFixture()
		{
			if (!SessionId.IsEmpty())
			{
				ClaireonPCGGraphEditToolBase::ToolData.Remove(SessionId);
				FClaireonSessionManager::Get().CloseSession(SessionId);
			}
			ClaireonPCGEditorSync::CancelPendingReconstruct(Graph.Get());
			Graph.Reset();
		}

		bool IsUsable() const { return Graph.IsValid() && !SessionId.IsEmpty(); }

		TSharedPtr<FJsonObject> Args() const
		{
			TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetStringField(TEXT("session_id"), SessionId);
			return A;
		}

		/** Add a node of the given settings class and return its index as the tools address it. */
		FString AddNode(UClass* SettingsClass, UPCGNode*& OutNode)
		{
			OutNode = nullptr;
			if (!Graph.IsValid() || !IsValid(SettingsClass))
			{
				return FString();
			}
			UPCGSettings* DefaultSettings = nullptr;
			UPCGNode* Node = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
			OutNode = Node;
			// Use indexes because fresh titles are unset and class names are ambiguous.
			return FString::FromInt(Graph->GetNodes().IndexOfByKey(Node));
		}
	};

	/** Run pcg_set_node_property against the fixture and hand back the raw result. */
	IClaireonTool::FToolResult SetProp(
		const FPCGGapFixture& Fixture, const FString& Node, const FString& Path, const FString& Value)
	{
		ClaireonPCGGraphTool_SetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = Fixture.Args();
		Args->SetStringField(TEXT("node"), Node);
		Args->SetStringField(TEXT("property_name"), Path);
		Args->SetStringField(TEXT("value"), Value);
		Args->SetBoolField(TEXT("suppress_output"), true);
		return Tool.Execute(Args);
	}

	/** An unsaved graph package with scoped asset-registry cleanup for path-based tools. */
	struct FPCGGapScratchAsset
	{
		FString PackagePath;
		FString ObjectPath;
		TStrongObjectPtr<UPCGGraph> Graph;

		explicit FPCGGapScratchAsset(const TCHAR* UniqueName)
		{
			PackagePath = FString::Printf(TEXT("/Game/__ClaireonTransient/%s"), UniqueName);
			ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, UniqueName);
		}

		~FPCGGapScratchAsset()
		{
			if (UPCGGraph* Live = Graph.Get(); IsValid(Live))
			{
				UPackage* Package = Live->GetPackage();
				// Remove the registry entry with the object to avoid phantom lookups.
				FAssetRegistryModule::AssetDeleted(Live);
				Live->ClearFlags(RF_Public | RF_Standalone);
				Live->MarkAsGarbage();
				if (IsValid(Package))
				{
					Package->ClearFlags(RF_Public | RF_Standalone);
					Package->MarkAsGarbage();
				}
			}
			Graph.Reset();
		}
	};
}

// Nested and indexed property paths.

UNTEST_UNIT_OPTS(Claireon, PCGPropertyPaths, ReachesNestedStructMember, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_NestedStruct"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSelfPruningSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	const auto Result = SetProp(Fixture, NodeId, TEXT("Parameters.RadiusSimilarityFactor"), TEXT("0.75"));
	UNTEST_EXPECT_FALSE(Result.bIsError);

	UPCGSelfPruningSettings* Settings = Cast<UPCGSelfPruningSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Settings->Parameters.RadiusSimilarityFactor, 0.75f));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPropertyPaths, ReachesInstancedSubObjectArray, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_MeshEntries"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGStaticMeshSpawnerSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	UPCGStaticMeshSpawnerSettings* Settings = Cast<UPCGStaticMeshSpawnerSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UPCGMeshSelectorWeighted* Selector = Cast<UPCGMeshSelectorWeighted>(Settings->MeshSelectorParameters);
	UNTEST_ASSERT_PTR(Selector);
	UNTEST_ASSERT_TRUE(Selector->MeshEntries.IsEmpty());

	// Use weight alone to avoid a content-mesh dependency.
	const auto Result = SetProp(Fixture, NodeId,
		TEXT("MeshSelectorParameters.MeshEntries"), TEXT("((Weight=3),(Weight=7))"));
	UNTEST_EXPECT_FALSE(Result.bIsError);

	UNTEST_ASSERT_TRUE(Selector->MeshEntries.Num() == 2);
	UNTEST_EXPECT_TRUE(Selector->MeshEntries[0].Weight == 3);
	UNTEST_EXPECT_TRUE(Selector->MeshEntries[1].Weight == 7);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPropertyPaths, IndexesIntoArrayElement, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_MeshEntryIndex"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGStaticMeshSpawnerSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	UNTEST_ASSERT_FALSE(SetProp(Fixture, NodeId,
		TEXT("MeshSelectorParameters.MeshEntries"), TEXT("((Weight=1),(Weight=1))")).bIsError);

	const auto Result = SetProp(Fixture, NodeId,
		TEXT("MeshSelectorParameters.MeshEntries[1].Weight"), TEXT("42"));
	UNTEST_EXPECT_FALSE(Result.bIsError);

	UPCGStaticMeshSpawnerSettings* Settings = Cast<UPCGStaticMeshSpawnerSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UPCGMeshSelectorWeighted* Selector = Cast<UPCGMeshSelectorWeighted>(Settings->MeshSelectorParameters);
	UNTEST_ASSERT_PTR(Selector);
	UNTEST_ASSERT_TRUE(Selector->MeshEntries.Num() == 2);
	UNTEST_EXPECT_TRUE(Selector->MeshEntries[0].Weight == 1);
	UNTEST_EXPECT_TRUE(Selector->MeshEntries[1].Weight == 42);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPropertyPaths, OutOfRangeIndexIsAnError, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_BadIndex"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGStaticMeshSpawnerSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	const auto Result = SetProp(Fixture, NodeId,
		TEXT("MeshSelectorParameters.MeshEntries[4].Weight"), TEXT("1"));
	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Reject deprecated aliases.

UNTEST_UNIT_OPTS(Claireon, PCGDeprecatedAlias, SelfPruningRadiusAliasIsRefused, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_DeprecatedRadius"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSelfPruningSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	UPCGSelfPruningSettings* Settings = Cast<UPCGSelfPruningSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	const float Before = Settings->Parameters.RadiusSimilarityFactor;

	// UHT removes the _DEPRECATED suffix, allowing the dead field to resolve by its old name.
	const auto Result = SetProp(Fixture, NodeId, TEXT("RadiusSimilarityFactor"), TEXT("0.77"));
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Settings->Parameters.RadiusSimilarityFactor, Before));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGDeprecatedAlias, SubgraphAliasIsRefused, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_DeprecatedSubgraph"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSubgraphSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	const auto Result = SetProp(Fixture, NodeId, TEXT("Subgraph"), TEXT("/Game/Nope.Nope"));
	UNTEST_EXPECT_TRUE(Result.bIsError);

	UPCGSubgraphSettings* Settings = Cast<UPCGSubgraphSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	// GetSubgraphInterface returns the always-present wrapper; inspect GetSubgraph for the referenced graph.
	UNTEST_EXPECT_TRUE(Settings->GetSubgraph() == nullptr);
	co_return;
}

// Node-type listing without a session.

UNTEST_UNIT_OPTS(Claireon, PCGListNodeTypes, NeedsNoSession, UNTEST_TIMEOUTMS(20000))
{
	ClaireonPCGGraphTool_ListNodeTypes Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("node_types"), Types));
	UNTEST_EXPECT_TRUE(Types->Num() > 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGListNodeTypes, FilterNarrowsTheSet, UNTEST_TIMEOUTMS(20000))
{
	ClaireonPCGGraphTool_ListNodeTypes Tool;

	TSharedPtr<FJsonObject> AllArgs = MakeShared<FJsonObject>();
	const auto All = Tool.Execute(AllArgs);
	UNTEST_ASSERT_FALSE(All.bIsError);
	const TArray<TSharedPtr<FJsonValue>>* AllTypes = nullptr;
	UNTEST_ASSERT_TRUE(All.Data->TryGetArrayField(TEXT("node_types"), AllTypes));

	TSharedPtr<FJsonObject> FilteredArgs = MakeShared<FJsonObject>();
	FilteredArgs->SetStringField(TEXT("filter"), TEXT("SurfaceSampler"));
	const auto Filtered = Tool.Execute(FilteredArgs);
	UNTEST_ASSERT_FALSE(Filtered.bIsError);
	const TArray<TSharedPtr<FJsonValue>>* FilteredTypes = nullptr;
	UNTEST_ASSERT_TRUE(Filtered.Data->TryGetArrayField(TEXT("node_types"), FilteredTypes));

	UNTEST_EXPECT_TRUE(FilteredTypes->Num() > 0);
	UNTEST_EXPECT_TRUE(FilteredTypes->Num() < AllTypes->Num());
	co_return;
}

// PCG graph creation.

UNTEST_UNIT_OPTS(Claireon, PCGCreate, MakesAGraphWithInputAndOutput, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapScratchAsset Scratch(TEXT("PCGGap_CreateOk"));

	ClaireonPCGGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UPCGGraph* Created = FindObject<UPCGGraph>(nullptr, *Scratch.ObjectPath);
	UNTEST_ASSERT_PTR(Created);
	Scratch.Graph.Reset(Created);

	UNTEST_EXPECT_TRUE(Created->GetNodes().IsEmpty());
	UNTEST_EXPECT_PTR(Created->GetInputNode());
	UNTEST_EXPECT_PTR(Created->GetOutputNode());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGCreate, RejectsPathOutsideGame, UNTEST_TIMEOUTMS(20000))
{
	ClaireonPCGGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Engine/NotAllowed/PCGGap_Rejected"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(FindObject<UPCGGraph>(nullptr, TEXT("/Engine/NotAllowed/PCGGap_Rejected.PCGGap_Rejected")) == nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGCreate, RefusesToOverwriteAnExistingAsset, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapScratchAsset Scratch(TEXT("PCGGap_CreateTwice"));

	ClaireonPCGGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), Scratch.PackagePath);

	UNTEST_ASSERT_FALSE(Tool.Execute(Args).bIsError);
	Scratch.Graph.Reset(FindObject<UPCGGraph>(nullptr, *Scratch.ObjectPath));
	UNTEST_ASSERT_TRUE(Scratch.Graph.IsValid());

	const auto Second = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(Second.bIsError);
	co_return;
}

// Set the live subgraph reference.

UNTEST_UNIT_OPTS(Claireon, PCGSetSubgraph, PointsANodeAtAGraph, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapScratchAsset Target(TEXT("PCGGap_SubTarget"));
	{
		ClaireonPCGGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), Target.PackagePath);
		UNTEST_ASSERT_FALSE(CreateTool.Execute(CreateArgs).bIsError);
		Target.Graph.Reset(FindObject<UPCGGraph>(nullptr, *Target.ObjectPath));
		UNTEST_ASSERT_TRUE(Target.Graph.IsValid());
	}

	FPCGGapFixture Fixture(TEXT("PCGGap_SubParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSubgraphSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	ClaireonPCGGraphTool_SetSubgraph Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId);
	Args->SetStringField(TEXT("subgraph_path"), Target.ObjectPath);
	Args->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Read through the sub-object the engine actually consults, not the deprecated alias.
	UPCGSubgraphSettings* Settings = Cast<UPCGSubgraphSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UNTEST_EXPECT_TRUE(Settings->GetSubgraph() == Target.Graph.Get());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSetSubgraph, RefusesANonSubgraphNode, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_SubWrongNode"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSelfPruningSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	ClaireonPCGGraphTool_SetSubgraph Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId);
	Args->SetStringField(TEXT("subgraph_path"), TEXT("/Game/Whatever.Whatever"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSetSubgraph, RefusesAnUnresolvablePath, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_SubBadPath"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSubgraphSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	ClaireonPCGGraphTool_SetSubgraph Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId);
	Args->SetStringField(TEXT("subgraph_path"), TEXT("/Game/__ClaireonTransient/NoSuchGraph.NoSuchGraph"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);

	UPCGSubgraphSettings* Settings = Cast<UPCGSubgraphSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	// GetSubgraphInterface returns the always-present wrapper; inspect GetSubgraph for the referenced graph.
	UNTEST_EXPECT_TRUE(Settings->GetSubgraph() == nullptr);
	co_return;
}

// Graph parameters.

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, DeclaresAFloatWithADefault, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamFloat"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("name"), TEXT("MinDensity"));
	Args->SetStringField(TEXT("type"), TEXT("float"));
	Args->SetStringField(TEXT("default_value"), TEXT("0.35"));
	Args->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(TEXT("MinDensity"));
	UNTEST_ASSERT_TRUE(Desc != nullptr);
	UNTEST_EXPECT_TRUE(Desc->ValueType == EPropertyBagPropertyType::Float);

	const TValueOrError<float, EPropertyBagResult> Value = Bag->GetValueFloat(TEXT("MinDensity"));
	UNTEST_ASSERT_TRUE(Value.HasValue());
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Value.GetValue(), 0.35f));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, RedeclaringSameTypeUpdatesTheDefault, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamIdempotent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;

	TSharedPtr<FJsonObject> First = Fixture.Args();
	First->SetStringField(TEXT("name"), TEXT("Spacing"));
	First->SetStringField(TEXT("type"), TEXT("float"));
	First->SetStringField(TEXT("default_value"), TEXT("1.0"));
	First->SetBoolField(TEXT("suppress_output"), true);
	UNTEST_ASSERT_FALSE(Tool.Execute(First).bIsError);

	TSharedPtr<FJsonObject> Second = Fixture.Args();
	Second->SetStringField(TEXT("name"), TEXT("Spacing"));
	Second->SetStringField(TEXT("type"), TEXT("float"));
	Second->SetStringField(TEXT("default_value"), TEXT("2.5"));
	Second->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Second);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	const TValueOrError<float, EPropertyBagResult> Value = Bag->GetValueFloat(TEXT("Spacing"));
	UNTEST_ASSERT_TRUE(Value.HasValue());
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Value.GetValue(), 2.5f));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, InvalidDefaultLeavesTheGraphUnchanged, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	// Cancel removes undo records without restoring objects; failed defaults must be validated before mutation.
	// Use a struct default because float import can coerce invalid text instead of failing.
	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamBadDefault"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("name"), TEXT("BadDefault"));
	Args->SetStringField(TEXT("type"), TEXT("struct"));
	Args->SetStringField(TEXT("type_object"), TEXT("/Script/CoreUObject.Vector"));
	Args->SetStringField(TEXT("default_value"), TEXT("garbage-that-is-not-a-vector"));
	Args->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("was not declared")));

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	UNTEST_EXPECT_TRUE(Bag->FindPropertyDescByName(TEXT("BadDefault")) == nullptr);

	TSharedPtr<FJsonObject> Retry = Fixture.Args();
	Retry->SetStringField(TEXT("name"), TEXT("BadDefault"));
	Retry->SetStringField(TEXT("type"), TEXT("struct"));
	Retry->SetStringField(TEXT("type_object"), TEXT("/Script/CoreUObject.Vector"));
	Retry->SetStringField(TEXT("default_value"), TEXT("(X=1,Y=2,Z=3)"));
	Retry->SetBoolField(TEXT("suppress_output"), true);
	UNTEST_ASSERT_FALSE(Tool.Execute(Retry).bIsError);
	UNTEST_EXPECT_TRUE(Bag->FindPropertyDescByName(TEXT("BadDefault")) != nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, RefusesToRetypeAnExistingParameter, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamRetype"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;

	TSharedPtr<FJsonObject> First = Fixture.Args();
	First->SetStringField(TEXT("name"), TEXT("Density"));
	First->SetStringField(TEXT("type"), TEXT("float"));
	First->SetBoolField(TEXT("suppress_output"), true);
	UNTEST_ASSERT_FALSE(Tool.Execute(First).bIsError);

	// Retyping can invalidate parent overrides.
	TSharedPtr<FJsonObject> Second = Fixture.Args();
	Second->SetStringField(TEXT("name"), TEXT("Density"));
	Second->SetStringField(TEXT("type"), TEXT("bool"));
	Second->SetBoolField(TEXT("suppress_output"), true);
	UNTEST_EXPECT_TRUE(Tool.Execute(Second).bIsError);

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(TEXT("Density"));
	UNTEST_ASSERT_TRUE(Desc != nullptr);
	UNTEST_EXPECT_TRUE(Desc->ValueType == EPropertyBagPropertyType::Float);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, RejectsAnUnknownType, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamBadType"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("name"), TEXT("Nope"));
	Args->SetStringField(TEXT("type"), TEXT("quaternion"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	UNTEST_EXPECT_TRUE(Bag->FindPropertyDescByName(TEXT("Nope")) == nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, ObjectBackedTypeNeedsATypeObject, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamNoTypeObject"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("name"), TEXT("Mesh"));
	Args->SetStringField(TEXT("type"), TEXT("softobject"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Landscape import keys.

UNTEST_UNIT_OPTS(Claireon, LandscapeImportData, KeysPayloadOnTheDefaultEditLayerGuid, UNTEST_TIMEOUTMS(20000))
{
	// Import looks up the base layer with FGuid(); both import maps must use that key.
	TMap<FGuid, TArray<uint16>> HeightData;
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerInfos;
	ClaireonLandscapeHelpers::BuildFlatLandscapeImportData(8, HeightData, LayerInfos);

	UNTEST_ASSERT_TRUE(HeightData.Contains(FGuid()));
	UNTEST_ASSERT_TRUE(LayerInfos.Contains(FGuid()));

	UNTEST_EXPECT_TRUE(HeightData.Num() == LayerInfos.Num());
	UNTEST_EXPECT_TRUE(HeightData.Num() == 1);

	const TArray<uint16>& Samples = HeightData[FGuid()];
	UNTEST_ASSERT_TRUE(Samples.Num() == 8 * 8);
	UNTEST_EXPECT_TRUE(Samples[0] == 32768);
	UNTEST_EXPECT_TRUE(Samples.Last() == 32768);
	co_return;
}

// Volume brush construction.

namespace ClaireonPCGToolGapTests_anon
{
	/** A scoped TriggerVolume in the editor world. */
	struct FScratchVolume
	{
		UWorld* World = nullptr;
		ATriggerVolume* Volume = nullptr;

		FScratchVolume()
		{
			World = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (IsValid(World))
			{
				FActorSpawnParameters Params;
				Params.Name = MakeUniqueObjectName(World->GetCurrentLevel(), ATriggerVolume::StaticClass(),
					TEXT("ClaireonGapTest_Volume"));
				Volume = World->SpawnActor<ATriggerVolume>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			}
		}

		~FScratchVolume()
		{
			if (IsValid(Volume))
			{
				Volume->Destroy();
			}
		}

		bool IsUsable() const { return IsValid(World) && IsValid(Volume); }

		FVector Extent() const
		{
			FVector Origin = FVector::ZeroVector;
			FVector BoxExtent = FVector::ZeroVector;
			if (IsValid(Volume))
			{
				Volume->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, BoxExtent);
			}
			return BoxExtent;
		}
	};

	TSharedPtr<FJsonObject> BuildBrushArgs(const AActor* Actor, double HalfX, double HalfY, double HalfZ)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		TSharedPtr<FJsonObject> Extent = MakeShared<FJsonObject>();
		Extent->SetNumberField(TEXT("x"), HalfX);
		Extent->SetNumberField(TEXT("y"), HalfY);
		Extent->SetNumberField(TEXT("z"), HalfZ);
		Args->SetObjectField(TEXT("extent"), Extent);
		return Args;
	}
}

UNTEST_UNIT_OPTS(Claireon, LevelBuildBrush, GivesASpawnedVolumeRealExtent, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FScratchVolume Scratch;
	UNTEST_ASSERT_TRUE(Scratch.IsUsable());

	// A spawned volume has no brush and therefore no useful bounds.
	UNTEST_ASSERT_TRUE(Scratch.Extent().IsNearlyZero());

	ClaireonTool_LevelBuildBrush Tool;
	const auto Result = Tool.Execute(BuildBrushArgs(Scratch.Volume, 250.0, 400.0, 150.0));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Convert half-extents to UCubeBuilder full dimensions and verify the round trip.
	const FVector Built = Scratch.Extent();
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Built.X, 250.0, 1.0));
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Built.Y, 400.0, 1.0));
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Built.Z, 150.0, 1.0));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LevelBuildBrush, RefusesANonVolumeActor, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	UWorld* World = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
	UNTEST_ASSERT_PTR(World);

	AActor* Plain = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	UNTEST_ASSERT_PTR(Plain);

	ClaireonTool_LevelBuildBrush Tool;
	const auto Result = Tool.Execute(BuildBrushArgs(Plain, 100.0, 100.0, 100.0));
	Plain->Destroy();

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LevelBuildBrush, RejectsANonPositiveExtent, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FScratchVolume Scratch;
	UNTEST_ASSERT_TRUE(Scratch.IsUsable());

	ClaireonTool_LevelBuildBrush Tool;
	const auto Result = Tool.Execute(BuildBrushArgs(Scratch.Volume, 100.0, 0.0, 100.0));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Scratch.Extent().IsNearlyZero());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LevelBuildBrush, RequiresAnActorSelector, UNTEST_TIMEOUTMS(20000))
{
	ClaireonTool_LevelBuildBrush Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Persistent landscape layers.

namespace ClaireonPCGToolGapTests_anon
{
	/**
	 * Assign a valid landscape GUID and create its info so registration and destruction remain symmetric.
	 * Otherwise the subsystem retains an actor whose null info can fault a later world tick.
	 */
	struct FScratchLandscape
	{
		UWorld* World = nullptr;
		ALandscape* Landscape = nullptr;

		FScratchLandscape()
		{
			World = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (IsValid(World))
			{
				Landscape = World->SpawnActor<ALandscape>();
				if (IsValid(Landscape))
				{
					Landscape->SetLandscapeGuid(FGuid::NewGuid());
					Landscape->CreateLandscapeInfo();
				}
			}
		}

		~FScratchLandscape()
		{
			if (IsValid(Landscape))
			{
				Landscape->Destroy();
			}
		}

		bool IsUsable() const { return IsValid(World) && IsValid(Landscape); }
	};
}

UNTEST_UNIT_OPTS(Claireon, LandscapeAddLayer, RegistersLayerInSerializedEditorSettings, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	// Check saveable layer ownership and proxy TargetLayers; ULandscapeInfo::Layers is runtime-only.
	FScratchLandscape Scratch;
	UNTEST_ASSERT_TRUE(Scratch.IsUsable());

	ALandscapeProxy* Proxy = Scratch.Landscape;
	UNTEST_ASSERT_TRUE(Proxy->GetTargetLayers().Num() == 0);

	ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>(
		Proxy, TEXT("ClaireonGapTest_Layer"), RF_Public | RF_Standalone | RF_Transactional);
	UNTEST_ASSERT_PTR(LayerInfo);

	UNTEST_EXPECT_TRUE(LayerInfo->GetOutermost() != GetTransientPackage());
	Proxy->AddTargetLayer(TEXT("ClaireonGapTest_Layer"), FLandscapeTargetLayerSettings(LayerInfo));

	UNTEST_ASSERT_TRUE(Proxy->GetTargetLayers().Num() == 1);
	UNTEST_EXPECT_TRUE(Proxy->HasTargetLayer(LayerInfo));
	UNTEST_EXPECT_TRUE(LayerInfo->GetOutermost() == Proxy->GetOutermost());

	co_return;
}

// Verify GUID and landscape info before teardown to catch invalid subsystem registration at its source.
UNTEST_UNIT_OPTS(Claireon, LandscapeAddLayer, SpawnsALandscapeTheSubsystemCanTick, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FScratchLandscape Scratch;
	UNTEST_ASSERT_TRUE(Scratch.IsUsable());

	// A valid GUID permits subsystem unregistration on destruction.
	UNTEST_ASSERT_TRUE(Scratch.Landscape->GetLandscapeGuid().IsValid());

	// The subsystem tick requires valid landscape info.
	UNTEST_EXPECT_PTR(Scratch.Landscape->GetLandscapeInfo());

	co_return;
}

// An override requires both the value and its property GUID flag to survive parent updates.
// Use an unflagged value as the propagation control.

namespace ClaireonPCGSubgraphOverrideTests_anon
{
	using namespace ClaireonPCGToolGapTests_anon;

	/** A subgraph instance referencing a scratch graph with one float parameter. */
	struct FSubgraphOverrideFixture
	{
		FPCGGapScratchAsset Target;
		FPCGGapFixture Parent;
		UPCGNode* Node = nullptr;
		FString NodeId;

		static constexpr const TCHAR* ParamName = TEXT("MinDensity");
		static constexpr float ParentDefault = 0.35f;

		FSubgraphOverrideFixture(const TCHAR* TargetName, const TCHAR* ParentName)
			: Target(TargetName)
			, Parent(ParentName)
		{
			ClaireonPCGGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
			CreateArgs->SetStringField(TEXT("asset_path"), Target.PackagePath);
			if (CreateTool.Execute(CreateArgs).bIsError)
			{
				return;
			}
			Target.Graph.Reset(FindObject<UPCGGraph>(nullptr, *Target.ObjectPath));
			if (!Target.Graph.IsValid())
			{
				return;
			}

			{
				const FPropertyBagPropertyDesc Desc(
					FName(ParamName), EPropertyBagPropertyType::Float, nullptr);
				Target.Graph->AddUserParameters({Desc});
				Target.Graph->UpdateUserParametersStruct(
					[](FInstancedPropertyBag& Bag)
					{
						Bag.SetValueFloat(FName(ParamName), ParentDefault);
					});
			}

			NodeId = Parent.AddNode(UPCGSubgraphSettings::StaticClass(), Node);
			if (!IsValid(Node))
			{
				return;
			}

			ClaireonPCGGraphTool_SetSubgraph SetTool;
			TSharedPtr<FJsonObject> SetArgs = Parent.Args();
			SetArgs->SetStringField(TEXT("node"), NodeId);
			SetArgs->SetStringField(TEXT("subgraph_path"), Target.ObjectPath);
			SetArgs->SetBoolField(TEXT("suppress_output"), true);
			if (SetTool.Execute(SetArgs).bIsError)
			{
				Node = nullptr;
			}
		}

		bool IsUsable() const
		{
			return Target.Graph.IsValid() && Parent.IsUsable() && Node != nullptr && GetInstance() != nullptr;
		}

		UPCGGraphInstance* GetInstance() const
		{
			UPCGSubgraphSettings* Settings = IsValid(Node) ? Cast<UPCGSubgraphSettings>(Node->GetSettings()) : nullptr;
			return IsValid(Settings) ? Cast<UPCGGraphInstance>(Settings->GetSubgraphInterface()) : nullptr;
		}

		/** An unreadable parameter is unset rather than represented by a sentinel float. */
		TOptional<float> ReadInstanceValue() const
		{
			const UPCGGraphInstance* Instance = GetInstance();
			const FInstancedPropertyBag* Bag = IsValid(Instance) ? Instance->GetUserParametersStruct() : nullptr;
			if (!Bag)
			{
				return TOptional<float>();
			}
			auto Value = Bag->GetValueFloat(FName(ParamName));
			return Value.IsValid() ? TOptional<float>(Value.GetValue()) : TOptional<float>();
		}

		/** True when the instance's value reads back and matches Expected. */
		bool InstanceValueIs(float Expected) const
		{
			const TOptional<float> Value = ReadInstanceValue();
			return Value.IsSet() && FMath::IsNearlyEqual(Value.GetValue(), Expected);
		}

		bool IsFlaggedOverridden() const
		{
			const UPCGGraphInstance* Instance = GetInstance();
			const FInstancedPropertyBag* Bag = IsValid(Instance) ? Instance->GetUserParametersStruct() : nullptr;
			const FPropertyBagPropertyDesc* Desc = Bag ? Bag->FindPropertyDescByName(FName(ParamName)) : nullptr;
			return Desc && Desc->CachedProperty && Instance->IsPropertyOverridden(Desc->CachedProperty);
		}

		/** Change the referenced graph parameter to exercise ValueModifiedByParent propagation. */
		void ChangeParentValue(float NewValue)
		{
			if (Target.Graph.IsValid())
			{
				Target.Graph->SetGraphParameter<float>(FName(ParamName), NewValue);
			}
		}

		IClaireonTool::FToolResult Override(const FString& Value)
		{
			ClaireonPCGGraphTool_SetSubgraphOverride Tool;
			TSharedPtr<FJsonObject> Args = Parent.Args();
			Args->SetStringField(TEXT("node"), NodeId);
			Args->SetStringField(TEXT("parameter"), ParamName);
			Args->SetStringField(TEXT("value"), Value);
			Args->SetBoolField(TEXT("suppress_output"), true);
			return Tool.Execute(Args);
		}
	};
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, SetsAPerInstanceValue, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvTarget"), TEXT("PCGGap_OvParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	const auto Result = Fixture.Override(TEXT("0.9"));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_EXPECT_TRUE(Fixture.IsFlaggedOverridden());
	UNTEST_EXPECT_TRUE(Fixture.InstanceValueIs(0.9f));

	auto ParentValue = Fixture.Target.Graph->GetUserParametersStruct()->GetValueFloat(
		FName(FSubgraphOverrideFixture::ParamName));
	UNTEST_ASSERT_TRUE(ParentValue.IsValid());
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(ParentValue.GetValue(), FSubgraphOverrideFixture::ParentDefault));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, SurvivesAParentValueChange, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvKeepTarget"), TEXT("PCGGap_OvKeepParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UNTEST_ASSERT_FALSE(Fixture.Override(TEXT("0.9")).bIsError);

	Fixture.ChangeParentValue(0.1f);

	UNTEST_EXPECT_TRUE(Fixture.InstanceValueIs(0.9f));
	UNTEST_EXPECT_TRUE(Fixture.IsFlaggedOverridden());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, IsClobberedWithoutTheOverrideFlag, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvCtrlTarget"), TEXT("PCGGap_OvCtrlParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	// Write the same value without an override GUID as the negative control.
	UPCGGraphInstance* Instance = Fixture.GetInstance();
	UNTEST_ASSERT_PTR(Instance);
	FInstancedPropertyBag* Bag = Instance->GetMutableUserParametersStruct_Unsafe();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	UNTEST_ASSERT_TRUE(
		Bag->SetValueFloat(FName(FSubgraphOverrideFixture::ParamName), 0.9f) == EPropertyBagResult::Success);
	UNTEST_ASSERT_FALSE(Fixture.IsFlaggedOverridden());

	Fixture.ChangeParentValue(0.1f);

	UNTEST_EXPECT_TRUE(Fixture.InstanceValueIs(0.1f));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, ClearingRestoresTheReferencedDefault, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvClearTarget"), TEXT("PCGGap_OvClearParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UNTEST_ASSERT_FALSE(Fixture.Override(TEXT("0.9")).bIsError);
	UNTEST_ASSERT_TRUE(Fixture.IsFlaggedOverridden());

	ClaireonPCGGraphTool_SetSubgraphOverride Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Parent.Args();
	Args->SetStringField(TEXT("node"), Fixture.NodeId);
	Args->SetStringField(TEXT("parameter"), FSubgraphOverrideFixture::ParamName);
	Args->SetBoolField(TEXT("override"), false);
	Args->SetBoolField(TEXT("suppress_output"), true);
	UNTEST_ASSERT_FALSE(Tool.Execute(Args).bIsError);

	UNTEST_EXPECT_FALSE(Fixture.IsFlaggedOverridden());
	UNTEST_EXPECT_TRUE(Fixture.InstanceValueIs(FSubgraphOverrideFixture::ParentDefault));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, RefusesAValueWhenClearing, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvBothTarget"), TEXT("PCGGap_OvBothParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_SetSubgraphOverride Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Parent.Args();
	Args->SetStringField(TEXT("node"), Fixture.NodeId);
	Args->SetStringField(TEXT("parameter"), FSubgraphOverrideFixture::ParamName);
	Args->SetStringField(TEXT("value"), TEXT("0.9"));
	Args->SetBoolField(TEXT("override"), false);
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_FALSE(Fixture.IsFlaggedOverridden());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, RefusesAnUndeclaredParameter, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvNoParamTarget"), TEXT("PCGGap_OvNoParamParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_SetSubgraphOverride Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Parent.Args();
	Args->SetStringField(TEXT("node"), Fixture.NodeId);
	Args->SetStringField(TEXT("parameter"), TEXT("NoSuchParameter"));
	Args->SetStringField(TEXT("value"), TEXT("1.0"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, RefusesANodeReferencingNoSubgraph, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_OvNoSubgraph"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSubgraphSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	// The wrapper always exists; check its referenced graph.
	ClaireonPCGGraphTool_SetSubgraphOverride Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId);
	Args->SetStringField(TEXT("parameter"), TEXT("MinDensity"));
	Args->SetStringField(TEXT("value"), TEXT("1.0"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, RefusesANonSubgraphNode, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_OvWrongNode"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGSelfPruningSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	ClaireonPCGGraphTool_SetSubgraphOverride Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId);
	Args->SetStringField(TEXT("parameter"), TEXT("MinDensity"));
	Args->SetStringField(TEXT("value"), TEXT("1.0"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

// Failed overrides must restore value and override flag; Cancel alone does neither.
// Use an integer because invalid float text can import as zero without failing.

namespace ClaireonPCGSubgraphOverrideTests_anon
{
	static constexpr const TCHAR* IntParamName = TEXT("StepCount");
	static constexpr int32 IntParentDefault = 7;

	/** Add an integer after linking the subgraph so the instance receives the Added notification. */
	static void OvAddIntParameter(const FSubgraphOverrideFixture& Fixture)
	{
		const FPropertyBagPropertyDesc Desc(
			FName(IntParamName), EPropertyBagPropertyType::Int32, nullptr);
		Fixture.Target.Graph->AddUserParameters({Desc});
		Fixture.Target.Graph->UpdateUserParametersStruct(
			[](FInstancedPropertyBag& Bag)
			{
				Bag.SetValueInt32(FName(IntParamName), IntParentDefault);
			});
	}

	static TOptional<int32> OvReadInstanceInt(const FSubgraphOverrideFixture& Fixture)
	{
		const UPCGGraphInstance* Instance = Fixture.GetInstance();
		const FInstancedPropertyBag* Bag = IsValid(Instance) ? Instance->GetUserParametersStruct() : nullptr;
		if (!Bag)
		{
			return TOptional<int32>();
		}
		auto Value = Bag->GetValueInt32(FName(IntParamName));
		return Value.IsValid() ? TOptional<int32>(Value.GetValue()) : TOptional<int32>();
	}

	static bool OvIntIsFlaggedOverridden(const FSubgraphOverrideFixture& Fixture)
	{
		const UPCGGraphInstance* Instance = Fixture.GetInstance();
		const FInstancedPropertyBag* Bag = IsValid(Instance) ? Instance->GetUserParametersStruct() : nullptr;
		const FPropertyBagPropertyDesc* Desc = Bag ? Bag->FindPropertyDescByName(FName(IntParamName)) : nullptr;
		return Desc && Desc->CachedProperty && Instance->IsPropertyOverridden(Desc->CachedProperty);
	}

	static IClaireonTool::FToolResult OvOverrideInt(const FSubgraphOverrideFixture& Fixture,
	                                                const TCHAR* Value)
	{
		ClaireonPCGGraphTool_SetSubgraphOverride Tool;
		TSharedPtr<FJsonObject> Args = Fixture.Parent.Args();
		Args->SetStringField(TEXT("node"), Fixture.NodeId);
		Args->SetStringField(TEXT("parameter"), IntParamName);
		Args->SetStringField(TEXT("value"), Value);
		Args->SetBoolField(TEXT("suppress_output"), true);
		return Tool.Execute(Args);
	}
}

UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, UnparseableValueLeavesNoOverrideFlag,
	UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvRbTarget"), TEXT("PCGGap_OvRbParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	OvAddIntParameter(Fixture);

	UNTEST_ASSERT_FALSE(OvIntIsFlaggedOverridden(Fixture));
	const TOptional<int32> Before = OvReadInstanceInt(Fixture);
	UNTEST_ASSERT_TRUE(Before.IsSet());
	UNTEST_ASSERT_EQ(Before.GetValue(), IntParentDefault);

	const auto Result = OvOverrideInt(Fixture, TEXT("not-a-number"));
	UNTEST_ASSERT_TRUE(Result.bIsError);

	UNTEST_EXPECT_FALSE(OvIntIsFlaggedOverridden(Fixture));

	const TOptional<int32> After = OvReadInstanceInt(Fixture);
	UNTEST_ASSERT_TRUE(After.IsSet());
	UNTEST_EXPECT_EQ(After.GetValue(), IntParentDefault);

	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("restored")));

	// Check the unrelated float so whole-bag restoration cannot erase other values.
	UNTEST_EXPECT_TRUE(Fixture.InstanceValueIs(FSubgraphOverrideFixture::ParentDefault));
	co_return;
}

// A valid integer override must still succeed.
UNTEST_UNIT_OPTS(Claireon, PCGSubgraphOverride, ParseableValueStillAppliesAndFlags,
	UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGSubgraphOverrideTests_anon;

	FSubgraphOverrideFixture Fixture(TEXT("PCGGap_OvOkTarget"), TEXT("PCGGap_OvOkParent"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	OvAddIntParameter(Fixture);

	const auto Result = OvOverrideInt(Fixture, TEXT("42"));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UNTEST_EXPECT_TRUE(OvIntIsFlaggedOverridden(Fixture));
	const TOptional<int32> After = OvReadInstanceInt(Fixture);
	UNTEST_ASSERT_TRUE(After.IsSet());
	UNTEST_EXPECT_EQ(After.GetValue(), 42);
	co_return;
}


// Graph-parameter binding.

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, AddReturnsThePropertyGuid, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamGuid"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGGraphTool_AddUserParameter Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("name"), TEXT("Density"));
	Args->SetStringField(TEXT("type"), TEXT("float"));
	Args->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// Return the parameter GUID because bag descriptors are unavailable through Python reflection.
	FString ReportedGuid;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetStringField(TEXT("property_guid"), ReportedGuid));
	UNTEST_ASSERT_FALSE(ReportedGuid.IsEmpty());

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(TEXT("Density"));
	UNTEST_ASSERT_TRUE(Desc != nullptr);
	UNTEST_EXPECT_TRUE(ReportedGuid == Desc->ID.ToString(EGuidFormats::Digits));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, GuidAcceptsAParameterName, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamByName"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	{
		ClaireonPCGGraphTool_AddUserParameter Tool;
		TSharedPtr<FJsonObject> Args = Fixture.Args();
		Args->SetStringField(TEXT("name"), TEXT("Density"));
		Args->SetStringField(TEXT("type"), TEXT("float"));
		Args->SetBoolField(TEXT("suppress_output"), true);
		UNTEST_ASSERT_FALSE(Tool.Execute(Args).bIsError);
	}

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGUserParameterGetSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	UNTEST_ASSERT_FALSE(SetProp(Fixture, NodeId, TEXT("PropertyGuid"), TEXT("Density")).bIsError);

	UPCGUserParameterGetSettings* Settings = Cast<UPCGUserParameterGetSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);

	const FInstancedPropertyBag* Bag = Fixture.Graph->GetUserParametersStruct();
	UNTEST_ASSERT_TRUE(Bag != nullptr);
	const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(TEXT("Density"));
	UNTEST_ASSERT_TRUE(Desc != nullptr);
	UNTEST_EXPECT_TRUE(Settings->PropertyGuid == Desc->ID);

	// Keep PropertyName aligned with the GUID because it determines the output label.
	UNTEST_EXPECT_TRUE(Settings->PropertyName == FName(TEXT("Density")));
	UNTEST_EXPECT_TRUE(Node->GetOutputPin(FName(TEXT("Density"))) != nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGUserParameters, UnknownParameterNameIsRefused, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_UserParamBadName"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGUserParameterGetSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	const auto Result = SetProp(Fixture, NodeId, TEXT("PropertyGuid"), TEXT("NoSuchParameter"));
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("NoSuchParameter")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("neither a GUID nor the name of a parameter")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("pcg_add_user_parameter")));

	UPCGUserParameterGetSettings* Settings = Cast<UPCGUserParameterGetSettings>(Node->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UNTEST_EXPECT_FALSE(Settings->PropertyGuid.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGConnect, UnboundParameterGetterIsRefused, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_ConnectUnbound"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Getter = nullptr;
	const FString GetterId = Fixture.AddNode(UPCGUserParameterGetSettings::StaticClass(), Getter);
	UNTEST_ASSERT_PTR(Getter);

	UPCGNode* Sink = nullptr;
	const FString SinkId = Fixture.AddNode(UPCGSelfPruningSettings::StaticClass(), Sink);
	UNTEST_ASSERT_PTR(Sink);

	// An unbound parameter getter must not accept a structurally plausible edge.
	UPCGUserParameterGetSettings* Settings = Cast<UPCGUserParameterGetSettings>(Getter->GetSettings());
	UNTEST_ASSERT_PTR(Settings);
	UNTEST_ASSERT_FALSE(Settings->PropertyGuid.IsValid());

	TArray<TObjectPtr<UPCGPin>> OutPins = Getter->GetOutputPins();
	UNTEST_ASSERT_TRUE(OutPins.Num() > 0);

	ClaireonPCGGraphTool_Connect Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("from_node"), GetterId);
	Args->SetStringField(TEXT("from_pin"), OutPins[0]->Properties.Label.ToString());
	Args->SetStringField(TEXT("to_node"), SinkId);
	Args->SetStringField(TEXT("to_pin"), TEXT("In"));
	Args->SetBoolField(TEXT("suppress_output"), true);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("bound to no live graph parameter")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("pcg_set_node_property")));
	UNTEST_EXPECT_FALSE(OutPins[0]->IsConnected());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGApplySpec, ResolvesGraphBoundaryNodes, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapScratchAsset Scratch(TEXT("PCGGap_SpecBoundary"));
	{
		ClaireonPCGGraphTool_Create Create;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
		UNTEST_ASSERT_FALSE(Create.Execute(CreateArgs).bIsError);
	}
	UPCGGraph* Created = FindObject<UPCGGraph>(nullptr, *Scratch.ObjectPath);
	UNTEST_ASSERT_PTR(Created);
	Scratch.Graph.Reset(Created);

	// Resolve the built-in Input node outside spec id_mappings; use an invalid pin to prove node lookup succeeded.
	TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
	Conn->SetStringField(TEXT("source_node"), TEXT("Input"));
	Conn->SetStringField(TEXT("source_pin"), TEXT("NoSuchPinOnTheInputNode"));
	Conn->SetStringField(TEXT("target_node"), TEXT("Output"));
	Conn->SetStringField(TEXT("target_pin"), TEXT("In"));

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeShared<FJsonValueObject>(Conn));

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	Spec->SetArrayField(TEXT("connections"), Connections);

	ClaireonPCGGraphTool_ApplySpec Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
	Args->SetObjectField(TEXT("spec"), Spec);

	const auto Result = Tool.Execute(Args);
	const FString Message = Result.Summary + Result.ErrorMessage;
	UNTEST_EXPECT_FALSE(Message.Contains(TEXT("source_node 'Input' not found")));
	UNTEST_EXPECT_TRUE(Message.Contains(TEXT("NoSuchPinOnTheInputNode")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGReadback, ExpandsInstancedSubObjects, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapFixture Fixture(TEXT("PCGGap_ReadbackNested"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = nullptr;
	const FString NodeId = Fixture.AddNode(UPCGStaticMeshSpawnerSettings::StaticClass(), Node);
	UNTEST_ASSERT_PTR(Node);

	UNTEST_ASSERT_FALSE(SetProp(Fixture, NodeId,
		TEXT("MeshSelectorParameters.MeshEntries"),
		TEXT("((Descriptor=(StaticMesh=\"/Engine/BasicShapes/Cube.Cube\"),Weight=3))")).bIsError);

	// Expand instanced-subobject properties so readback can verify nested configuration.
	const FString Readback = ClaireonPCGGraphHelpers::ReadNodeProperties(Node);
	UNTEST_EXPECT_TRUE(Readback.Contains(TEXT("MeshSelectorParameters.MeshEntries")));
	UNTEST_EXPECT_TRUE(Readback.Contains(TEXT("Weight=3")));

	// Keep the subobject pointer line to identify its installed class.
	UNTEST_EXPECT_TRUE(Readback.Contains(TEXT("MeshSelectorParameters:")));
	co_return;
}

// Generation and inspection.

UNTEST_UNIT_OPTS(Claireon, PCGGenerate, RefusesAGraphWithNoPlacedComponent, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	// A graph without bound components must not report successful empty generation.
	FPCGGapScratchAsset Scratch(TEXT("PCGGap_GenerateUnplaced"));
	{
		ClaireonPCGGraphTool_Create Create;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
		UNTEST_ASSERT_FALSE(Create.Execute(CreateArgs).bIsError);
	}
	UPCGGraph* Created = FindObject<UPCGGraph>(nullptr, *Scratch.ObjectPath);
	UNTEST_ASSERT_PTR(Created);
	Scratch.Graph.Reset(Created);

	ClaireonPCGGraphTool_Generate Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("nothing to")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGGenerate, RequiresATarget, UNTEST_TIMEOUTMS(20000))
{
	ClaireonPCGGraphTool_Generate Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("asset_path"))
		|| Result.ErrorMessage.Contains(TEXT("No editor world")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGInspectData, RequiresAssetPathAndNode, UNTEST_TIMEOUTMS(20000))
{
	ClaireonPCGGraphTool_InspectData Tool;

	TSharedPtr<FJsonObject> NoNode = MakeShared<FJsonObject>();
	NoNode->SetStringField(TEXT("asset_path"), TEXT("/Game/Nope"));
	const auto NoNodeResult = Tool.Execute(NoNode);
	UNTEST_ASSERT_TRUE(NoNodeResult.bIsError);
	UNTEST_EXPECT_TRUE(NoNodeResult.ErrorMessage.Contains(TEXT("asset_path and node"))
		|| NoNodeResult.ErrorMessage.Contains(TEXT("No editor world")));

	TSharedPtr<FJsonObject> Neither = MakeShared<FJsonObject>();
	const auto NeitherResult = Tool.Execute(Neither);
	UNTEST_ASSERT_TRUE(NeitherResult.bIsError);
	UNTEST_EXPECT_TRUE(NeitherResult.ErrorMessage.Contains(TEXT("asset_path and node"))
		|| NeitherResult.ErrorMessage.Contains(TEXT("No editor world")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGInspectData, NamesTheAvailablePinsWhenOneIsWrong, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	FPCGGapScratchAsset Scratch(TEXT("PCGGap_InspectBadPin"));
	{
		ClaireonPCGGraphTool_Create Create;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
		UNTEST_ASSERT_FALSE(Create.Execute(CreateArgs).bIsError);
	}
	UPCGGraph* Created = FindObject<UPCGGraph>(nullptr, *Scratch.ObjectPath);
	UNTEST_ASSERT_PTR(Created);
	Scratch.Graph.Reset(Created);

	UPCGSettings* DefaultSettings = nullptr;
	UPCGNode* Node = Created->AddNodeOfType(
		TSubclassOf<UPCGSettings>(UPCGSelfPruningSettings::StaticClass()), DefaultSettings);
	UNTEST_ASSERT_PTR(Node);

	// Unknown pins must fail and list available pins.
	ClaireonPCGGraphTool_InspectData Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), Scratch.PackagePath);
	Args->SetStringField(TEXT("node"), TEXT("0"));
	Args->SetStringField(TEXT("pin"), TEXT("NoSuchOutputPin"));
	Args->SetBoolField(TEXT("generate"), false);
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("NoSuchOutputPin")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("Available")));
	co_return;
}

// Include metadata defaults and every supported point-data representation in statistics.

namespace ClaireonPCGToolGapTests_anon
{
	/** Attribute "W" (float, default 5): point 0 explicit 9, point 1 no entry -> 5. Expect mean 7. */
	static void PCGGap_ExpectTwoPointStats(const ClaireonPCGGraphHelpers::FPointStatistics& Stats, FString& OutError)
	{
		OutError.Reset();
		if (Stats.Points != 2)
		{
			OutError = FString::Printf(TEXT("points=%d, expected 2"), Stats.Points);
			return;
		}
		const ClaireonPCGGraphHelpers::FPointStat* W = Stats.Attributes.Find(TEXT("W"));
		if (!W)
		{
			OutError = TEXT("attribute W missing from statistics");
			return;
		}
		if (W->Count != 2 || !FMath::IsNearlyEqual(W->Min, 5.0) || !FMath::IsNearlyEqual(W->Max, 9.0)
			|| !FMath::IsNearlyEqual(W->Mean(), 7.0))
		{
			OutError = FString::Printf(TEXT("W count=%d min=%g max=%g mean=%g, expected 2/5/9/7"),
				W->Count, W->Min, W->Max, W->Mean());
			return;
		}
		const ClaireonPCGGraphHelpers::FPointStat* AllDefault = Stats.Attributes.Find(TEXT("AllDefault"));
		if (!AllDefault || AllDefault->Count != 2 || !FMath::IsNearlyEqual(AllDefault->Mean(), 3.0))
		{
			OutError = TEXT("an attribute no point wrote explicitly must still report its default for every point");
			return;
		}
		if (!Stats.NonNumericAttributes.Contains(TEXT("Label")))
		{
			OutError = TEXT("the string attribute must be named as non-numeric, not dropped");
			return;
		}
		if (Stats.Density.Count != 2 || !FMath::IsNearlyEqual(Stats.Density.Mean(), 0.75)
			|| !FMath::IsNearlyEqual(Stats.PositionZ.Min, 10.0) || !FMath::IsNearlyEqual(Stats.PositionZ.Max, 30.0))
		{
			OutError = TEXT("density/position statistics do not match the two authored points");
		}
	}

	/** Declares W (default 5), AllDefault (default 3), Label (string) and returns the entry for the explicit point. */
	static int64 PCGGap_AuthorMetadata(UPCGMetadata* Metadata)
	{
		FPCGMetadataAttribute<float>* W = Metadata->CreateAttribute<float>(TEXT("W"), 5.0f, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
		Metadata->CreateAttribute<float>(TEXT("AllDefault"), 3.0f, true, false);
		Metadata->CreateAttribute<FString>(TEXT("Label"), FString(TEXT("x")), false, false);
		const int64 Entry = Metadata->AddEntry();
		if (W)
		{
			W->SetValue(Entry, 9.0f);
		}
		return Entry;
	}
}

UNTEST_UNIT_OPTS(Claireon, PCGInspectStats, PointDataCountsDefaultValuedEntries, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	UPCGPointData* PointData = NewObject<UPCGPointData>(GetTransientPackage(), NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(PointData);
	UPCGMetadata* Metadata = PointData->MutableMetadata();
	UNTEST_ASSERT_PTR(Metadata);
	const int64 ExplicitEntry = PCGGap_AuthorMetadata(Metadata);

	TArray<FPCGPoint>& Points = PointData->GetMutablePoints();
	Points.SetNum(2);
	Points[0].Density = 1.0f;
	Points[0].Transform.SetLocation(FVector(0, 0, 10));
	Points[0].MetadataEntry = ExplicitEntry;
	Points[1].Density = 0.5f;
	Points[1].Transform.SetLocation(FVector(0, 0, 30));
	Points[1].MetadataEntry = PCGInvalidEntryKey;   // no entry: reads every attribute's default

	ClaireonPCGGraphHelpers::FPointStatistics Stats;
	UNTEST_ASSERT_TRUE(ClaireonPCGGraphHelpers::AccumulatePointData(PointData, Stats));
	FString Error;
	PCGGap_ExpectTwoPointStats(Stats, Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	if (!Error.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("[PCGInspectStats] UPCGPointData: %s"), *Error);
	}

	ClaireonPCGGraphHelpers::FPointStatistics Untouched;
	UNTEST_EXPECT_FALSE(ClaireonPCGGraphHelpers::AccumulatePointData(nullptr, Untouched));
	UNTEST_EXPECT_EQ(Untouched.Points, 0);
	co_return;
}

#if !UE_VERSION_OLDER_THAN(5, 6, 0)
// Run equivalent statistics on UPCGPointArrayData, which is not derived from UPCGPointData.
UNTEST_UNIT_OPTS(Claireon, PCGInspectStats, PointArrayDataIsReadAsPointData, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	UPCGPointArrayData* ArrayData = NewObject<UPCGPointArrayData>(GetTransientPackage(), NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(ArrayData);
	UPCGMetadata* Metadata = ArrayData->MutableMetadata();
	UNTEST_ASSERT_PTR(Metadata);
	const int64 ExplicitEntry = PCGGap_AuthorMetadata(Metadata);

	ArrayData->SetNumPoints(2);
	{
		FPCGPointDensity::ValueRange Density = ArrayData->GetDensityValueRange();
		FPCGPointTransform::ValueRange Transform = ArrayData->GetTransformValueRange();
		FPCGPointMetadataEntry::ValueRange Entry = ArrayData->GetMetadataEntryValueRange();
		Density[0] = 1.0f;
		Density[1] = 0.5f;
		Transform[0].SetLocation(FVector(0, 0, 10));
		Transform[1].SetLocation(FVector(0, 0, 30));
		Entry[0] = ExplicitEntry;
		Entry[1] = PCGInvalidEntryKey;
	}

	UNTEST_ASSERT_TRUE(Cast<UPCGPointData>(ArrayData) == nullptr);

	ClaireonPCGGraphHelpers::FPointStatistics Stats;
	UNTEST_ASSERT_TRUE(ClaireonPCGGraphHelpers::AccumulatePointData(ArrayData, Stats));
	FString Error;
	PCGGap_ExpectTwoPointStats(Stats, Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	if (!Error.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("[PCGInspectStats] UPCGPointArrayData: %s"), *Error);
	}
	co_return;
}
#endif

// Apply actor selection before the component cap.
UNTEST_UNIT_OPTS(Claireon, PCGLiveComponents, ActorSelectorIsAppliedBeforeTheCap, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGToolGapTests_anon;

	UWorld* World = IsValid(GEditor) ? GEditor->GetEditorWorldContext().World() : nullptr;
	UNTEST_ASSERT_PTR(World);

	FPCGGapFixture Fixture(TEXT("PCGGap_LiveComponentsCap"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	struct FScopedActors
	{
		TArray<AActor*> Actors;
		~FScopedActors()
		{
			for (AActor* Actor : Actors)
			{
				if (IsValid(Actor))
				{
					Actor->Destroy();
				}
			}
		}
	} Spawned;

	const TCHAR* Labels[] = { TEXT("PCGGapCap_A"), TEXT("PCGGapCap_B"), TEXT("PCGGapCap_C") };
	for (const TCHAR* Label : Labels)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		UNTEST_ASSERT_PTR(Actor);
		Spawned.Actors.Add(Actor);
		Actor->SetActorLabel(Label);
		UPCGComponent* Component = NewObject<UPCGComponent>(Actor);
		UNTEST_ASSERT_PTR(Component);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		Component->SetGraph(Graph);
	}

	TArray<UPCGComponent*> All;
	int32 Skipped = 0;
	ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(Graph, All, Skipped, /*MaxComponents=*/1);
	UNTEST_EXPECT_EQ(All.Num(), 1);
	UNTEST_EXPECT_EQ(Skipped, 2);

	for (const TCHAR* Label : Labels)
	{
		TArray<UPCGComponent*> Named;
		int32 NamedSkipped = 0;
		ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(Graph, Named, NamedSkipped, /*MaxComponents=*/1, Label);
		UNTEST_EXPECT_EQ(Named.Num(), 1);
		UNTEST_EXPECT_EQ(NamedSkipped, 0);
		UNTEST_EXPECT_TRUE(Named.Num() == 1 && Named[0]->GetOwner()->GetActorLabel() == Label);
	}

	TArray<UPCGComponent*> None;
	int32 NoneSkipped = 0;
	ClaireonPCGGraphHelpers::CollectLiveComponentsUsingGraph(Graph, None, NoneSkipped, 1, TEXT("PCGGapCap_Nope"));
	UNTEST_EXPECT_EQ(None.Num(), 0);
	co_return;
}

#endif // WITH_UNTESTED
