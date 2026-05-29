// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_UObjectInspect.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace ClaireonToolUObjectInspectSpec
{
	/**
	 * Build args for one inspect call. Returns the populated JSON arguments
	 * object.
	 */
	TSharedPtr<FJsonObject> BuildArgs(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("object_path"), ObjectPath);
		return Args;
	}

	/**
	 * Construct a tool instance on the stack and execute it. Returns the
	 * IClaireonTool::FToolResult. NO UNTEST_ASSERT_* inside -- caller asserts.
	 */
	IClaireonTool::FToolResult RunInspect(const TSharedPtr<FJsonObject>& Args)
	{
		ClaireonTool_UObjectInspect Tool;
		return Tool.Execute(Args);
	}

	/**
	 * Locate a named property entry in a listing-mode response. Returns the
	 * JSON object on hit, or null if not found.
	 */
	TSharedPtr<FJsonObject> FindPropertyEntry(
		const TSharedPtr<FJsonObject>& Data,
		const FString& Name)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("properties"), Properties))
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Properties)
		{
			TSharedPtr<FJsonObject> Obj = Entry->AsObject();
			if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
			{
				return Obj;
			}
		}
		return nullptr;
	}
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ListsAStaticMeshActorCdo, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	IClaireonTool::FToolResult Result = RunInspect(BuildArgs(TEXT("/Script/Engine.StaticMeshActor")));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_ASSERT_EQ(Result.Data->GetStringField(TEXT("class")), FString(TEXT("StaticMeshActor")));
	UNTEST_ASSERT_TRUE(Result.Data->GetBoolField(TEXT("is_cdo")));

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("properties"), Properties));
	UNTEST_ASSERT_TRUE(Properties != nullptr && Properties->Num() > 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, OmitsValueWhenIncludeValuesFalse, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	IClaireonTool::FToolResult Result = RunInspect(BuildArgs(Fixture->GetPathName()));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("properties"), Properties));
	for (const TSharedPtr<FJsonValue>& Entry : *Properties)
	{
		UNTEST_ASSERT_FALSE(Entry->AsObject()->HasField(TEXT("value")));
	}

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, IncludesValueWhenIncludeValuesTrue, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetBoolField(TEXT("include_values"), true);
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> BpReadOnlyEntry = FindPropertyEntry(Result.Data, TEXT("BpReadOnly"));
	UNTEST_ASSERT_TRUE(BpReadOnlyEntry.IsValid());
	UNTEST_ASSERT_TRUE(BpReadOnlyEntry->HasField(TEXT("value")));
	UNTEST_ASSERT_EQ(static_cast<int32>(BpReadOnlyEntry->GetNumberField(TEXT("value"))), 7);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, TargetedBlueprintReadOnly, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("BpReadOnly"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 7);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, TargetedPlainNoBpSpecifier, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	// (a) Targeted read returns the value.
	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("Plain"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 11);

	// (b) Listing reports bp_access == "none" for Plain.
	IClaireonTool::FToolResult List = RunInspect(BuildArgs(Fixture->GetPathName()));
	UNTEST_ASSERT_FALSE(List.bIsError);
	TSharedPtr<FJsonObject> PlainEntry = FindPropertyEntry(List.Data, TEXT("Plain"));
	UNTEST_ASSERT_TRUE(PlainEntry.IsValid());
	UNTEST_ASSERT_EQ(PlainEntry->GetStringField(TEXT("bp_access")), FString(TEXT("none")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, TargetedProtectedField, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("ProtectedField"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 21);

	IClaireonTool::FToolResult List = RunInspect(BuildArgs(Fixture->GetPathName()));
	UNTEST_ASSERT_FALSE(List.bIsError);
	TSharedPtr<FJsonObject> Entry = FindPropertyEntry(List.Data, TEXT("ProtectedField"));
	UNTEST_ASSERT_TRUE(Entry.IsValid());
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("access")), FString(TEXT("protected")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, TargetedPrivateField, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("PrivateField"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 33);

	IClaireonTool::FToolResult List = RunInspect(BuildArgs(Fixture->GetPathName()));
	UNTEST_ASSERT_FALSE(List.bIsError);
	TSharedPtr<FJsonObject> Entry = FindPropertyEntry(List.Data, TEXT("PrivateField"));
	UNTEST_ASSERT_TRUE(Entry.IsValid());
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("access")), FString(TEXT("private")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ArrayIndexing, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();
	Fixture->Numbers = { 100, 200 };

	TSharedPtr<FJsonObject> Args0 = BuildArgs(Fixture->GetPathName());
	Args0->SetStringField(TEXT("property_path"), TEXT("Numbers[0]"));
	IClaireonTool::FToolResult R0 = RunInspect(Args0);
	UNTEST_ASSERT_FALSE(R0.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(R0.Data->GetNumberField(TEXT("value"))), 100);

	TSharedPtr<FJsonObject> Args1 = BuildArgs(Fixture->GetPathName());
	Args1->SetStringField(TEXT("property_path"), TEXT("Numbers[1]"));
	IClaireonTool::FToolResult R1 = RunInspect(Args1);
	UNTEST_ASSERT_FALSE(R1.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(R1.Data->GetNumberField(TEXT("value"))), 200);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, NestedStructDotPath, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();
	Fixture->Foo.X = 55;

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("Foo.X"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 55);

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ComponentSubProperty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	AClaireonUObjectInspectActorFixture* Actor =
		NewObject<AClaireonUObjectInspectActorFixture>(GetTransientPackage());
	Actor->AddToRoot();
	// SomeField defaults to 42 via the fixture initializer.

	TSharedPtr<FJsonObject> Args = BuildArgs(Actor->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("MyComp.SomeField"));
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_EQ(static_cast<int32>(Result.Data->GetNumberField(TEXT("value"))), 42);

	Actor->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ListingIncludesTransient, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	IClaireonTool::FToolResult Result = RunInspect(BuildArgs(Fixture->GetPathName()));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> Entry = FindPropertyEntry(Result.Data, TEXT("TransientField"));
	UNTEST_ASSERT_TRUE(Entry.IsValid());

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ListingIncludesDelegateAssignable, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetBoolField(TEXT("include_values"), true);
	IClaireonTool::FToolResult Result = RunInspect(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> Entry = FindPropertyEntry(Result.Data, TEXT("OnSomething"));
	UNTEST_ASSERT_TRUE(Entry.IsValid());
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("bp_access")), FString(TEXT("assignable")));
	UNTEST_ASSERT_TRUE(Entry->HasField(TEXT("value")));
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("value")), FString(TEXT("<unbound>")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ErrorMissingObjectAllowLoad, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	TSharedPtr<FJsonObject> Args = BuildArgs(TEXT("/Game/Does/Not/Exist.Foo"));
	Args->SetBoolField(TEXT("allow_load"), true);
	IClaireonTool::FToolResult Result = RunInspect(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_FALSE(Result.ErrorMessage.IsEmpty());

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ErrorMissingObjectNoLoad, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	TSharedPtr<FJsonObject> Args = BuildArgs(TEXT("/Game/Does/Not/Exist.Foo"));
	Args->SetBoolField(TEXT("allow_load"), false);
	IClaireonTool::FToolResult Result = RunInspect(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("not loaded")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, ErrorUnresolvedPropertyPath, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	TSharedPtr<FJsonObject> Args = BuildArgs(Fixture->GetPathName());
	Args->SetStringField(TEXT("property_path"), TEXT("DoesNotExist"));
	IClaireonTool::FToolResult Result = RunInspect(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("DoesNotExist")));

	Fixture->RemoveFromRoot();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, UObjectInspect, MaxDepthClamps, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolUObjectInspectSpec;

	UClaireonUObjectInspectFixture* Fixture = NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	// max_depth = -1 clamps to 0 without error.
	TSharedPtr<FJsonObject> Args1 = BuildArgs(Fixture->GetPathName());
	Args1->SetNumberField(TEXT("max_depth"), -1);
	IClaireonTool::FToolResult R1 = RunInspect(Args1);
	UNTEST_ASSERT_FALSE(R1.bIsError);
	UNTEST_ASSERT_TRUE(R1.Data.IsValid());

	// max_depth = 999 clamps to 8 without error.
	TSharedPtr<FJsonObject> Args2 = BuildArgs(Fixture->GetPathName());
	Args2->SetNumberField(TEXT("max_depth"), 999);
	IClaireonTool::FToolResult R2 = RunInspect(Args2);
	UNTEST_ASSERT_FALSE(R2.bIsError);
	UNTEST_ASSERT_TRUE(R2.Data.IsValid());

	Fixture->RemoveFromRoot();
	co_return;
}

#endif // WITH_UNTESTED
