// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// WI-13 self-tests: actor identity and PIE world resolution.
//
// Covers:
//  - level_list_actors matches a wildcard pattern against the actor OBJECT
//    NAME as well as the label, and returns both fields per actor.
//  - ClaireonPathResolver resolves rooted in-memory mounts (/Memory/...)
//    instead of failing with the filesystem-path error; bogus absolute
//    filesystem paths still produce the existing error.
//  - ClaireonPIEWorldResolver selection: fabricated server+client context
//    candidates, net_mode='client' picks the client context; pie_instance
//    filtering; parse validation; and the converted tools declare the shared
//    pie_instance / net_mode schema params.
//
// The list_actors tests drive the real tool Execute path against the Untest
// world via ClaireonPIEWorldResolver::SetPIEWorldOverrideForTesting (test-only
// seam), cleared by an RAII guard so assert-exits cannot leak the override.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonPIEWorldResolver.h"
#include "ClaireonPathResolver.h"
#include "Tools/ClaireonTool_ListActors.h"
#include "Tools/ClaireonTool_PIEGetActor.h"
#include "Tools/ClaireonTool_PIEGetComponent.h"
#include "Tools/ClaireonTool_StateTreeRuntimeInspect.h"
#include "Tools/ClaireonTool_UObjectInspect.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonTestTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace ClaireonActorIdentityTestsInternal
{
	// File-local discriminator prefix (ClaireonActorIdentityTests_) per
	// linux-build-server-v2 unity-batching rules.

	struct FClaireonActorIdentityTests_ScopedPIEWorldOverride
	{
		explicit FClaireonActorIdentityTests_ScopedPIEWorldOverride(UWorld* World)
		{
			ClaireonPIEWorldResolver::SetPIEWorldOverrideForTesting(World);
		}
		~FClaireonActorIdentityTests_ScopedPIEWorldOverride()
		{
			ClaireonPIEWorldResolver::SetPIEWorldOverrideForTesting(nullptr);
		}
	};

	// Scans a list_actors result's actors[] for an entry whose Field equals
	// Value. Bool-returning helper so tests can assert outside any lambda.
	bool ClaireonActorIdentityTests_FindActorEntry(
		const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field,
		const FString& Value,
		TSharedPtr<FJsonObject>& OutEntry)
	{
		OutEntry.Reset();
		if (!Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Data->TryGetArrayField(TEXT("actors"), Actors) || Actors == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Actors)
		{
			const TSharedPtr<FJsonObject> EntryObj = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!EntryObj.IsValid())
			{
				continue;
			}
			FString FieldValue;
			if (EntryObj->TryGetStringField(Field, FieldValue) && FieldValue == Value)
			{
				OutEntry = EntryObj;
				return true;
			}
		}
		return false;
	}

	// Spawns a plain AActor with an explicit object name and a differing
	// label so pattern-matching against name vs label is distinguishable.
	AActor* ClaireonActorIdentityTests_SpawnNamedActor(UWorld* World, const TCHAR* ObjectName, const TCHAR* Label)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = FName(ObjectName);
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
		if (IsValid(Actor))
		{
			Actor->SetActorLabel(Label, /*bMarkDirty=*/false);
		}
		return Actor;
	}

	// Checks a tool schema declares both shared PIE selector params.
	bool ClaireonActorIdentityTests_SchemaHasPIESelectors(IClaireonTool& Tool)
	{
		const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
		if (!Schema.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Props) || Props == nullptr)
		{
			return false;
		}
		return (*Props)->HasField(TEXT("pie_instance")) && (*Props)->HasField(TEXT("net_mode"));
	}
} // namespace ClaireonActorIdentityTestsInternal

using namespace ClaireonActorIdentityTestsInternal;

// ===========================================================================
// level_list_actors: object-name pattern matching + dual identity fields
// ===========================================================================

UNTEST_WORLD(Claireon, ActorIdentity, ListActorsMatchesObjectNamePattern)
{
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = ClaireonActorIdentityTests_SpawnNamedActor(
		World, TEXT("WI13_ObjNameTarget"), TEXT("WI13 Friendly Label"));
	UNTEST_ASSERT_PTR(Actor);

	FClaireonActorIdentityTests_ScopedPIEWorldOverride Override(World);

	ClaireonTool_ListActors Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	// Pattern equals the OBJECT NAME; the label differs. Pre-fix this
	// returned 0 actors because only GetActorLabel() was matched.
	Args->SetStringField(TEXT("label_pattern"), TEXT("WI13_ObjNameTarget"));

	const IClaireonTool::FToolResult ToolResult = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(ToolResult.bIsError);
	UNTEST_ASSERT_TRUE(ToolResult.Data.IsValid());

	TSharedPtr<FJsonObject> Entry;
	const bool bFoundByName = ClaireonActorIdentityTests_FindActorEntry(
		ToolResult.Data, TEXT("name"), TEXT("WI13_ObjNameTarget"), Entry);
	UNTEST_ASSERT_TRUE(bFoundByName);
	UNTEST_ASSERT_TRUE(Entry.IsValid());

	// Both identity fields must be present and carry the expected values.
	FString EntryLabel;
	UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("label"), EntryLabel));
	UNTEST_EXPECT_STREQ(*EntryLabel, TEXT("WI13 Friendly Label"));
	UNTEST_EXPECT_TRUE(Entry->HasField(TEXT("path")));

	co_return;
}

UNTEST_WORLD(Claireon, ActorIdentity, ListActorsMatchesLabelPatternAndRejectsNonMatches)
{
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = ClaireonActorIdentityTests_SpawnNamedActor(
		World, TEXT("WI13_LabelTargetObj"), TEXT("WI13 Label Only Target"));
	UNTEST_ASSERT_PTR(Actor);

	FClaireonActorIdentityTests_ScopedPIEWorldOverride Override(World);

	ClaireonTool_ListActors Tool;

	// Pattern matching the LABEL (with wildcard) finds the actor.
	TSharedPtr<FJsonObject> LabelArgs = MakeShared<FJsonObject>();
	LabelArgs->SetStringField(TEXT("label_pattern"), TEXT("WI13 Label Only*"));
	const IClaireonTool::FToolResult LabelResult = Tool.Execute(LabelArgs);
	UNTEST_ASSERT_FALSE(LabelResult.bIsError);
	UNTEST_ASSERT_TRUE(LabelResult.Data.IsValid());

	TSharedPtr<FJsonObject> Entry;
	const bool bFoundByLabel = ClaireonActorIdentityTests_FindActorEntry(
		LabelResult.Data, TEXT("label"), TEXT("WI13 Label Only Target"), Entry);
	UNTEST_ASSERT_TRUE(bFoundByLabel);
	FString EntryName;
	UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("name"), EntryName));
	UNTEST_EXPECT_STREQ(*EntryName, TEXT("WI13_LabelTargetObj"));

	// A pattern matching neither identity returns zero actors (the OR match
	// must not degrade into match-everything).
	TSharedPtr<FJsonObject> MissArgs = MakeShared<FJsonObject>();
	MissArgs->SetStringField(TEXT("label_pattern"), TEXT("WI13_NoSuchActorAnywhere*"));
	const IClaireonTool::FToolResult MissResult = Tool.Execute(MissArgs);
	UNTEST_ASSERT_FALSE(MissResult.bIsError);
	UNTEST_ASSERT_TRUE(MissResult.Data.IsValid());
	double MissCount = -1.0;
	UNTEST_ASSERT_TRUE(MissResult.Data->TryGetNumberField(TEXT("total_count"), MissCount));
	UNTEST_EXPECT_EQ(static_cast<int32>(MissCount), 0);

	co_return;
}

// ===========================================================================
// ClaireonPathResolver: /Memory/ mounts and absolute-path error preservation
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ActorIdentity, PathResolverMemoryPathResolves, UNTEST_TIMEOUTMS(5000))
{
	UPackage* Package = CreatePackage(TEXT("/Memory/ClaireonWI13PathTest"));
	UNTEST_ASSERT_PTR(Package);

	// UObject itself is CLASS_Abstract: allocating one fires the abstract-class
	// ensure in StaticAllocateObject (UObjectGlobals.cpp:3336) and its stack
	// walk blows the test timeout. Use a concrete in-module fixture class.
	UObject* MemObject = NewObject<UClaireonUObjectInspectFixture>(Package, FName(TEXT("WI13MemResident")), RF_Transient);
	UNTEST_ASSERT_PTR(MemObject);

	const FString ObjectPath = MemObject->GetPathName();
	UNTEST_ASSERT_STREQ(*ObjectPath, TEXT("/Memory/ClaireonWI13PathTest.WI13MemResident"));

	// Pre-fix this fell through to the absolute-filesystem heuristic and
	// failed with "does not contain 'Content/'".
	const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(ObjectPath);
	UNTEST_ASSERT_TRUE(Resolved.bSuccess);
	UNTEST_EXPECT_STREQ(*Resolved.ResolvedPath.Path, *ObjectPath);
	UNTEST_EXPECT_STREQ(*Resolved.ResolvedPath.PackagePath, TEXT("/Memory/ClaireonWI13PathTest"));

	// End-to-end through uobject_inspect, which routes object_path through
	// the resolver: the live in-memory object must be inspectable.
	ClaireonTool_UObjectInspect InspectTool;
	TSharedPtr<FJsonObject> InspectArgs = MakeShared<FJsonObject>();
	InspectArgs->SetStringField(TEXT("object_path"), ObjectPath);
	const IClaireonTool::FToolResult InspectResult = InspectTool.Execute(InspectArgs);
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	UNTEST_ASSERT_TRUE(InspectResult.Data.IsValid());
	FString ReportedPath;
	UNTEST_ASSERT_TRUE(InspectResult.Data->TryGetStringField(TEXT("object_path"), ReportedPath));
	UNTEST_EXPECT_STREQ(*ReportedPath, *ObjectPath);

	// Teardown: transient in-memory objects only; nothing written to Content.
	MemObject->MarkAsGarbage();
	Package->MarkAsGarbage();

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ActorIdentity, PathResolverBogusAbsolutePathStillErrors, UNTEST_TIMEOUTMS(5000))
{
	const ClaireonPathResolver::FResolveResult Resolved =
		ClaireonPathResolver::Resolve(TEXT("Z:/NoSuchPlace/NoAsset"));
	UNTEST_ASSERT_FALSE(Resolved.bSuccess);
	UNTEST_EXPECT_TRUE(Resolved.Error.Contains(TEXT("does not contain 'Content/'")));
	co_return;
}

// ===========================================================================
// ClaireonPIEWorldResolver: selection core against fabricated contexts
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ActorIdentity, PIEWorldSelectionNetModeAndInstance, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonPIEWorldResolver;

	// Fabricated Play-as-Client topology: context 0 is the server world,
	// context 1 is the client world.
	TArray<FPIEContextCandidate> Candidates;
	{
		FPIEContextCandidate Server;
		Server.PIEInstance = 0;
		Server.bHasWorld = true;
		Server.bIsServer = true;
		Candidates.Add(Server);

		FPIEContextCandidate Client;
		Client.PIEInstance = 1;
		Client.bHasWorld = true;
		Client.bIsClient = true;
		Candidates.Add(Client);
	}

	FString Error;

	// Default request preserves historical behavior: first PIE context (the
	// server world under Play-as-Client).
	FPIEWorldRequest DefaultRequest;
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Candidates, DefaultRequest, Error), 0);

	// net_mode='client' picks the client context -- the core WI-13 fix.
	FPIEWorldRequest ClientRequest;
	ClientRequest.NetMode = FString(TEXT("client"));
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Candidates, ClientRequest, Error), 1);

	// net_mode='server' picks the server context.
	FPIEWorldRequest ServerRequest;
	ServerRequest.NetMode = FString(TEXT("server"));
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Candidates, ServerRequest, Error), 0);

	// pie_instance selects by instance index.
	FPIEWorldRequest InstanceRequest;
	InstanceRequest.PIEInstance = 1;
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Candidates, InstanceRequest, Error), 1);

	// Unmatched pie_instance fails loudly, naming the input that failed.
	FPIEWorldRequest MissingInstanceRequest;
	MissingInstanceRequest.PIEInstance = 7;
	Error.Empty();
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Candidates, MissingInstanceRequest, Error), static_cast<int32>(INDEX_NONE));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("pie_instance=7")));

	// No live candidates -> the canonical no-PIE error (exact text contract:
	// converted tools surface this verbatim when PIE is not running).
	TArray<FPIEContextCandidate> Empty;
	Error.Empty();
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Empty, DefaultRequest, Error), static_cast<int32>(INDEX_NONE));
	UNTEST_EXPECT_STREQ(*Error, TEXT("No active PIE session. Start Play-in-Editor first."));

	// A context whose world pointer is gone counts as not live.
	TArray<FPIEContextCandidate> Dead;
	{
		FPIEContextCandidate NoWorld;
		NoWorld.PIEInstance = 0;
		NoWorld.bHasWorld = false;
		NoWorld.bIsServer = true;
		Dead.Add(NoWorld);
	}
	Error.Empty();
	UNTEST_EXPECT_EQ(SelectPIEContextIndex(Dead, DefaultRequest, Error), static_cast<int32>(INDEX_NONE));
	UNTEST_EXPECT_STREQ(*Error, TEXT("No active PIE session. Start Play-in-Editor first."));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ActorIdentity, PIEWorldRequestParsingValidation, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonPIEWorldResolver;

	FString Error;
	FPIEWorldRequest Request;

	// Absent arguments object parses to an empty request.
	UNTEST_EXPECT_TRUE(ParseRequest(nullptr, Request, Error));
	UNTEST_EXPECT_FALSE(Request.PIEInstance.IsSet());
	UNTEST_EXPECT_FALSE(Request.NetMode.IsSet());

	// Invalid net_mode fails with the exact documented error.
	TSharedPtr<FJsonObject> BadNetMode = MakeShared<FJsonObject>();
	BadNetMode->SetStringField(TEXT("net_mode"), TEXT("banana"));
	Error.Empty();
	UNTEST_EXPECT_FALSE(ParseRequest(BadNetMode, Request, Error));
	UNTEST_EXPECT_STREQ(*Error, TEXT("Invalid net_mode 'banana'. Valid values: 'server', 'client'."));

	// net_mode is case-insensitive on input, normalized to lowercase.
	TSharedPtr<FJsonObject> CasedNetMode = MakeShared<FJsonObject>();
	CasedNetMode->SetStringField(TEXT("net_mode"), TEXT("Client"));
	Error.Empty();
	UNTEST_EXPECT_TRUE(ParseRequest(CasedNetMode, Request, Error));
	UNTEST_ASSERT_TRUE(Request.NetMode.IsSet());
	UNTEST_EXPECT_STREQ(*Request.NetMode.GetValue(), TEXT("client"));

	// pie_instance must be a JSON number.
	TSharedPtr<FJsonObject> BadInstance = MakeShared<FJsonObject>();
	BadInstance->SetStringField(TEXT("pie_instance"), TEXT("zero"));
	Error.Empty();
	UNTEST_EXPECT_FALSE(ParseRequest(BadInstance, Request, Error));
	UNTEST_EXPECT_STREQ(*Error, TEXT("Invalid pie_instance: expected an integer."));

	// Valid combined request.
	TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetNumberField(TEXT("pie_instance"), 2);
	Good->SetStringField(TEXT("net_mode"), TEXT("server"));
	Error.Empty();
	UNTEST_EXPECT_TRUE(ParseRequest(Good, Request, Error));
	UNTEST_ASSERT_TRUE(Request.PIEInstance.IsSet());
	UNTEST_EXPECT_EQ(Request.PIEInstance.GetValue(), 2);
	UNTEST_ASSERT_TRUE(Request.NetMode.IsSet());
	UNTEST_EXPECT_STREQ(*Request.NetMode.GetValue(), TEXT("server"));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ActorIdentity, PIEWorldSchemaParamsOnConvertedTools, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_ListActors ListActorsTool;
	UNTEST_EXPECT_TRUE(ClaireonActorIdentityTests_SchemaHasPIESelectors(ListActorsTool));

	ClaireonTool_PIEGetActor GetActorTool;
	UNTEST_EXPECT_TRUE(ClaireonActorIdentityTests_SchemaHasPIESelectors(GetActorTool));

	ClaireonTool_PIEGetComponent GetComponentTool;
	UNTEST_EXPECT_TRUE(ClaireonActorIdentityTests_SchemaHasPIESelectors(GetComponentTool));

	ClaireonTool_StateTreeRuntimeInspect StateTreeTool;
	UNTEST_EXPECT_TRUE(ClaireonActorIdentityTests_SchemaHasPIESelectors(StateTreeTool));

	co_return;
}

#endif // WITH_UNTESTED
