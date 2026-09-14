// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Compare normalized effective variable state across setters and both readers.
// Aliases may emit different tokens; flags and replication must describe the same resulting bits.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonSessionManager.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_AddInterface.h"
#include "Tools/ClaireonBlueprintGraphTool_AddLocalVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_SetVariableProperties.h"
#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonTool_GetVariableProperties.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"

namespace ClaireonVarPropsRoundTripTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static void VPR_Cleanup(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> ToDelete;
			ToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
		}
	}

	/** bp_create a scratch Actor Blueprint and bp_open it. Empty string on failure. */
	static FString VPR_CreateAndOpen(const TCHAR* AssetPath)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
			if (CreateTool.Execute(Args).bIsError)
			{
				return FString();
			}
		}
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = OpenTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static bool VPR_Save(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_Save Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		return !Tool.Execute(Args).bIsError;
	}

	static bool VPR_AddIntVariable(const FString& SessionId, const TCHAR* Name)
	{
		ClaireonBlueprintGraphTool_AddVariable Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("variable_name"), Name);
		Args->SetStringField(TEXT("variable_type"), TEXT("int"));
		return !Tool.Execute(Args).bIsError;
	}

	static TSharedPtr<FJsonValue> VPR_Str(const FString& S)
	{
		return MakeShared<FJsonValueString>(S);
	}

	/** Set properties on one variable. `Mutate` fills in the tool-specific fields. */
	static IClaireonTool::FToolResult VPR_Set(
		const FString& SessionId, const TCHAR* VarName,
		TFunctionRef<void(TSharedPtr<FJsonObject>&)> Mutate)
	{
		ClaireonBlueprintGraphTool_SetVariableProperties Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("variable_name"), VarName);
		Args->SetStringField(TEXT("response_mode"), TEXT("full"));
		Mutate(Args);
		return Tool.Execute(Args);
	}

	/** One variable's description as bp_get_variable_properties reports it. */
	static TSharedPtr<FJsonObject> VPR_GetVar(const FString& SessionId, const TCHAR* VarName)
	{
		ClaireonTool_GetVariableProperties Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("variable_name"), VarName);
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
		if (!R.Data->TryGetArrayField(TEXT("variables"), Vars) || !Vars || Vars->Num() != 1)
		{
			return nullptr;
		}
		return (*Vars)[0]->AsObject();
	}

	/** The same variable as bp_get_properties reports it. */
	static TSharedPtr<FJsonObject> VPR_GetViaProperties(const TCHAR* AssetPath, const TCHAR* VarName)
	{
		ClaireonTool_GetBlueprintProperties Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
		if (!R.Data->TryGetArrayField(TEXT("variables"), Vars) || !Vars)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& V : *Vars)
		{
			TSharedPtr<FJsonObject> Obj = V->AsObject();
			FString Name;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("variable_name"), Name)
				&& Name.Equals(VarName, ESearchCase::IgnoreCase))
			{
				return Obj;
			}
		}
		return nullptr;
	}

	/** Flags joined in sorted order, so comparison is on the SET not on emission order. */
	static FString VPR_JoinFlags(const TSharedPtr<FJsonObject>& VarObj)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (VarObj.IsValid() && VarObj->TryGetArrayField(TEXT("flags"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				Out.Add(V->AsString());
			}
		}
		Out.Sort();
		return FString::Join(Out, TEXT(","));
	}

	static FString VPR_Field(const TSharedPtr<FJsonObject>& VarObj, const TCHAR* Key)
	{
		FString Value;
		if (VarObj.IsValid())
		{
			VarObj->TryGetStringField(Key, Value);
		}
		return Value;
	}

	static TArray<TSharedPtr<FJsonValue>> VPR_Flags(const TArray<FString>& Tokens)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& T : Tokens)
		{
			Out.Add(VPR_Str(T));
		}
		return Out;
	}
}

// Read back every supported field.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_EverySettableFieldRoundTrips,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_AllFields");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("Health")));

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("tooltip"), TEXT("Current hit points"));
	Meta->SetStringField(TEXT("ClampMin"), TEXT("0"));

	IClaireonTool::FToolResult Set = VPR_Set(SessionId, TEXT("Health"),
		[&Meta](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetStringField(TEXT("category"), TEXT("Combat|Vitals"));
			Args->SetStringField(TEXT("tooltip"), TEXT("Current hit points"));
			Args->SetStringField(TEXT("display_name"), TEXT("Health Points"));
			Args->SetStringField(TEXT("replication"), TEXT("RepNotify"));
			Args->SetStringField(TEXT("rep_notify_func"), TEXT("OnRep_Health"));
			Args->SetStringField(TEXT("replication_condition"), TEXT("COND_OwnerOnly"));
			Args->SetObjectField(TEXT("metadata"), Meta);
			Args->SetArrayField(TEXT("flags"),
				VPR_Flags({TEXT("BlueprintReadOnly"), TEXT("SaveGame")}));
		});
	UNTEST_ASSERT_FALSE(Set.bIsError);

	TSharedPtr<FJsonObject> Var = VPR_GetVar(SessionId, TEXT("Health"));
	UNTEST_ASSERT_TRUE(Var.IsValid());

	FString Field;
	Field = VPR_Field(Var, TEXT("variable_name"));         UNTEST_EXPECT_STREQ(*Field, TEXT("Health"));
	Field = VPR_Field(Var, TEXT("category"));              UNTEST_EXPECT_STREQ(*Field, TEXT("Combat|Vitals"));
	Field = VPR_Field(Var, TEXT("tooltip"));               UNTEST_EXPECT_STREQ(*Field, TEXT("Current hit points"));
	// display_name is stored in MD_DisplayName metadata rather than FriendlyName.
	Field = VPR_Field(Var, TEXT("display_name"));          UNTEST_EXPECT_STREQ(*Field, TEXT("Health Points"));
	Field = VPR_Field(Var, TEXT("rep_notify_func"));       UNTEST_EXPECT_STREQ(*Field, TEXT("OnRep_Health"));
	Field = VPR_Field(Var, TEXT("replication_condition")); UNTEST_EXPECT_STREQ(*Field, TEXT("COND_OwnerOnly"));

	Field = VPR_Field(Var, TEXT("replication"));           UNTEST_EXPECT_STREQ(*Field, TEXT("RepNotify"));

	// Include flags seeded by bp_add_variable; flags[] adds to existing state.
	FString Flags = VPR_JoinFlags(Var);
	UNTEST_EXPECT_STREQ(*Flags, TEXT("BlueprintReadOnly,EditAnywhere,RepNotify,SaveGame"));

	// Tooltip must appear both as metadata and as a first-class field.
	const TSharedPtr<FJsonObject>* MetaOut = nullptr;
	UNTEST_ASSERT_TRUE(Var->TryGetObjectField(TEXT("metadata"), MetaOut) && MetaOut != nullptr);
	FString ClampMin;
	UNTEST_EXPECT_TRUE((*MetaOut)->TryGetStringField(TEXT("ClampMin"), ClampMin));
	UNTEST_EXPECT_STREQ(*ClampMin, TEXT("0"));
	FString MetaTooltip;
	UNTEST_EXPECT_TRUE((*MetaOut)->TryGetStringField(TEXT("tooltip"), MetaTooltip));
	UNTEST_EXPECT_STREQ(*MetaTooltip, TEXT("Current hit points"));

	VPR_Cleanup(AssetPath);
	co_return;
}

// Compare normalized output for many-to-one flag aliases.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_AliasTokensNormalizeOnRead,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Aliases");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("InstanceOnly")));
	FString Baseline = VPR_JoinFlags(VPR_GetVar(SessionId, TEXT("InstanceOnly")));
	UNTEST_EXPECT_STREQ(*Baseline, TEXT("BlueprintReadWrite,EditAnywhere"));

	// Set EditDefaultsOnly first so EditInstanceOnly has a bit to clear.
	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("InstanceOnly"),
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetArrayField(TEXT("flags"), VPR_Flags({TEXT("EditDefaultsOnly")}));
		}).bIsError);
	FString Defaults = VPR_JoinFlags(VPR_GetVar(SessionId, TEXT("InstanceOnly")));
	UNTEST_EXPECT_STREQ(*Defaults, TEXT("BlueprintReadWrite,EditDefaultsOnly"));

	// EditInstanceOnly clears DisableEditOnInstance and reads back as EditAnywhere.
	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("InstanceOnly"),
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetArrayField(TEXT("flags"), VPR_Flags({TEXT("EditInstanceOnly")}));
		}).bIsError);
	FString InstanceFlags = VPR_JoinFlags(VPR_GetVar(SessionId, TEXT("InstanceOnly")));
	UNTEST_EXPECT_STREQ(*InstanceFlags, TEXT("BlueprintReadWrite,EditAnywhere"));

	// Replicated and Net both represent CPF_Net in different field vocabularies.
	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("NetVar")));
	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("NetVar"),
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetArrayField(TEXT("flags"), VPR_Flags({TEXT("Replicated")}));
		}).bIsError);
	TSharedPtr<FJsonObject> Rep = VPR_GetVar(SessionId, TEXT("NetVar"));
	FString RepFlags = VPR_JoinFlags(Rep);
	UNTEST_EXPECT_STREQ(*RepFlags, TEXT("BlueprintReadWrite,EditAnywhere,Net"));
	FString RepField = VPR_Field(Rep, TEXT("replication"));
	UNTEST_EXPECT_STREQ(*RepField, TEXT("Replicated"));

	VPR_Cleanup(AssetPath);
	co_return;
}

// Exercise compound flag combinations and independent axes.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_CompoundEditabilityCases,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Compound");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	struct FCase
	{
		const TCHAR* VarName;
		TArray<FString> InFlags;
		const TCHAR* ExpectedSorted;
	};

	const TArray<FCase> Cases = {
		// Include seeded flags when comparing compound output.
		{TEXT("DefaultsOnly"), {TEXT("EditDefaultsOnly")},
		 TEXT("BlueprintReadWrite,EditDefaultsOnly")},
		{TEXT("VisibleOnly"), {TEXT("VisibleAnywhere")},
		 TEXT("BlueprintReadWrite,VisibleAnywhere")},
		// BlueprintReadOnly also clears DisableEditOnInstance through the engine setter.
		{TEXT("ReadOnlyEditable"), {TEXT("BlueprintReadOnly"), TEXT("EditAnywhere")},
		 TEXT("BlueprintReadOnly,EditAnywhere")},
		{TEXT("SpawnReplicated"), {TEXT("ExposeOnSpawn"), TEXT("Replicated")},
		 TEXT("BlueprintReadWrite,EditAnywhere,ExposeOnSpawn,Net")},
	};

	for (const FCase& Case : Cases)
	{
		UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, Case.VarName));
		UNTEST_ASSERT_FALSE(VPR_Set(SessionId, Case.VarName,
			[&Case](TSharedPtr<FJsonObject>& Args)
			{
				Args->SetArrayField(TEXT("flags"), VPR_Flags(Case.InFlags));
			}).bIsError);

		FString Actual = VPR_JoinFlags(VPR_GetVar(SessionId, Case.VarName));
		UNTEST_EXPECT_STREQ(*Actual, Case.ExpectedSorted);
	}

	VPR_Cleanup(AssetPath);
	co_return;
}

// Read effective residue after flags and clear_flags operations.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_ClearFlagsConvergeWithFlags,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_ClearFlags");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("Toggled")));

	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("Toggled"),
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetArrayField(TEXT("flags"), VPR_Flags(
				{TEXT("BlueprintReadOnly"), TEXT("Transient"), TEXT("SaveGame")}));
		}).bIsError);
	FString AfterSet = VPR_JoinFlags(VPR_GetVar(SessionId, TEXT("Toggled")));
	UNTEST_EXPECT_STREQ(*AfterSet, TEXT("BlueprintReadOnly,EditAnywhere,SaveGame,Transient"));

	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("Toggled"),
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetArrayField(TEXT("clear_flags"),
				VPR_Flags({TEXT("Transient"), TEXT("SaveGame")}));
		}).bIsError);

	// Use Transient and SaveGame to exercise generic clear_flags handling.
	FString AfterClear = VPR_JoinFlags(VPR_GetVar(SessionId, TEXT("Toggled")));
	UNTEST_EXPECT_STREQ(*AfterClear, TEXT("BlueprintReadOnly,EditAnywhere"));

	VPR_Cleanup(AssetPath);
	co_return;
}

// Save and compare overlapping fields from both property readers.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_GetPropertiesAgreesOnTheOverlap,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Overlap");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("Ammo")));

	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("tooltip"), TEXT("Rounds remaining"));

	UNTEST_ASSERT_FALSE(VPR_Set(SessionId, TEXT("Ammo"),
		[&Meta](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetStringField(TEXT("category"), TEXT("Weapon"));
			Args->SetStringField(TEXT("display_name"), TEXT("Ammo Count"));
			Args->SetStringField(TEXT("replication"), TEXT("Replicated"));
			Args->SetObjectField(TEXT("metadata"), Meta);
			Args->SetArrayField(TEXT("flags"),
				VPR_Flags({TEXT("BlueprintReadWrite"), TEXT("EditAnywhere")}));
		}).bIsError);
	UNTEST_ASSERT_TRUE(VPR_Save(SessionId));

	TSharedPtr<FJsonObject> FromVarTool = VPR_GetVar(SessionId, TEXT("Ammo"));
	UNTEST_ASSERT_TRUE(FromVarTool.IsValid());
	TSharedPtr<FJsonObject> FromProperties = VPR_GetViaProperties(AssetPath, TEXT("Ammo"));
	UNTEST_ASSERT_TRUE(FromProperties.IsValid());

	for (const TCHAR* Key : {TEXT("variable_name"), TEXT("type"), TEXT("category"),
	                         TEXT("display_name"), TEXT("tooltip"), TEXT("replication"),
	                         TEXT("rep_notify_func")})
	{
		FString A = VPR_Field(FromVarTool, Key);
		FString B = VPR_Field(FromProperties, Key);
		UNTEST_EXPECT_STREQ(*A, *B);
	}
	FString FlagsA = VPR_JoinFlags(FromVarTool);
	FString FlagsB = VPR_JoinFlags(FromProperties);
	UNTEST_EXPECT_STREQ(*FlagsA, *FlagsB);

	VPR_Cleanup(AssetPath);
	co_return;
}

// Implemented interfaces.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_InterfacesArePopulated,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Interfaces");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	{
		ClaireonBlueprintGraphTool_AddInterface Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("interface_class"), TEXT("Interface_CollisionDataProvider"));
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	UNTEST_ASSERT_TRUE(VPR_Save(SessionId));

	ClaireonTool_GetBlueprintProperties Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Interfaces = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("interfaces"), Interfaces)
		&& Interfaces != nullptr);
	UNTEST_EXPECT_GE(Interfaces->Num(), 1);

	VPR_Cleanup(AssetPath);
	co_return;
}

// Read locals from their owning function entries rather than NewVariables.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_FunctionLocalsAreReported,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Locals");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	{
		ClaireonBlueprintGraphTool_AddFunction Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("function_name"), TEXT("ComputeThing"));
		UNTEST_ASSERT_FALSE(Tool.Execute(Args).bIsError);
	}
	{
		ClaireonBlueprintGraphTool_AddLocalVariable Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("function_name"), TEXT("ComputeThing"));
		Args->SetStringField(TEXT("variable_name"), TEXT("Scratch"));
		Args->SetStringField(TEXT("variable_type"), TEXT("int"));
		UNTEST_ASSERT_FALSE(Tool.Execute(Args).bIsError);
	}

	ClaireonTool_GetVariableProperties Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("functions"), Functions)
		&& Functions != nullptr);

	bool bFoundLocal = false;
	for (const TSharedPtr<FJsonValue>& FuncVal : *Functions)
	{
		TSharedPtr<FJsonObject> FuncObj = FuncVal->AsObject();
		if (!FuncObj.IsValid())
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Locals = nullptr;
		if (!FuncObj->TryGetArrayField(TEXT("local_variables"), Locals) || !Locals)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& LocalVal : *Locals)
		{
			TSharedPtr<FJsonObject> LocalObj = LocalVal->AsObject();
			if (LocalObj.IsValid()
				&& VPR_Field(LocalObj, TEXT("variable_name")) == TEXT("Scratch"))
			{
				bFoundLocal = true;
			}
		}
	}
	UNTEST_EXPECT_TRUE(bFoundLocal);

	VPR_Cleanup(AssetPath);
	co_return;
}

// Distinguish invalid source arguments before Blueprint resolution.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_ArgumentErrorPathsAreDistinct,
	UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_GetVariableProperties Tool;

	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Supply one of")));
	}

	// Reject two sources rather than choosing one implicitly.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_VPR_Nope"));
		Args->SetStringField(TEXT("session_id"), TEXT("some-session"));
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("not both")));
	}

	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_VPR_DoesNotExist"));
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Failed to load Blueprint")));
	}

	co_return;
}

// Unknown variable names must fail instead of producing an empty audit result.
UNTEST_UNIT_OPTS(Claireon, VariableProperties, VariableProperties_UnknownVariableIsAnError,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonVarPropsRoundTripTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_VPR_Unknown");
	const FString SessionId = VPR_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(VPR_AddIntVariable(SessionId, TEXT("RealVariable")));

	ClaireonTool_GetVariableProperties Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("variable_name"), TEXT("NoSuchVariable"));
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("function locals")));

	VPR_Cleanup(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
