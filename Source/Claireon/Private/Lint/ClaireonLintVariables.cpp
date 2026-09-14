// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLintTypes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

namespace ClaireonLint
{

namespace ClaireonLintVariablesInternal
{
	/** How a Blueprint's own graphs touch one member variable. */
	struct FClaireonLintVarUsage
	{
		bool bWrittenByGraph = false;
		bool bReadByGraph = false;

		/**
		 * Construction-script writes are tracked separately. A value normalised at
		 * construction time is a legitimate designer-facing default, not derived
		 * state, so it must not be reported as an unmarked transient.
		 */
		bool bWrittenByConstructionScript = false;
	};

	bool ClaireonLintVars_IsConstructionScript(const UEdGraph* Graph)
	{
		return IsValid(Graph) && Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript;
	}

	TMap<FName, FClaireonLintVarUsage> ClaireonLintVars_CollectUsage(UBlueprint* Blueprint)
	{
		TMap<FName, FClaireonLintVarUsage> Usage;

		// Count only references to this Blueprint's member declarations; bare names can
		// also identify locals or another class's members.
		UClass* SelfClass = IsValid(Blueprint->SkeletonGeneratedClass)
			? Blueprint->SkeletonGeneratedClass.Get()
			: Blueprint->GeneratedClass.Get();
		const auto ResolvesToOwnMember = [Blueprint, SelfClass](const FMemberReference& Ref) -> bool
		{
			if (Ref.IsLocalScope())
			{
				// Ignore locals even when they share a member name.
				return false;
			}
			if (Ref.IsSelfContext())
			{
				// Inherited names cannot shadow NewVariables entries.
				return true;
			}
			const UClass* Owner = Ref.GetMemberParentClass();
			if (!IsValid(Owner))
			{
				return false;
			}
			// Generated and skeleton classes are siblings; compare their owning Blueprint.
			if (UBlueprint::GetBlueprintFromClass(Owner) == Blueprint)
			{
				return true;
			}
			return IsValid(SelfClass) && SelfClass->IsChildOf(Owner);
		};

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);

		for (const UEdGraph* Graph : Graphs)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			const bool bIsConstructionScript = ClaireonLintVars_IsConstructionScript(Graph);

			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_VariableSet* Setter = Cast<UK2Node_VariableSet>(Node); IsValid(Setter))
				{
					if (ResolvesToOwnMember(Setter->VariableReference))
					{
						FClaireonLintVarUsage& Entry = Usage.FindOrAdd(Setter->VariableReference.GetMemberName());
						Entry.bWrittenByGraph = true;
						Entry.bWrittenByConstructionScript |= bIsConstructionScript;
					}
				}
				else if (const UK2Node_VariableGet* Getter = Cast<UK2Node_VariableGet>(Node); IsValid(Getter))
				{
					if (ResolvesToOwnMember(Getter->VariableReference))
					{
						Usage.FindOrAdd(Getter->VariableReference.GetMemberName()).bReadByGraph = true;
					}
				}
				else if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node); IsValid(Delegate))
				{
					// Dispatcher nodes use DelegateReference. Count owned dispatcher access as reads,
					// not assignment to the variable.
					if (ResolvesToOwnMember(Delegate->DelegateReference))
					{
						Usage.FindOrAdd(Delegate->DelegateReference.GetMemberName()).bReadByGraph = true;
					}
				}
			}
		}

		return Usage;
	}

	bool ClaireonLintVars_HasMeta(const FBPVariableDescription& Var, const TCHAR* Key)
	{
		for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
		{
			if (Entry.DataKey == FName(Key))
			{
				return true;
			}
		}
		return false;
	}

	/** Editable on a placed instance: exposed for edit and not explicitly locked out. */
	bool ClaireonLintVars_IsInstanceEditable(const FBPVariableDescription& Var)
	{
		return (Var.PropertyFlags & CPF_Edit) != 0
			&& (Var.PropertyFlags & CPF_DisableEditOnInstance) == 0;
	}
}
using namespace ClaireonLintVariablesInternal;

void RunVariableRules(const FClaireonLintContext& Context, UBlueprint* Blueprint, TArray<FClaireonLintFinding>& OutFindings)
{
	if (!IsValid(Blueprint))
	{
		return;
	}

	const TMap<FName, FClaireonLintVarUsage> Usage = ClaireonLintVars_CollectUsage(Blueprint);

	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		const FClaireonLintVarUsage* Use = Usage.Find(Var.VarName);
		const bool bWritten   = Use && Use->bWrittenByGraph;
		const bool bRead      = Use && Use->bReadByGraph;
		const bool bCtorWrite = Use && Use->bWrittenByConstructionScript;

		const bool bTransient         = (Var.PropertyFlags & CPF_Transient) != 0;
		const bool bSaveGame          = (Var.PropertyFlags & CPF_SaveGame) != 0;
		const bool bConfig            = (Var.PropertyFlags & CPF_Config) != 0;
		const bool bReplicated        = (Var.PropertyFlags & (CPF_Net | CPF_RepNotify)) != 0;
		const bool bExposeOnSpawn     = ClaireonLintVars_HasMeta(Var, TEXT("ExposeOnSpawn"));
		const bool bInstanceEditable  = ClaireonLintVars_IsInstanceEditable(Var);
		const bool bBlueprintReadOnly = (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0;

		// BlueprintReadOnly does not apply to event dispatchers.
		const bool bIsDispatcher = Var.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;

		// Graph writes alone do not prove derived state; account for legitimate persisted defaults.
		if (bWritten && !bTransient)
		{
			TArray<FString> Suppressors;
			if (bSaveGame)         { Suppressors.Add(TEXT("SaveGame")); }
			if (bConfig)           { Suppressors.Add(TEXT("Config")); }
			if (bReplicated)       { Suppressors.Add(TEXT("Replicated")); }
			if (bExposeOnSpawn)    { Suppressors.Add(TEXT("ExposeOnSpawn")); }
			if (bCtorWrite)        { Suppressors.Add(TEXT("ConstructionScriptWrite")); }
			if (bInstanceEditable) { Suppressors.Add(TEXT("InstanceEditable")); }

			if (Suppressors.Num() == 0)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("transient-not-marked");
				Finding.Scope = EClaireonLintScope::Variables;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::Medium;
				Finding.Target = Var.VarName.ToString();
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetBoolField(TEXT("written_by_graph"), true);
				Finding.Evidence->SetBoolField(TEXT("read_by_graph"), bRead);
				Finding.Message = FString::Printf(
					TEXT("'%s' is written by this Blueprint's own graphs but is not Transient. If it is derived state rather than configuration, Transient documents that and keeps it out of the saved asset. Confirm nothing external writes it first."),
					*Var.VarName.ToString());
				OutFindings.Add(MoveTemp(Finding));
			}
		}

		// Runtime writes can overwrite designer-set instance values.
		if (bWritten && bInstanceEditable && !bCtorWrite && !bExposeOnSpawn)
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("transient-instance-editable");
			Finding.Scope = EClaireonLintScope::Variables;
			Finding.Severity = EClaireonLintSeverity::Warning;
			Finding.Confidence = EClaireonLintConfidence::Medium;
			Finding.Target = Var.VarName.ToString();
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetBoolField(TEXT("instance_editable"), true);
			Finding.Evidence->SetBoolField(TEXT("written_by_graph"), true);
			Finding.Message = FString::Printf(
				TEXT("'%s' is editable on a placed instance and also written by this Blueprint's graphs, so a value set in the details panel is likely overwritten at runtime."),
				*Var.VarName.ToString());
			OutFindings.Add(MoveTemp(Finding));
		}

		// External systems may write variables that this Blueprint only reads.
		if (bRead && !bWritten && !bInstanceEditable && !bBlueprintReadOnly && !bIsDispatcher)
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("config-not-readonly");
			Finding.Scope = EClaireonLintScope::Variables;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::Medium;
			Finding.Target = Var.VarName.ToString();
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetBoolField(TEXT("read_by_graph"), true);
			Finding.Evidence->SetBoolField(TEXT("written_by_graph"), false);
			Finding.Message = FString::Printf(
				TEXT("'%s' is only ever read inside this Blueprint. If nothing external writes it either, BlueprintReadOnly says so to the next reader."),
				*Var.VarName.ToString());
			OutFindings.Add(MoveTemp(Finding));
		}

		const FString Category = Var.Category.ToString();
		if (Category.IsEmpty() || Category.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("uncategorized");
			Finding.Scope = EClaireonLintScope::Variables;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::High;
			Finding.Target = Var.VarName.ToString();
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetStringField(TEXT("category"), Category);
			Finding.Message = FString::Printf(
				TEXT("Variable '%s' has no category. Categories are the only grouping a reader gets in the details panel."),
				*Var.VarName.ToString());

			Finding.SuggestedFix = MakeShared<FJsonObject>();
			Finding.SuggestedFix->SetStringField(TEXT("tool"), TEXT("bp_set_variable_properties"));
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			Args->SetStringField(TEXT("variable_name"), Var.VarName.ToString());
			Args->SetStringField(TEXT("category"), TEXT("<choose one>"));
			Finding.SuggestedFix->SetObjectField(TEXT("args"), Args);

			OutFindings.Add(MoveTemp(Finding));
		}

		// This asset-only search cannot prove a variable unused. Other Blueprints, native
		// code, and external systems may reference it; suggest search rather than deletion.
		if (!bRead && !bWritten)
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("unreferenced-variable");
			Finding.Scope = EClaireonLintScope::Hygiene;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::Unverified;
			Finding.Target = Var.VarName.ToString();
			Finding.Evidence = MakeShared<FJsonObject>();

			TArray<TSharedPtr<FJsonValue>> Searched;
			Searched.Add(MakeShared<FJsonValueString>(TEXT("this Blueprint's own graphs")));
			Finding.Evidence->SetArrayField(TEXT("sources_searched"), Searched);

			TArray<TSharedPtr<FJsonValue>> Blind;
			Blind.Add(MakeShared<FJsonValueString>(TEXT("child Blueprints")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("level scripts")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("Sequencer bindings")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("data assets")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("native C++ code")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("reflection by name")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("generated or unloaded content")));
			Finding.Evidence->SetArrayField(TEXT("blind_spots"), Blind);

			Finding.Message = FString::Printf(
				TEXT("'%s' has no reference inside this Blueprint. That is not evidence it is unused -- only this asset was searched. Run a corpus search before drawing any conclusion."),
				*Var.VarName.ToString());

			Finding.SuggestedFix = MakeShared<FJsonObject>();
			Finding.SuggestedFix->SetStringField(TEXT("tool"), TEXT("bp_search"));
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("query"), Var.VarName.ToString());
			Finding.SuggestedFix->SetObjectField(TEXT("args"), Args);

			OutFindings.Add(MoveTemp(Finding));
		}
	}
}

}
