// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Work item #0000 Item 3: ClaireonTool_GetBlueprintProperties emits plain `name`
// alongside entity-prefixed aliases (variable_name, function_name,
// component_name). Per-ambiguity-resolution this spec file is the chosen home
// for get_properties alias coverage.

#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_AddComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_Compile.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"

namespace
{
	FString GetPropSpec_ExtractSessionId(const FString& Text)
	{
		int32 Start = Text.Find(TEXT("Session ID: "));
		if (Start == INDEX_NONE) { return FString(); }
		Start += 12;
		int32 End = Text.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) { End = Text.Len(); }
		return Text.Mid(Start, End - Start).TrimStartAndEnd();
	}
}

// =====================================================================================
// ITEM_03 Test 3: properties alias coverage. Variables/functions/components entries
// each carry both plain `name` and prefixed alias, equal values.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintPropertiesTest_FieldAliases_EntityPrefixed,
	"Claireon.GetBlueprintProperties.FieldAliases.EntityPrefixed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintPropertiesTest_FieldAliases_EntityPrefixed::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_GetPropsAliases");

	FString SessionId;
	{
		ClaireonBlueprintGraphTool_Create Create;
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("asset_path"), AssetPath);
		A->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		auto R = Create.Execute(A);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("Create BP failed: %s"), *R.GetContentAsString()));
			return false;
		}
		SessionId = GetPropSpec_ExtractSessionId(R.GetContentAsString());
		if (SessionId.IsEmpty()) { AddError(TEXT("No session id")); return false; }
	}

	// Add a variable so the variables array has an entry.
	{
		ClaireonBlueprintGraphTool_AddVariable AddVar;
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("session_id"), SessionId);
		A->SetStringField(TEXT("variable_name"), TEXT("TestIntVar"));
		A->SetStringField(TEXT("variable_type"), TEXT("int"));
		auto R = AddVar.Execute(A);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("add_variable failed: %s"), *R.GetContentAsString()));
		}
	}

	// Add a component so the components array has at least one entry beyond the default.
	{
		ClaireonBlueprintGraphTool_AddComponent AddComp;
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("session_id"), SessionId);
		A->SetStringField(TEXT("component_name"), TEXT("TestStaticMesh"));
		A->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.StaticMeshComponent"));
		auto R = AddComp.Execute(A);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("add_component failed: %s"), *R.GetContentAsString()));
		}
	}

	// Compile + close so LoadObject can resolve the asset.
	{
		ClaireonBlueprintGraphTool_Compile Compile;
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("session_id"), SessionId);
		Compile.Execute(A);

		ClaireonBlueprintGraphTool_Close Close;
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("session_id"), SessionId);
		Close.Execute(C);
	}

	ClaireonTool_GetBlueprintProperties Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	auto Result = Tool.Execute(Args);
	if (Result.bIsError || !Result.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("get_properties failed: %s"), *Result.GetContentAsString()));
		return false;
	}

	auto CheckArrayAliases = [&](const TCHAR* ArrayField, const TCHAR* AliasKey) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Result.Data->TryGetArrayField(ArrayField, Arr) || !Arr)
		{
			// If array missing, no entries to check -- not a failure.
			return true;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!V->TryGetObject(Obj) || !Obj) { continue; }
			FString Name, Alias;
			if (!(*Obj)->TryGetStringField(TEXT("name"), Name))
			{
				AddError(FString::Printf(TEXT("'%s' entry missing 'name'"), ArrayField));
				return false;
			}
			if (!(*Obj)->TryGetStringField(AliasKey, Alias))
			{
				AddError(FString::Printf(TEXT("'%s' entry missing '%s' alias"), ArrayField, AliasKey));
				return false;
			}
			if (Name != Alias)
			{
				AddError(FString::Printf(TEXT("'%s' entry name '%s' != alias '%s' ('%s')"), ArrayField, *Name, *Alias, AliasKey));
				return false;
			}
		}
		return true;
	};

	if (!CheckArrayAliases(TEXT("variables"), TEXT("variable_name"))) { return false; }
	if (!CheckArrayAliases(TEXT("functions"), TEXT("function_name"))) { return false; }
	if (!CheckArrayAliases(TEXT("components"), TEXT("component_name"))) { return false; }

	return true;
}
