// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_UObjectSetProperty.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace ClaireonToolUObjectSetPropertySpec
{
	// File-local named namespace (not anonymous): anonymous namespaces from
	// separate .cpp merge and collide under unity batching on v2.

	TSharedPtr<FJsonObject> BuildArgs(
		const FString& ObjectPath,
		const FString& PropertyPath,
		const FString& Value)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("object_path"), ObjectPath);
		Args->SetStringField(TEXT("property_path"), PropertyPath);
		Args->SetStringField(TEXT("value"), Value);
		return Args;
	}

	/** Construct on the stack and run. NO UNTEST_ASSERT_* inside -- caller asserts. */
	IClaireonTool::FToolResult RunSet(const TSharedPtr<FJsonObject>& Args)
	{
		ClaireonTool_UObjectSetProperty Tool;
		return Tool.Execute(Args);
	}

	/** Rooted fixture so the object survives GC for the duration of one test. */
	UClaireonUObjectInspectFixture* MakeFixture()
	{
		UClaireonUObjectInspectFixture* Fixture =
			NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
		Fixture->AddToRoot();
		return Fixture;
	}

	int32 ReadInt(UObject* Object, const FString& Path)
	{
		FString Error;
		const FString Text = ClaireonPropertyUtils::ReadPropertyByPath(Object, Path, Error);
		return FCString::Atoi(*Text);
	}
}

// ---------------------------------------------------------------------------
// The guard: non-editable properties are refused without the opt-in.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, RefusesNonEditableWithoutOptIn, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	// `Plain` is a bare UPROPERTY() -- no EditAnywhere, so the details panel would
	// never show it. Default-deny.
	IClaireonTool::FToolResult Result =
		RunSet(BuildArgs(Fixture->GetPathName(), TEXT("Plain"), TEXT("77")));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("allow_non_editable")));
	// The value must be untouched by a refused write.
	UNTEST_EXPECT_EQ(Fixture->Plain, 11);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, RefusalHintIsACompleteCallableArgSet, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	IClaireonTool::FToolResult Result =
		RunSet(BuildArgs(Fixture->GetPathName(), TEXT("Plain"), TEXT("77")));

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Hint.IsValid());

	FString HintError;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Result.Hint, HintError));

	// Self-reference: "re-issue this call, corrected".
	UNTEST_EXPECT_EQ(Result.Hint->GetStringField(TEXT("tool")), FString(TEXT("uobject_set_property")));

	// args must be COMPLETE and directly callable -- never a delta. Every required
	// field of the original call has to be present alongside the correction.
	const TSharedPtr<FJsonObject>* HintArgs = nullptr;
	UNTEST_ASSERT_TRUE(Result.Hint->TryGetObjectField(TEXT("args"), HintArgs));
	UNTEST_EXPECT_EQ((*HintArgs)->GetStringField(TEXT("object_path")), Fixture->GetPathName());
	UNTEST_EXPECT_EQ((*HintArgs)->GetStringField(TEXT("property_path")), FString(TEXT("Plain")));
	UNTEST_EXPECT_EQ((*HintArgs)->GetStringField(TEXT("value")), FString(TEXT("77")));
	UNTEST_EXPECT_TRUE((*HintArgs)->GetBoolField(TEXT("allow_non_editable")));

	Fixture->RemoveFromRoot();
	co_return;
}

// ---------------------------------------------------------------------------
// Full reflection reach, once the opt-in is given.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, WritesNonEditableWithOptIn, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName(), TEXT("Plain"), TEXT("77"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Fixture->Plain, 77);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_EQ(Result.Data->GetStringField(TEXT("old_value")), FString(TEXT("11")));
	UNTEST_EXPECT_EQ(Result.Data->GetStringField(TEXT("new_value")), FString(TEXT("77")));
	UNTEST_EXPECT_TRUE(Result.Data->GetBoolField(TEXT("non_editable_override")));
	// A waived guard must be visible in the result, not only in the log.
	UNTEST_EXPECT_TRUE(Result.Warnings.Num() > 0);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, WritesPrivateField, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName(), TEXT("PrivateField"), TEXT("55"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_EQ(Result.Data->GetStringField(TEXT("access")), FString(TEXT("private")));
	// Read back through reflection -- the field is private, so there is no accessor.
	UNTEST_EXPECT_EQ(ReadInt(Fixture, TEXT("PrivateField")), 55);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, WritesNestedStructMember, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName(), TEXT("Foo.X"), TEXT("9"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Fixture->Foo.X, 9);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, WritesArrayElement, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();
	Fixture->Numbers = { 1, 2, 3 };

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName(), TEXT("Numbers[1]"), TEXT("42"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(Fixture->Numbers.Num(), 3);
	UNTEST_EXPECT_EQ(Fixture->Numbers[1], 42);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, WritesTransientField, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	// Reach matches uobject_inspect, which deliberately keeps transient fields:
	// runtime state is exactly what a live-debugging write is for.
	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName(), TEXT("TransientField"), TEXT("13"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Fixture->TransientField, 13);

	Fixture->RemoveFromRoot();
	co_return;
}

// ---------------------------------------------------------------------------
// Sub-object traversal: the owner reported (and Modify()'d) is the component,
// not the root the path started from.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, ReportsComponentAsResolvedOwner, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	// The CDO of the actor fixture owns a default subobject component, so the write
	// target for "MyComp.SomeField" is the component -- resolvable without a world.
	TSharedPtr<FJsonObject> Args = BuildArgs(
		TEXT("/Script/Claireon.ClaireonUObjectInspectActorFixture"),
		TEXT("MyComp.SomeField"),
		TEXT("64"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	// Snapshot and restore the CDO BEFORE asserting: UNTEST_ASSERT_* co_returns on
	// failure, so any restore placed after an assert can be skipped and leak the
	// mutation into sibling tests.
	AClaireonUObjectInspectActorFixture* CDO =
		GetMutableDefault<AClaireonUObjectInspectActorFixture>();
	const int32 WrittenValue = (IsValid(CDO) && IsValid(CDO->MyComp)) ? CDO->MyComp->SomeField : INDEX_NONE;
	if (IsValid(CDO) && IsValid(CDO->MyComp))
	{
		CDO->MyComp->SomeField = 42;
	}

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_EQ(WrittenValue, 64);

	// resolved_on is emitted only when the owner differs from the root object.
	FString ResolvedOn;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("resolved_on"), ResolvedOn));
	UNTEST_EXPECT_FALSE(ResolvedOn.IsEmpty());

	// The change cycle must have run on the COMPONENT, not just the root actor.
	// Without this the nav-octree desync survives the write -- the reported bug.
	UNTEST_EXPECT_TRUE(Result.Data->GetBoolField(TEXT("owner_change_notified")));

	co_return;
}

// ---------------------------------------------------------------------------
// Error paths.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, ErrorsOnUnknownProperty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	TSharedPtr<FJsonObject> Args =
		BuildArgs(Fixture->GetPathName(), TEXT("ThisDoesNotExist"), TEXT("1"));
	Args->SetBoolField(TEXT("allow_non_editable"), true);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("ThisDoesNotExist")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, ErrorsOnUnresolvableObject, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	TSharedPtr<FJsonObject> Args =
		BuildArgs(TEXT("/Game/DoesNotExist/Nope"), TEXT("Plain"), TEXT("1"));
	Args->SetBoolField(TEXT("allow_load"), false);

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectSetProperty, ErrorsOnMissingValue, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectSetPropertySpec;

	UClaireonUObjectInspectFixture* Fixture = MakeFixture();

	// An EMPTY value is legal (it clears a container); an ABSENT one is not.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("object_path"), Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("Plain"));

	IClaireonTool::FToolResult Result = RunSet(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("value")));

	Fixture->RemoveFromRoot();
	co_return;
}

#endif // WITH_UNTESTED
