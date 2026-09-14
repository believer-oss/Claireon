// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test PCG change flags, notification batching, pin rebuilding, editor reconciliation, and refresh with transient fixtures.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Tools/ClaireonPCGEditorSync.h"
#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/ClaireonPCGGraphTool_AddNode.h"
#include "Tools/ClaireonPCGGraphTool_Connect.h"
#include "Tools/ClaireonPCGGraphTool_Disconnect.h"
#include "Tools/ClaireonPCGGraphTool_RemoveNode.h"
#include "Tools/ClaireonPCGGraphTool_Refresh.h"
#include "Tools/ClaireonPCGGraphTool_SetNodeProperty.h"
#include "UObject/StrongObjectPtr.h"

namespace ClaireonPCGEditorSyncTests_anon
{
	using namespace ClaireonPCGGraphHelpers;

	/** Keep a transient graph rooted while its registered session invokes real tools. */
	struct FPCGSyncFixture
	{
		TStrongObjectPtr<UPCGGraph> Graph;
		FString SessionId;

		explicit FPCGSyncFixture(const TCHAR* UniqueName)
		{
			Graph.Reset(NewObject<UPCGGraph>(GetTransientPackage(), NAME_None, RF_Transient));

			// Synthetic session paths exercise lookup without disk assets.
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

		~FPCGSyncFixture()
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
	};

	/** Record graph notifications and unbind before the stack-owned observer is destroyed. */
	struct FChangeTypeRecorder
	{
		TArray<EPCGChangeType> Observed;

		explicit FChangeTypeRecorder(UPCGGraph* InGraph)
			: Graph(InGraph)
		{
			if (IsValid(InGraph))
			{
				Handle = InGraph->OnGraphChangedDelegate.AddLambda(
					[this](UPCGGraphInterface* /*Changed*/, EPCGChangeType ChangeType)
					{
						Observed.Add(ChangeType);
					});
			}
		}

		~FChangeTypeRecorder()
		{
			if (Handle.IsValid())
			{
				if (UPCGGraph* Live = Graph.Get(); IsValid(Live))
				{
					Live->OnGraphChangedDelegate.Remove(Handle);
				}
				Handle.Reset();
			}
		}

		int32 Num() const { return Observed.Num(); }

		/** Union of every observed change type. */
		EPCGChangeType Union() const
		{
			EPCGChangeType All = EPCGChangeType::None;
			for (EPCGChangeType Observed_ : Observed)
			{
				All |= Observed_;
			}
			return All;
		}

		void Reset() { Observed.Reset(); }

	private:
		TWeakObjectPtr<UPCGGraph> Graph;
		FDelegateHandle Handle;
	};

	/** Resolve unexported UPCGQualityBranchSettings by name; its bUse*Pin flags change dynamic outputs. */
	static UClass* DynamicPinSettingsClass()
	{
		FString Error;
		return ClaireonPCGGraphHelpers::ResolveSettingsClass(TEXT("PCGQualityBranchSettings"), Error);
	}

	/** Add a node of the given settings class straight through the engine API. */
	static UPCGNode* AddNodeOfClass(UPCGGraph* Graph, UClass* SettingsClass)
	{
		if (!IsValid(Graph) || !IsValid(SettingsClass))
		{
			return nullptr;
		}
		UPCGSettings* DefaultSettings = nullptr;
		return Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
	}

	/** Address fresh nodes by index because titles are unset and class names are ambiguous. */
	static FString NodeId(UPCGGraph* Graph, const UPCGNode* Node)
	{
		if (!IsValid(Graph) || !IsValid(Node))
		{
			return FString();
		}
		return FString::FromInt(Graph->GetNodes().IndexOfByKey(Node));
	}

	/** First input pin label, read from the node rather than hardcoded. */
	static FString FirstInputPinLabel(const UPCGNode* Node)
	{
		return (IsValid(Node) && Node->GetInputPins().Num() > 0)
			? Node->GetInputPins()[0]->Properties.Label.ToString()
			: FString();
	}

	/** First output pin label, read from the node rather than hardcoded. */
	static FString FirstOutputPinLabel(const UPCGNode* Node)
	{
		return (IsValid(Node) && Node->GetOutputPins().Num() > 0)
			? Node->GetOutputPins()[0]->Properties.Label.ToString()
			: FString();
	}
} // namespace ClaireonPCGEditorSyncTests_anon

// Operation change flags.

UNTEST_UNIT_OPTS(Claireon, PCGNotifyFlags, EdgeOpsAreEdgeStructural, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonPCGGraphHelpers;

	for (const EPCGGraphEditOp Op : { EPCGGraphEditOp::Connect, EPCGGraphEditOp::Disconnect, EPCGGraphEditOp::DisconnectAll })
	{
		const EPCGChangeType Flags = GetChangeTypeForOp(Op);
		UNTEST_EXPECT_TRUE(EnumHasAllFlags(Flags, EPCGChangeType::Edge | EPCGChangeType::Structural));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNotifyFlags, NodeOpsAreNodeStructural, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonPCGGraphHelpers;

	for (const EPCGGraphEditOp Op : { EPCGGraphEditOp::AddNode, EPCGGraphEditOp::RemoveNode })
	{
		const EPCGChangeType Flags = GetChangeTypeForOp(Op);
		UNTEST_EXPECT_TRUE(EnumHasAllFlags(Flags, EPCGChangeType::Node | EPCGChangeType::Structural));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNotifyFlags, EveryOpMapsToSomething, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonPCGGraphHelpers;

	for (uint8 Raw = 0; Raw < static_cast<uint8>(EPCGGraphEditOp::Count); ++Raw)
	{
		const EPCGChangeType Flags = GetChangeTypeForOp(static_cast<EPCGGraphEditOp>(Raw));
		UNTEST_EXPECT_TRUE(Flags != EPCGChangeType::None);
		UNTEST_EXPECT_FALSE(Flags == EPCGChangeType::Cosmetic);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNotifyFlags, ChangeTypeToStringNamesFlags, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonPCGGraphHelpers;

	const FString Text = ChangeTypeToString(EPCGChangeType::Edge | EPCGChangeType::Structural);
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Edge")));
	UNTEST_EXPECT_TRUE(Text.Contains(TEXT("Structural")));
	UNTEST_EXPECT_STREQ(ChangeTypeToString(EPCGChangeType::None), TEXT("None"));
	co_return;
}

// Engine notification routing.

UNTEST_UNIT_OPTS(Claireon, PCGNotifyRouting, AddNodeNotifiesNodeStructural, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_AddNode"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	ClaireonPCGGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("settings_class"), TEXT("PCGQualityBranchSettings"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Recorder.Num() > 0);
	UNTEST_EXPECT_TRUE(EnumHasAllFlags(Recorder.Union(), EPCGChangeType::Node | EPCGChangeType::Structural));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNotifyRouting, ConnectNotifiesEdgeStructural, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Connect"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Source = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UPCGNode* Target = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Source);
	UNTEST_ASSERT_PTR(Target);

	const FString FromPin = FirstOutputPinLabel(Source);
	const FString ToPin = FirstInputPinLabel(Target);
	UNTEST_ASSERT_FALSE(FromPin.IsEmpty());
	UNTEST_ASSERT_FALSE(ToPin.IsEmpty());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	ClaireonPCGGraphTool_Connect Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("from_node"), NodeId(Fixture.Graph.Get(), Source));
	Args->SetStringField(TEXT("from_pin"), FromPin);
	Args->SetStringField(TEXT("to_node"), NodeId(Fixture.Graph.Get(), Target));
	Args->SetStringField(TEXT("to_pin"), ToPin);
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Recorder.Num() > 0);
	UNTEST_EXPECT_TRUE(EnumHasAllFlags(Recorder.Union(), EPCGChangeType::Edge | EPCGChangeType::Structural));
	co_return;
}

// Rejected edits must not notify.
UNTEST_UNIT_OPTS(Claireon, PCGNotifyRouting, NoNotifyOnFailedEdit, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_FailedEdit"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	ClaireonPCGGraphTool_RemoveNode Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), TEXT("__no_such_node_zz__"));
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Recorder.Num(), 0);
	co_return;
}

// Notification batching.

UNTEST_UNIT_OPTS(Claireon, PCGNotifyBatching, PauseScopeCoalescesToOne, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;
	using namespace ClaireonPCGGraphHelpers;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Batch1"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	{
		FPCGGraphNotifyPauseScope Pause(Fixture.Graph.Get());
		NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Edge);
		NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Node);
		NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Settings);
		UNTEST_EXPECT_EQ(Recorder.Num(), 0);
	}

	UNTEST_EXPECT_EQ(Recorder.Num(), 1);
	UNTEST_EXPECT_TRUE(EnumHasAllFlags(Recorder.Union(),
		EPCGChangeType::Edge | EPCGChangeType::Node | EPCGChangeType::Settings));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNotifyBatching, NestedPauseScopesBalance, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;
	using namespace ClaireonPCGGraphHelpers;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Batch2"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	{
		FPCGGraphNotifyPauseScope Outer(Fixture.Graph.Get());
		{
			FPCGGraphNotifyPauseScope Inner(Fixture.Graph.Get());
			NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Structural);
		}
		UNTEST_EXPECT_EQ(Recorder.Num(), 0);
	}

	UNTEST_EXPECT_EQ(Recorder.Num(), 1);
	co_return;
}

// Do not release a pause against a destroyed graph.
UNTEST_UNIT_OPTS(Claireon, PCGNotifyBatching, PauseScopeSurvivesGraphLoss, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGGraphHelpers;

	{
		FPCGGraphNotifyPauseScope Pause(nullptr);
	}

	UPCGGraph* Graph = NewObject<UPCGGraph>(GetTransientPackage(), NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(Graph);
	{
		FPCGGraphNotifyPauseScope Pause(Graph);
		NotifyGraphChanged(Graph, EPCGChangeType::Structural);
	}
	co_return;
}

// Dynamic settings changes rebuild pins.

UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, SettingsChangeRebuildsPins, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_PinRebuild"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);
	const int32 PinsBefore = Node->GetOutputPins().Num();

	ClaireonPCGGraphTool_SetNodeProperty Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId(Fixture.Graph.Get(), Node));
	Args->SetStringField(TEXT("property_name"), TEXT("bUseLowPin"));
	Args->SetStringField(TEXT("value"), TEXT("true"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	const int32 PinsAfter = Node->GetOutputPins().Num();
	UNTEST_EXPECT_TRUE(PinsAfter > PinsBefore);

	bool bFoundLow = false;
	for (const UPCGPin* Pin : Node->GetOutputPins())
	{
		if (IsValid(Pin) && Pin->Properties.Label.ToString().Contains(TEXT("Low")))
		{
			bFoundLow = true;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundLow);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, ReportsEngineDerivedChangeType, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_ChangeType"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	ClaireonPCGGraphTool_SetNodeProperty Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId(Fixture.Graph.Get(), Node));
	Args->SetStringField(TEXT("property_name"), TEXT("bUseHighPin"));
	Args->SetStringField(TEXT("value"), TEXT("true"));
	const auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	FString ChangeTypeText;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("change_type"), ChangeTypeText));
	UNTEST_EXPECT_TRUE(ChangeTypeText.Contains(TEXT("Settings")));
	// Dynamic-pin changes must include the Node flag.
	UNTEST_EXPECT_TRUE(ChangeTypeText.Contains(TEXT("Node")));

	UNTEST_ASSERT_TRUE(Recorder.Num() > 0);
	UNTEST_EXPECT_TRUE(EnumHasAnyFlags(Recorder.Union(), EPCGChangeType::Settings));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, InvalidatedEdgeIsDropped, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_EdgeDrop"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Source = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UPCGNode* Target = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Source);
	UNTEST_ASSERT_PTR(Target);

	// Remove a connected dynamic pin and verify its edge disappears.
	auto SetBool = [&Fixture](UPCGNode* Node, const TCHAR* Property, const TCHAR* Value)
	{
		ClaireonPCGGraphTool_SetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = Fixture.Args();
		Args->SetStringField(TEXT("node"), NodeId(Fixture.Graph.Get(), Node));
		Args->SetStringField(TEXT("property_name"), Property);
		Args->SetStringField(TEXT("value"), Value);
		return Tool.Execute(Args);
	};

	UNTEST_EXPECT_FALSE(SetBool(Source, TEXT("bUseLowPin"), TEXT("true")).bIsError);

	const UPCGPin* LowPin = nullptr;
	for (const UPCGPin* Pin : Source->GetOutputPins())
	{
		if (IsValid(Pin) && Pin->Properties.Label.ToString().Contains(TEXT("Low")))
		{
			LowPin = Pin;
		}
	}
	UNTEST_ASSERT_PTR(LowPin);
	UNTEST_ASSERT_TRUE(Target->GetInputPins().Num() > 0);

	const FName LowLabel = LowPin->Properties.Label;
	const FName TargetLabel = Target->GetInputPins()[0]->Properties.Label;
	Fixture.Graph->AddEdge(Source, LowLabel, Target, TargetLabel);

	const UPCGPin* TargetPin = Target->GetInputPin(TargetLabel);
	UNTEST_ASSERT_PTR(TargetPin);
	UNTEST_ASSERT_TRUE(TargetPin->EdgeCount() > 0);

	UNTEST_EXPECT_FALSE(SetBool(Source, TEXT("bUseLowPin"), TEXT("false")).bIsError);

	const UPCGPin* TargetPinAfter = Target->GetInputPin(TargetLabel);
	UNTEST_ASSERT_PTR(TargetPinAfter);
	UNTEST_EXPECT_EQ(TargetPinAfter->EdgeCount(), 0);
	co_return;
}

// Invalid object paths must not overwrite required constructor subobjects with null.
UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, WrongClassObjectPathIsRejected, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_ObjProp"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FString ResolveError;
	UClass* SubgraphClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(TEXT("PCGSubgraphSettings"), ResolveError);
	UNTEST_ASSERT_PTR(SubgraphClass);

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), SubgraphClass);
	UNTEST_ASSERT_PTR(Node);
	UPCGSettings* Settings = Node->GetSettings();
	UNTEST_ASSERT_PTR(Settings);

	FProperty* Property = Settings->GetClass()->FindPropertyByName(TEXT("SubgraphInstance"));
	UNTEST_ASSERT_PTR(Property);
	FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
	UNTEST_ASSERT_PTR(ObjectProperty);
	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
	UObject* Before = ObjectProperty->GetObjectPropertyValue(ValuePtr);
	UNTEST_ASSERT_PTR(Before);

	// A graph cannot substitute for a graph-instance property.
	FString Error;
	EPCGChangeType ChangeType = EPCGChangeType::None;
	const bool bSet = ClaireonPCGGraphHelpers::SetNodeProperty(
		Node, TEXT("SubgraphInstance"), Fixture.Graph->GetPathName(), Error, ChangeType);

	UNTEST_EXPECT_FALSE(bSet);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("PCGGraphInstance")));
	UNTEST_EXPECT_TRUE(ObjectProperty->GetObjectPropertyValue(ValuePtr) == Before);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, ExplicitNoneClearsObjectProperty, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_ObjClear"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FString ResolveError;
	UClass* SubgraphClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(TEXT("PCGSubgraphSettings"), ResolveError);
	UNTEST_ASSERT_PTR(SubgraphClass);
	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), SubgraphClass);
	UNTEST_ASSERT_PTR(Node);

	FString Error;
	EPCGChangeType ChangeType = EPCGChangeType::None;
	const bool bSet = ClaireonPCGGraphHelpers::SetNodeProperty(
		Node, TEXT("SubgraphOverride"), TEXT("None"), Error, ChangeType);
	UNTEST_EXPECT_TRUE(bSet);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGPinRebuild, MissingPropertyIsAnError, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_BadProp"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);

	ClaireonPCGGraphTool_SetNodeProperty Tool;
	TSharedPtr<FJsonObject> Args = Fixture.Args();
	Args->SetStringField(TEXT("node"), NodeId(Fixture.Graph.Get(), Node));
	Args->SetStringField(TEXT("property_name"), TEXT("__nope_zz__"));
	Args->SetStringField(TEXT("value"), TEXT("true"));
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// Assign distinct positions to newly authored nodes.

UNTEST_UNIT_OPTS(Claireon, PCGNodePlacement, AddedNodesDoNotStack, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Placement"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	for (int32 i = 0; i < 4; ++i)
	{
		ClaireonPCGGraphTool_AddNode Tool;
		TSharedPtr<FJsonObject> Args = Fixture.Args();
		Args->SetStringField(TEXT("settings_class"), TEXT("PCGQualityBranchSettings"));
		UNTEST_EXPECT_FALSE(Tool.Execute(Args).bIsError);
	}

	const TArray<UPCGNode*>& Nodes = Fixture.Graph->GetNodes();
	UNTEST_ASSERT_EQ(Nodes.Num(), 4);

	TSet<FString> Slots;
	for (const UPCGNode* Node : Nodes)
	{
		UNTEST_ASSERT_PTR(Node);
		UNTEST_EXPECT_FALSE(Node->PositionX == 0 && Node->PositionY == 0);
		Slots.Add(FString::Printf(TEXT("%d,%d"), Node->PositionX, Node->PositionY));
	}
	UNTEST_EXPECT_EQ(Slots.Num(), Nodes.Num());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGNodePlacement, ExistingPositionIsNotOverwritten, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_PlacementKeep"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);

	Node->PositionX = 1234;
	Node->PositionY = 5678;
	ClaireonPCGGraphHelpers::AssignDefaultNodePosition(Fixture.Graph.Get(), Node);

	UNTEST_EXPECT_EQ(Node->PositionX, 1234);
	UNTEST_EXPECT_EQ(Node->PositionY, 5678);
	co_return;
}

// Place legacy nodes in the view without dirtying their stored positions.
UNTEST_UNIT_OPTS(Claireon, PCGNodePlacement, DisplayPositionLeavesTheNodeAlone, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_PlacementDisplay"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UPCGNode* Node = AddNodeOfClass(Fixture.Graph.Get(), DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);
	Node->PositionX = 0;
	Node->PositionY = 0;

	int32 X = -1;
	int32 Y = -1;
	ClaireonPCGGraphHelpers::ComputeDisplayPosition(Fixture.Graph.Get(), Node, X, Y);

	UNTEST_EXPECT_FALSE(X == 0 && Y == 0);
	UNTEST_EXPECT_EQ(Node->PositionX, 0);
	UNTEST_EXPECT_EQ(Node->PositionY, 0);

	int32 X2 = -1;
	int32 Y2 = -1;
	ClaireonPCGGraphHelpers::ComputeDisplayPosition(Fixture.Graph.Get(), Node, X2, Y2);
	UNTEST_EXPECT_EQ(X2, X);
	UNTEST_EXPECT_EQ(Y2, Y);
	co_return;
}

// Coalesce reconstruction per graph and tick.

UNTEST_UNIT_OPTS(Claireon, PCGCoalescing, BurstSchedulesOneFlush, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Burst"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGEditorSync::ResetStats();

	for (int32 i = 0; i < 20; ++i)
	{
		ClaireonPCGEditorSync::RequestReconstruct(Fixture.Graph.Get());
	}
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 1);

	ClaireonPCGEditorSync::FlushPendingNow();

	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().FlushCount, 1);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().GraphsFlushed, 1);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGCoalescing, DistinctGraphsBothFlush, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture A(TEXT("PCGSync_TwoA"));
	FPCGSyncFixture B(TEXT("PCGSync_TwoB"));
	UNTEST_ASSERT_TRUE(A.IsUsable() && B.IsUsable());

	ClaireonPCGEditorSync::ResetStats();
	ClaireonPCGEditorSync::RequestReconstruct(A.Graph.Get());
	ClaireonPCGEditorSync::RequestReconstruct(B.Graph.Get());
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 2);

	ClaireonPCGEditorSync::FlushPendingNow();
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().FlushCount, 1);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().GraphsFlushed, 2);
	co_return;
}

// Settings and cosmetic changes preserve view state; structural changes request reconstruction.
UNTEST_UNIT_OPTS(Claireon, PCGCoalescing, OnlyStructuralEditsScheduleReconstruct, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;
	using namespace ClaireonPCGGraphHelpers;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Sched"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGEditorSync::FlushPendingNow();

	NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Cosmetic);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);

	NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Settings);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);

	NotifyGraphChanged(Fixture.Graph.Get(), EPCGChangeType::Structural);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 1);

	ClaireonPCGEditorSync::FlushPendingNow();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGCoalescing, CancelRemovesPendingRequest, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Cancel"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGEditorSync::FlushPendingNow();
	ClaireonPCGEditorSync::RequestReconstruct(Fixture.Graph.Get());
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 1);

	ClaireonPCGEditorSync::CancelPendingReconstruct(Fixture.Graph.Get());
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);
	co_return;
}

// Report editor-view reconstruction capability and state.

// An unopened graph has no stale cached view.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, CleanGraphHasNothingToRefresh, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_NoEditor"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::GetEditorViewState(Fixture.Graph.Get())
		== EPCGEditorViewState::NoEditorGraph);
	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Fixture.Graph.Get())
		== EPCGReconstructResult::NothingToRefresh);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, RebuildIsAvailableOnThisEngine, UNTEST_TIMEOUTMS(10000))
{
	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::IsReconstructAvailable());
	co_return;
}

// Check reflected class/property names individually so engine renames produce useful failures.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, ReflectedEditorClassesResolve, UNTEST_TIMEOUTMS(10000))
{
	UClass* BaseClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraphNodeBase"));
	UNTEST_ASSERT_PTR(BaseClass);
	UNTEST_ASSERT_PTR(FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraphNode")));

	FProperty* PCGNodeProperty = BaseClass->FindPropertyByName(TEXT("PCGNode"));
	UNTEST_ASSERT_PTR(PCGNodeProperty);
	UNTEST_EXPECT_PTR(CastField<FObjectPropertyBase>(PCGNodeProperty));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, EveryViewStateIsDescribed, UNTEST_TIMEOUTMS(10000))
{
	const FString OpenText = ClaireonPCGEditorSync::DescribeViewState(EPCGEditorViewState::EditorOpen);
	const FString CachedText = ClaireonPCGEditorSync::DescribeViewState(EPCGEditorViewState::CachedButClosed);
	const FString CleanText = ClaireonPCGEditorSync::DescribeViewState(EPCGEditorViewState::NoEditorGraph);

	UNTEST_EXPECT_FALSE(OpenText.IsEmpty());
	UNTEST_EXPECT_FALSE(CachedText.IsEmpty());
	UNTEST_EXPECT_FALSE(CleanText.IsEmpty());
	UNTEST_EXPECT_TRUE(OpenText != CleanText);
	UNTEST_EXPECT_TRUE(CachedText != CleanText);

	// Guidance must match whether reflected reconstruction is available.
	if (ClaireonPCGEditorSync::IsReconstructAvailable())
	{
		UNTEST_EXPECT_TRUE(OpenText.Contains(TEXT("rebuilt")));
		UNTEST_EXPECT_TRUE(CachedText.Contains(TEXT("rebuilt")));
		UNTEST_EXPECT_FALSE(OpenText.Contains(TEXT("restart")));
	}
	else
	{
		UNTEST_EXPECT_TRUE(OpenText.Contains(TEXT("restart")));
		UNTEST_EXPECT_TRUE(CachedText.Contains(TEXT("restart")));
	}
	co_return;
}

// Do not create a cached editor graph merely to flush an unopened asset.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, FlushNeverCountsAPhantomRebuild, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_NoAlloc"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGEditorSync::ResetStats();
	ClaireonPCGEditorSync::RequestReconstruct(Fixture.Graph.Get());
	ClaireonPCGEditorSync::FlushPendingNow();

	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().GraphsFlushed, 1);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().Reconstructed, 0);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().StaleViewsDetected, 0);
	co_return;
}

// Reconcile runtime nodes into a cached editor graph, including input/output and pins.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, RebuildMatchesEditorNodesToPCGNodes, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Rebuild"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);

	// Outer the cached editor graph to its PCG graph, as the engine does.
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 0);

	UNTEST_ASSERT_PTR(AddNodeOfClass(Graph, DynamicPinSettingsClass()));
	UNTEST_ASSERT_PTR(AddNodeOfClass(Graph, DynamicPinSettingsClass()));

	const EPCGReconstructResult Result = ClaireonPCGEditorSync::ReconstructOpenEditor(Graph);
	UNTEST_EXPECT_TRUE(Result == EPCGReconstructResult::Reconstructed);

	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 4);

	int32 WithPins = 0;
	for (UEdGraphNode* EdNode : EditorGraph->Nodes)
	{
		if (IsValid(EdNode) && EdNode->Pins.Num() > 0)
		{
			++WithPins;
		}
	}
	UNTEST_EXPECT_EQ(WithPins, 4);

	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 4);
	co_return;
}

// Verify connected runtime edges appear in the editor view.
// Final link counts do not detect ordering-related ensures because a later endpoint can repair the link.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, RebuildLinksEveryEdge, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_RebuildLinks"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);

	TArray<UPCGNode*> Chain;
	for (int32 i = 0; i < 5; ++i)
	{
		UPCGNode* Node = AddNodeOfClass(Graph, DynamicPinSettingsClass());
		UNTEST_ASSERT_PTR(Node);
		Chain.Add(Node);
	}

	int32 ExpectedEdges = 0;
	for (int32 i = 0; i + 1 < Chain.Num(); ++i)
	{
		const FString FromPin = FirstOutputPinLabel(Chain[i]);
		const FString ToPin = FirstInputPinLabel(Chain[i + 1]);
		UNTEST_ASSERT_FALSE(FromPin.IsEmpty());
		UNTEST_ASSERT_FALSE(ToPin.IsEmpty());
		Graph->AddEdge(Chain[i], FName(*FromPin), Chain[i + 1], FName(*ToPin));
	}

	for (UPCGNode* Node : Chain)
	{
		for (const UPCGPin* Pin : Node->GetOutputPins())
		{
			if (Pin)
			{
				ExpectedEdges += Pin->Edges.Num();
			}
		}
	}
	UNTEST_ASSERT_EQ(ExpectedEdges, 4);

	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);

	int32 EditorLinks = 0;
	for (UEdGraphNode* EdNode : EditorGraph->Nodes)
	{
		if (!IsValid(EdNode)) { continue; }
		for (UEdGraphPin* Pin : EdNode->Pins)
		{
			if (Pin && Pin->Direction == EEdGraphPinDirection::EGPD_Output)
			{
				EditorLinks += Pin->LinkedTo.Num();
			}
		}
	}
	UNTEST_EXPECT_EQ(EditorLinks, ExpectedEdges);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, RebuildDropsEditorNodesForRemovedPCGNodes, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_RebuildDrop"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);

	UPCGNode* Doomed = AddNodeOfClass(Graph, DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Doomed);
	ClaireonPCGEditorSync::ReconstructOpenEditor(Graph);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 3);

	Graph->RemoveNode(Doomed);
	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 2);
	co_return;
}

// Native settings edits must update synthesized editor pins through the runtime change delegate without another tool call.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, SynthesizedNodeRebuildsPinsOnNativeEdit, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_NativeEdit"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);

	UPCGNode* Node = AddNodeOfClass(Graph, DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph)
		== EPCGReconstructResult::Reconstructed);

	UEdGraphNode* EdNode = nullptr;
	for (UEdGraphNode* Candidate : EditorGraph->Nodes)
	{
		if (!IsValid(Candidate))
		{
			continue;
		}
		const FObjectProperty* LinkProperty =
			FindFProperty<FObjectProperty>(Candidate->GetClass(), TEXT("PCGNode"));
		if (LinkProperty && LinkProperty->GetObjectPropertyValue_InContainer(Candidate) == Node)
		{
			EdNode = Candidate;
			break;
		}
	}
	UNTEST_ASSERT_PTR(EdNode);

	const auto CountOutputs = [](const UEdGraphNode* N)
	{
		int32 Count = 0;
		for (const UEdGraphPin* Pin : N->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				++Count;
			}
		}
		return Count;
	};
	const int32 EdOutputsBefore = CountOutputs(EdNode);
	const int32 RuntimeOutputsBefore = Node->GetOutputPins().Num();

	// Deliver a native property-change event with no subsequent Claireon refresh.
	UPCGSettings* Settings = Node->GetSettings();
	UNTEST_ASSERT_PTR(Settings);
	FBoolProperty* UseLowPin = FindFProperty<FBoolProperty>(Settings->GetClass(), TEXT("bUseLowPin"));
	UNTEST_ASSERT_TRUE(UseLowPin != nullptr);
	UseLowPin->SetPropertyValue_InContainer(Settings, true);
	FPropertyChangedEvent Event(UseLowPin, EPropertyChangeType::ValueSet);
	// Use the public UObject surface because UPCGSettings declares the override protected.
	static_cast<UObject*>(Settings)->PostEditChangeProperty(Event);

	UNTEST_EXPECT_TRUE(Node->GetOutputPins().Num() > RuntimeOutputsBefore);

	UNTEST_EXPECT_TRUE(CountOutputs(EdNode) > EdOutputsBefore);
	bool bEdHasLowPin = false;
	for (const UEdGraphPin* Pin : EdNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output
			&& Pin->PinName.ToString().Contains(TEXT("Low")))
		{
			bEdHasLowPin = true;
		}
	}
	UNTEST_EXPECT_TRUE(bEdHasLowPin);
	co_return;
}

// Refresh arguments, view reporting, and opt-in regeneration.

UNTEST_UNIT_OPTS(Claireon, PCGRefreshTool, RejectsNeitherIdNorPath, UNTEST_TIMEOUTMS(10000))
{
	ClaireonPCGGraphTool_Refresh Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGRefreshTool, RejectsBothIdAndPath, UNTEST_TIMEOUTMS(10000))
{
	ClaireonPCGGraphTool_Refresh Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("whatever"));
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/Whatever"));
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGRefreshTool, UnknownSessionIsAnError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonPCGGraphTool_Refresh Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("__not_a_session_zz__"));
	const auto Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGRefreshTool, ReportsEveryFieldAndDoesNotRegenerateByDefault, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Refresh"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	FChangeTypeRecorder Recorder(Fixture.Graph.Get());

	ClaireonPCGGraphTool_Refresh Tool;
	const auto Result = Tool.Execute(Fixture.Args());

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	bool bNotified = false, bReconstructed = true, bAvailable = false;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("notified"), bNotified));
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("reconstructed"), bReconstructed));
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("reconstruct_available"), bAvailable));
	UNTEST_EXPECT_TRUE(bNotified);
	// No cached editor graph means no reconstruction is needed.
	UNTEST_EXPECT_FALSE(bReconstructed);
	UNTEST_EXPECT_EQ(bAvailable, ClaireonPCGEditorSync::IsReconstructAvailable());

	double Regenerated = -1.0, Parents = -1.0, Editors = -1.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("components_regenerated"), Regenerated));
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("parents_reconstructed"), Parents));
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("editors_reconstructed"), Editors));
	UNTEST_EXPECT_EQ(static_cast<int32>(Regenerated), 0);
	UNTEST_EXPECT_EQ(static_cast<int32>(Editors), 0);

	FString ReconstructStatus, ViewState;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("reconstruct_status"), ReconstructStatus));
	UNTEST_EXPECT_FALSE(ReconstructStatus.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("view_state"), ViewState));
	UNTEST_EXPECT_FALSE(ViewState.IsEmpty());

	UNTEST_ASSERT_TRUE(Recorder.Num() > 0);
	UNTEST_EXPECT_TRUE(EnumHasAnyFlags(Recorder.Union(), EPCGChangeType::Structural));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PCGRefreshTool, ClearsPendingCoalescedRequest, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_RefreshCancel"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());

	ClaireonPCGEditorSync::FlushPendingNow();
	ClaireonPCGEditorSync::RequestReconstruct(Fixture.Graph.Get());
	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::GetPendingCount() >= 1);

	ClaireonPCGGraphTool_Refresh Tool;
	const auto Result = Tool.Execute(Fixture.Args());
	UNTEST_EXPECT_FALSE(Result.bIsError);

	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);
	co_return;
}

// Preflight reconstruction before removing orphans so missing capabilities cannot leave a partial view.

UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, MissingEditorNodeClassLeavesTheViewUntouched, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	struct FScopedMissingClass
	{
		explicit FScopedMissingClass(FName ClassName)
		{
			ClaireonPCGEditorSync::SetMissingEditorNodeClassForTests(ClassName);
		}
		~FScopedMissingClass()
		{
			ClaireonPCGEditorSync::SetMissingEditorNodeClassForTests(NAME_None);
		}
	};

	FPCGSyncFixture Fixture(TEXT("PCGSync_RebuildAtomic"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);

	UPCGNode* Doomed = AddNodeOfClass(Graph, DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Doomed);
	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);
	UNTEST_ASSERT_EQ(EditorGraph->Nodes.Num(), 3);

	// Queue both an orphan removal and a missing counterpart to test atomic reconciliation.
	Graph->RemoveNode(Doomed);
	UPCGNode* Added = AddNodeOfClass(Graph, DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Added);
	const int32 RuntimeNodesBefore = Graph->GetNodes().Num();

	TArray<UEdGraphNode*> ViewBefore = EditorGraph->Nodes;
	UNTEST_ASSERT_EQ(ViewBefore.Num(), 3);

	{
		// Remove the general editor-node class so counterpart construction is impossible.
		FScopedMissingClass Missing(TEXT("PCGEditorGraphNode"));

		UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Unavailable);

		UNTEST_ASSERT_EQ(EditorGraph->Nodes.Num(), ViewBefore.Num());
		for (int32 Index = 0; Index < ViewBefore.Num(); ++Index)
		{
			UNTEST_EXPECT_TRUE(EditorGraph->Nodes[Index] == ViewBefore[Index]);
		}

		UNTEST_EXPECT_EQ(Graph->GetNodes().Num(), RuntimeNodesBefore);
		UNTEST_EXPECT_TRUE(Graph->GetNodes().Contains(Added));

		UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Unavailable);
		UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), ViewBefore.Num());
	}

	// Restore the class and verify the complete reconciliation succeeds.
	UNTEST_EXPECT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);
	UNTEST_ASSERT_EQ(EditorGraph->Nodes.Num(), 3);
	int32 Survivors = 0;
	for (UEdGraphNode* Earlier : ViewBefore)
	{
		if (EditorGraph->Nodes.Contains(Earlier))
		{
			++Survivors;
		}
	}
	UNTEST_EXPECT_EQ(Survivors, 2);
	co_return;
}

// Within one request, settle pending reconstruction before later node or edge edits; no tick can intervene.
UNTEST_UNIT_OPTS(Claireon, PCGReconstructGuard, SettleDrainsAPendingRequestBeforeAnEdgeEdit, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGEditorSyncTests_anon;

	FPCGSyncFixture Fixture(TEXT("PCGSync_Settle"));
	UNTEST_ASSERT_TRUE(Fixture.IsUsable());
	UPCGGraph* Graph = Fixture.Graph.Get();

	UClass* EditorGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGEditorGraph"));
	UNTEST_ASSERT_PTR(EditorGraphClass);
	UEdGraph* EditorGraph = NewObject<UEdGraph>(Graph, EditorGraphClass, NAME_None, RF_Transient);
	UNTEST_ASSERT_PTR(EditorGraph);
	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::ReconstructOpenEditor(Graph) == EPCGReconstructResult::Reconstructed);
	UNTEST_ASSERT_EQ(EditorGraph->Nodes.Num(), 2);

	ClaireonPCGEditorSync::ResetStats();

	ClaireonPCGEditorSync::SettlePendingReconstruct(Graph);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().FlushCount, 0);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 2);

	UPCGNode* Added = AddNodeOfClass(Graph, DynamicPinSettingsClass());
	UNTEST_ASSERT_PTR(Added);
	ClaireonPCGEditorSync::RequestReconstruct(Graph);
	UNTEST_ASSERT_TRUE(ClaireonPCGEditorSync::GetPendingCount() > 0);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 2);

	ClaireonPCGEditorSync::SettlePendingReconstruct(Graph);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetPendingCount(), 0);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().FlushCount, 1);
	UNTEST_EXPECT_EQ(ClaireonPCGEditorSync::GetStats().Reconstructed, 1);
	UNTEST_EXPECT_EQ(EditorGraph->Nodes.Num(), 3);
	co_return;
}

#endif // WITH_UNTESTED
