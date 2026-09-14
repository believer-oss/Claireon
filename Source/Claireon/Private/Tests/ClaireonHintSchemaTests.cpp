// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Validate accepted hint shapes and specific rejection reasons.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonHintSchemaTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	static TSharedPtr<FJsonObject> Hint_Make()
	{
		return MakeShared<FJsonObject>();
	}

	static TSharedPtr<FJsonObject> Hint_ArgsObject()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/Some/Asset"));
		return Args;
	}

	/** Return the rejection reason for rule-specific assertions. */
	static bool Hint_Rejects(const TSharedPtr<FJsonObject>& Hint, FString& OutError)
	{
		return !IClaireonTool::ValidateHint(Hint, OutError);
	}
}


UNTEST_UNIT(Claireon, HintSchema, HintSchema_ToolAloneAccepted)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("tool_search"));
	Hint->SetStringField(TEXT("reason"), TEXT("unknown tool 'bp_opn'"));

	FString Error;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ResourceAloneAccepted)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
	Hint->SetStringField(TEXT("reason"), TEXT("the graph shape rules live here"));

	FString Error;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ToolWithArgsAccepted)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("bp_get_graph"));
	Hint->SetStringField(TEXT("reason"), TEXT("connectivity is not emitted at this detail level"));
	Hint->SetObjectField(TEXT("args"), Hint_ArgsObject());

	FString Error;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Hint, Error));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ToolWithOptionsAccepted)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("bp_get_graph"));
	Hint->SetStringField(TEXT("reason"), TEXT("either detail level answers this"));
	TArray<TSharedPtr<FJsonValue>> Options;
	Options.Add(MakeShared<FJsonValueObject>(Hint_ArgsObject()));
	Options.Add(MakeShared<FJsonValueObject>(Hint_ArgsObject()));
	Hint->SetArrayField(TEXT("options"), Options);

	FString Error;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Hint, Error));
	co_return;
}

// Require hint builders to produce valid shapes.
UNTEST_UNIT(Claireon, HintSchema, HintSchema_BuildersProduceValidShapes)
{
	FString Error;

	const TSharedPtr<FJsonObject> Prose = IClaireonTool::MakeGuidanceHint(
		TEXT("tool_search"), TEXT("signature mismatch on bp_compile"));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Prose, Error));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("name"), TEXT("bp_compile"));
	const TSharedPtr<FJsonObject> WithArgs = IClaireonTool::MakeGuidanceHint(
		TEXT("tool_search"), TEXT("signature mismatch on bp_compile"), Args);
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(WithArgs, Error));

	const TSharedPtr<FJsonObject> Resource = IClaireonTool::MakeResourceHint(
		TEXT("claireon://instructions/blueprint-authoring"), TEXT("read before non-trivial edits"));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Resource, Error));

	// Omit empty args to distinguish unspecified arguments from an explicit empty call.
	const TSharedPtr<FJsonObject> EmptyArgs = IClaireonTool::MakeGuidanceHint(
		TEXT("tool_search"), TEXT("reason"), MakeShared<FJsonObject>());
	UNTEST_EXPECT_FALSE(EmptyArgs->HasField(TEXT("args")));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(EmptyArgs, Error));
	co_return;
}


UNTEST_UNIT(Claireon, HintSchema, HintSchema_NullRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(nullptr, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("null")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_NeitherToolNorResourceRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("reason"), TEXT("something happened"));

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'tool' or 'resource'")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_BothToolAndResourceRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	Hint->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
	Hint->SetStringField(TEXT("reason"), TEXT("two next actions is not one hint"));

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("mutually exclusive")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ResourceWithArgsRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
	Hint->SetStringField(TEXT("reason"), TEXT("read this"));
	Hint->SetObjectField(TEXT("args"), Hint_ArgsObject());

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("nothing to call")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ResourceWithOptionsRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
	Hint->SetStringField(TEXT("reason"), TEXT("read this"));
	TArray<TSharedPtr<FJsonValue>> Options;
	Options.Add(MakeShared<FJsonValueObject>(Hint_ArgsObject()));
	Hint->SetArrayField(TEXT("options"), Options);

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("nothing to call")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_ArgsAndOptionsTogetherRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("bp_get_graph"));
	Hint->SetStringField(TEXT("reason"), TEXT("call it differently"));
	Hint->SetObjectField(TEXT("args"), Hint_ArgsObject());
	TArray<TSharedPtr<FJsonValue>> Options;
	Options.Add(MakeShared<FJsonValueObject>(Hint_ArgsObject()));
	Hint->SetArrayField(TEXT("options"), Options);

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'args' and 'options'")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_MissingOrEmptyReasonRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> Missing = Hint_Make();
	Missing->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(Missing, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'reason'")));

	TSharedPtr<FJsonObject> Empty = Hint_Make();
	Empty->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	Empty->SetStringField(TEXT("reason"), TEXT(""));
	UNTEST_EXPECT_TRUE(Hint_Rejects(Empty, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'reason'")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_EmptyTargetRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> EmptyTool = Hint_Make();
	EmptyTool->SetStringField(TEXT("tool"), TEXT(""));
	EmptyTool->SetStringField(TEXT("reason"), TEXT("r"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(EmptyTool, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'tool'")));

	TSharedPtr<FJsonObject> EmptyResource = Hint_Make();
	EmptyResource->SetStringField(TEXT("resource"), TEXT(""));
	EmptyResource->SetStringField(TEXT("reason"), TEXT("r"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(EmptyResource, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'resource'")));
	co_return;
}

// Reject resource targets that are not addressable URIs.
UNTEST_UNIT(Claireon, HintSchema, HintSchema_ResourceWithoutSchemeRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> BareName = Hint_Make();
	BareName->SetStringField(TEXT("resource"), TEXT("blueprint-authoring.md"));
	BareName->SetStringField(TEXT("reason"), TEXT("read this"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(BareName, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("URI with a scheme")));

	TSharedPtr<FJsonObject> Schemeless = Hint_Make();
	Schemeless->SetStringField(TEXT("resource"), TEXT("://instructions/blueprint-authoring"));
	Schemeless->SetStringField(TEXT("reason"), TEXT("read this"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(Schemeless, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("URI with a scheme")));
	co_return;
}

// Reject undocumented message fields.
UNTEST_UNIT(Claireon, HintSchema, HintSchema_UnknownFieldRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	TSharedPtr<FJsonObject> Hint = Hint_Make();
	Hint->SetStringField(TEXT("tool"), TEXT("bp_get_state"));
	Hint->SetStringField(TEXT("reason"), TEXT("session is still locked"));
	Hint->SetStringField(TEXT("message"), TEXT("session is still locked"));

	FString Error;
	UNTEST_EXPECT_TRUE(Hint_Rejects(Hint, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("unknown field")));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("message")));
	co_return;
}

// Optional rate-limit keys must be nonempty for either target kind.
UNTEST_UNIT(Claireon, HintSchema, HintSchema_KeyFieldAcceptedAndEmptyKeyRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> Keyed = Hint_Make();
	Keyed->SetStringField(TEXT("resource"), TEXT("claireon://instructions/blueprint-authoring"));
	Keyed->SetStringField(TEXT("reason"), TEXT("judgement rules live here"));
	Keyed->SetStringField(TEXT("key"), TEXT("claireon.lint.judgement-reference"));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(Keyed, Error));
	UNTEST_EXPECT_TRUE(Error.IsEmpty());

	TSharedPtr<FJsonObject> KeyedTool = Hint_Make();
	KeyedTool->SetStringField(TEXT("tool"), TEXT("uobject_inspect"));
	KeyedTool->SetStringField(TEXT("reason"), TEXT("defaults omitted at this detail level"));
	KeyedTool->SetStringField(TEXT("key"), TEXT("bp_get_component_details_defaults_omitted"));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(KeyedTool, Error));

	TSharedPtr<FJsonObject> EmptyKey = Hint_Make();
	EmptyKey->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	EmptyKey->SetStringField(TEXT("reason"), TEXT("r"));
	EmptyKey->SetStringField(TEXT("key"), TEXT(""));
	UNTEST_EXPECT_TRUE(Hint_Rejects(EmptyKey, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'key'")));

	TSharedPtr<FJsonObject> FromFactory = IClaireonTool::MakeResourceHint(
		TEXT("claireon://instructions/blueprint-authoring"), TEXT("r"),
		FName(TEXT("claireon.lint.judgement-reference")));
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(FromFactory, Error));
	UNTEST_EXPECT_STREQ(*FromFactory->GetStringField(TEXT("key")), TEXT("claireon.lint.judgement-reference"));
	TSharedPtr<FJsonObject> NoKey = IClaireonTool::MakeGuidanceHint(TEXT("bp_open"), TEXT("r"));
	UNTEST_EXPECT_FALSE(NoKey->HasField(TEXT("key")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_WrongFieldTypesRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> ToolNotString = Hint_Make();
	ToolNotString->SetNumberField(TEXT("tool"), 3);
	ToolNotString->SetStringField(TEXT("reason"), TEXT("r"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(ToolNotString, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("must be a string")));

	TSharedPtr<FJsonObject> ArgsNotObject = Hint_Make();
	ArgsNotObject->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	ArgsNotObject->SetStringField(TEXT("reason"), TEXT("r"));
	ArgsNotObject->SetStringField(TEXT("args"), TEXT("asset_path=/Game/X"));
	UNTEST_EXPECT_TRUE(Hint_Rejects(ArgsNotObject, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'args' must be an object")));

	TSharedPtr<FJsonObject> OptionsNotArray = Hint_Make();
	OptionsNotArray->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	OptionsNotArray->SetStringField(TEXT("reason"), TEXT("r"));
	OptionsNotArray->SetObjectField(TEXT("options"), Hint_ArgsObject());
	UNTEST_EXPECT_TRUE(Hint_Rejects(OptionsNotArray, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'options' must be an array")));
	co_return;
}

UNTEST_UNIT(Claireon, HintSchema, HintSchema_DegenerateOptionsRejected)
{
	using namespace ClaireonHintSchemaTestsNS;
	FString Error;

	TSharedPtr<FJsonObject> EmptyOptions = Hint_Make();
	EmptyOptions->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	EmptyOptions->SetStringField(TEXT("reason"), TEXT("r"));
	EmptyOptions->SetArrayField(TEXT("options"), TArray<TSharedPtr<FJsonValue>>());
	UNTEST_EXPECT_TRUE(Hint_Rejects(EmptyOptions, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("empty array")));

	TSharedPtr<FJsonObject> ScalarOption = Hint_Make();
	ScalarOption->SetStringField(TEXT("tool"), TEXT("bp_lint"));
	ScalarOption->SetStringField(TEXT("reason"), TEXT("r"));
	TArray<TSharedPtr<FJsonValue>> Options;
	Options.Add(MakeShared<FJsonValueObject>(Hint_ArgsObject()));
	Options.Add(MakeShared<FJsonValueString>(TEXT("scope=layout")));
	ScalarOption->SetArrayField(TEXT("options"), Options);
	UNTEST_EXPECT_TRUE(Hint_Rejects(ScalarOption, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("'options'[1]")));
	co_return;
}

#endif // WITH_UNTESTED
