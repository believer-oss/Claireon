// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for statetree_set_state_property.
// These tests exercise the ResolvePropertyPath dot-path navigator and the
// SetStateProperty helper directly. They synthesize a transient
// UStateTreeState and call into ClaireonStateTreeHelpers without round-
// tripping the editor data, which keeps them fast and PIE-independent.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/ClaireonStateTreeTool_SetStateProperty.h"
#include "StateTreeState.h"
#include "StateTreeEvents.h"
#include "GameplayTagContainer.h"
#include "UObject/Package.h"

namespace
{
	UStateTreeState* MakeTransientState()
	{
		return NewObject<UStateTreeState>(GetTransientPackage(), UStateTreeState::StaticClass(), NAME_None, RF_Transient);
	}
}

// ---------------------------------------------------------------------------
// Smoke: tool surface
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_SetStateProperty Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("statetree_set_state_property"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

// ---------------------------------------------------------------------------
// ResolvePropertyPath: top-level field
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, ResolveTopLevel, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	UNTEST_ASSERT_TRUE(State != nullptr);

	FProperty* LeafProp = nullptr;
	void* LeafAddr = nullptr;
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::ResolvePropertyPath(
		UStateTreeState::StaticClass(), State, TEXT("bHasRequiredEventToEnter"),
		LeafProp, LeafAddr, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(LeafProp != nullptr);
	UNTEST_ASSERT_TRUE(LeafAddr != nullptr);
	co_return;
}

// ---------------------------------------------------------------------------
// ResolvePropertyPath: dot-path traversal into RequiredEventToEnter.Tag
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, ResolveDotPath, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	UNTEST_ASSERT_TRUE(State != nullptr);

	FProperty* LeafProp = nullptr;
	void* LeafAddr = nullptr;
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::ResolvePropertyPath(
		UStateTreeState::StaticClass(), State, TEXT("RequiredEventToEnter.Tag"),
		LeafProp, LeafAddr, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(LeafProp != nullptr);
	UNTEST_ASSERT_TRUE(LeafAddr != nullptr);
	UNTEST_ASSERT_STREQ(*LeafProp->GetName(), TEXT("Tag"));
	co_return;
}

// ---------------------------------------------------------------------------
// SetStateProperty: happy path -- bHasRequiredEventToEnter
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, HappyBoolFlag, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	UNTEST_ASSERT_TRUE(State != nullptr);
	UNTEST_ASSERT_TRUE(State->bHasRequiredEventToEnter == false);

	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("bHasRequiredEventToEnter"), TEXT("true"), Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(State->bHasRequiredEventToEnter == true);
	co_return;
}

// ---------------------------------------------------------------------------
// SetStateProperty: dot-path -- RequiredEventToEnter.bConsumeEventOnSelect
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, HappyDotPathBool, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	UNTEST_ASSERT_TRUE(State != nullptr);
	UNTEST_ASSERT_TRUE(State->RequiredEventToEnter.bConsumeEventOnSelect == true);

	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("RequiredEventToEnter.bConsumeEventOnSelect"), TEXT("false"), Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(State->RequiredEventToEnter.bConsumeEventOnSelect == false);
	co_return;
}

// ---------------------------------------------------------------------------
// SetStateProperty: dot-path -- RequiredEventToEnter.Tag (gameplay tag)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, HappyDotPathTag, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	UNTEST_ASSERT_TRUE(State != nullptr);

	FString Error;
	// Use ImportText format expected by FGameplayTag.
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("RequiredEventToEnter.Tag"),
		TEXT("(TagName=\"Demo.Event.MoveTo.Request\")"),
		Error);
	// Tag may not be registered in test environments; the importer still
	// succeeds even when the tag is unregistered (it stores the raw name).
	if (!bOk)
	{
		// Surface the specific error for debugging if the import fails.
		UE_LOG(LogTemp, Warning, TEXT("[StateTreeSetStateProperty] HappyDotPathTag import failure: %s"), *Error);
	}
	UNTEST_ASSERT_TRUE(bOk);
	co_return;
}

// ---------------------------------------------------------------------------
// Excluded field: Color
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, ExcludedColor, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("Color"), TEXT("(R=1,G=0,B=0,A=1)"), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("Color is not supported")));
	co_return;
}

// ---------------------------------------------------------------------------
// Excluded field: Parameters.Parameters
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, ExcludedParameters, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("Parameters.Parameters"), TEXT(""), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("apply_spec for Parameters bag mutation")));
	co_return;
}

// ---------------------------------------------------------------------------
// Unknown top-level field
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetStateProperty, UnknownField, UNTEST_TIMEOUTMS(5000))
{
	UStateTreeState* State = MakeTransientState();
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetStateProperty(
		*State, TEXT("DoesNotExist"), TEXT("0"), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("Property 'DoesNotExist' not found")));
	co_return;
}

#endif // WITH_UNTESTED
