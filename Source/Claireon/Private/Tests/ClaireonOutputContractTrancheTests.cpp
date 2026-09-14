// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test delegate type round trips, replacement of graph-island output, and graph-kind wire values.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonGraphIslands.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace ClaireonOutputContractTrancheTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** Read AActor::OnDestroyed through stock reflection to exercise a package-owned native delegate signature. */
	static bool OCT_MakeNativeDelegatePinType(FEdGraphPinType& OutPinType, UFunction*& OutSignature)
	{
		const FMulticastDelegateProperty* Prop = CastField<FMulticastDelegateProperty>(
			AActor::StaticClass()->FindPropertyByName(FName(TEXT("OnDestroyed"))));
		if (!Prop)
		{
			return false;
		}
		OutSignature = Prop->SignatureFunction;
		if (!IsValid(OutSignature))
		{
			return false;
		}
		return GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Prop, OutPinType);
	}

	/** A transient graph with N unconnected non-comment nodes, so N islands. */
	static UEdGraph* OCT_MakeGraphWithSingletons(int32 Count)
	{
		UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		Graph->Schema = UEdGraphSchema_K2::StaticClass();
		for (int32 I = 0; I < Count; ++I)
		{
			UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transient);
			if (!IsValid(Node))
			{
				continue;
			}
			Node->NodeGuid = FGuid::NewGuid();
			Node->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Int, FName(TEXT("In")));
			Graph->Nodes.Add(Node);
		}
		return Graph;
	}
}

// Delegate format/parse round trip.

UNTEST_UNIT_OPTS(Claireon, OutputContract, Delegate_TypeStringRoundTrips, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonOutputContractTrancheTestsNS;

	FEdGraphPinType PinType;
	UFunction* Signature = nullptr;
	UNTEST_ASSERT_TRUE(OCT_MakeNativeDelegatePinType(PinType, Signature));
	UNTEST_ASSERT_TRUE(PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);

	const FString Formatted = ClaireonBlueprintHelpers::FormatVariableTypeString(PinType);
	UNTEST_EXPECT_TRUE(Formatted.StartsWith(TEXT("MCDelegate<")));
	UNTEST_EXPECT_TRUE(Formatted.EndsWith(TEXT(">")));
	UNTEST_EXPECT_TRUE(Formatted.Contains(Signature->GetName()));

	const ClaireonBlueprintHelpers::FParseVariableTypeResult Parsed =
		ClaireonBlueprintHelpers::ParseVariableTypeChecked(Formatted);
	UNTEST_ASSERT_TRUE(Parsed.bSucceeded);
	UNTEST_EXPECT_TRUE(Parsed.PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);

	const UFunction* Reresolved = ClaireonBlueprintHelpers::ResolveDelegateSignatureFunction(Parsed.PinType);
	UNTEST_ASSERT_TRUE(IsValid(Reresolved));
	UNTEST_EXPECT_TRUE(Reresolved == Signature);

	UNTEST_EXPECT_TRUE(ClaireonBlueprintHelpers::FormatVariableTypeString(Parsed.PinType) == Formatted);
	co_return;
}

// Bare delegates remain invalid without a signature.
UNTEST_UNIT_OPTS(Claireon, OutputContract, Delegate_BareWordIsStillRefused, UNTEST_TIMEOUTMS(60000))
{
	for (const TCHAR* Bare : {TEXT("delegate"), TEXT("mcdelegate"), TEXT("dispatcher"),
	                          TEXT("multicastdelegate"), TEXT("singledelegate")})
	{
		const ClaireonBlueprintHelpers::FParseVariableTypeResult Result =
			ClaireonBlueprintHelpers::ParseVariableTypeChecked(Bare);
		UNTEST_EXPECT_FALSE(Result.bSucceeded);
		UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("signature_function")));
	}

	const ClaireonBlueprintHelpers::FParseVariableTypeResult Empty =
		ClaireonBlueprintHelpers::ParseVariableTypeChecked(TEXT("MCDelegate<>"));
	UNTEST_EXPECT_FALSE(Empty.bSucceeded);

	const ClaireonBlueprintHelpers::FParseVariableTypeResult Bogus =
		ClaireonBlueprintHelpers::ParseVariableTypeChecked(
			TEXT("MCDelegate</Script/Engine.Actor.NoSuchThing__DelegateSignature>"));
	UNTEST_EXPECT_FALSE(Bogus.bSucceeded);
	UNTEST_EXPECT_TRUE(Bogus.Error.Contains(TEXT("could not be resolved")));
	co_return;
}

// Accept the same aliases in angle-form types.
UNTEST_UNIT_OPTS(Claireon, OutputContract, Delegate_AngleFormAcceptsEveryAlias, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonOutputContractTrancheTestsNS;

	FEdGraphPinType PinType;
	UFunction* Signature = nullptr;
	UNTEST_ASSERT_TRUE(OCT_MakeNativeDelegatePinType(PinType, Signature));
	const FString Path = Signature->GetPathName();

	struct FAliasCase { const TCHAR* Alias; bool bMulticast; };
	const FAliasCase Cases[] = {
		{TEXT("MCDelegate"), true},
		{TEXT("Dispatcher"), true},
		{TEXT("MulticastDelegate"), true},
		{TEXT("MulticastInlineDelegate"), true},
		{TEXT("Delegate"), false},
		{TEXT("SingleDelegate"), false},
	};

	for (const FAliasCase& Case : Cases)
	{
		const FString Spelling = FString::Printf(TEXT("%s<%s>"), Case.Alias, *Path);
		const ClaireonBlueprintHelpers::FParseVariableTypeResult Result =
			ClaireonBlueprintHelpers::ParseVariableTypeChecked(Spelling);
		UNTEST_ASSERT_TRUE(Result.bSucceeded);
		UNTEST_EXPECT_TRUE(Result.PinType.PinCategory ==
			(Case.bMulticast ? UEdGraphSchema_K2::PC_MCDelegate : UEdGraphSchema_K2::PC_Delegate));
	}
	co_return;
}

// Require signature metadata and an explicit round-trip verdict.
UNTEST_UNIT_OPTS(Claireon, OutputContract, Delegate_TypeJsonStatesRoundTripStatus,
	UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonOutputContractTrancheTestsNS;

	FEdGraphPinType PinType;
	UFunction* Signature = nullptr;
	UNTEST_ASSERT_TRUE(OCT_MakeNativeDelegatePinType(PinType, Signature));

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	ClaireonBlueprintHelpers::WriteVariableTypeJson(PinType, Out);

	bool bRoundTrips = false;
	UNTEST_ASSERT_TRUE(Out->TryGetBoolField(TEXT("type_round_trips"), bRoundTrips));
	UNTEST_EXPECT_TRUE(bRoundTrips);

	FString SignaturePath;
	UNTEST_ASSERT_TRUE(Out->TryGetStringField(TEXT("signature_function"), SignaturePath));
	UNTEST_EXPECT_TRUE(SignaturePath == Signature->GetPathName());

	const TSharedPtr<FJsonObject>* Spec = nullptr;
	UNTEST_ASSERT_TRUE(Out->TryGetObjectField(TEXT("variable_type_spec"), Spec) && Spec);
	FString Base, SpecSignature;
	UNTEST_EXPECT_TRUE((*Spec)->TryGetStringField(TEXT("base"), Base));
	UNTEST_EXPECT_TRUE(Base == TEXT("mcdelegate"));
	UNTEST_EXPECT_TRUE((*Spec)->TryGetStringField(TEXT("signature_function"), SpecSignature));
	UNTEST_EXPECT_TRUE(SpecSignature == SignaturePath);

	const ClaireonBlueprintHelpers::FParseVariableTypeResult FromSpec =
		ClaireonBlueprintHelpers::ParseVariableTypeSpec(*Spec);
	UNTEST_EXPECT_TRUE(FromSpec.bSucceeded);

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	TSharedPtr<FJsonObject> IntOut = MakeShared<FJsonObject>();
	ClaireonBlueprintHelpers::WriteVariableTypeJson(IntType, IntOut);
	bool bIntRoundTrips = false;
	UNTEST_EXPECT_TRUE(IntOut->TryGetBoolField(TEXT("type_round_trips"), bIntRoundTrips));
	UNTEST_EXPECT_TRUE(bIntRoundTrips);
	UNTEST_EXPECT_FALSE(IntOut->HasField(TEXT("signature_function")));
	UNTEST_EXPECT_FALSE(IntOut->HasField(TEXT("variable_type_spec")));
	co_return;
}

// Build replaces prior output, including for invalid graphs.

UNTEST_UNIT_OPTS(Claireon, OutputContract, GraphIslands_BuildReplacesOutput, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonOutputContractTrancheTestsNS;

	UEdGraph* ThreeNodes = OCT_MakeGraphWithSingletons(3);
	UEdGraph* TwoNodes = OCT_MakeGraphWithSingletons(2);
	UNTEST_ASSERT_TRUE(IsValid(ThreeNodes) && IsValid(TwoNodes));

	TArray<ClaireonGraphIslands::FIsland> Islands;
	ClaireonGraphIslands::Build(ThreeNodes, Islands);
	UNTEST_ASSERT_EQ(Islands.Num(), 3);

	ClaireonGraphIslands::Build(TwoNodes, Islands);
	UNTEST_EXPECT_EQ(Islands.Num(), 2);

	ClaireonGraphIslands::Build(nullptr, Islands);
	UNTEST_EXPECT_EQ(Islands.Num(), 0);
	co_return;
}

// Preserve distinct graph-kind wire spellings.

UNTEST_UNIT_OPTS(Claireon, OutputContract, GraphKind_WireStringsRoundTrip, UNTEST_TIMEOUTMS(60000))
{
	using EKind = ClaireonBlueprintHelpers::EClaireonGraphKind;

	const EKind AllKinds[] = {
		EKind::Ubergraph, EKind::Function, EKind::Macro,
		EKind::DelegateSignature, EKind::InterfaceImplementation, EKind::Extension,
	};

	const TCHAR* ExpectedWire[] = {
		TEXT("ubergraph"), TEXT("function"), TEXT("macro"),
		TEXT("delegate_signature"), TEXT("interface_implementation"), TEXT("extension"),
	};

	TSet<FString> Seen;
	for (int32 I = 0; I < UE_ARRAY_COUNT(AllKinds); ++I)
	{
		const FString Wire = ClaireonBlueprintHelpers::GraphKindToWireString(AllKinds[I]);
		UNTEST_EXPECT_TRUE(Wire == ExpectedWire[I]);

		UNTEST_EXPECT_FALSE(Seen.Contains(Wire));
		Seen.Add(Wire);

		EKind Parsed = EKind::Ubergraph;
		UNTEST_ASSERT_TRUE(ClaireonBlueprintHelpers::ParseGraphKind(Wire, Parsed));
		UNTEST_EXPECT_TRUE(Parsed == AllKinds[I]);
	}
	UNTEST_EXPECT_EQ(Seen.Num(), 6);

	// Reject unknown input rather than leaving the default enum value.
	for (const TCHAR* Unknown : {TEXT(""), TEXT("interface"), TEXT("ubergraphs"), TEXT("composite")})
	{
		EKind Sentinel = EKind::Extension;
		UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::ParseGraphKind(Unknown, Sentinel));
		UNTEST_EXPECT_TRUE(Sentinel == EKind::Extension);
	}
	co_return;
}

#endif // WITH_UNTESTED
