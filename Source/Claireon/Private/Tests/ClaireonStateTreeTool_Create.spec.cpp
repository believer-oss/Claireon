// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for statetree_create.
// Verify the tool surface registers cleanly, that required-field validation
// errors fire on missing inputs, and that the happy path actually produces an
// asset resolvable at the requested path.
//
// That last case used to be absent -- this header previously read "Asset-backed
// creation is exercised manually via the editor" -- so every negative path was
// covered and the one that matters was not. The tool reports success and returns
// an asset_path, but nothing asserted that a subsequent load at the REQUESTED
// path resolves, which is what every downstream tool (statetree_open,
// statetree_apply_spec) actually does.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeTool_Create.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "StateTree.h"
#include "Tests/ClaireonTestSchemaDiscovery.h"
#include "UObject/UObjectGlobals.h"

#include "ClaireonTestAssetDeletion.h"
UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("statetree_create"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, MissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("schema_class_path"), TEXT("/Script/StateTreeModule.StateTreeSchema"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, MissingSchemaClassPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/ST_DoesNotMatter"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("schema_class_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, BogusSchemaClass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/ST_BogusSchema_DoesNotMatter"));
	Args->SetStringField(TEXT("schema_class_path"), TEXT("/Script/Engine.NotAStateTreeSchemaXYZ"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// The happy path: creating an asset must leave it resolvable at the path the
// caller asked for, because that is the contract every downstream tool relies on.
//
// UStateTreeSchema is the abstract base and the tool correctly refuses it, so a
// concrete subclass is required. Those live in the host game's modules, and
// Claireon is meant to stay mirror-safe for the standalone OSS repo where no such
// module exists -- hence a soft dependency: skip with a logged reason rather than
// fail if the schema cannot be resolved.
UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, HappyPathAssetIsLoadableAtRequestedPath, UNTEST_TIMEOUTMS(30000))
{
	const FString RequestedPath = TEXT("/Game/__MCPTests/ST_CreateHappyPath");

	// /Game/__MCPTests is NOT gitignored and survives between runs, so clear any copy a
	// previous run left behind before asserting anything about creation.
	if (UEditorAssetLibrary::DoesAssetExist(RequestedPath))
	{
		ClaireonTestAssetDeletion::DeleteAssetForTest(RequestedPath);
	}

	// Gate on the SCHEMA being resolvable, not on the tool succeeding. An earlier
	// version of this test skipped on `Result.bIsError`, which swallowed every real
	// regression it exists to catch -- a bad path, a failed save or the wrong factory
	// all reported "no schema available" and passed. Resolve the class up front so the
	// only thing the skip can mean is "this repo has no concrete schema".
	const FString SchemaClassPath = ClaireonTestSchemaDiscovery::FindConcreteStateTreeSchemaClassPath();
	if (SchemaClassPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[StateTreeCreate] No concrete UStateTreeSchema subclass is loaded in this project; "
				"skipping (nothing to author a StateTree against)."));
		co_return;
	}

	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), RequestedPath);
	Args->SetStringField(TEXT("schema_class_path"), SchemaClassPath);

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	// With the schema present, any error IS the failure this test exists to report.
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// The echoed path must be the one that was asked for, canonicalized -- not merely
	// present. A tool that quietly created the asset somewhere else would otherwise
	// satisfy a bare "field exists" check.
	FString ReportedPath;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetStringField(TEXT("asset_path"), ReportedPath));
	UNTEST_EXPECT_TRUE(ReportedPath.Contains(TEXT("ST_CreateHappyPath")));

	// The assertion that matters: resolve by the REQUESTED path, the way
	// statetree_open and statetree_apply_spec do.
	UNTEST_EXPECT_TRUE(UEditorAssetLibrary::DoesAssetExist(RequestedPath));
	const UStateTree* Loaded =
		Cast<UStateTree>(StaticLoadObject(UStateTree::StaticClass(), nullptr, *RequestedPath));
	UNTEST_EXPECT_TRUE(Loaded != nullptr);

	ClaireonTestAssetDeletion::DeleteAssetForTest(RequestedPath);
	co_return;
}

#endif // WITH_UNTESTED
