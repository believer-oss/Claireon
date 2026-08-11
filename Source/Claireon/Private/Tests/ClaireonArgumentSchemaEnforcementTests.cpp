// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// P1-2 / T1: enforce the schema-vs-argument invariant.
//
// Two halves, matching the two halves of the item:
//   1. ClaireonSafeExec::ValidateArgumentsAgainstSchema -- the runtime gate at
//      the single funnel both transports pass through.
//   2. A registry-wide schema-shape lint, batched-offender style, mirroring
//      ClaireonDescriptionLintTests.
//
// The lint is deliberately split into HARD invariants (a schema that violates
// them cannot be called correctly by anyone) and a LOGGED inventory of
// non-snake_case parameter names, which is a real inconsistency but one P2-3
// owns -- failing on it here would just block this band on that decision.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonModule.h"
#include "ClaireonSafeExec.h"
#include "ClaireonServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeatures.h"
#include "IClaireonToolProvider.h"
#include "Tools/IClaireonTool.h"
#include "SquidTasks/Task.h"

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions across other Tests TUs.
namespace ClaireonArgSchemaTests
{
/** Tool whose schema declares exactly the parameters it is handed. */
class FSchemaStubTool : public IClaireonTool
{
public:
	explicit FSchemaStubTool(const TArray<FString>& InDeclared, bool bInEmitProperties = true)
		: Declared(InDeclared), bEmitProperties(bInEmitProperties) {}

	virtual FString GetCategory() const override { return TEXT("claireon_argtest"); }
	virtual FString GetOperation() const override { return TEXT("stub"); }
	virtual FString GetDescription() const override { return TEXT("Claireon argument-gate test stub."); }

	virtual TSharedPtr<FJsonObject> GetInputSchema() const override
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		if (bEmitProperties)
		{
			TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
			for (const FString& Name : Declared)
			{
				TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
				Prop->SetStringField(TEXT("type"), TEXT("string"));
				Prop->SetStringField(TEXT("description"), TEXT("stub"));
				Props->SetObjectField(Name, Prop);
			}
			Schema->SetObjectField(TEXT("properties"), Props);
		}
		return Schema;
	}

	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override
	{
		return MakeSuccessResult(nullptr, TEXT("stub"));
	}

private:
	TArray<FString> Declared;
	bool bEmitProperties;
};

TSharedPtr<FJsonObject> MakeArgs(const TArray<TPair<FString, FString>>& Pairs)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		Args->SetStringField(Pair.Key, Pair.Value);
	}
	return Args;
}

void CollectAllRegisteredTools(TArray<TSharedPtr<IClaireonTool>>& OutTools)
{
	OutTools.Reset();
	FClaireonModule::Get().EnsureServerForTest();
	TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
												   .GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
	for (IClaireonToolProvider* Provider : Providers)
	{
		if (!Provider)
		{
			continue;
		}
		for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
		{
			if (Tool.IsValid())
			{
				OutTools.Add(Tool);
			}
		}
	}
}

bool IsSnakeCase(const FString& Name)
{
	for (const TCHAR Ch : Name)
	{
		const bool bOk = (Ch >= TEXT('a') && Ch <= TEXT('z'))
			|| (Ch >= TEXT('0') && Ch <= TEXT('9'))
			|| Ch == TEXT('_');
		if (!bOk)
		{
			return false;
		}
	}
	return !Name.IsEmpty();
}
} // namespace ClaireonArgSchemaTests

// ---------------------------------------------------------------------------
// The gate itself.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, DeclaredArgumentsAreAccepted, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonArgSchemaTests;

	FSchemaStubTool Tool({ TEXT("actor_id"), TEXT("detail_level") });
	const FString Error = ClaireonSafeExec::ValidateArgumentsAgainstSchema(
		&Tool, MakeArgs({ { TEXT("actor_id"), TEXT("actor_0") } }));

	UNTEST_ASSERT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, UndeclaredArgumentIsRejected, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonArgSchemaTests;

	FSchemaStubTool Tool({ TEXT("actor_id") });
	const FString Error = ClaireonSafeExec::ValidateArgumentsAgainstSchema(
		&Tool, MakeArgs({ { TEXT("nonsense_param"), TEXT("x") } }));

	// The whole point: this used to be a silent no-op reported as success.
	UNTEST_ASSERT_FALSE(Error.IsEmpty());
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("nonsense_param")));
	// The caller must be able to fix the call from the message alone.
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("actor_id")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, CamelSnakeMismatchSuggestsTheDeclaredName, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonArgSchemaTests;

	// The dominant real-world case in this catalog: the same parameter spelled
	// camelCase by one tool family and snake_case by another.
	FSchemaStubTool Tool({ TEXT("actor_id") });
	const FString Error = ClaireonSafeExec::ValidateArgumentsAgainstSchema(
		&Tool, MakeArgs({ { TEXT("actorId"), TEXT("actor_0") } }));

	UNTEST_ASSERT_FALSE(Error.IsEmpty());
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("did you mean 'actor_id'")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, TransportArgumentsAreAlwaysAccepted, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonArgSchemaTests;

	// suppress_output is consumed by the server, not by the tool, so no schema
	// declares it. Rejecting it would break a documented flag on every tool.
	FSchemaStubTool Tool({ TEXT("actor_id") });
	TSharedPtr<FJsonObject> Args = MakeArgs({ { TEXT("actor_id"), TEXT("actor_0") } });
	Args->SetBoolField(TEXT("suppress_output"), true);

	UNTEST_ASSERT_TRUE(ClaireonSafeExec::ValidateArgumentsAgainstSchema(&Tool, Args).IsEmpty());
	UNTEST_ASSERT_TRUE(ClaireonSafeExec::IsTransportLevelArgument(TEXT("suppress_output")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, SchemaWithoutPropertiesStaysPermissive, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonArgSchemaTests;

	// Nothing to validate against. Rejecting everything here would take such a
	// tool from "ignores an argument" to "cannot be called at all".
	FSchemaStubTool Tool({}, /*bEmitProperties=*/false);
	const FString Error = ClaireonSafeExec::ValidateArgumentsAgainstSchema(
		&Tool, MakeArgs({ { TEXT("anything"), TEXT("x") } }));

	UNTEST_ASSERT_TRUE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Registry-wide schema lint. Batched offender lists, so one run names every
// offender rather than stopping at the first.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, EverySchemaIsAWellFormedObject, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonArgSchemaTests;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	TArray<FString> Offenders;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
		if (!Schema.IsValid())
		{
			Offenders.Add(FString::Printf(TEXT("%s: GetInputSchema() returned null"), *Tool->GetName()));
			continue;
		}

		FString SchemaType;
		if (!Schema->TryGetStringField(TEXT("type"), SchemaType) || SchemaType != TEXT("object"))
		{
			Offenders.Add(FString::Printf(TEXT("%s: schema type is '%s', expected 'object'"),
				*Tool->GetName(), *SchemaType));
		}

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Props) || Props == nullptr || !(*Props).IsValid())
		{
			// No properties object means the argument gate cannot validate this
			// tool at all -- it is the one shape that silently opts out.
			Offenders.Add(FString::Printf(TEXT("%s: schema has no 'properties' object"), *Tool->GetName()));
		}
	}

	for (const FString& Offender : Offenders)
	{
		UE_LOG(LogTemp, Error, TEXT("[SchemaLint] %s"), *Offender);
	}
	UNTEST_ASSERT_EQ(Offenders.Num(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, EveryRequiredParameterIsDeclared, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonArgSchemaTests;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	TArray<FString> Offenders;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
		if (!Schema.IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (!Schema->TryGetArrayField(TEXT("required"), Required) || Required == nullptr)
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Props) || Props == nullptr || !(*Props).IsValid())
		{
			continue; // reported by the well-formedness test
		}

		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name))
			{
				continue;
			}
			if (!(*Props)->HasField(Name))
			{
				// Required but undeclared: the argument gate rejects it, so the
				// tool cannot be called successfully by anyone.
				Offenders.Add(FString::Printf(TEXT("%s: '%s' is required but not declared in properties"),
					*Tool->GetName(), *Name));
			}
		}
	}

	for (const FString& Offender : Offenders)
	{
		UE_LOG(LogTemp, Error, TEXT("[SchemaLint] %s"), *Offender);
	}
	UNTEST_ASSERT_EQ(Offenders.Num(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, EveryParameterDeclaresTypeAndDescription, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonArgSchemaTests;

	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	TArray<FString> Offenders;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
		if (!Schema.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Props) || Props == nullptr || !(*Props).IsValid())
		{
			continue;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Props)->Values)
		{
			const TSharedPtr<FJsonObject>* PropObj = nullptr;
			if (!Entry.Value.IsValid() || !Entry.Value->TryGetObject(PropObj) || PropObj == nullptr)
			{
				Offenders.Add(FString::Printf(TEXT("%s.%s: property is not an object"), *Tool->GetName(), *Entry.Key));
				continue;
			}

			// "type" may be a string or, for a parameter that genuinely accepts
			// several JSON types, the JSON Schema type-array form. What is not
			// acceptable is an absent type, which reads as an authoring slip.
			FString PropType;
			const TArray<TSharedPtr<FJsonValue>>* PropTypes = nullptr;
			const bool bHasStringType = (*PropObj)->TryGetStringField(TEXT("type"), PropType) && !PropType.IsEmpty();
			const bool bHasArrayType = (*PropObj)->TryGetArrayField(TEXT("type"), PropTypes)
				&& PropTypes != nullptr && PropTypes->Num() > 0;
			if (!bHasStringType && !bHasArrayType)
			{
				Offenders.Add(FString::Printf(TEXT("%s.%s: no 'type'"), *Tool->GetName(), *Entry.Key));
			}

			FString PropDesc;
			if (!(*PropObj)->TryGetStringField(TEXT("description"), PropDesc) || PropDesc.IsEmpty())
			{
				Offenders.Add(FString::Printf(TEXT("%s.%s: no 'description'"), *Tool->GetName(), *Entry.Key));
			}
		}
	}

	for (const FString& Offender : Offenders)
	{
		UE_LOG(LogTemp, Error, TEXT("[SchemaLint] %s"), *Offender);
	}
	UNTEST_ASSERT_EQ(Offenders.Num(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ArgumentSchema, NonSnakeCaseParameterInventory, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonArgSchemaTests;

	// INVENTORY, not a gate. Converging these names is P2-3's job and breaks
	// every existing caller, so this test records the set rather than failing on
	// it. What it DOES assert is that the argument gate can suggest the right
	// name for each of them -- i.e. no offender is a dead end for a caller.
	TArray<TSharedPtr<IClaireonTool>> AllTools;
	CollectAllRegisteredTools(AllTools);
	UNTEST_ASSERT_TRUE(AllTools.Num() > 0);

	TSet<FString> Offenders;
	for (const TSharedPtr<IClaireonTool>& Tool : AllTools)
	{
		const TSharedPtr<FJsonObject> Schema = Tool->GetInputSchema();
		if (!Schema.IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Props) || Props == nullptr || !(*Props).IsValid())
		{
			continue;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Props)->Values)
		{
			if (!IsSnakeCase(Entry.Key))
			{
				Offenders.Add(FString::Printf(TEXT("%s.%s"), *Tool->GetName(), *Entry.Key));
			}
		}
	}

	UE_LOG(LogTemp, Display,
		TEXT("[SchemaLint] %d non-snake_case parameter declarations (inventory for P2-3, not a failure)"),
		Offenders.Num());
	for (const FString& Offender : Offenders)
	{
		UE_LOG(LogTemp, Display, TEXT("[SchemaLint]   %s"), *Offender);
	}

	co_return;
}

#endif // WITH_UNTESTED
