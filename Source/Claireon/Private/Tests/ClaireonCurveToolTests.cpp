// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Workstream E (F3): the curve_* tool family. Every mutating case asserts against
// a FRESH LOAD of the saved asset, not just the tool's response JSON -- a response
// that reports success while nothing reached disk is exactly the failure class
// these tools exist to avoid.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonCurveTool_AddKey.h"
#include "Tools/ClaireonCurveTool_ClearKeys.h"
#include "Tools/ClaireonCurveTool_Create.h"
#include "Tools/ClaireonCurveTool_SetKeys.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonCTTestsInternal
{
	static FString CT_ObjectPath(const FString& AssetPath)
	{
		return AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	}

	static void CT_CleanupAsset(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);
		if (UObject* Asset = FSoftObjectPath(CT_ObjectPath(AssetPath)).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	static IClaireonTool::FToolResult CT_Create(const FString& AssetPath, const TCHAR* CurveType)
	{
		FClaireonCurveTool_Create Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("curve_type"), CurveType);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult CT_AddKey(
		const FString& AssetPath, double Time, double Value,
		const TCHAR* InterpMode = nullptr, const TCHAR* CurveName = nullptr)
	{
		FClaireonCurveTool_AddKey Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("time"), Time);
		Args->SetNumberField(TEXT("value"), Value);
		if (InterpMode) { Args->SetStringField(TEXT("interp_mode"), InterpMode); }
		if (CurveName)  { Args->SetStringField(TEXT("curve_name"), CurveName); }
		return Tool.Execute(Args);
	}

	/** Force the saved asset out of memory so the next load really comes from disk. */
	static void CT_UnloadAsset(const FString& AssetPath)
	{
		UObject* Loaded = FSoftObjectPath(CT_ObjectPath(AssetPath)).TryLoad();
		if (!IsValid(Loaded)) { return; }
		UPackage* Package = Loaded->GetOutermost();
		if (!IsValid(Package)) { return; }
		TArray<UPackage*> ToUnload;
		ToUnload.Add(Package);
		UPackageTools::FUnloadPackageParams UnloadParams(ToUnload);
		UPackageTools::UnloadPackages(UnloadParams);
		CollectGarbage(RF_NoFlags);
	}

	/** Unload then reload, so assertions see serialized state rather than the live object. */
	static UCurveBase* CT_FreshLoad(const FString& AssetPath)
	{
		CT_UnloadAsset(AssetPath);
		return Cast<UCurveBase>(FSoftObjectPath(CT_ObjectPath(AssetPath)).TryLoad());
	}

	static int32 CT_ResponseInt(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		int32 Out = -1;
		if (R.Data.IsValid()) { R.Data->TryGetNumberField(Field, Out); }
		return Out;
	}
} // namespace ClaireonCTTestsInternal

using namespace ClaireonCTTestsInternal;

UNTEST_UNIT_OPTS(Claireon, CurveTool, Create_AllThreeTypesHaveExpectedSubCurveCounts, UNTEST_TIMEOUTMS(60000))
{
	struct FCase { const TCHAR* Path; const TCHAR* Type; int32 ExpectedCurves; };
	const FCase Cases[] = {
		{TEXT("/Game/__MCPTests/Curve_CT_Float"),  TEXT("float"),        1},
		{TEXT("/Game/__MCPTests/Curve_CT_Vector"), TEXT("vector"),       3},
		{TEXT("/Game/__MCPTests/Curve_CT_Color"),  TEXT("linear_color"), 4},
	};

	for (const FCase& Case : Cases)
	{
		CT_CleanupAsset(Case.Path);

		IClaireonTool::FToolResult R = CT_Create(Case.Path, Case.Type);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[CurveTool] create %s failed: %s"), Case.Type, *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_EQ(CT_ResponseInt(R, TEXT("num_curves")), Case.ExpectedCurves);

		UCurveBase* Reloaded = CT_FreshLoad(Case.Path);
		UNTEST_ASSERT_PTR(Reloaded);
		UNTEST_EXPECT_EQ(Reloaded->GetCurves().Num(), Case.ExpectedCurves);

		CT_CleanupAsset(Case.Path);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, Create_ExistingPathAndBadTypeAreNamedErrors, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_Dup");

	CT_CleanupAsset(AssetPath);

	IClaireonTool::FToolResult First = CT_Create(AssetPath, TEXT("float"));
	UNTEST_ASSERT_FALSE(First.bIsError);

	// Existing path is an error, never a silent overwrite.
	IClaireonTool::FToolResult Second = CT_Create(AssetPath, TEXT("float"));
	UNTEST_EXPECT_TRUE(Second.bIsError);
	UNTEST_EXPECT_TRUE(Second.ErrorMessage.Contains(TEXT("already exists")));

	IClaireonTool::FToolResult BadType = CT_Create(TEXT("/Game/__MCPTests/Curve_CT_BadType"), TEXT("quaternion"));
	UNTEST_EXPECT_TRUE(BadType.bIsError);
	UNTEST_EXPECT_TRUE(BadType.ErrorMessage.Contains(TEXT("Unknown curve_type")));

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, AddKey_FloatTwoKeysSurviveFreshLoad, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_AddKeyFloat");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("float")).bIsError);

	// curve_index omitted on purpose: 0 is the default and the only legal value here.
	IClaireonTool::FToolResult First = CT_AddKey(AssetPath, 0.0, 1.0, TEXT("cubic"));
	if (First.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[CurveTool] add_key failed: %s"), *First.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(First.bIsError);
	UNTEST_EXPECT_EQ(CT_ResponseInt(First, TEXT("key_count")), 1);

	IClaireonTool::FToolResult Second = CT_AddKey(AssetPath, 2.0, 0.5, TEXT("cubic"));
	UNTEST_ASSERT_FALSE(Second.bIsError);
	UNTEST_EXPECT_EQ(CT_ResponseInt(Second, TEXT("key_count")), 2);

	UCurveFloat* Reloaded = Cast<UCurveFloat>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurve.GetNumKeys(), 2);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(0.0f), 1.0f, 0.001f);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(2.0f), 0.5f, 0.001f);

	// Interp mode round-tripped too.
	for (auto It = Reloaded->FloatCurve.GetKeyHandleIterator(); It; ++It)
	{
		UNTEST_EXPECT_TRUE(Reloaded->FloatCurve.GetKeyInterpMode(*It) == RCIM_Cubic);
	}

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, SetKeys_FireRateRampEvaluatesAfterFreshLoad, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_FireRate");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("float")).bIsError);

	// The original report's ramp: rate falls off over 8 seconds.
	const double Ramp[5][2] = {{0.0, 1.0}, {2.0, 0.8}, {4.0, 0.6}, {6.0, 0.45}, {8.0, 0.3}};
	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const auto& Pair : Ramp)
	{
		TArray<TSharedPtr<FJsonValue>> Tuple;
		Tuple.Add(MakeShared<FJsonValueNumber>(Pair[0]));
		Tuple.Add(MakeShared<FJsonValueNumber>(Pair[1]));
		Keys.Add(MakeShared<FJsonValueArray>(Tuple));
	}

	FClaireonCurveTool_SetKeys Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetArrayField(TEXT("keys"), Keys);
	Args->SetStringField(TEXT("interp_mode"), TEXT("linear"));
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[CurveTool] set_keys failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_EQ(CT_ResponseInt(R, TEXT("key_count")), 5);

	UCurveFloat* Reloaded = Cast<UCurveFloat>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurve.GetNumKeys(), 5);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(0.0f), 1.0f,  0.001f);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(4.0f), 0.6f,  0.001f);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(8.0f), 0.3f,  0.001f);
	// Linear interpolation between authored keys.
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(1.0f), 0.9f,  0.001f);

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, SetKeys_ReplacesRatherThanAppends, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_Replace");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("float")).bIsError);

	// Seed three, then replace with two.
	UNTEST_ASSERT_FALSE(CT_AddKey(AssetPath, 0.0, 0.0).bIsError);
	UNTEST_ASSERT_FALSE(CT_AddKey(AssetPath, 1.0, 1.0).bIsError);
	IClaireonTool::FToolResult Third = CT_AddKey(AssetPath, 2.0, 2.0);
	UNTEST_ASSERT_FALSE(Third.bIsError);
	UNTEST_EXPECT_EQ(CT_ResponseInt(Third, TEXT("key_count")), 3);

	TArray<TSharedPtr<FJsonValue>> Keys;
	for (int32 I = 0; I < 2; ++I)
	{
		TArray<TSharedPtr<FJsonValue>> Tuple;
		Tuple.Add(MakeShared<FJsonValueNumber>(static_cast<double>(I) * 5.0));
		Tuple.Add(MakeShared<FJsonValueNumber>(static_cast<double>(I)));
		Keys.Add(MakeShared<FJsonValueArray>(Tuple));
	}

	FClaireonCurveTool_SetKeys Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetArrayField(TEXT("keys"), Keys);
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_EXPECT_EQ(CT_ResponseInt(R, TEXT("key_count")), 2);

	UCurveFloat* Reloaded = Cast<UCurveFloat>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurve.GetNumKeys(), 2);

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, AddKey_VectorCurveNameTargetsOnlyThatSubCurve, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_VectorY");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("vector")).bIsError);

	IClaireonTool::FToolResult R = CT_AddKey(AssetPath, 1.0, 42.0, TEXT("linear"), TEXT("Y"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[CurveTool] vector add_key failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UCurveVector* Reloaded = Cast<UCurveVector>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	// Y got the key; X and Z are untouched.
	UNTEST_EXPECT_EQ(Reloaded->FloatCurves[1].GetNumKeys(), 1);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurves[0].GetNumKeys(), 0);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurves[2].GetNumKeys(), 0);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurves[1].Eval(1.0f), 42.0f, 0.001f);

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, ClearKeys_NoSelectorClearsEverySubCurve, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_ClearAll");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("linear_color")).bIsError);

	const TCHAR* Channels[] = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
	for (const TCHAR* Channel : Channels)
	{
		UNTEST_ASSERT_FALSE(CT_AddKey(AssetPath, 0.5, 0.25, TEXT("linear"), Channel).bIsError);
	}

	FClaireonCurveTool_ClearKeys Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	// Neither curve_index nor curve_name: clear the whole asset.
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[CurveTool] clear_keys failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* ClearedNames = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("cleared_curve_names"), ClearedNames));
	UNTEST_ASSERT_TRUE(ClearedNames != nullptr);
	UNTEST_EXPECT_EQ(ClearedNames->Num(), 4);
	TSet<FString> Reported;
	for (const TSharedPtr<FJsonValue>& Value : *ClearedNames)
	{
		if (Value.IsValid()) { Reported.Add(Value->AsString()); }
	}
	for (const TCHAR* Channel : Channels)
	{
		UNTEST_EXPECT_TRUE(Reported.Contains(FString(Channel)));
	}

	UCurveLinearColor* Reloaded = Cast<UCurveLinearColor>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	for (int32 I = 0; I < 4; ++I)
	{
		UNTEST_EXPECT_EQ(Reloaded->FloatCurves[I].GetNumKeys(), 0);
	}

	CT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, SelectorErrorsNameValidNamesAndRejectFloatCurveName, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* VectorPath = TEXT("/Game/__MCPTests/Curve_CT_BadName");
	static const TCHAR* FloatPath  = TEXT("/Game/__MCPTests/Curve_CT_FloatName");

	CT_CleanupAsset(VectorPath);
	CT_CleanupAsset(FloatPath);
	UNTEST_ASSERT_FALSE(CT_Create(VectorPath, TEXT("vector")).bIsError);
	UNTEST_ASSERT_FALSE(CT_Create(FloatPath, TEXT("float")).bIsError);

	// A bad name must list what IS valid for this asset's actual type.
	IClaireonTool::FToolResult BadName = CT_AddKey(VectorPath, 0.0, 0.0, nullptr, TEXT("Q"));
	UNTEST_EXPECT_TRUE(BadName.bIsError);
	UNTEST_EXPECT_TRUE(BadName.ErrorMessage.Contains(TEXT("X")));
	UNTEST_EXPECT_TRUE(BadName.ErrorMessage.Contains(TEXT("Y")));
	UNTEST_EXPECT_TRUE(BadName.ErrorMessage.Contains(TEXT("Z")));

	// A float asset's one curve is named after the asset, so curve_name is meaningless.
	IClaireonTool::FToolResult FloatName = CT_AddKey(FloatPath, 0.0, 0.0, nullptr, TEXT("X"));
	UNTEST_EXPECT_TRUE(FloatName.bIsError);
	UNTEST_EXPECT_TRUE(FloatName.ErrorMessage.Contains(TEXT("not supported for float")));

	// Out-of-range index reports the legal range.
	{
		FClaireonCurveTool_AddKey Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), VectorPath);
		Args->SetNumberField(TEXT("time"), 0.0);
		Args->SetNumberField(TEXT("value"), 0.0);
		Args->SetNumberField(TEXT("curve_index"), 7);
		IClaireonTool::FToolResult BadIndex = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(BadIndex.bIsError);
		UNTEST_EXPECT_TRUE(BadIndex.ErrorMessage.Contains(TEXT("out of range")));
		UNTEST_EXPECT_TRUE(BadIndex.ErrorMessage.Contains(TEXT("0..2")));
	}

	CT_CleanupAsset(VectorPath);
	CT_CleanupAsset(FloatPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CurveTool, SetKeys_MalformedListLeavesExistingKeysIntact, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/Curve_CT_BadKeys");

	CT_CleanupAsset(AssetPath);
	UNTEST_ASSERT_FALSE(CT_Create(AssetPath, TEXT("float")).bIsError);
	UNTEST_ASSERT_FALSE(CT_AddKey(AssetPath, 0.0, 7.0).bIsError);

	// Second element is a 3-tuple -> reject before Reset, so the seeded key survives.
	TArray<TSharedPtr<FJsonValue>> Good;
	Good.Add(MakeShared<FJsonValueNumber>(1.0));
	Good.Add(MakeShared<FJsonValueNumber>(2.0));
	TArray<TSharedPtr<FJsonValue>> Bad;
	Bad.Add(MakeShared<FJsonValueNumber>(1.0));
	Bad.Add(MakeShared<FJsonValueNumber>(2.0));
	Bad.Add(MakeShared<FJsonValueNumber>(3.0));

	TArray<TSharedPtr<FJsonValue>> Keys;
	Keys.Add(MakeShared<FJsonValueArray>(Good));
	Keys.Add(MakeShared<FJsonValueArray>(Bad));

	FClaireonCurveTool_SetKeys Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetArrayField(TEXT("keys"), Keys);
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("keys[1]")));

	UCurveFloat* Reloaded = Cast<UCurveFloat>(CT_FreshLoad(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	UNTEST_EXPECT_EQ(Reloaded->FloatCurve.GetNumKeys(), 1);
	UNTEST_EXPECT_NEAR(Reloaded->FloatCurve.Eval(0.0f), 7.0f, 0.001f);

	CT_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
