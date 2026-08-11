// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Baseline pinning tests for Workstream B (silent-failure elimination),
// Stage 001 of the Claireon BP feedback plan (Work #6704):
//   - F7 (RESOLVED): setting a K2Node_SpawnActorFromClass 'Class' pin to a
//     BP actor class with an ExposeOnSpawn variable makes that variable's
//     pin exist on the SAME call (no bp_reconstruct_node needed), and a
//     dotless Blueprint asset path is a hard error naming the pin, not a
//     stale-pins silent failure.
//   - B3 (PARTIAL, pin only the CURRENT pre-fix behavior): setting a
//     wildcard pin's default value errors loudly today, but with the
//     engine's generic "not a valid default" text, not a wildcard-specific
//     message. This test is updated (not duplicated) in Stage 004 when the
//     wildcard promotion fix lands.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "Tools/ClaireonBlueprintGraphTool_SetVariableProperties.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonSessionManager.h"
#include "ClaireonTestTypes.h"
#include "PackageTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_Select.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonWSTTestsInternal
{
	// Release any session auto-opened on the asset, then force-delete it.
	static void WST_CleanupAsset(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	// Create (asset_path, parent_class Actor) + open a bp session in one call
	// via ClaireonBlueprintGraphTool_Create, whose Execute both creates the
	// asset and returns a session_id. Empty string on failure.
	static FString WST_CreateAndOpenSession(const TCHAR* AssetPath)
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		IClaireonTool::FToolResult R = CreateTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid()) return FString();
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static UEdGraph* WST_FindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	template <typename T>
	static T* WST_FindEventGraphNodeOfClass(UBlueprint* BP)
	{
		UEdGraph* EventGraph = WST_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (T* Typed = Cast<T>(Node)) return Typed;
		}
		return nullptr;
	}

	static bool WST_NodeHasPin(UEdGraphNode* Node, const TCHAR* PinName)
	{
		if (!IsValid(Node)) return false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static UEdGraphPin* WST_FindPin(UEdGraphNode* Node, const TCHAR* PinName)
	{
		if (!IsValid(Node)) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static IClaireonTool::FToolResult WST_SetPinValue(
		const FString& SessionId, UEdGraphNode* Node, const TCHAR* PinName, const TCHAR* Value)
	{
		ClaireonBlueprintGraphTool_SetPinValue SetPinTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("pin_name"), PinName);
		Args->SetStringField(TEXT("value"), Value);
		return SetPinTool.Execute(Args);
	}

	static bool WST_HasWarningContaining(const IClaireonTool::FToolResult& Result, const TCHAR* Needle)
	{
		for (const FString& W : Result.Warnings)
		{
			if (W.Contains(Needle)) { return true; }
		}
		return false;
	}

	/** Search the response's caller-visible text surfaces for a substring. */
	static bool WST_ResponseTextContains(const IClaireonTool::FToolResult& Result, const TCHAR* Needle)
	{
		if (Result.Summary.Contains(Needle)) { return true; }
		if (Result.Logs.Contains(Needle)) { return true; }
		if (Result.Data.IsValid())
		{
			FString Serialized;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
			if (FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer))
			{
				return Serialized.Contains(Needle);
			}
		}
		return false;
	}

	/**
	 * Add a MakeArray node and grow it to two element pins, so sibling promotion
	 * is observable. Returns nullptr on any failure.
	 */
	static UK2Node_MakeArray* WST_AddMakeArrayWithTwoElements(const FString& SessionId, UBlueprint* BP)
	{
		{
			ClaireonBlueprintGraphTool_AddNode AddNodeTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("session_id"), SessionId);
			Args->SetStringField(TEXT("node_type"), TEXT("MakeArray"));
			IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
			if (R.bIsError) { return nullptr; }
		}

		UK2Node_MakeArray* Node = WST_FindEventGraphNodeOfClass<UK2Node_MakeArray>(BP);
		if (!IsValid(Node)) { return nullptr; }

		// MakeArray starts with a single '[0]'; AddInputPin gives us a sibling.
		if (!WST_FindPin(Node, TEXT("[1]")))
		{
			Node->AddInputPin();
		}
		return Node;
	}
}

// ============================================================================
// F7 (RESOLVED, positive pin): setting the Class pin on
// K2Node_SpawnActorFromClass to a BP actor class with an ExposeOnSpawn
// variable makes that variable's pin exist on the node in the SAME
// bp_set_pin_value call, and a follow-up bp_connect_pins into it succeeds
// with no bp_reconstruct_node in between.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SpawnActorClassPinRefresh_ExposeOnSpawnPinExistsSameCallAndConnects, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* TargetAssetPath = TEXT("/Game/__MCPTests/BP_WST_F7Target");
	static const TCHAR* MainAssetPath = TEXT("/Game/__MCPTests/BP_WST_F7Main");

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);

	// --- Target BP: one bool member var flagged ExposeOnSpawn, compiled+saved. ---
	FString TargetSessionId = WST_CreateAndOpenSession(TargetAssetPath);
	UNTEST_ASSERT_FALSE(TargetSessionId.IsEmpty());

	{
		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("ExposedFlag"));
		Args->SetStringField(TEXT("variable_type"), TEXT("bool"));
		IClaireonTool::FToolResult R = AddVarTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	{
		ClaireonBlueprintGraphTool_SetVariableProperties SetVarPropsTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("ExposedFlag"));
		TArray<TSharedPtr<FJsonValue>> Flags;
		Flags.Add(MakeShared<FJsonValueString>(TEXT("ExposeOnSpawn")));
		Args->SetArrayField(TEXT("flags"), Flags);
		IClaireonTool::FToolResult R = SetVarPropsTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	{
		// Compile + save so the flag is baked into the on-disk GeneratedClass
		// the Main BP's Class pin resolution will load.
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		IClaireonTool::FToolResult R = SaveTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	FClaireonSessionManager::Get().ReleaseByAssetPath(TargetAssetPath);

	const FString TargetClassPath = FString(TargetAssetPath) + TEXT(".") + FPackageName::GetShortName(TargetAssetPath) + TEXT("_C");

	// --- Main BP: a SpawnActor node (base Actor, no ExposeOnSpawn pins yet)
	//     plus a bool VariableGet to use as the connect-into source. ---
	FString MainSessionId = WST_CreateAndOpenSession(MainAssetPath);
	UNTEST_ASSERT_FALSE(MainSessionId.IsEmpty());

	UBlueprint* MainBP = Cast<UBlueprint>(FSoftObjectPath(
		FString(MainAssetPath) + TEXT(".") + FPackageName::GetShortName(MainAssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(MainBP);

	{
		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("SourceFlag"));
		Args->SetStringField(TEXT("variable_type"), TEXT("bool"));
		IClaireonTool::FToolResult R = AddVarTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	UK2Node_SpawnActorFromClass* SpawnNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));
		Args->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		SpawnNode = WST_FindEventGraphNodeOfClass<UK2Node_SpawnActorFromClass>(MainBP);
		UNTEST_ASSERT_PTR(SpawnNode);
	}

	UK2Node_VariableGet* SourceVarGet = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
		Args->SetStringField(TEXT("variable_name"), TEXT("SourceFlag"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		SourceVarGet = WST_FindEventGraphNodeOfClass<UK2Node_VariableGet>(MainBP);
		UNTEST_ASSERT_PTR(SourceVarGet);
	}

	// No ExposeOnSpawn pins yet -- base Actor has none.
	UNTEST_EXPECT_FALSE(WST_NodeHasPin(SpawnNode, TEXT("ExposedFlag")));

	// Set the Class pin to the target BP class; the ExposeOnSpawn var pin
	// must exist on the node in this SAME call (F7's whole point).
	{
		ClaireonBlueprintGraphTool_SetPinValue SetPinTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_guid"), SpawnNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("pin_name"), TEXT("Class"));
		Args->SetStringField(TEXT("value"), TargetClassPath);
		IClaireonTool::FToolResult R = SetPinTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	UEdGraphPin* ExposedPin = WST_FindPin(SpawnNode, TEXT("ExposedFlag"));
	UNTEST_ASSERT_PTR(ExposedPin);

	// Connect the source var's output straight into it -- no
	// bp_reconstruct_node call happened in between.
	{
		ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("source_node_guid"), SourceVarGet->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("source_pin_name"), TEXT("SourceFlag"));
		Args->SetStringField(TEXT("target_node_guid"), SpawnNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("target_pin_name"), TEXT("ExposedFlag"));
		IClaireonTool::FToolResult R = ConnectTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	UEdGraphPin* SourceOutPin = WST_FindPin(SourceVarGet, TEXT("SourceFlag"));
	UNTEST_ASSERT_PTR(SourceOutPin);
	UNTEST_EXPECT_TRUE(ExposedPin->LinkedTo.Contains(SourceOutPin));

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);
	co_return;
}

// ============================================================================
// F7 negative (RESOLVED): a dotless Blueprint asset path on the Class pin
// resolves to the UBlueprint asset object itself (not its generated UClass),
// which the engine schema rejects with "Literal on pin Class is not a
// class." -- a hard error, not stale pins.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SpawnActorClassPin_DotlessBlueprintPath_LiteralNotAClassError, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* TargetAssetPath = TEXT("/Game/__MCPTests/BP_WST_F7NegTarget");
	static const TCHAR* MainAssetPath = TEXT("/Game/__MCPTests/BP_WST_F7NegMain");

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);

	FString TargetSessionId = WST_CreateAndOpenSession(TargetAssetPath);
	UNTEST_ASSERT_FALSE(TargetSessionId.IsEmpty());
	FClaireonSessionManager::Get().ReleaseByAssetPath(TargetAssetPath);

	FString MainSessionId = WST_CreateAndOpenSession(MainAssetPath);
	UNTEST_ASSERT_FALSE(MainSessionId.IsEmpty());

	UBlueprint* MainBP = Cast<UBlueprint>(FSoftObjectPath(
		FString(MainAssetPath) + TEXT(".") + FPackageName::GetShortName(MainAssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(MainBP);

	UK2Node_SpawnActorFromClass* SpawnNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));
		Args->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		SpawnNode = WST_FindEventGraphNodeOfClass<UK2Node_SpawnActorFromClass>(MainBP);
		UNTEST_ASSERT_PTR(SpawnNode);
	}

	// Dotless -- no '.' and no '_C' class-object suffix -- resolves (via the
	// package-path shorthand) to the UBlueprint asset object, not a UClass.
	{
		ClaireonBlueprintGraphTool_SetPinValue SetPinTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_guid"), SpawnNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("pin_name"), TEXT("Class"));
		Args->SetStringField(TEXT("value"), TargetAssetPath);
		IClaireonTool::FToolResult R = SetPinTool.Execute(Args);
		UNTEST_ASSERT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("is not a class")));
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Class")));
	}

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);
	co_return;
}

// ============================================================================
// B-1 (Stage 004): writing a literal into an unconnected wildcard element pin
// promotes the pin (and its container siblings + output pin) instead of
// failing engine validation. This is the Stage 001 B3 pin, updated in place to
// assert the post-fix behavior.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MakeArrayWildcardSet_PromotesToRealFromLiteral, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteReal");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	UEdGraphPin* ElementPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(ElementPin);
	UNTEST_ASSERT_TRUE(ElementPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard);

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, MakeArrayNode, TEXT("[0]"), TEXT("0.25"));

	// Promotion, not the pre-fix generic validation error.
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[WriteSafety] set_pin_value failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(WST_HasWarningContaining(R, TEXT("promoted wildcard")));

	// Pin arrays can be rebuilt by node notifications; re-find rather than reuse.
	UEdGraphPin* PromotedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinSubCategory == UEdGraphSchema_K2::PC_Double);
	UNTEST_EXPECT_TRUE(PromotedPin->DefaultValue == TEXT("0.25"));

	// Sibling element pin promoted too.
	UEdGraphPin* SiblingPin = WST_FindPin(MakeArrayNode, TEXT("[1]"));
	UNTEST_ASSERT_PTR(SiblingPin);
	UNTEST_EXPECT_TRUE(SiblingPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real);

	// Output array pin promoted, container kind preserved.
	UEdGraphPin* OutputPin = MakeArrayNode->GetOutputPin();
	UNTEST_ASSERT_PTR(OutputPin);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.ContainerType == EPinContainerType::Array);

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MakeArrayWildcardSet_PromotesToBool, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteBool");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, MakeArrayNode, TEXT("[0]"), TEXT("true"));
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphPin* PromotedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean);
	UNTEST_EXPECT_TRUE(PromotedPin->DefaultValue == TEXT("true"));

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MakeArrayWildcardSet_Int64OverflowPromotesToInt64, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteInt64");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	// Exceeds int32 -> must land on PC_Int64, not PC_Int.
	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, MakeArrayNode, TEXT("[0]"), TEXT("99999999999"));
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphPin* PromotedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64);

	// And an in-range integer still lands on PC_Int.
	static const TCHAR* SmallAssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteInt32");
	WST_CleanupAsset(SmallAssetPath);
	FString SmallSessionId = WST_CreateAndOpenSession(SmallAssetPath);
	UNTEST_ASSERT_FALSE(SmallSessionId.IsEmpty());
	UBlueprint* SmallBP = Cast<UBlueprint>(FSoftObjectPath(
		FString(SmallAssetPath) + TEXT(".") + FPackageName::GetShortName(SmallAssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(SmallBP);
	UK2Node_MakeArray* SmallNode = WST_AddMakeArrayWithTwoElements(SmallSessionId, SmallBP);
	UNTEST_ASSERT_PTR(SmallNode);
	IClaireonTool::FToolResult SmallR = WST_SetPinValue(SmallSessionId, SmallNode, TEXT("[0]"), TEXT("42"));
	UNTEST_ASSERT_FALSE(SmallR.bIsError);
	UEdGraphPin* SmallPin = WST_FindPin(SmallNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(SmallPin);
	UNTEST_EXPECT_TRUE(SmallPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);

	WST_CleanupAsset(SmallAssetPath);
	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MakeSetWildcardSet_PromotesElementAndOutput, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteSet");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeSet* MakeSetNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("MakeSet"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		MakeSetNode = WST_FindEventGraphNodeOfClass<UK2Node_MakeSet>(BP);
	}
	UNTEST_ASSERT_PTR(MakeSetNode);

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, MakeSetNode, TEXT("[0]"), TEXT("7"));
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphPin* PromotedPin = WST_FindPin(MakeSetNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);

	UEdGraphPin* OutputPin = MakeSetNode->GetOutputPin();
	UNTEST_ASSERT_PTR(OutputPin);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.ContainerType == EPinContainerType::Set);

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MakeMapWildcardSet_KeyAndValueHalvesPromoteIndependently, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteMap");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeMap* MakeMapNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("MakeMap"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		MakeMapNode = WST_FindEventGraphNodeOfClass<UK2Node_MakeMap>(BP);
	}
	UNTEST_ASSERT_PTR(MakeMapNode);

	TArray<UEdGraphPin*> KeyPins;
	TArray<UEdGraphPin*> ValuePins;
	MakeMapNode->GetKeyAndValuePins(KeyPins, ValuePins);
	UNTEST_ASSERT_TRUE(KeyPins.Num() >= 1);
	UNTEST_ASSERT_TRUE(ValuePins.Num() >= 1);

	const FString KeyPinName = KeyPins[0]->PinName.ToString();
	const FString ValuePinName = ValuePins[0]->PinName.ToString();

	// Key half: a string literal.
	IClaireonTool::FToolResult KeyR = WST_SetPinValue(SessionId, MakeMapNode, *KeyPinName, TEXT("alpha"));
	UNTEST_ASSERT_FALSE(KeyR.bIsError);
	// Value half: an integer literal, so the two halves are distinguishable.
	IClaireonTool::FToolResult ValueR = WST_SetPinValue(SessionId, MakeMapNode, *ValuePinName, TEXT("7"));
	UNTEST_ASSERT_FALSE(ValueR.bIsError);

	UEdGraphPin* OutputPin = MakeMapNode->GetOutputPin();
	UNTEST_ASSERT_PTR(OutputPin);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.ContainerType == EPinContainerType::Map);
	// Key type lives on the pin type proper, value type on PinValueType.
	UNTEST_EXPECT_TRUE(OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Int);

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, WildcardSet_ObjectPathLiteralPromotesToObject, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteObject");
	// A real asset to point at: the fixture Blueprint from the case above is gone
	// by now, so use this test's own Blueprint as the referenced asset.
	static const TCHAR* TargetAssetPath = TEXT("/Game/__MCPTests/BP_WST_B1ObjectTarget");

	WST_CleanupAsset(AssetPath);
	WST_CleanupAsset(TargetAssetPath);

	const FString TargetSession = WST_CreateAndOpenSession(TargetAssetPath);
	UNTEST_ASSERT_FALSE(TargetSession.IsEmpty());
	{
		// Save so the asset registry knows about it before we reference it by path.
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSession);
		IClaireonTool::FToolResult R = SaveTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, MakeArrayNode, TEXT("[0]"), TargetAssetPath);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphPin* PromotedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinSubCategoryObject.IsValid());

	WST_CleanupAsset(AssetPath);
	WST_CleanupAsset(TargetAssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, WildcardSet_UnresolvableObjectPathFallsBackToString, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1PromoteFakePath");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	// Object-path shaped but names no real asset -> string promotion + ambiguity warning.
	IClaireonTool::FToolResult R = WST_SetPinValue(
		SessionId, MakeArrayNode, TEXT("[0]"), TEXT("/Game/__MCPTests/NoSuchAsset_XYZ"));
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(WST_HasWarningContaining(R, TEXT("looked like an object path")));

	UEdGraphPin* PromotedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String);

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, NonWildcardPinWrite_Unaffected, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1NonWildcard");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	// PrintString's InString is a concrete FString pin -- the wildcard branch must
	// not run and must not add a promotion warning.
	UEdGraphNode* PrintNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		Args->SetStringField(TEXT("class_name"), TEXT("KismetSystemLibrary"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		PrintNode = WST_FindEventGraphNodeOfClass<UK2Node_CallFunction>(BP);
	}
	UNTEST_ASSERT_PTR(PrintNode);

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, PrintNode, TEXT("InString"), TEXT("hello"));
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_FALSE(WST_HasWarningContaining(R, TEXT("promoted wildcard")));

	UEdGraphPin* InStringPin = WST_FindPin(PrintNode, TEXT("InString"));
	UNTEST_ASSERT_PTR(InStringPin);
	UNTEST_EXPECT_TRUE(InStringPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String);
	UNTEST_EXPECT_TRUE(InStringPin->DefaultValue == TEXT("hello"));

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, ConnectFirstWildcardResolution_StillWorksAfterExtraction, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1ConnectFirst");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	// Regression for the PropagateWildcardTypesViaLinks extraction: connecting a
	// typed output into a wildcard element pin must still resolve it.
	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	UEdGraphNode* PrintNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("GetGameTimeInSeconds"));
		Args->SetStringField(TEXT("class_name"), TEXT("GameplayStatics"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		PrintNode = WST_FindEventGraphNodeOfClass<UK2Node_CallFunction>(BP);
	}
	UNTEST_ASSERT_PTR(PrintNode);

	{
		ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("source_node_guid"), PrintNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("source_pin_name"), TEXT("ReturnValue"));
		Args->SetStringField(TEXT("target_node_guid"), MakeArrayNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("target_pin_name"), TEXT("[0]"));
		IClaireonTool::FToolResult R = ConnectTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[WriteSafety] connect_pins failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	UEdGraphPin* ResolvedPin = WST_FindPin(MakeArrayNode, TEXT("[0]"));
	UNTEST_ASSERT_PTR(ResolvedPin);
	UNTEST_EXPECT_TRUE(ResolvedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard);

	UEdGraphPin* OutputPin = MakeArrayNode->GetOutputPin();
	UNTEST_ASSERT_PTR(OutputPin);
	UNTEST_EXPECT_TRUE(OutputPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard);

	WST_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, NonContainerWildcardPin_PromotesViaDirectSetFallback, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B1SelectWildcard");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	// A Select node's option pins are wildcards but the node is NOT a
	// UK2Node_MakeContainer: the Cast miss must fall back to a direct set with no
	// sibling walk and no crash.
	UK2Node_Select* SelectNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("Select"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		SelectNode = WST_FindEventGraphNodeOfClass<UK2Node_Select>(BP);
	}
	UNTEST_ASSERT_PTR(SelectNode);

	// Find any unconnected wildcard input pin on the node.
	FString WildcardPinName;
	for (UEdGraphPin* Pin : SelectNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
			&& Pin->LinkedTo.Num() == 0)
		{
			WildcardPinName = Pin->PinName.ToString();
			break;
		}
	}
	UNTEST_ASSERT_FALSE(WildcardPinName.IsEmpty());

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, SelectNode, *WildcardPinName, TEXT("0.5"));
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(WST_HasWarningContaining(R, TEXT("promoted wildcard")));

	UEdGraphPin* PromotedPin = WST_FindPin(SelectNode, *WildcardPinName);
	UNTEST_ASSERT_PTR(PromotedPin);
	UNTEST_EXPECT_TRUE(PromotedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard);

	WST_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// B-3 (Stage 004): a previous op's status must not be echoed as this op's
// status, and move_node must report a status of its own.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, MoveNodeAfterOtherOp_NoStaleStatusLeak, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_B3StaleStatus");

	WST_CleanupAsset(AssetPath);
	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	// Op 1: add a node, which writes its own status.
	UK2Node_MakeArray* MakeArrayNode = WST_AddMakeArrayWithTwoElements(SessionId, BP);
	UNTEST_ASSERT_PTR(MakeArrayNode);

	// Op 2: move it. Its response must carry the move status, not op 1's.
	ClaireonBlueprintGraphTool_MoveNode MoveTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("node_guid"), MakeArrayNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	Args->SetNumberField(TEXT("position_x"), 512);
	Args->SetNumberField(TEXT("position_y"), 256);
	Args->SetStringField(TEXT("response_mode"), TEXT("status"));
	IClaireonTool::FToolResult R = MoveTool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	// move_node now reports its own status...
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("Moved node")) || WST_ResponseTextContains(R, TEXT("Moved node")));
	UNTEST_EXPECT_TRUE(WST_ResponseTextContains(R, TEXT("512")));
	// ...and does not echo the add_node status left in the shared session data.
	UNTEST_EXPECT_FALSE(WST_ResponseTextContains(R, TEXT("Added ")));

	UNTEST_EXPECT_EQ(MakeArrayNode->NodePosX, 512);
	UNTEST_EXPECT_EQ(MakeArrayNode->NodePosY, 256);

	WST_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// B-2 (Stage 005): object-reference leaf writes canonicalize a dotless /Game
// path to Package.Object, or reject naming the canonical form. A dotless soft
// path used to import "successfully" as a package-only reference that never
// resolved -- the write reported success and produced a dangling pointer.
// ============================================================================
namespace ClaireonB2TestsInternal
{
	static const TCHAR* kB2TargetAsset = TEXT("/Game/__MCPTests/BP_B2RefTarget");
	static const TCHAR* kB2HolderPackage = TEXT("/Game/__MCPTests/DA_B2Holder");

	/** Create + save a Blueprint to act as the referenced asset. Empty on failure. */
	static FString B2_MakeReferencedAsset()
	{
		ClaireonWSTTestsInternal::WST_CleanupAsset(kB2TargetAsset);
		const FString SessionId = ClaireonWSTTestsInternal::WST_CreateAndOpenSession(kB2TargetAsset);
		if (SessionId.IsEmpty()) { return FString(); }

		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		if (SaveTool.Execute(Args).bIsError) { return FString(); }

		FClaireonSessionManager::Get().ReleaseByAssetPath(kB2TargetAsset);
		return kB2TargetAsset;
	}

	/** A saved-to-disk holder data asset carrying the soft/hard ref properties. */
	static UClaireonSpecDataAsset* B2_MakeHolderAsset()
	{
		UPackage* Package = CreatePackage(kB2HolderPackage);
		if (!IsValid(Package)) { return nullptr; }

		const FString AssetName = FPackageName::GetShortName(FString(kB2HolderPackage));
		UClaireonSpecDataAsset* Holder = NewObject<UClaireonSpecDataAsset>(
			Package, UClaireonSpecDataAsset::StaticClass(), FName(*AssetName),
			RF_Public | RF_Standalone);
		if (!IsValid(Holder)) { return nullptr; }
		FAssetRegistryModule::AssetCreated(Holder);
		return Holder;
	}

	static bool B2_SaveHolder(UClaireonSpecDataAsset* Holder)
	{
		if (!IsValid(Holder)) { return false; }
		UPackage* Package = Holder->GetOutermost();
		Package->MarkPackageDirty();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			FString(kB2HolderPackage), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::Save(Package, Holder, *FileName, SaveArgs).IsSuccessful();
	}

	static void B2_Cleanup()
	{
		ClaireonWSTTestsInternal::WST_CleanupAsset(kB2HolderPackage);
		ClaireonWSTTestsInternal::WST_CleanupAsset(kB2TargetAsset);
	}
} // namespace ClaireonB2TestsInternal

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SoftObjectDotlessPath_CanonicalizedAndSurvivesFreshLoad, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;
	using namespace ClaireonB2TestsInternal;

	B2_Cleanup();

	const FString TargetPath = B2_MakeReferencedAsset();
	UNTEST_ASSERT_FALSE(TargetPath.IsEmpty());

	UClaireonSpecDataAsset* Holder = B2_MakeHolderAsset();
	UNTEST_ASSERT_PTR(Holder);

	// Dotless path -- the form that used to store package-only and never resolve.
	FString WriteError;
	const bool bWrote = ClaireonPropertyUtils::WritePropertyByPath(
		Holder, TEXT("Speaker"), TargetPath, WriteError);
	if (!bWrote)
	{
		UE_LOG(LogTemp, Error, TEXT("[WriteSafety] soft-ref write failed: %s"), *WriteError);
	}
	UNTEST_ASSERT_TRUE(bWrote);

	// Canonicalized: AssetName is populated, not NAME_None.
	const FSoftObjectPath StoredPath = Holder->Speaker.ToSoftObjectPath();
	UNTEST_EXPECT_FALSE(StoredPath.GetAssetName().IsEmpty());
	UNTEST_EXPECT_TRUE(StoredPath.ToString().Contains(TEXT(".")));

	const FString ExpectedPath = TargetPath + TEXT(".") + FPackageName::GetShortName(TargetPath);
	UNTEST_EXPECT_TRUE(StoredPath.ToString() == ExpectedPath);
	UNTEST_EXPECT_PTR(StoredPath.TryLoad());

	// Survives save + a genuine fresh load (package unloaded, then re-resolved).
	UNTEST_ASSERT_TRUE(B2_SaveHolder(Holder));

	UPackage* HolderPackage = Holder->GetOutermost();
	Holder = nullptr;
	TArray<UPackage*> ToUnload;
	ToUnload.Add(HolderPackage);
	UPackageTools::FUnloadPackageParams UnloadParams(ToUnload);
	UPackageTools::UnloadPackages(UnloadParams);
	CollectGarbage(RF_NoFlags);

	const FString HolderObjectPath = FString(kB2HolderPackage) + TEXT(".")
		+ FPackageName::GetShortName(FString(kB2HolderPackage));
	UClaireonSpecDataAsset* Reloaded =
		Cast<UClaireonSpecDataAsset>(FSoftObjectPath(HolderObjectPath).TryLoad());
	UNTEST_ASSERT_PTR(Reloaded);

	const FSoftObjectPath ReloadedPath = Reloaded->Speaker.ToSoftObjectPath();
	UNTEST_EXPECT_FALSE(ReloadedPath.GetAssetName().IsEmpty());
	UNTEST_EXPECT_TRUE(ReloadedPath.ToString() == ExpectedPath);
	UNTEST_EXPECT_PTR(ReloadedPath.TryLoad());

	B2_Cleanup();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SoftObjectDotlessPath_NonexistentAsset_SpecificErrorNoWrite, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonB2TestsInternal;

	B2_Cleanup();

	UClaireonSpecDataAsset* Holder = B2_MakeHolderAsset();
	UNTEST_ASSERT_PTR(Holder);
	UNTEST_ASSERT_TRUE(Holder->Speaker.IsNull());

	FString WriteError;
	const bool bWrote = ClaireonPropertyUtils::WritePropertyByPath(
		Holder, TEXT("Speaker"), TEXT("/Game/__MCPTests/NoSuchAsset_B2"), WriteError);

	// Rejected, with the canonical form named so the caller can fix the call.
	UNTEST_EXPECT_FALSE(bWrote);
	UNTEST_EXPECT_TRUE(WriteError.Contains(TEXT("Package.Object form")));
	UNTEST_EXPECT_TRUE(WriteError.Contains(TEXT("/Game/__MCPTests/NoSuchAsset_B2.NoSuchAsset_B2")));
	// And nothing was written.
	UNTEST_EXPECT_TRUE(Holder->Speaker.IsNull());

	B2_Cleanup();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, HardObjectDotlessPath_StillResolves, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonB2TestsInternal;

	B2_Cleanup();

	const FString TargetPath = B2_MakeReferencedAsset();
	UNTEST_ASSERT_FALSE(TargetPath.IsEmpty());

	UClaireonSpecDataAsset* Holder = B2_MakeHolderAsset();
	UNTEST_ASSERT_PTR(Holder);

	// Hard refs were already correct (the engine appends the leaf name itself);
	// this pins that canonicalization did not regress them.
	FString WriteError;
	const bool bWrote = ClaireonPropertyUtils::WritePropertyByPath(
		Holder, TEXT("HardSpeaker"), TargetPath, WriteError);
	if (!bWrote)
	{
		UE_LOG(LogTemp, Error, TEXT("[WriteSafety] hard-ref write failed: %s"), *WriteError);
	}
	UNTEST_ASSERT_TRUE(bWrote);
	UNTEST_EXPECT_PTR(Holder->HardSpeaker.Get());

	B2_Cleanup();
	co_return;
}

// ============================================================================
// Split-pin defaults (Work #6704, split-pin report): a parent-pin write on a
// pin that has been split into sub-pins used to "succeed" while the compiler
// read the untouched sub-pin zeros. bp_set_pin_value now distributes the
// struct literal onto the sub-pins.
// ============================================================================
namespace ClaireonSplitPinTestsInternal
{
	using namespace ClaireonWSTTestsInternal;

	// Add a CallFunction node and return it. nullptr on failure.
	static UEdGraphNode* SPT_AddCallFunctionNode(const FString& SessionId, UBlueprint* BP,
		const TCHAR* FunctionClass, const TCHAR* FunctionName)
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_class"), FunctionClass);
		Args->SetStringField(TEXT("function_name"), FunctionName);
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[SplitPin] add_node %s.%s failed: %s"),
				FunctionClass, FunctionName, *R.ErrorMessage);
			return nullptr;
		}
		return WST_FindEventGraphNodeOfClass<UK2Node_CallFunction>(BP);
	}

	// Split an unconnected struct input pin via the schema (what the editor's
	// context menu does). Memory rule: only ever split UNCONNECTED pins.
	static bool SPT_SplitPin(UEdGraphNode* Node, const TCHAR* PinName)
	{
		UEdGraphPin* Pin = WST_FindPin(Node, PinName);
		if (!Pin || Pin->LinkedTo.Num() > 0 || Pin->SubPins.Num() > 0)
		{
			return false;
		}
		GetDefault<UEdGraphSchema_K2>()->SplitPin(Pin, /*bNotify*/false);
		return Pin->SubPins.Num() > 0;
	}

	static double SPT_PinDefaultAsDouble(UEdGraphNode* Node, const TCHAR* PinName)
	{
		UEdGraphPin* Pin = WST_FindPin(Node, PinName);
		return Pin ? FCString::Atod(*Pin->DefaultValue) : TNumericLimits<double>::Max();
	}
} // namespace ClaireonSplitPinTestsInternal

// Named form "(X=..,Y=..,Z=..)" distributes by member onto a split vector pin,
// and the response says so instead of silently writing the dead parent string.
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SplitVectorParentWrite_NamedFormDistributesToSubPins, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;
	using namespace ClaireonSplitPinTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_SplitVector");
	WST_CleanupAsset(AssetPath);

	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UEdGraphNode* Node = SPT_AddCallFunctionNode(SessionId, BP,
		TEXT("KismetSystemLibrary"), TEXT("DrawDebugLine"));
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_TRUE(SPT_SplitPin(Node, TEXT("LineStart")));

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, Node,
		TEXT("LineStart"), TEXT("(X=20.0,Y=40.0,Z=0.05)"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SplitPin] parent write failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(WST_HasWarningContaining(R, TEXT("distributed")));

	// The values the compiler will actually read live on the sub-pins.
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("LineStart_X")), 20.0);
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("LineStart_Y")), 40.0);
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("LineStart_Z")), 0.05);

	WST_CleanupAsset(AssetPath);
	co_return;
}

// Bare form "10,20,30" on a split ROTATOR pin maps by the engine's own bare
// parse order (Pitch,Yaw,Roll -- FDefaultValueHelper::ParseRotator), NOT the
// Roll,Pitch,Yaw order the sub-pins are listed in. Same literal, same meaning,
// split or unsplit.
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SplitRotatorParentWrite_BareFormUsesEngineComponentOrder, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;
	using namespace ClaireonSplitPinTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_SplitRotator");
	WST_CleanupAsset(AssetPath);

	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UEdGraphNode* Node = SPT_AddCallFunctionNode(SessionId, BP,
		TEXT("KismetMathLibrary"), TEXT("GetForwardVector"));
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_TRUE(SPT_SplitPin(Node, TEXT("InRot")));

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, Node, TEXT("InRot"), TEXT("10,20,30"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SplitPin] rotator parent write failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("InRot_Pitch")), 10.0);
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("InRot_Yaw")), 20.0);
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("InRot_Roll")), 30.0);

	WST_CleanupAsset(AssetPath);
	co_return;
}

// A member name with no matching sub-pin is a hard error that names the valid
// members, and NOTHING is written (all-leaves-validate-before-any-write).
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, SplitParentWrite_UnknownMemberErrorsAndWritesNothing, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;
	using namespace ClaireonSplitPinTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_WST_SplitUnknown");
	WST_CleanupAsset(AssetPath);

	FString SessionId = WST_CreateAndOpenSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UEdGraphNode* Node = SPT_AddCallFunctionNode(SessionId, BP,
		TEXT("KismetSystemLibrary"), TEXT("DrawDebugLine"));
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_TRUE(SPT_SplitPin(Node, TEXT("LineEnd")));

	IClaireonTool::FToolResult R = WST_SetPinValue(SessionId, Node,
		TEXT("LineEnd"), TEXT("(X=7.0,Q=5.0)"));
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("no sub-pin for member 'Q'")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Available members")));

	// X validated fine but must NOT have been written -- the write set is
	// all-or-nothing.
	UNTEST_EXPECT_EQ(SPT_PinDefaultAsDouble(Node, TEXT("LineEnd_X")), 0.0);

	WST_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// F7 latent variant: the same class-pin -> spawn-param-pin refresh holds for
// K2Node_LatentGameplayTaskCall (the node the #6704 sessions actually hit on
// 'Spawn Actor for Gameplay Task'), with no bp_reconstruct_node in between.
// The node class is engine-editor-module territory, so it is located by class
// NAME, never Cast<T> (same rule as the NodeCreation A1 tests).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackWriteSafety, LatentTaskClassPinRefresh_ExposeOnSpawnPinExistsSameCall, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonWSTTestsInternal;

	static const TCHAR* TargetAssetPath = TEXT("/Game/__MCPTests/BP_WST_LatentTarget");
	static const TCHAR* MainAssetPath = TEXT("/Game/__MCPTests/BP_WST_LatentMain");

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);

	// Target BP: one bool member var flagged ExposeOnSpawn, compiled+saved.
	FString TargetSessionId = WST_CreateAndOpenSession(TargetAssetPath);
	UNTEST_ASSERT_FALSE(TargetSessionId.IsEmpty());
	{
		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("ExposedFlag"));
		Args->SetStringField(TEXT("variable_type"), TEXT("bool"));
		IClaireonTool::FToolResult R = AddVarTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	{
		ClaireonBlueprintGraphTool_SetVariableProperties SetVarPropsTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("ExposedFlag"));
		TArray<TSharedPtr<FJsonValue>> Flags;
		Flags.Add(MakeShared<FJsonValueString>(TEXT("ExposeOnSpawn")));
		Args->SetArrayField(TEXT("flags"), Flags);
		IClaireonTool::FToolResult R = SetVarPropsTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	{
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TargetSessionId);
		IClaireonTool::FToolResult R = SaveTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	FClaireonSessionManager::Get().ReleaseByAssetPath(TargetAssetPath);

	const FString TargetClassPath = FString(TargetAssetPath) + TEXT(".") + FPackageName::GetShortName(TargetAssetPath) + TEXT("_C");

	// Main BP: a latent task node from the spawn-flavored fixture factory
	// (its TSubclassOf param named 'Class' is what creates the Class pin).
	FString MainSessionId = WST_CreateAndOpenSession(MainAssetPath);
	UNTEST_ASSERT_FALSE(MainSessionId.IsEmpty());
	UBlueprint* MainBP = Cast<UBlueprint>(FSoftObjectPath(
		FString(MainAssetPath) + TEXT(".") + FPackageName::GetShortName(MainAssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(MainBP);

	UEdGraphNode* LatentNode = nullptr;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), MainSessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("LatentGameplayTaskCall"));
		Args->SetStringField(TEXT("function_name"), TEXT("ClaireonTestSpawnThing"));
		Args->SetStringField(TEXT("function_class"), TEXT("ClaireonTestGameplayTask"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[WriteSafety] latent add_node failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);

		UEdGraph* EventGraph = WST_FindEventGraph(MainBP);
		UNTEST_ASSERT_PTR(EventGraph);
		for (UEdGraphNode* Candidate : EventGraph->Nodes)
		{
			if (Candidate && Candidate->GetClass()->GetName() == TEXT("K2Node_LatentGameplayTaskCall"))
			{
				LatentNode = Candidate;
				break;
			}
		}
		UNTEST_ASSERT_PTR(LatentNode);
	}

	// No ExposeOnSpawn pin before the class is set.
	UNTEST_EXPECT_TRUE(WST_NodeHasPin(LatentNode, TEXT("Class")));
	UNTEST_EXPECT_FALSE(WST_NodeHasPin(LatentNode, TEXT("ExposedFlag")));

	// Set the Class pin; the spawn-param pin must exist on the SAME call.
	{
		IClaireonTool::FToolResult R = WST_SetPinValue(MainSessionId, LatentNode,
			TEXT("Class"), *TargetClassPath);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[WriteSafety] latent class-pin write failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	UNTEST_EXPECT_TRUE(WST_NodeHasPin(LatentNode, TEXT("ExposedFlag")));

	WST_CleanupAsset(TargetAssetPath);
	WST_CleanupAsset(MainAssetPath);
	co_return;
}

#endif // WITH_UNTESTED
