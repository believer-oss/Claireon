// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonAstralJsonGuard.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonAstralJsonTransportTestsPrivate
{
	static FString Rpc(const FString& Arguments, const TCHAR* Name = TEXT("astral_matrix_author"))
	{
		return FString::Printf(TEXT("{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s}}"), Name, *Arguments);
	}
}
using namespace ClaireonAstralJsonTransportTestsPrivate;

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, BothIngressParsersRejectDecodedCollisions,
	UNTEST_TIMEOUTMS(30000.0))
{
	const TCHAR* Payloads[] = {
		TEXT(R"json({"ember_cost":{"Ember":1,"ember":2}})json"),
		TEXT(R"json({"ember_cost":{"ember":1,"ember":2}})json"),
		TEXT(R"json({"spec":{"nodes":[{"ember_cost":{"\u0045mber":1,"ember":2}}]}})json"),
		TEXT(R"json({"a":{"x":1,"\u0078":2}})json"),
		TEXT(R"json({"a":{"\"":1,"\u0022":2}})json"),
		TEXT(R"json({"a":{"":1,"":2}})json")
	};
	for (const TCHAR* Payload : Payloads)
	{
		const auto Http = ClaireonAstralJsonGuard::DeserializeRpc(Rpc(Payload));
		const auto Python = ClaireonAstralJsonGuard::DeserializeToolArguments(TEXT("astral_matrix_author"), Payload);
		UNTEST_EXPECT_TRUE(Http.bAmbiguousKeys);
		UNTEST_EXPECT_TRUE(Python.bAmbiguousKeys);
		UNTEST_EXPECT_FALSE(Http.Object.IsValid());
		UNTEST_EXPECT_FALSE(Python.Object.IsValid());
		UNTEST_ASSERT_TRUE(Http.RequestId.IsValid());
		UNTEST_EXPECT_EQ(Http.RequestId->AsNumber(), 41.0);
		UNTEST_EXPECT_FALSE(Http.Error.IsEmpty());
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, EveryAstralOperationIsGuarded,
	UNTEST_TIMEOUTMS(30000.0))
{
	for (const TCHAR* Operation : {TEXT("author"), TEXT("inspect"), TEXT("validate"), TEXT("add_node"),
		TEXT("remove_node"), TEXT("set_property"), TEXT("reparent"), TEXT("save")})
	{
		const FString Name = FString(TEXT("astral_matrix_")) + Operation;
		const FString Raw = TEXT(R"json({"asset_path":"a","ASSET_PATH":"b"})json");
		UNTEST_EXPECT_TRUE(ClaireonAstralJsonGuard::DeserializeRpc(Rpc(Raw, *Name)).bAmbiguousKeys);
		UNTEST_EXPECT_TRUE(ClaireonAstralJsonGuard::DeserializeToolArguments(Name, Raw).bAmbiguousKeys);
	}
	UNTEST_EXPECT_TRUE(ClaireonAstralJsonGuard::DeserializeToolArguments(TEXT("ASTRAL_MATRIX_AUTHOR"), TEXT("{\"X\":1,\"x\":2}")).bAmbiguousKeys);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, AmbiguousRoutingCannotHideAstralInvocation,
	UNTEST_TIMEOUTMS(30000.0))
{
	const TCHAR* Requests[] = {
		TEXT(R"json({"method":"tools/call","params":{"name":"astral_matrix_save","name":"other"}})json"),
		TEXT(R"json({"method":"tools/call","params":{"name":"other","Name":"astral_matrix_save"}})json"),
		TEXT(R"json({"params":{"arguments":{},"name":"astral_matrix_save"},"params":{"name":"other"},"method":"tools/call"})json"),
		TEXT(R"json({"method":"tools/call","Method":"tools/list","params":{"name":"astral_matrix_save"}})json"),
		TEXT(R"json({"method":"tools/call","p\u0061rams":{"arguments":{"x":1,"X":2},"n\u0061me":"astral_matrix_save"}})json")
	};
	for (const TCHAR* Raw : Requests)
	{
		const auto Result = ClaireonAstralJsonGuard::DeserializeRpc(Raw);
		UNTEST_EXPECT_TRUE(Result.bAmbiguousKeys);
		UNTEST_EXPECT_FALSE(Result.Object.IsValid());
	}
	const auto IdCollision = ClaireonAstralJsonGuard::DeserializeRpc(
		TEXT(R"json({"id":1,"ID":2,"method":"tools/call","params":{"name":"astral_matrix_save"}})json"));
	UNTEST_EXPECT_TRUE(IdCollision.bAmbiguousKeys);
	UNTEST_EXPECT_FALSE(IdCollision.RequestId.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, SeparateObjectsAndEscapedStringsRemainValid,
	UNTEST_TIMEOUTMS(30000.0))
{
	const FString Raw = TEXT(R"json({"nodes":[{"Ember":1},{"ember":2}],"other":{"ember":3},"text":"\"Ember\":1,\"ember\":2 braces {} [] \\ end","\u0061":1,"a/b":2,"a~b":3})json");
	const auto Http = ClaireonAstralJsonGuard::DeserializeRpc(Rpc(Raw));
	const auto Python = ClaireonAstralJsonGuard::DeserializeToolArguments(TEXT("astral_matrix_author"), Raw);
	UNTEST_EXPECT_TRUE(Http.Object.IsValid());
	UNTEST_ASSERT_TRUE(Python.Object.IsValid());
	UNTEST_EXPECT_EQ(Python.Object->GetNumberField(TEXT("a")), 1.0);
	UNTEST_EXPECT_EQ(Python.Object->GetArrayField(TEXT("nodes")).Num(), 2);
	UNTEST_EXPECT_TRUE(Python.Object->GetStringField(TEXT("text")).Contains(TEXT("braces {} [] \\ end")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, OtherToolsKeepExistingDomBehavior,
	UNTEST_TIMEOUTMS(30000.0))
{
	const FString Raw = TEXT(R"json({"x":1,"x":2,"nested":{"name":"astral_matrix_save"}})json");
	for (const TCHAR* Name : {TEXT("other"), TEXT("astral_matrix_author_extra"), TEXT("fs.astral_matrix_author")})
	{
		const auto Http = ClaireonAstralJsonGuard::DeserializeRpc(Rpc(Raw, Name));
		const auto Python = ClaireonAstralJsonGuard::DeserializeToolArguments(Name, Raw);
		UNTEST_EXPECT_FALSE(Http.bAmbiguousKeys);
		UNTEST_EXPECT_TRUE(Http.Object.IsValid());
		UNTEST_ASSERT_TRUE(Python.Object.IsValid());
		UNTEST_EXPECT_EQ(Python.Object->GetNumberField(TEXT("x")), 2.0);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AstralMatrixMCPTransport, MalformedJsonNeverProducesDispatchArguments,
	UNTEST_TIMEOUTMS(30000.0))
{
	for (const TCHAR* Raw : {TEXT("{\"x\":1,"), TEXT("{\"x\":\"\\q\"}"), TEXT("{} {}"), TEXT("[{}]")})
	{
		const auto Result = ClaireonAstralJsonGuard::DeserializeToolArguments(TEXT("astral_matrix_author"), Raw);
		UNTEST_EXPECT_FALSE(Result.Object.IsValid());
		UNTEST_EXPECT_FALSE(Result.bAmbiguousKeys);
	}
	co_return;
}
#endif
