// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for statetree_set_schema_property.
// Surface-level: real schema mutation requires an open session + asset, exercised manually.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeTool_SetSchemaProperty.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, StateTreeSetSchemaProperty, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_SetSchemaProperty Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("statetree_set_schema_property"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

#endif // WITH_UNTESTED
