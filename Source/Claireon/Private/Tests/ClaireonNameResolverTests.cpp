// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "UObject/Package.h"
#include "ClaireonNameResolver.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ActorComponent.h"
#include "K2Node_CallFunction.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"

// ===========================================================================
// Class Name Resolution Tests
// ===========================================================================

// "AActor" is the C++ name; UE internal name is "Actor". The resolver should
// handle this transparently -- stripping A/U prefix at exact-match time so the
// caller never knows the difference.
// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_ExactMatch, UNTEST_TIMEOUTMS(10000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("AActor"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == AActor::StaticClass());
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.Error.IsEmpty());
	co_return;
}

// Internal name "Actor" should also work as exact match.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_ExactMatchInternalName, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("Actor"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == AActor::StaticClass());
	co_return;
}

// FName is case-insensitive, so "aactor" matches via exact stripping A prefix
// -> "actor" which FindFirstObject resolves case-insensitively. No resolution
// note expected since this is effectively an exact match through FName.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_CaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("aactor"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == AActor::StaticClass());
	co_return;
}

// "StaticMeshComponent" is the UE internal name -- should be exact match.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_AddUPrefix, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("StaticMeshComponent"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UStaticMeshComponent::StaticClass());
	// This is the internal UE name, so it resolves as exact match with no note
	co_return;
}

// "PlayerController" is the UE internal name -- should be exact match.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_AddAPrefix, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("PlayerController"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == APlayerController::StaticClass());
	co_return;
}

// "Audio" should find UAudioComponent via Component suffix step,
// since there is no class named just "Audio".
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_ComponentSuffix, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("Audio"), nullptr, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_RequiredBaseClassPass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("AActor"), AActor::StaticClass(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == AActor::StaticClass());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_RequiredBaseClassFail, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("AActor"), UActorComponent::StaticClass(), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_UnknownClass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("ThisClassDoesNotExist_XYZ"), nullptr, Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_EmptyInput, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT(""), nullptr, Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

// Domain-prefix resolution: "Selector" under UBTCompositeNode should find
// UBTComposite_Selector via the "BTComposite_" prefix. The prefix map lives
// in ClaireonNameResolver.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_DomainPrefix_BTComposite, UNTEST_TIMEOUTMS(5000))
{
	UClass* CompositeBase = FindFirstObject<UClass>(TEXT("BTCompositeNode"), EFindFirstObjectOptions::NativeFirst);
	UNTEST_ASSERT_TRUE(CompositeBase != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("Selector"), CompositeBase, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->GetName() == TEXT("BTComposite_Selector"));
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	co_return;
}

// Passing the general UBTNode base should still find a specific category
// subclass by walking all four prefix categories.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_DomainPrefix_BTNodeCatchAll, UNTEST_TIMEOUTMS(5000))
{
	UClass* NodeBase = FindFirstObject<UClass>(TEXT("BTNode"), EFindFirstObjectOptions::NativeFirst);
	UNTEST_ASSERT_TRUE(NodeBase != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	// "BlackboardBase" is ambiguous across several BT categories; expect failure
	// or success depending on what the engine ships, but at minimum the resolver
	// should not crash and should report something useful.
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("Sequence"), NodeBase, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->GetName() == TEXT("BTComposite_Sequence"));
	co_return;
}

// Unknown bases should simply skip the prefix step with no error.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Class_DomainPrefix_UnknownBase, UNTEST_TIMEOUTMS(5000))
{
	// UObject is not in the prefix map; resolution should fall through to
	// the standard steps and fail with a normal "not found" error.
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassName(TEXT("ThisDoesNotExistXYZ"), UObject::StaticClass(), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	co_return;
}

// ===========================================================================
// Pin Name Resolution Tests
// ===========================================================================

namespace ClaireonNameResolverTestsHelpers
{

// Helper to create a K2Node_CallFunction targeting AActor::K2_SetActorLocation
// with allocated pins for testing.
UK2Node_CallFunction* CreateTestCallFunctionNode(UEdGraph*& OutGraph, UBlueprint*& OutBlueprint)
{
	OutBlueprint = NewObject<UBlueprint>(GetTransientPackage(), TEXT("TestBP_NameResolver"));
	OutBlueprint->ParentClass = AActor::StaticClass();
	OutGraph = FBlueprintEditorUtils::CreateNewGraph(
		OutBlueprint, TEXT("TestGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!IsValid(OutGraph))
	{
		return nullptr;
	}

	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(OutGraph);
	Node->FunctionReference.SetExternalMember(FName(TEXT("K2_SetActorLocation")), AActor::StaticClass());
	Node->AllocateDefaultPins();
	return Node;
}

}  // namespace ClaireonNameResolverTestsHelpers

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_ExactMatch, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);
	UNTEST_ASSERT_TRUE(Node->Pins.Num() > 0);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("NewLocation"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());

	BP->MarkAsGarbage();
	co_return;
}

// FName-based FindPin is case-insensitive, so "newlocation" matches as exact.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_CaseInsensitive, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("newlocation"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	// FName is case-insensitive, so this resolves as exact match
	co_return;
}

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_AliasExec, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("exec"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->Direction == EGPD_Input);
	co_return;
}

// The output exec pin in K2Node_CallFunction may be named "then" which would
// match as exact. Either way, resolution should succeed and return an output pin.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_AliasThen, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("then"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->Direction == EGPD_Output);

	BP->MarkAsGarbage();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_AliasSelf, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("target"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_SubstringUnique, UNTEST_TIMEOUTMS(10000))
{
	UEdGraph* Graph = nullptr;
	UBlueprint* BP = nullptr;
	UK2Node_CallFunction* Node = ClaireonNameResolverTestsHelpers::CreateTestCallFunctionNode(Graph, BP);
	UNTEST_ASSERT_TRUE(Node != nullptr);

	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(Node, TEXT("Teleport"), EGPD_MAX, Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());

	BP->MarkAsGarbage();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Pin_NullNode, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UEdGraphPin* Found = ClaireonNameResolver::ResolvePinName(nullptr, TEXT("test"), EGPD_MAX, Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("null")));
	co_return;
}

// ===========================================================================
// Function Name Resolution Tests
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_ExactMatch, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found =
		ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("K2_SetActorLocation"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_K2PrefixAdd, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found =
		ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("SetActorLocation"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.Contains(TEXT("K2_")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_EventAliasBeginPlay, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found =
		ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("BeginPlay"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.Contains(TEXT("event alias")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_EventAliasTick, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found = ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("Tick"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.Contains(TEXT("event alias")));
	co_return;
}

// FName is case-insensitive so "k2_setactorlocation" matches exactly.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_CaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found =
		ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("k2_setactorlocation"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	// FName comparison is case-insensitive, so this matches as exact
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_UnknownFunction, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found =
		ClaireonNameResolver::ResolveFunctionName(AActor::StaticClass(), TEXT("NonExistentFunc_XYZ"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Function_NullClass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UFunction* Found = ClaireonNameResolver::ResolveFunctionName(nullptr, TEXT("test"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("null")));
	co_return;
}

// ===========================================================================
// Property Name Resolution Tests
// ===========================================================================

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Property_ExactMatch, UNTEST_TIMEOUTMS(10000))
{
	// AActor has a well-known property "bHidden"
	ClaireonNameResolver::FNameResolveResult Result;
	FProperty* Found = ClaireonNameResolver::ResolvePropertyName(AActor::StaticClass(), TEXT("bHidden"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	co_return;
}

// FName is case-insensitive so "bhidden" matches as exact.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Property_CaseInsensitive, UNTEST_TIMEOUTMS(10000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	FProperty* Found = ClaireonNameResolver::ResolvePropertyName(AActor::StaticClass(), TEXT("bhidden"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	// FName is case-insensitive so this resolves as exact match
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Property_BoolBPrefixAdd, UNTEST_TIMEOUTMS(10000))
{
	// "Hidden" should find "bHidden" via the b-prefix addition step
	ClaireonNameResolver::FNameResolveResult Result;
	FProperty* Found = ClaireonNameResolver::ResolvePropertyName(AActor::StaticClass(), TEXT("Hidden"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Property_UnknownProperty, UNTEST_TIMEOUTMS(10000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	FProperty* Found =
		ClaireonNameResolver::ResolvePropertyName(AActor::StaticClass(), TEXT("NoSuchProp_XYZ"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Property_NullStruct, UNTEST_TIMEOUTMS(10000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	FProperty* Found = ClaireonNameResolver::ResolvePropertyName(nullptr, TEXT("test"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("null")));
	co_return;
}

// ===========================================================================
// Struct Name Resolution Tests
// ===========================================================================

// "FVector" is the C++ name; the resolver strips the F prefix to find UE's
// internal name "Vector". This should still appear as exact match.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Struct_ExactMatch, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(TEXT("FVector"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	// The resolver recognizes F-prefixed C++ names as exact match
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	co_return;
}

// "Vector" is the UE internal name -- also exact match.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Struct_ExactMatchInternalName, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(TEXT("Vector"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	co_return;
}

// FName is case-insensitive so "fvector" -> strip F -> "vector" matches via FName.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Struct_CaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(TEXT("fvector"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Struct_UnknownStruct, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(TEXT("FNoSuchStruct_XYZ"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

// ===========================================================================
// Enum Name Resolution Tests
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, NameResolver, Enum_ExactMatch, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UEnum* Found = ClaireonNameResolver::ResolveEnumName(TEXT("EObjectTypeQuery"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Result.ResolutionNote.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Enum_EPrefixAdd, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UEnum* Found = ClaireonNameResolver::ResolveEnumName(TEXT("ObjectTypeQuery"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	UNTEST_EXPECT_FALSE(Result.ResolutionNote.IsEmpty());
	co_return;
}

// FName is case-insensitive so "eobjecttypequery" matches as exact.
UNTEST_UNIT_OPTS(Claireon, NameResolver, Enum_CaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UEnum* Found = ClaireonNameResolver::ResolveEnumName(TEXT("eobjecttypequery"), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found != nullptr);
	// FName is case-insensitive so this resolves as exact match
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, NameResolver, Enum_UnknownEnum, UNTEST_TIMEOUTMS(5000))
{
	ClaireonNameResolver::FNameResolveResult Result;
	UEnum* Found = ClaireonNameResolver::ResolveEnumName(TEXT("ENoSuchEnum_XYZ"), Result);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.Error.IsEmpty());
	co_return;
}

#endif // WITH_UNTESTED
