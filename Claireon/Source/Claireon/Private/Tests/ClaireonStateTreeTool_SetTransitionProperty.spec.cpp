// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for claireon.statetree_set_transition_property (#0000 / F2).
// Exercises SetTransitionProperty helper directly on a transient FStateTreeTransition.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/ClaireonStateTreeTool_SetTransitionProperty.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"

// ---------------------------------------------------------------------------
// Smoke: tool surface
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_SetTransitionProperty Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("claireon.statetree_set_transition_property"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

// ---------------------------------------------------------------------------
// Happy path: bConsumeEventOnSelect via canonical RequiredEvent prefix
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, HappyConsumeCanonical, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	UNTEST_ASSERT_TRUE(Transition.RequiredEvent.bConsumeEventOnSelect == true);

	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("RequiredEvent.bConsumeEventOnSelect"), TEXT("false"), Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(Transition.RequiredEvent.bConsumeEventOnSelect == false);
	co_return;
}

// ---------------------------------------------------------------------------
// Happy path: bare bConsumeEventOnSelect normalized to RequiredEvent. prefix
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, HappyConsumeBareNormalize, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	UNTEST_ASSERT_TRUE(Transition.RequiredEvent.bConsumeEventOnSelect == true);

	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("bConsumeEventOnSelect"), TEXT("false"), Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(Transition.RequiredEvent.bConsumeEventOnSelect == false);
	co_return;
}

// ---------------------------------------------------------------------------
// Happy path: bDelayTransition + DelayDuration
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, HappyDelay, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	FString Error;
	UNTEST_ASSERT_TRUE(ClaireonStateTreeHelpers::SetTransitionProperty(Transition, TEXT("bDelayTransition"), TEXT("true"), Error));
	UNTEST_ASSERT_TRUE(Transition.bDelayTransition == true);

	UNTEST_ASSERT_TRUE(ClaireonStateTreeHelpers::SetTransitionProperty(Transition, TEXT("DelayDuration"), TEXT("2.5"), Error));
	UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(Transition.DelayDuration, 2.5f));
	co_return;
}

// ---------------------------------------------------------------------------
// Happy path: bTransitionEnabled
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, HappyEnabled, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	UNTEST_ASSERT_TRUE(Transition.bTransitionEnabled == true);

	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("bTransitionEnabled"), TEXT("false"), Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(Transition.bTransitionEnabled == false);
	co_return;
}

// ---------------------------------------------------------------------------
// Excluded field: Target -> error referencing modify_transition
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, ExcludedTarget, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("Target"), TEXT(""), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("claireon.statetree_modify_transition")));
	co_return;
}

// ---------------------------------------------------------------------------
// Excluded field: State sub-path -> error
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, ExcludedStateSubPath, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("State.LinkType"), TEXT(""), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("claireon.statetree_modify_transition")));
	co_return;
}

// ---------------------------------------------------------------------------
// Unknown field -> Property '<name>' not found on FStateTreeTransition
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeSetTransitionProperty, UnknownField, UNTEST_TIMEOUTMS(5000))
{
	FStateTreeTransition Transition;
	FString Error;
	const bool bOk = ClaireonStateTreeHelpers::SetTransitionProperty(
		Transition, TEXT("DoesNotExist"), TEXT("0"), Error);
	UNTEST_ASSERT_TRUE(!bOk);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("Property 'DoesNotExist' not found")));
	co_return;
}

#endif // WITH_UNTESTED
