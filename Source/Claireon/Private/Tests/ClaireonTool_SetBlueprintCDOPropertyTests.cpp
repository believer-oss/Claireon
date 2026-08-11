// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonLog.h"
#include "ClaireonTestTypes.h"
#include "Tools/ClaireonTool_SetBlueprintCDOProperty.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace ClaireonTool_SetBlueprintCDOPropertyTestsHelpers
{

IClaireonTool::FToolResult InvokeSetCDOProp(
	const FString& AssetPath,
	const FString& PropertyName,
	const FString& Value,
	const FString& PropertyPath = FString())
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("property_name"), PropertyName);
	Args->SetStringField(TEXT("value"), Value);
	if (!PropertyPath.IsEmpty())
	{
		Args->SetStringField(TEXT("property_path"), PropertyPath);
	}
	ClaireonTool_SetBlueprintCDOProperty Tool;
	return Tool.Execute(Args);
}

// ---------------------------------------------------------------------------
// Plugin-owned fixture Blueprints
//
// Every discovery helper in this file used to scan the first 50-100 Blueprints the
// asset registry handed back and use whatever qualified. That was wrong twice over:
//
//   1. Order-dependent, and measurably so. The scan window's contents shift as
//      earlier tests create their own fixtures. On the pre-change baseline (run
//      20260801_093825) PrimitiveArrayLeaf_WriteByIndex and
//      ChildBlueprintInheritance_OverrideRecordedOnChild BOTH logged
//      "SKIP ... found in the first 100 scanned assets" -- and both still scored as
//      PASS, because Untest has no skip primitive and an early co_return is a pass.
//      Between them that was ~130 lines of test body covering nothing.
//   2. It mutated the user's assets. The callers write to the discovered Blueprint's
//      CDO, and BlueprintDirtyStateAfterWrite cleared its package dirty flag -- so
//      running this suite in an interactive editor could discard a real unsaved edit.
//
// Everything now comes from AClaireonTestCDOStructArrayActor
// (Private/Tests/ClaireonTestTypes.h), whose constructor seeds every shape these
// tests need. SCS components and the child Blueprint are built here rather than in
// the native class because they are Blueprint-level constructs -- a native
// CreateDefaultSubobject component is not an SCS node.
//
// The fixture Blueprints live in in-memory packages only (CreatePackage, never
// UPackage::Save), so they leave no .uasset for the
// `git status --porcelain -- Content/` gate and need no delete. That matters: a
// delete would run ObjectTools::ForceDeleteObjects, whose whole-object-graph
// referencer scan crashes this suite nondeterministically.
// ---------------------------------------------------------------------------

// Named, not discovered: see the comment on AClaireonTestCDOStructArrayActor.
static const TCHAR* const kFixtureBoolPropertyName  = TEXT("bClaireonTestBool");
static const TCHAR* const kFixtureNameArrayName     = TEXT("NameArray");
static const TCHAR* const kFixtureStructArrayName   = TEXT("StructArray");
static const TCHAR* const kFixtureSceneComponentVar = TEXT("ClaireonTestSceneComp");
static const TCHAR* const kFixturePathBase          = TEXT("/Game/__MCPTests/BP_ClaireonCDOStructArrayFixture");

// The one committed project asset this file names. Read-only, used by
// TransactionUndo_SubstrateProbe to compare an on-disk Blueprint CDO against the
// synthetic fixture's. See the block comment on that test for why this asset and why
// nothing is written to it.
//
// CM_FSThirdPerson_Default is parented to UFSCameraMode_ThirdPerson, which inherits
// ULyraCameraMode_ThirdPerson, whose constructor unconditionally Add()s seven
// FLyraPenetrationAvoidanceFeeler entries -- so PenetrationAvoidanceFeelers is populated
// on the CDO by C++, not by designer-authored defaults that could be edited away.
// TraceInterval is the struct's only non-float, non-transient member, and therefore the
// only leaf whose exported text is safe to compare exactly.
static const TCHAR* const kProbeOnDiskBlueprintObjectPath =
	TEXT("/Game/BP/Camera/Default/CM_FSThirdPerson_Default.CM_FSThirdPerson_Default");
static const TCHAR* const kProbeOnDiskArrayName = TEXT("PenetrationAvoidanceFeelers");
static const TCHAR* const kProbeOnDiskLeafPath  = TEXT("PenetrationAvoidanceFeelers[0].TraceInterval");

// Reset the fixture CDO's writable state to the NATIVE class defaults.
//
// Several tests share one fixture Blueprint and each writes to it. Undo does not
// happen in the Untest commandlet -- GEditor->Trans is null there, so
// UndoTransaction() is a no-op (see TransactionUndo_SubstrateProbe below) -- which
// means a write persists for the rest of the process and the next test would read a
// leftover as its "original". Copying from the native CDO rather than hardcoding the
// constructor's values keeps this correct if the fixture's defaults ever change.
void ResetFixtureCDOToNativeDefaults(UBlueprint* BP)
{
	if (!IsValid(BP) || !IsValid(BP->GeneratedClass))
	{
		return;
	}
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UClass* NativeClass = AClaireonTestCDOStructArrayActor::StaticClass();
	UObject* NativeCDO = IsValid(NativeClass) ? NativeClass->GetDefaultObject() : nullptr;
	if (!IsValid(CDO) || !IsValid(NativeCDO))
	{
		return;
	}

	const TCHAR* const ResetProps[] = {
		kFixtureStructArrayName, kFixtureNameArrayName, kFixtureBoolPropertyName };
	for (const TCHAR* PropName : ResetProps)
	{
		FProperty* Dest = FindFProperty<FProperty>(BP->GeneratedClass, PropName);
		FProperty* Src = FindFProperty<FProperty>(NativeClass, PropName);
		if (!Dest || !Src)
		{
			continue;
		}
		Dest->CopyCompleteValue(
			Dest->ContainerPtrToValuePtr<void>(CDO),
			Src->ContainerPtrToValuePtr<void>(NativeCDO));
	}
}

// Fetch (creating on first use) a fixture Blueprint parented to
// AClaireonTestCDOStructArrayActor. FixtureSuffix gives a caller its OWN Blueprint
// where sharing one would be wrong.
//
// The SCS node is added unconditionally rather than on request: the fixture is cached
// by package path, so a conditional add would make the shape depend on which helper
// touched it first.
UBlueprint* GetOrCreateFixtureBlueprint(const FString& FixtureSuffix)
{
	const FString FixturePath = FString(kFixturePathBase) + FixtureSuffix;
	const FString AssetName = FPackageName::GetShortName(FixturePath);
	const FString ObjectPath = FixturePath + TEXT(".") + AssetName;

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad());
	if (!IsValid(BP))
	{
		UPackage* Package = CreatePackage(*FixturePath);
		if (!IsValid(Package)) { return nullptr; }
		BP = FKismetEditorUtilities::CreateBlueprint(
			AClaireonTestCDOStructArrayActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(BP)) { return nullptr; }
		FAssetRegistryModule::AssetCreated(BP);

		// USceneComponent specifically: Component_SCSTemplate_ResolvedOnComponent
		// writes "Mobility", which USceneComponent declares.
		if (IsValid(BP->SimpleConstructionScript))
		{
			USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(
				USceneComponent::StaticClass(), FName(kFixtureSceneComponentVar));
			if (IsValid(Node))
			{
				BP->SimpleConstructionScript->AddNode(Node);
			}
		}

		// Compile so GeneratedClass and its CDO are fully realised and the SCS node's
		// generated component property exists on the class. Without this the CDO
		// exists but does not behave like a loaded Blueprint's.
		FKismetEditorUtilities::CompileBlueprint(BP);
	}
	if (!IsValid(BP->GeneratedClass)) { return nullptr; }
	if (!IsValid(BP->GeneratedClass->GetDefaultObject())) { return nullptr; }

	ResetFixtureCDOToNativeDefaults(BP);
	return BP;
}

// Parent/child fixture pair for ChildBlueprintInheritance_OverrideRecordedOnChild.
// That test needs a Blueprint whose ParentClass is itself a Blueprint-generated class
// (it reads BP->ParentClass->ClassGeneratedBy and casts it to UBlueprint), which a
// native parent cannot satisfy -- hence a second Blueprint layered on the first.
// A dedicated parent, not the shared fixture, so creating the child cannot reinstance
// a class the other tests are holding a CDO pointer into.
bool GetOrCreateChildFixturePair(UBlueprint*& OutParentBP, UBlueprint*& OutChildBP)
{
	OutChildBP = nullptr;
	OutParentBP = GetOrCreateFixtureBlueprint(TEXT("_ChildParent"));
	if (!IsValid(OutParentBP) || !IsValid(OutParentBP->GeneratedClass)) { return false; }

	const FString ChildPath = FString(kFixturePathBase) + TEXT("_Child");
	const FString ChildName = FPackageName::GetShortName(ChildPath);
	const FString ChildObjectPath = ChildPath + TEXT(".") + ChildName;

	OutChildBP = Cast<UBlueprint>(FSoftObjectPath(ChildObjectPath).TryLoad());
	if (!IsValid(OutChildBP))
	{
		UPackage* Package = CreatePackage(*ChildPath);
		if (!IsValid(Package)) { return false; }
		OutChildBP = FKismetEditorUtilities::CreateBlueprint(
			OutParentBP->GeneratedClass,
			Package,
			FName(*ChildName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(OutChildBP)) { return false; }
		FAssetRegistryModule::AssetCreated(OutChildBP);
		FKismetEditorUtilities::CompileBlueprint(OutChildBP);
	}
	if (!IsValid(OutChildBP->GeneratedClass)) { return false; }
	if (!IsValid(OutChildBP->GeneratedClass->GetDefaultObject())) { return false; }

	// Both CDOs start from the native defaults: the test compares the child's
	// post-write value against the parent's untouched one, so a leftover on either
	// side from an earlier run of this test would decide the verdict.
	ResetFixtureCDOToNativeDefaults(OutParentBP);
	ResetFixtureCDOToNativeDefaults(OutChildBP);
	return true;
}

UBlueprint* LoadBlueprintWithSCSComponents_Local()
{
	UBlueprint* BP = GetOrCreateFixtureBlueprint(FString());
	if (!IsValid(BP) || !IsValid(BP->SimpleConstructionScript)) { return nullptr; }
	// Confirm rather than assume: if CreateNode/AddNode ever stops taking, the
	// caller must fail rather than silently address nothing.
	if (BP->SimpleConstructionScript->GetAllNodes().Num() < 1) { return nullptr; }
	return BP;
}

// Return the fixture Blueprint plus the name of its top-level CPF_Edit bool.
UBlueprint* FindBlueprintWithBoolCDOProperty(FString& OutPropertyName)
{
	OutPropertyName.Reset();
	UBlueprint* BP = GetOrCreateFixtureBlueprint(FString());
	if (!IsValid(BP) || !IsValid(BP->GeneratedClass)) { return nullptr; }

	// The property is named, but its flags are still checked: only CPF_Edit,
	// non-transient, non-deprecated properties participate reliably in the editor's
	// Modify/undo flow, which is what the callers assert against. A rename or a
	// specifier change on the fixture must fail here, not downstream.
	FBoolProperty* Prop = FindFProperty<FBoolProperty>(BP->GeneratedClass, kFixtureBoolPropertyName);
	if (!Prop) { return nullptr; }
	if (!Prop->HasAnyPropertyFlags(CPF_Edit)) { return nullptr; }
	if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) { return nullptr; }
	OutPropertyName = Prop->GetName();
	return BP;
}

// Returns a value string that is PROVABLY DIFFERENT from Original for the given
// member property.
//
// WHY THIS EXISTS: the previous form of several tests in this file computed
//     FString NewValue = Original.IsEmpty() ? TEXT("1") : Original;
// which, in the common (non-empty) path, wrote back the value that had just been
// read. Every downstream assertion of the shape "value == Original" then held
// even if the tool under test did absolutely nothing -- the tests could not fail.
// Any test that writes must write something different from what it read.
//
// Fixed sentinels are used instead of "Original + 1" so there is no number
// parsing, no overflow case, and no float formatting ambiguity.
FString MakeDistinctValue(const FProperty* Member, const FString& Original)
{
	const FString Trimmed = Original.TrimStartAndEnd();
	if (Member && Member->IsA<FBoolProperty>())
	{
		return Trimmed.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("False") : TEXT("True");
	}
	if (Member && Member->IsA<FIntProperty>())
	{
		return Trimmed.Equals(TEXT("7")) ? TEXT("8") : TEXT("7");
	}
	// FName / FStr. ExportText_Direct is called with PPF_None by
	// ClaireonPropertyUtils::ReadPropertyByPath, so these types export
	// undelimited and the read-back compares exactly.
	const FString Marker = TEXT("ClaireonTestValue");
	return Trimmed.Equals(Marker) ? Marker + TEXT("2") : Marker;
}

// Return the fixture Blueprint whose CDO exposes a TArray<struct> with at least one
// element and a writable primitive member, plus the array name, member name, and member
// FProperty (the last is needed by MakeDistinctValue to build a type-correct value).
//
// FixtureSuffix gives a caller its OWN Blueprint. TransactionUndo_RestoresArrayElement
// needs that: undo replays the transaction buffer, and sharing one CDO across three
// tests that each write-then-undo makes the restored value depend on the other tests'
// history. Callers that only write are happy to share.
UBlueprint* FindBlueprintWithStructArray(FString& OutArrayName, FString& OutMemberName, FProperty*& OutMember,
	const FString& FixtureSuffix = FString())
{
	// GetOrCreateFixtureBlueprint already reset the CDO to the native class defaults,
	// so every caller sees the same starting values regardless of run order.
	UBlueprint* BP = GetOrCreateFixtureBlueprint(FixtureSuffix);
	if (!IsValid(BP) || !IsValid(BP->GeneratedClass)) { return nullptr; }
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	if (!IsValid(CDO)) { return nullptr; }

	FArrayProperty* ArrProp = FindFProperty<FArrayProperty>(BP->GeneratedClass, kFixtureStructArrayName);
	if (!ArrProp || !ArrProp->Inner) { return nullptr; }
	FStructProperty* InnerStruct = CastField<FStructProperty>(ArrProp->Inner);
	if (!InnerStruct || !InnerStruct->Struct) { return nullptr; }

	// The constructor seeds one element; confirm rather than assume, so a future change
	// to the fixture cannot silently make "[0]" address nothing.
	FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(CDO));
	if (Helper.Num() < 1) { return nullptr; }

	for (TFieldIterator<FProperty> SIt(InnerStruct->Struct); SIt; ++SIt)
	{
		FProperty* Member = *SIt;
		if (!Member) { continue; }
		if (Member->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) { continue; }
		// Only types whose ExportText form round-trips to a byte-identical string, so
		// read-back assertions can use exact comparison. FFloatProperty /
		// FDoubleProperty are deliberately excluded: their exported text formatting is
		// not exact-comparable.
		if (Member->IsA<FBoolProperty>() ||
			Member->IsA<FIntProperty>() ||
			Member->IsA<FNameProperty>() ||
			Member->IsA<FStrProperty>())
		{
			OutArrayName = ArrProp->GetName();
			OutMemberName = Member->GetName();
			OutMember = Member;
			return BP;
		}
	}
	return nullptr;
}

// Return the fixture Blueprint whose CDO exposes a TArray<FName> with 3 elements.
UBlueprint* FindBlueprintWithFNameArray(FString& OutArrayName)
{
	OutArrayName.Reset();
	UBlueprint* BP = GetOrCreateFixtureBlueprint(FString());
	if (!IsValid(BP) || !IsValid(BP->GeneratedClass)) { return nullptr; }
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	if (!IsValid(CDO)) { return nullptr; }

	FArrayProperty* ArrProp = FindFProperty<FArrayProperty>(BP->GeneratedClass, kFixtureNameArrayName);
	if (!ArrProp || !ArrProp->Inner) { return nullptr; }
	if (!ArrProp->Inner->IsA<FNameProperty>()) { return nullptr; }

	// PrimitiveArrayLeaf_WriteByIndex writes [2] and asserts [0] and [1] are untouched,
	// so three elements is the minimum that makes it mean anything. Confirm rather than
	// assume, so shrinking the fixture fails here instead of downstream.
	FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(CDO));
	if (Helper.Num() < 3) { return nullptr; }

	OutArrayName = ArrProp->GetName();
	return BP;
}

// Returns the asset path (package path) for a Blueprint as expected by the tool's
// asset_path argument.
FString GetBPAssetPath(UBlueprint* BP)
{
	if (!IsValid(BP)) return FString();
	return BP->GetPathName();
}

}  // namespace ClaireonTool_SetBlueprintCDOPropertyTestsHelpers

// ---------------------------------------------------------------------------
// Test 1 -- SchemaPlumbing_PathConcatenation
// ---------------------------------------------------------------------------
// This is the ONLY test pinning the "<path>[0].<Member>" concatenation the tool
// performs in Step 5, so its discovery results are asserted rather than used as
// an `if` guard: the previous form wrapped the entire body in `if (BoolBP)` /
// `if (StructBP)` and asserted neither, so a discovery miss (or any change that
// broke asset loading) silently produced a green test with zero assertions run.
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, SchemaPlumbing_PathConcatenation, UNTEST_TIMEOUTMS(30000))
{
	// (a) empty property_path + single-segment property_name -- the fixture's top-level bool.
	FString BoolProp;
	UBlueprint* BoolBP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithBoolCDOProperty(BoolProp);
	UNTEST_ASSERT_PTR(BoolBP);
	UNTEST_ASSERT_PTR(BoolBP->GeneratedClass.Get());
	{
		FString AssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BoolBP);
		UObject* BoolCDO = BoolBP->GeneratedClass->GetDefaultObject();
		UNTEST_ASSERT_PTR(BoolCDO);

		FString ReadError;
		FString Original = ClaireonPropertyUtils::ReadPropertyByPath(BoolCDO, BoolProp, ReadError);
		// Flip the value -- interpret any non-True as False, else True. Provably
		// different from Original, so the read-back below can actually fail.
		const FString Flipped = Original.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("False") : TEXT("True");

		IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(AssetPath, BoolProp, Flipped);
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.Summary.Contains(BoolProp));

		FString AfterWrite = ClaireonPropertyUtils::ReadPropertyByPath(BoolCDO, BoolProp, ReadError);
		UNTEST_EXPECT_STRCASEEQ(*AfterWrite, *Flipped);

		if (IsValid(GEditor)) GEditor->UndoTransaction();
	}

	// (d) -- path with [N] suffix, load-bearing concatenation assertion.
	FString ArrName, MemberName;
	FProperty* Member = nullptr;
	UBlueprint* StructBP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithStructArray(ArrName, MemberName, Member);
	UNTEST_ASSERT_PTR(StructBP);
	UNTEST_ASSERT_PTR(Member);
	UNTEST_ASSERT_PTR(StructBP->GeneratedClass.Get());
	{
		FString AssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(StructBP);
		const FString PathWithIndex = ArrName + TEXT("[0]");
		const FString Leaf = PathWithIndex + TEXT(".") + MemberName;

		UObject* CDO = StructBP->GeneratedClass->GetDefaultObject();
		UNTEST_ASSERT_PTR(CDO);
		FString ReadError;
		const FString Original = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);
		// Never write back Original -- see MakeDistinctValue.
		const FString NewValue = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::MakeDistinctValue(Member, Original);
		UNTEST_ASSERT_STRCASENE(*NewValue, *Original);

		IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(AssetPath, MemberName, NewValue, PathWithIndex);
		UNTEST_ASSERT_FALSE(R.bIsError);
		// Summary format is "Set <asset>.<combined> = '<val>' (was '<old>')" --
		// ClaireonTool_SetBlueprintCDOProperty.cpp puts the full combined path in
		// Summary, so Summary is the correct channel for this assertion.
		// Confirm that the '.' and member name appear after the bracket group, i.e.
		// the combined path includes "[0].<MemberName>".
		const FString Expected = TEXT("[0].") + MemberName;
		UNTEST_EXPECT_TRUE(R.Summary.Contains(Expected));

		// And the concatenated path actually addressed that leaf.
		FString AfterWrite = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);
		UNTEST_EXPECT_STRCASEEQ(*AfterWrite, *NewValue);

		if (IsValid(GEditor)) GEditor->UndoTransaction();
	}

	co_return;
}

// ---------------------------------------------------------------------------
// Test 2 -- CDO_PlainProperty_ResolvedOnCDO
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, CDO_PlainProperty_ResolvedOnCDO, UNTEST_TIMEOUTMS(30000))
{
	FString BoolProp;
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithBoolCDOProperty(BoolProp);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);

	FString ReadError;
	FString Original = ClaireonPropertyUtils::ReadPropertyByPath(CDO, BoolProp, ReadError);
	const FString Flipped = Original.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("False") : TEXT("True");

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), BoolProp, Flipped);
	UNTEST_ASSERT_TRUE(!R.bIsError);
	UNTEST_ASSERT_PTR(R.Data.Get());

	// Capture everything BEFORE asserting, so a failing assertion (which co_returns)
	// still runs the cleanup below.
	FString ResolvedOn;
	const bool bHasResolvedOn = R.Data->TryGetStringField(TEXT("resolved_on"), ResolvedOn);
	const FString NewVal = ClaireonPropertyUtils::ReadPropertyByPath(CDO, BoolProp, ReadError);
	if (IsValid(GEditor)) GEditor->UndoTransaction();

	// resolved_on is the whole point of this test, and the tool emits the field
	// only when Resolved.ResolvedOn is non-empty
	// (ClaireonTool_SetBlueprintCDOProperty.cpp Step 11). The previous form
	// guarded on `if (R.Data->TryGetStringField(...))`, so a resolver regression
	// that returned an empty ResolvedOn omitted the field entirely and this test
	// passed green while proving nothing. Assert the presence first, then the
	// value -- same shape as Component_SCSTemplate_ResolvedOnComponent below.
	UNTEST_EXPECT_TRUE(bHasResolvedOn);
	if (bHasResolvedOn)
	{
		UNTEST_EXPECT_STREQ(*ResolvedOn, TEXT("CDO"));
	}

	UNTEST_EXPECT_STRCASEEQ(*NewVal, *Flipped);

	// NOTE: the UndoTransaction above is cleanup only, not coverage, and it does not even
	// achieve that under -run=UntestRunTests (GEditor->Trans is null there). Restoration
	// is not asserted here; TransactionUndo_SubstrateProbe carries the undo question,
	// and the fixture CDO is reset to native defaults on the next fetch either way.

	co_return;
}

// ---------------------------------------------------------------------------
// Test 3 -- Component_SCSTemplate_ResolvedOnComponent
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, Component_SCSTemplate_ResolvedOnComponent, UNTEST_TIMEOUTMS(30000))
{
	// Both skips this test used to take were project-content-dependent: it scanned the
	// first 50 Blueprints for one with SCS nodes, then for a node carrying a 'Mobility'
	// property, and warn-and-co_return'd on either miss -- which Untest scores as a
	// PASS. It also wrote 'Mobility' straight onto whichever user asset it landed on.
	// The fixture adds its own USceneComponent SCS node, so a miss here is a broken
	// fixture and must fail.
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::LoadBlueprintWithSCSComponents_Local();
	UNTEST_ASSERT_PTR(BP);
	// .Get(): UNTEST_ASSERT_PTR takes `const T*` and has no TObjectPtr overload.
	UNTEST_ASSERT_PTR(BP->SimpleConstructionScript.Get());

	USCS_Node* TargetNode = nullptr;
	for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
	{
		if (!IsValid(Node) || !IsValid(Node->ComponentTemplate)) continue;
		if (Node->ComponentTemplate->GetClass()->FindPropertyByName(FName(TEXT("Mobility"))))
		{
			TargetNode = Node;
			break;
		}
	}
	UNTEST_ASSERT_PTR(TargetNode);

	FString VarName = TargetNode->GetVariableName().ToString();
	FString AssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP);

	// Use property_path = VarName and property_name = "Mobility" -- combined = VarName.Mobility.
	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(AssetPath, TEXT("Mobility"), TEXT("Static"), VarName);
	UNTEST_ASSERT_TRUE(!R.bIsError);
	UNTEST_ASSERT_PTR(R.Data.Get());

	FString ResolvedOn;
	bool bHasResolvedOn = R.Data->TryGetStringField(TEXT("resolved_on"), ResolvedOn);
	UNTEST_EXPECT_TRUE(bHasResolvedOn);
	if (bHasResolvedOn)
	{
		UNTEST_EXPECT_TRUE(ResolvedOn != TEXT("CDO"));
	}

	// Read directly from the component template
	FString ReadError;
	FString ReadBack = ClaireonPropertyUtils::ReadPropertyByPath(
		TargetNode->ComponentTemplate, TEXT("Mobility"), ReadError);
	UNTEST_EXPECT_TRUE(ReadBack.Contains(TEXT("Static")));

	if (IsValid(GEditor)) GEditor->UndoTransaction();
	co_return;
}

// ---------------------------------------------------------------------------
// Test 4 -- Write_ArrayElementValueLandsOnCDOAndDirtiesPackage
//
// RENAMED from TransactionRoundTrip_ArrayElementSurvivesSaveReload. The old name
// promised a save/reload round-trip the body never performed, and its two
// surviving assertions were "the package is dirty" and "the read produced no
// error" -- neither compares a value, so nothing round-tripped and nothing could
// fail. It also used the "NewValue = Original" idiom (write back the value you
// just read), which makes any post-write value comparison vacuous.
//
// COVERAGE DELIBERATELY NOT TAKEN: an actual on-disk save + package reload. The
// fixture is created in an in-memory package on purpose so the suite leaves no
// .uasset behind for the `git status --porcelain -- Content/` gate; saving it would
// defeat that. (The original reason given here -- that the only qualifying assets
// were tracked project assets -- no longer applies now that the fixture is
// plugin-owned; the in-memory constraint is the reason that remains.)
// What is asserted instead is that the mutation is present on the CDO the editor
// would serialize, and that the package is marked dirty so a save WOULD persist
// it. Serialization fidelity itself remains unpinned by this file.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, Write_ArrayElementValueLandsOnCDOAndDirtiesPackage, UNTEST_TIMEOUTMS(30000))
{
	FString ArrName, MemberName;
	FProperty* Member = nullptr;
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithStructArray(ArrName, MemberName, Member);
	// Was a warn-and-co_return, which Untest scores as a PASS (there is no skip
	// primitive), so a discovery miss silently covered nothing. The helper now builds a
	// plugin-owned fixture rather than scanning, so a null here means the fixture itself
	// is broken -- fail.
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(Member);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);

	const FString AssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP);
	const FString Leaf = ArrName + TEXT("[0].") + MemberName;

	FString ReadError;
	const FString Original = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);
	UNTEST_ASSERT_TRUE(ReadError.IsEmpty());

	// Provably different from Original -- see MakeDistinctValue.
	const FString NewValue = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::MakeDistinctValue(Member, Original);
	UNTEST_ASSERT_STRCASENE(*NewValue, *Original);

	// Clear the dirty marker so the `bDirty` assertion below measures THIS write rather
	// than the fixture's creation, which dirtied the package. Safe because the package is
	// the plugin's own in-memory fixture -- never a user asset. Doing this to a
	// *discovered project* Blueprint (which is what this file used to do) is what would
	// destroy a developer's unsaved edit.
	BP->GetOutermost()->SetDirtyFlag(false);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(AssetPath, MemberName, NewValue, ArrName + TEXT("[0]"));
	// A failure of the tool under test is a test failure, never a skip.
	const bool bWriteFailed = R.bIsError;
	const FString WriteError = R.ErrorMessage;

	const bool bDirty = BP->GetOutermost()->IsDirty();
	const FString AfterWrite = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);
	const FString AfterWriteError = ReadError;

	// Best-effort cleanup only; FindBlueprintWithStructArray resets the fixture CDO on
	// the next fetch, so nothing depends on this undo landing.
	if (IsValid(GEditor)) GEditor->UndoTransaction();

	if (bWriteFailed)
	{
		UE_LOG(LogClaireon, Error, TEXT("[SetBlueprintCDOProperty] write failed on '%s' path '%s': %s"), *AssetPath, *Leaf, *WriteError);
	}
	UNTEST_ASSERT_FALSE(bWriteFailed);
	UNTEST_EXPECT_TRUE(AfterWriteError.IsEmpty());
	UNTEST_EXPECT_STRCASEEQ(*AfterWrite, *NewValue);
	UNTEST_EXPECT_TRUE(bDirty);
	co_return;
}

// ---------------------------------------------------------------------------
// Test 5 -- TransactionUndo_RestoresArrayElement
//
// The old body wrote back the value it had just read
// (`NewValue = Original.IsEmpty() ? TEXT("1") : Original`), so the final
// "Restored == Original" assertion held with GEditor->UndoTransaction()
// completely broken -- the value never changed in the first place. It also
// skipped on `if (R.bIsError)` and on `if (!GEditor)`, so a tool failure or a
// missing editor turned a test whose name promises undo coverage into a green
// no-op. Sequence now is: write a DIFFERENT value -> assert the read-back
// changed -> undo -> assert restoration.
// ---------------------------------------------------------------------------
// STILL DISABLED, deliberately and visibly -- see
// Docs/llm/todo/claireon-test-suite-debt.md item 6 and
// Docs/llm/todo/claireon-product-defects.md.
//
// History: this test never verified undo in any recorded run. Its
// FindBlueprintWithStructArray call used to scan project Blueprints, and when that scan
// came up empty the test warn-and-co_return'd, which Untest scores as a PASS. That is
// exactly what happened in run 20260729_162120, where SchemaPlumbing_PathConcatenation
// failed with "StructBP is nullptr" from the SAME helper while this test "passed".
// Pointed at the deterministic plugin-owned fixture it runs for real and FAILS: after
// GEditor->UndoTransaction() the array element still holds the written value. That
// reproduces with a dedicated fixture (so not cross-test transaction history) and after
// FKismetEditorUtilities::CompileBlueprint (so not an unrealised GeneratedClass/CDO).
//
// It stays disabled because it asserts a capability the product does not have, and one
// this harness could not observe even if it did. Both halves were settled by reading
// engine source; TransactionUndo_SubstrateProbe below is the runnable form:
//
//   1. PRODUCT. bp_set_cdo_property opens an FScopedTransaction and calls
//      CDO->Modify(), but a Blueprint CDO is allocated with only
//      RF_Public|RF_ClassDefaultObject|RF_ArchetypeObject (UClass::CreateDefaultObject,
//      Class.cpp), and SaveToTransactionBuffer() refuses any object without
//      RF_Transactional (UObjectGlobals.cpp). So Modify() marks the package dirty and
//      records nothing. Epic hits the same wall and works around it explicitly --
//      WidgetBlueprintEditorUtils.cpp does `WidgetCDO->SetFlags(RF_Transactional);
//      WidgetCDO->Modify();` before mutating a Widget Blueprint CDO. The tool is
//      missing that SetFlags. The substrate is irrelevant: the flag is absent on every
//      Blueprint CDO, in-memory fixture or fully loaded on-disk asset alike, so
//      possibility (b) from the debt doc is refuted.
//   2. HARNESS. UEditorEngine::UndoTransaction() is `return Trans && Trans->Undo(...)`,
//      and GEditor->Trans is only created in UEditorEngine::Init -- which an editor
//      commandlet never reaches, because LaunchEngineLoop.cpp:4089 calls
//      GEditor->InitEditor(this) instead. CanTransact() is `Trans != nullptr && ...`,
//      so under -run=UntestRunTests FScopedTransaction is a no-op and no undo of any
//      kind happens. Two other tests in this plugin
//      (PropertyUtils_Write.PrimitiveAndUndo, PropertyResolver_Actor.WriteReadRoundTrip)
//      already gate on UndoTransaction()'s return value for exactly this reason.
//
// Consequence for anyone re-running the experiment: "undo did not restore" is NOT
// evidence of (1) on its own, because (2) produces the same observation for any
// substrate. Re-enable this test only after the tool sets RF_Transactional AND the run
// has a transaction buffer; until then it would be red for an environmental reason.
// Do NOT "fix" it by reverting the helper to discovery -- that restores the vacuous pass.
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, TransactionUndo_RestoresArrayElement, UNTEST_DISABLED())
{
	// The tool under test opens an FScopedTransaction, so GEditor is a hard
	// requirement here, not an environment nicety. Asserting it makes the gap
	// visible instead of silently greening the test.
	UNTEST_ASSERT_PTR(GEditor);

	FString ArrName, MemberName;
	FProperty* Member = nullptr;
	// Dedicated fixture: undo replays the transaction buffer, so this test must not share
	// a CDO with the other two that write-then-undo against the same property.
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithStructArray(
		ArrName, MemberName, Member, TEXT("_Undo"));
	// Was a warn-and-co_return, which Untest scores as a PASS. The helper now builds a
	// plugin-owned fixture rather than scanning, so a null here is a broken fixture.
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(Member);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());
	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);

	const FString AssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP);
	const FString Leaf = ArrName + TEXT("[0].") + MemberName;

	FString ReadError;
	const FString Original = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);
	UNTEST_ASSERT_TRUE(ReadError.IsEmpty());

	// Provably different from Original -- see MakeDistinctValue.
	const FString NewValue = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::MakeDistinctValue(Member, Original);
	UNTEST_ASSERT_STRCASENE(*NewValue, *Original);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(AssetPath, MemberName, NewValue, ArrName + TEXT("[0]"));
	if (R.bIsError)
	{
		UE_LOG(LogClaireon, Error, TEXT("[SetBlueprintCDOProperty] write failed on '%s' path '%s': %s"), *AssetPath, *Leaf, *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	// The write must be observable BEFORE the undo, otherwise the restoration
	// assertion below proves nothing.
	const FString AfterWrite = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);

	GEditor->UndoTransaction();
	const FString Restored = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf, ReadError);

	UNTEST_EXPECT_STRCASEEQ(*AfterWrite, *NewValue);
	UNTEST_EXPECT_STRCASEEQ(*Restored, *Original);
	co_return;
}

// ---------------------------------------------------------------------------
// Test 5b -- TransactionUndo_SubstrateProbe
//
// The runnable half of the question TransactionUndo_RestoresArrayElement is disabled
// for (Docs/llm/todo/claireon-test-suite-debt.md item 6): when
// bp_set_cdo_property's FScopedTransaction fails to roll back a CDO array-element
// write, is that
//   (a) a product defect -- the tool cannot get a CDO into the transaction buffer at
//       all, or
//   (b) an artefact of the synthetic in-memory fixture, with a fully loaded on-disk
//       Blueprint behaving differently?
//
// The debt doc's prescribed discriminator was "point the test at a named on-disk
// Blueprint and see whether undo restores". That experiment is CONFOUNDED in this
// harness and must not be run as the deciding evidence: GEditor->Trans is null under
// -run=UntestRunTests (LaunchEngineLoop.cpp:4089 calls GEditor->InitEditor, not
// UEditorEngine::Init, and only Init calls CreateTrans), so no undo happens for ANY
// substrate and "did not restore" would read as (a) whether or not (a) is true.
//
// What IS environment-independent is RF_Transactional on the CDO.
// UObject::Modify() -> SaveToTransactionBuffer() refuses any object lacking that flag,
// so a CDO without it can never enter the buffer -- commandlet or warm editor.
// Comparing that one flag between a NAMED on-disk project Blueprint and the synthetic
// fixture settles (a) vs (b) outright:
//   equal              -> substrate is not the variable, so (b) is refuted
//   both false         -> the tool's transaction cannot capture the CDO       -> (a)
//   on-disk true only  -> only an on-disk CDO is a valid substrate            -> (b)
//
// The on-disk Blueprint is NAMED, never discovered -- a scan miss would
// warn-and-co_return, which Untest scores as a PASS. If it stops loading, or its
// struct array stops being populated, this test FAILS (which is also the guard debt
// item 7 asks for: a fixture pinned to a shipping asset that rots silently).
// Nothing is written to it: reading an object flag needs no mutation, which is also
// what keeps this test out of the user's content. The write/undo half runs on the
// plugin-owned fixture only.
//
// Chosen asset: /Game/BP/Camera/Default/CM_FSThirdPerson_Default, parented to
// UFSCameraMode_ThirdPerson -> ULyraCameraMode_ThirdPerson, whose constructor
// unconditionally Add()s seven FLyraPenetrationAvoidanceFeeler entries to
// PenetrationAvoidanceFeelers (LyraCameraMode_ThirdPerson.cpp). So the CDO's
// TArray<struct> is populated by C++, not by designer-authored defaults that could be
// edited away, and FLyraPenetrationAvoidanceFeeler::TraceInterval is an int32 -- an
// exact-comparable leaf, unlike the struct's four float members.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, TransactionUndo_SubstrateProbe, UNTEST_TIMEOUTMS(60000))
{
	// The tool under test opens an FScopedTransaction, so GEditor is a hard requirement.
	UNTEST_ASSERT_PTR(GEditor);

	// ---- The named on-disk Blueprint. Read-only. ----
	// TryLoad rather than LoadObject: same synchronous resolve, but it is the pattern the
	// rest of this file uses and it keeps the no-load-object lint rule satisfied.
	UBlueprint* OnDiskBP = Cast<UBlueprint>(
		FSoftObjectPath(ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kProbeOnDiskBlueprintObjectPath).TryLoad());
	if (!IsValid(OnDiskBP))
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[SetBlueprintCDOProperty] TransactionUndo_SubstrateProbe: failed to load the named ")
			TEXT("on-disk fixture '%s'. If that asset was renamed or deleted, repoint this test at ")
			TEXT("another committed Blueprint whose CDO has a C++-populated TArray<struct>."),
			ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kProbeOnDiskBlueprintObjectPath);
	}
	UNTEST_ASSERT_PTR(OnDiskBP);
	UNTEST_ASSERT_PTR(OnDiskBP->GeneratedClass.Get());
	UObject* OnDiskCDO = OnDiskBP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(OnDiskCDO);

	// Pin the shape, so this fixture cannot rot into a no-op the way the attenuation
	// fixtures did (debt item 7).
	FArrayProperty* OnDiskArr = FindFProperty<FArrayProperty>(
		OnDiskBP->GeneratedClass, ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kProbeOnDiskArrayName);
	UNTEST_ASSERT_TRUE(OnDiskArr != nullptr);
	UNTEST_ASSERT_TRUE(CastField<FStructProperty>(OnDiskArr->Inner) != nullptr);
	{
		FScriptArrayHelper OnDiskHelper(OnDiskArr, OnDiskArr->ContainerPtrToValuePtr<void>(OnDiskCDO));
		UNTEST_ASSERT_GE(OnDiskHelper.Num(), 1);
	}
	FString ProbeReadError;
	const FString OnDiskLeafValue = ClaireonPropertyUtils::ReadPropertyByPath(
		OnDiskCDO, ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kProbeOnDiskLeafPath, ProbeReadError);
	UNTEST_EXPECT_TRUE(ProbeReadError.IsEmpty());
	UNTEST_EXPECT_FALSE(OnDiskLeafValue.IsEmpty());

	// ---- The synthetic in-memory fixture. ----
	FString ArrName, MemberName;
	FProperty* Member = nullptr;
	UBlueprint* FixtureBP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithStructArray(
		ArrName, MemberName, Member, TEXT("_Probe"));
	UNTEST_ASSERT_PTR(FixtureBP);
	UNTEST_ASSERT_PTR(Member);
	UNTEST_ASSERT_PTR(FixtureBP->GeneratedClass.Get());
	UObject* FixtureCDO = FixtureBP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(FixtureCDO);

	// ---- The discriminator. ----
	const bool bOnDiskCDOTransactional = OnDiskCDO->HasAnyFlags(RF_Transactional);
	const bool bFixtureCDOTransactional = FixtureCDO->HasAnyFlags(RF_Transactional);
	const bool bHasTransactionBuffer = GEditor->CanTransact();

	// Write to the FIXTURE (never the named project asset) and see what the tool's own
	// transaction achieves.
	const FString FixtureAssetPath = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(FixtureBP);
	const FString FixtureLeaf = ArrName + TEXT("[0].") + MemberName;
	FString ReadError;
	const FString Original = ClaireonPropertyUtils::ReadPropertyByPath(FixtureCDO, FixtureLeaf, ReadError);
	UNTEST_ASSERT_TRUE(ReadError.IsEmpty());
	const FString NewValue = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::MakeDistinctValue(Member, Original);
	UNTEST_ASSERT_STRCASENE(*NewValue, *Original);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		FixtureAssetPath, MemberName, NewValue, ArrName + TEXT("[0]"));
	if (R.bIsError)
	{
		UE_LOG(LogClaireon, Error, TEXT("[SetBlueprintCDOProperty] probe write failed on '%s' path '%s': %s"),
			*FixtureAssetPath, *FixtureLeaf, *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	const FString AfterWrite = ClaireonPropertyUtils::ReadPropertyByPath(FixtureCDO, FixtureLeaf, ReadError);

	const bool bUndone = GEditor->UndoTransaction();
	const FString Restored = ClaireonPropertyUtils::ReadPropertyByPath(FixtureCDO, FixtureLeaf, ReadError);

	// One grep-able line carrying every fact the verdict rests on, so a single run of
	// `-TestFilter Claireon.SetBlueprintCDOProperty.` settles the question.
	UE_LOG(LogClaireon, Warning,
		TEXT("[SetBlueprintCDOProperty] TransactionUndo_SubstrateProbe VERDICT: ")
		TEXT("ondisk_cdo_transactional=%s fixture_cdo_transactional=%s can_transact=%s ")
		TEXT("undo_returned=%s original='%s' after_write='%s' restored='%s' ")
		TEXT("(ondisk asset '%s', fixture leaf '%s')"),
		bOnDiskCDOTransactional ? TEXT("true") : TEXT("false"),
		bFixtureCDOTransactional ? TEXT("true") : TEXT("false"),
		bHasTransactionBuffer ? TEXT("true") : TEXT("false"),
		bUndone ? TEXT("true") : TEXT("false"),
		*Original, *AfterWrite, *Restored,
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kProbeOnDiskBlueprintObjectPath,
		*FixtureLeaf);

	// The write itself must have landed, or nothing below means anything.
	UNTEST_EXPECT_STRCASEEQ(*AfterWrite, *NewValue);

	// SETTLED, by this probe's first real execution on 2026-08-01. The original form
	// asserted bOnDiskCDOTransactional == bFixtureCDOTransactional, predicting both false
	// on the reasoning that RF_Transactional is absent from every Blueprint CDO because
	// UClass::CreateDefaultObject never sets it and nothing adds it later. That reasoning
	// was WRONG, and this assertion is what caught it. Measured:
	//
	//   ondisk_cdo_transactional=true  fixture_cdo_transactional=false
	//
	// RF_Transactional is part of RF_Load (ObjectMacros.h:598), so it round-trips through
	// save/load. An authored on-disk Blueprint's CDO therefore carries it, while a
	// freshly synthesized in-memory fixture's does not. Substrate IS the variable, which
	// is exactly possibility (b) -- so (b) was never refuted, and the defect was real but
	// narrower than first written up: bp_set_cdo_property failed to record undo only for
	// CDOs lacking the flag, which is the tool-created and in-memory cases, not ordinary
	// authored assets.
	//
	// So the equality is NOT asserted -- it is false by design. What is asserted is the
	// invariant this tool actually owns: after bp_set_cdo_property writes to a CDO, that
	// CDO must be transactional, because the tool sets RF_Transactional before Modify().
	// That holds with or without a transaction buffer, so it is checkable here even though
	// GEditor->Trans is null under a commandlet.
	const bool bFixtureCDOTransactionalAfterWrite = FixtureCDO->HasAnyFlags(RF_Transactional);
	UNTEST_EXPECT_TRUE(bFixtureCDOTransactionalAfterWrite);

	// The on-disk flag is recorded rather than asserted: it depends on how that particular
	// asset was authored and saved, so pinning it would be brittle. The VERDICT line above
	// carries it for anyone re-examining the substrate question.

	// Undo restoration is only assertable where an undo can happen at all. Gated on
	// UndoTransaction()'s return value, the same convention as
	// PropertyUtils_Write.PrimitiveAndUndo and PropertyResolver_Actor.WriteReadRoundTrip.
	if (bUndone)
	{
		UNTEST_EXPECT_STRCASEEQ(*Restored, *Original);
	}
	else
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[SetBlueprintCDOProperty] TransactionUndo_SubstrateProbe: not asserting undo ")
			TEXT("restoration -- GEditor->UndoTransaction() returned false, so this process has no ")
			TEXT("editor transaction buffer (GEditor->Trans is null outside UEditorEngine::Init, ")
			TEXT("e.g. in a commandlet). The RF_Transactional comparison above is the verdict."));
	}
	co_return;
}

// ---------------------------------------------------------------------------
// Test 6 -- BlueprintDirtyStateAfterWrite
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, BlueprintDirtyStateAfterWrite, UNTEST_TIMEOUTMS(30000))
{
	FString BoolProp;
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithBoolCDOProperty(BoolProp);
	// Same plugin-owned fixture as SchemaPlumbing_PathConcatenation. A null here means
	// the fixture's bool went missing or lost CPF_Edit, never a reason to pass silently.
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);
	FString ReadError;
	FString Original = ClaireonPropertyUtils::ReadPropertyByPath(CDO, BoolProp, ReadError);
	const FString Flipped = Original.Equals(TEXT("True"), ESearchCase::IgnoreCase) ? TEXT("False") : TEXT("True");

	// Clear the dirty marker so `bDirty` below measures THIS write and not the fixture's
	// creation. Safe because this is the plugin's own in-memory fixture package -- doing it
	// to a *discovered project* Blueprint, which is what this test used to do, is the way
	// this suite could destroy a developer's unsaved edit. Removing the call rather than
	// repointing it would make the one assertion this test has vacuous: the package is
	// already dirty from CreateBlueprint/CompileBlueprint.
	BP->GetOutermost()->SetDirtyFlag(false);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), BoolProp, Flipped);
	const bool bWriteFailed = R.bIsError;
	const bool bDirty = BP->GetOutermost()->IsDirty();

	if (IsValid(GEditor)) GEditor->UndoTransaction();

	UNTEST_EXPECT_FALSE(bWriteFailed);
	UNTEST_EXPECT_TRUE(bDirty);
	co_return;
}

// ---------------------------------------------------------------------------
// Test 7 -- ErrorPassthrough_MalformedArrayIndex
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, ErrorPassthrough_MalformedArrayIndex, UNTEST_TIMEOUTMS(30000))
{
	FString BoolProp;
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithBoolCDOProperty(BoolProp);
	UNTEST_ASSERT_PTR(BP);

	// Intentionally malformed: unmatched '['
	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP),
		TEXT("anything"),
		TEXT("anything"),
		TEXT("waves[0.spawn_count"));
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Malformed array index")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("waves[0")));
	co_return;
}

// ---------------------------------------------------------------------------
// Test 8 -- ErrorPassthrough_NonexistentComponent
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, ErrorPassthrough_NonexistentComponent, UNTEST_TIMEOUTMS(30000))
{
	FString BoolProp;
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithBoolCDOProperty(BoolProp);
	UNTEST_ASSERT_PTR(BP);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP),
		TEXT("RelativeLocation"),
		TEXT("(X=0,Y=0,Z=0)"),
		TEXT("NoSuchComponent1234"));
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("not found")));
	co_return;
}

// ---------------------------------------------------------------------------
// Test 9 -- PrimitiveArrayLeaf_WriteByIndex
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, PrimitiveArrayLeaf_WriteByIndex, UNTEST_TIMEOUTMS(30000))
{
	FString ArrName;
	// MEASURED: on the pre-fixture baseline (run 20260801_093825) this test logged
	// "SKIP ... no Blueprint with a TArray<FName> of >= 3 elements found in the first 100
	// scanned assets" and still scored PASS -- the whole body below never ran. The helper
	// now returns the plugin-owned fixture, whose constructor seeds three FNames, so a
	// null here is a broken fixture and must fail.
	UBlueprint* BP = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::FindBlueprintWithFNameArray(ArrName);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_FALSE(ArrName.IsEmpty());
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);
	FString ReadError;
	const FString Leaf2 = ArrName + TEXT("[2]");
	const FString Leaf0 = ArrName + TEXT("[0]");
	const FString Leaf1 = ArrName + TEXT("[1]");

	const FString Orig0 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf0, ReadError);
	const FString Orig1 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf1, ReadError);
	const FString Orig2 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf2, ReadError);

	// Guarantee the written value differs from what is already there, so the
	// read-back assertion below can actually fail.
	const FString NewName = Orig2.Equals(TEXT("NewTagName"), ESearchCase::IgnoreCase) ? TEXT("NewTagName2") : TEXT("NewTagName");
	UNTEST_ASSERT_STRCASENE(*NewName, *Orig2);

	// Tool requires non-empty property_name; put the full path there with empty property_path.
	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP),
		Leaf2,
		NewName);
	const bool bWriteFailed = R.bIsError;

	const FString New2 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf2, ReadError);
	const FString After0 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf0, ReadError);
	const FString After1 = ClaireonPropertyUtils::ReadPropertyByPath(CDO, Leaf1, ReadError);

	// Best-effort cleanup only; FindBlueprintWithFNameArray resets the fixture CDO on the
	// next fetch, so nothing depends on this undo landing.
	if (IsValid(GEditor)) GEditor->UndoTransaction();

	UNTEST_EXPECT_FALSE(bWriteFailed);
	UNTEST_EXPECT_STRCASEEQ(*New2, *NewName);
	UNTEST_EXPECT_STREQ(*After0, *Orig0);
	UNTEST_EXPECT_STREQ(*After1, *Orig1);
	co_return;
}

// ---------------------------------------------------------------------------
// Test 10 -- ChildBlueprintInheritance_OverrideRecordedOnChild
//
// Needs a parent/child Blueprint pair with a shared TArray<FStructProperty> CDO
// field: the child's ParentClass must itself be a Blueprint-generated class.
//
// This used to be 57 lines of project scan that skipped -- and therefore PASSED,
// Untest having no skip primitive -- when no such pair turned up. MEASURED: on the
// pre-fixture baseline (run 20260801_093825) it logged
// "SKIP ... no child/parent Blueprint pair with a populated TArray<struct> found in
// the first 100 scanned assets", so this test has covered nothing. The old comment
// claimed transient parent/child creation was too unstable for the harness; that is
// not borne out -- the plugin-owned struct-array fixture has been created and
// compiled this way since PR #24587, and ClaireonMaterialTests does the same.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, ChildBlueprintInheritance_OverrideRecordedOnChild, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* ParentBP = nullptr;
	UBlueprint* ChildBP = nullptr;
	const bool bPairBuilt = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetOrCreateChildFixturePair(ParentBP, ChildBP);
	// Plugin-owned fixtures: a failure here is a broken fixture, never a reason to pass.
	UNTEST_ASSERT_TRUE(bPairBuilt);
	UNTEST_ASSERT_PTR(ParentBP);
	UNTEST_ASSERT_PTR(ChildBP);
	UNTEST_ASSERT_PTR(ChildBP->GeneratedClass.Get());
	UNTEST_ASSERT_PTR(ParentBP->GeneratedClass.Get());

	// The inheritance contract this test exists for: the child's parent must be a
	// Blueprint-generated class, not a native one, or the override has nowhere to be
	// "recorded on the child" relative to.
	// TSubclassOf / TObjectPtr do not bind to UNTEST_ASSERT_PTR's `const T*`.
	UNTEST_ASSERT_TRUE(IsValid(ChildBP->ParentClass));
	UNTEST_ASSERT_PTR(ChildBP->ParentClass->ClassGeneratedBy.Get());
	UBlueprint* ResolvedParent = Cast<UBlueprint>(ChildBP->ParentClass->ClassGeneratedBy.Get());
	UNTEST_ASSERT_EQ(ResolvedParent, ParentBP);

	// Resolve the array + exact-comparable member off the PARENT class, using the same
	// member-selection rules as FindBlueprintWithStructArray so both tests pin the same
	// shape. Resolved here rather than through that helper because the helper is bound to
	// the shared fixture Blueprint, and this test needs its own parent.
	FString ArrName;
	FString MemberName;
	FProperty* MemberProp = nullptr;
	{
		FArrayProperty* ArrProp = FindFProperty<FArrayProperty>(
			ParentBP->GeneratedClass, ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::kFixtureStructArrayName);
		UNTEST_ASSERT_TRUE(ArrProp != nullptr);
		UNTEST_ASSERT_TRUE(ArrProp->Inner != nullptr);
		FStructProperty* InnerStruct = CastField<FStructProperty>(ArrProp->Inner);
		UNTEST_ASSERT_TRUE(InnerStruct != nullptr);
		UNTEST_ASSERT_TRUE(InnerStruct->Struct != nullptr);

		UObject* ParentCDOForShape = ParentBP->GeneratedClass->GetDefaultObject();
		UNTEST_ASSERT_PTR(ParentCDOForShape);
		FScriptArrayHelper ShapeHelper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(ParentCDOForShape));
		UNTEST_ASSERT_GE(ShapeHelper.Num(), 1);

		for (TFieldIterator<FProperty> SIt(InnerStruct->Struct); SIt; ++SIt)
		{
			FProperty* Member = *SIt;
			if (!Member) { continue; }
			if (Member->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) { continue; }
			// Exact-comparable types only (see FindBlueprintWithStructArray):
			// float/double exported text is not safe to compare by string.
			if (Member->IsA<FBoolProperty>() ||
				Member->IsA<FIntProperty>() ||
				Member->IsA<FNameProperty>() ||
				Member->IsA<FStrProperty>())
			{
				ArrName = ArrProp->GetName();
				MemberName = Member->GetName();
				MemberProp = Member;
				break;
			}
		}
	}
	UNTEST_ASSERT_TRUE(MemberProp != nullptr);

	UObject* ChildCDO = ChildBP->GeneratedClass->GetDefaultObject();
	UObject* ParentCDO = ParentBP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(ChildCDO);
	UNTEST_ASSERT_PTR(ParentCDO);

	const FString Leaf = ArrName + TEXT("[0].") + MemberName;
	FString ReadError;
	const FString ParentOriginal = ClaireonPropertyUtils::ReadPropertyByPath(ParentCDO, Leaf, ReadError);
	UNTEST_ASSERT_TRUE(ReadError.IsEmpty());
	const FString ChildOriginal = ClaireonPropertyUtils::ReadPropertyByPath(ChildCDO, Leaf, ReadError);
	UNTEST_ASSERT_TRUE(ReadError.IsEmpty());

	// The old body used `NewValue = ChildOriginal.IsEmpty() ? TEXT("2") : ChildOriginal`
	// -- writing back the value it had just read -- and then asserted only that the
	// PARENT was unchanged. Since nothing changed anywhere, that assertion held with
	// the tool doing nothing at all, and the child (the subject of the test name) was
	// never asserted. Write a distinct value, then assert BOTH halves of the
	// inheritance contract: the child records the override, the parent does not.
	const FString NewValue = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::MakeDistinctValue(MemberProp, ChildOriginal);
	UNTEST_ASSERT_STRCASENE(*NewValue, *ChildOriginal);

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(ChildBP), MemberName, NewValue, ArrName + TEXT("[0]"));
	if (R.bIsError)
	{
		UE_LOG(LogClaireon, Error, TEXT("[SetBlueprintCDOProperty] write failed on child '%s' path '%s': %s"), *ChildBP->GetPathName(), *Leaf, *R.ErrorMessage);
	}
	const bool bWriteFailed = R.bIsError;

	const FString ChildAfter = ClaireonPropertyUtils::ReadPropertyByPath(ChildCDO, Leaf, ReadError);
	const FString ParentAfter = ClaireonPropertyUtils::ReadPropertyByPath(ParentCDO, Leaf, ReadError);

	// Best-effort cleanup only. The fixture is plugin-owned and in-memory, and
	// GetOrCreateChildFixturePair resets both CDOs on the next fetch, so nothing depends
	// on this undo landing -- which it does not in the commandlet (GEditor->Trans is
	// null there; see TransactionUndo_SubstrateProbe).
	if (IsValid(GEditor)) GEditor->UndoTransaction();

	UNTEST_ASSERT_FALSE(bWriteFailed);
	// The override is recorded on the child...
	UNTEST_EXPECT_STRCASEEQ(*ChildAfter, *NewValue);
	// ...and the parent CDO is untouched.
	UNTEST_EXPECT_STREQ(*ParentAfter, *ParentOriginal);
	co_return;
}

// ---------------------------------------------------------------------------
// Tests for the CPF_InstancedReference auto-construct path: writes to a
// UPROPERTY(Instanced) FObjectProperty slot interpret the value as a class
// path and construct an embedded sub-object instead of trying to resolve
// the path as an existing object reference.
// ---------------------------------------------------------------------------

namespace ClaireonTool_SetBlueprintCDOPropertyTests_Private
{
	// ClaireonInstancedSlotHolder::DefaultTargetingInstance is a plugin-owned
	// UPROPERTY(Instanced) UObject* slot. Both fixture classes are declared
	// in-module (Private/Tests/ClaireonTestTypes.h), so they are ALWAYS
	// available in a process running these tests -- their absence would itself
	// be a defect. Tests therefore assert on them rather than skipping.
	static const TCHAR* InstancedBPParentClassPath  = TEXT("/Script/Claireon.ClaireonInstancedSlotHolder");
	static const TCHAR* InstancedSubObjectClassPath = TEXT("/Script/Claireon.ClaireonInstancedSlotValue");
	static const TCHAR* InstancedPropertyName       = TEXT("DefaultTargetingInstance");
	static const TCHAR* InstancedTestBPPath         = TEXT("/Game/__MCPTests/BP_SetCDOProp_Instanced");

	UBlueprint* CreateInstancedTestBlueprint()
	{
		UClass* ParentClass = FSoftClassPath(InstancedBPParentClassPath).TryLoadClass<UObject>();
		if (!IsValid(ParentClass)) return nullptr;

		const FString ObjectPath = FString(InstancedTestBPPath) + TEXT(".") + FPackageName::GetShortName(InstancedTestBPPath);
		if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad()); IsValid(Existing))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(InstancedTestBPPath);
		if (!IsValid(Package)) return nullptr;

		const FString AssetName = FPackageName::GetShortName(InstancedTestBPPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(BP)) return nullptr;

		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			InstancedTestBPPath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::Save(Package, BP, *PackageFileName, SaveArgs);

		return BP;
	}

	void CleanupInstancedTestBlueprint()
	{
		const FString ObjectPath = FString(InstancedTestBPPath) + TEXT(".") + FPackageName::GetShortName(InstancedTestBPPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}
}
using namespace ClaireonTool_SetBlueprintCDOPropertyTests_Private;

// Test 11 -- Writing a class path to a UPROPERTY(Instanced) UObject* slot
// auto-constructs the sub-object via SetInstancedSubObject and the slot
// resolves to a live instance of the requested class.
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, InstancedSlot_ClassPathConstructsSubObject, UNTEST_TIMEOUTMS(60000))
{
	// In-module fixture class: it always loads, so a null here is a defect.
	UClass* SubObjectClass = FSoftClassPath(InstancedSubObjectClassPath).TryLoadClass<UObject>();
	UNTEST_ASSERT_PTR(SubObjectClass);

	CleanupInstancedTestBlueprint();
	UBlueprint* BP = CreateInstancedTestBlueprint();
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);

	FObjectProperty* SlotProp = CastField<FObjectProperty>(
		BP->GeneratedClass->FindPropertyByName(FName(InstancedPropertyName)));
	UNTEST_ASSERT_PTR(SlotProp);
	UNTEST_ASSERT_TRUE(SlotProp->HasAnyPropertyFlags(CPF_InstancedReference));

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), InstancedPropertyName, InstancedSubObjectClassPath);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	// The tool always sets InstancedNote on this path, so "note" is guaranteed
	// present on success -- assert its presence instead of guarding on it, or a
	// regression that routed the write away from SetInstancedSubObject would just
	// drop the field and pass.
	FString Note;
	const bool bHasNote = R.Data->TryGetStringField(TEXT("note"), Note);
	UNTEST_EXPECT_TRUE(bHasNote);
	if (bHasNote)
	{
		UNTEST_EXPECT_TRUE(Note.Contains(TEXT("SetInstancedSubObject")));
	}

	UObject* Value = SlotProp->GetObjectPropertyValue(SlotProp->ContainerPtrToValuePtr<void>(CDO));
	UNTEST_ASSERT_PTR(Value);
	UNTEST_EXPECT_TRUE(Value->IsA(SubObjectClass));

	CleanupInstancedTestBlueprint();
	co_return;
}

// Test 12 -- Writing "None" to a populated UPROPERTY(Instanced) slot clears
// it and marks the previous value as garbage.
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, InstancedSlot_NoneClearsSlot, UNTEST_TIMEOUTMS(60000))
{
	// In-module fixture class: it always loads, so a null here is a defect.
	UClass* SubObjectClass = FSoftClassPath(InstancedSubObjectClassPath).TryLoadClass<UObject>();
	UNTEST_ASSERT_PTR(SubObjectClass);

	CleanupInstancedTestBlueprint();
	UBlueprint* BP = CreateInstancedTestBlueprint();
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	// Seed the slot first so we have something to clear.
	IClaireonTool::FToolResult Seed = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), InstancedPropertyName, InstancedSubObjectClassPath);
	UNTEST_ASSERT_FALSE(Seed.bIsError);

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	FObjectProperty* SlotProp = CastField<FObjectProperty>(
		BP->GeneratedClass->FindPropertyByName(FName(InstancedPropertyName)));
	UNTEST_ASSERT_PTR(SlotProp);
	UNTEST_ASSERT_PTR(SlotProp->GetObjectPropertyValue(SlotProp->ContainerPtrToValuePtr<void>(CDO)));

	IClaireonTool::FToolResult Clear = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), InstancedPropertyName, TEXT("None"));
	UNTEST_ASSERT_FALSE(Clear.bIsError);

	UObject* AfterClear = SlotProp->GetObjectPropertyValue(SlotProp->ContainerPtrToValuePtr<void>(CDO));
	UNTEST_EXPECT_NULLPTR(AfterClear);

	CleanupInstancedTestBlueprint();
	co_return;
}

// Test 13 -- A class-path value that doesn't resolve to a real UClass is
// rejected before any mutation and surfaces an error.
UNTEST_UNIT_OPTS(Claireon, SetBlueprintCDOProperty, InstancedSlot_UnresolvableClassPathErrors, UNTEST_TIMEOUTMS(60000))
{
	CleanupInstancedTestBlueprint();
	UBlueprint* BP = CreateInstancedTestBlueprint();
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);
	FObjectProperty* SlotProp = CastField<FObjectProperty>(
		BP->GeneratedClass->FindPropertyByName(FName(InstancedPropertyName)));
	UNTEST_ASSERT_PTR(SlotProp);

	UObject* BeforeAttempt = SlotProp->GetObjectPropertyValue(SlotProp->ContainerPtrToValuePtr<void>(CDO));

	IClaireonTool::FToolResult R = ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::InvokeSetCDOProp(
		ClaireonTool_SetBlueprintCDOPropertyTestsHelpers::GetBPAssetPath(BP), InstancedPropertyName, TEXT("/Script/NotARealModule.NotARealClass"));
	UNTEST_EXPECT_TRUE(R.bIsError);

	UObject* AfterAttempt = SlotProp->GetObjectPropertyValue(SlotProp->ContainerPtrToValuePtr<void>(CDO));
	UNTEST_EXPECT_EQ(AfterAttempt, BeforeAttempt);

	CleanupInstancedTestBlueprint();
	co_return;
}

#endif // WITH_UNTESTED
