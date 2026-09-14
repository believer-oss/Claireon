// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// The functions scope checks declaration metadata and observed side effects, one
// function graph per call. graph_name selects graphs independently of lint scope.

#include "ClaireonLintTypes.h"

#include "ClaireonBlueprintHelpers.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_VariableSet.h"
#include "UObject/Class.h"

namespace ClaireonLint
{

namespace ClaireonLintFunctionsInternal
{
	/** The graph's function entry, or null when it has none. */
	const UK2Node_FunctionEntry* ClaireonLintFns_FindEntry(const UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				return Entry;
			}
		}
		return nullptr;
	}

	/** One observed side effect, named well enough to put in evidence. */
	struct FClaireonLintFnSideEffect
	{
		FString NodeGuid;
		FString Title;
		FString Kind;
	};

	/**
	 * Count member-variable writes and resolved calls lacking BlueprintPure as side
	 * effects. Locals do not count. Unresolved calls and uninspected macros are unknown:
	 * they suppress could-be-pure suggestions, not observed-side-effect findings.
	 */
	void ClaireonLintFns_CollectSideEffects(const UEdGraph* Graph,
		TArray<FClaireonLintFnSideEffect>& OutEffects, int32& OutUnknownCount)
	{
		OutEffects.Reset();
		OutUnknownCount = 0;
		if (!IsValid(Graph))
		{
			return;
		}

		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			if (const UK2Node_VariableSet* Setter = Cast<UK2Node_VariableSet>(Node); IsValid(Setter))
			{
				if (Setter->VariableReference.IsLocalScope())
				{
					// Local to this function: invisible outside one evaluation, not a side effect.
					continue;
				}
				OutEffects.Add(FClaireonLintFnSideEffect{
					Setter->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					Setter->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					TEXT("variable_write")});
				continue;
			}

			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node); IsValid(Call))
			{
				const UFunction* Target = Call->GetTargetFunction();
				if (!IsValid(Target))
				{
					// An unresolved call is unknown, not evidence of a side effect.
					++OutUnknownCount;
					continue;
				}
				if (!Target->HasAnyFunctionFlags(FUNC_BlueprintPure))
				{
					OutEffects.Add(FClaireonLintFnSideEffect{
						Call->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
						Call->GetNodeTitle(ENodeTitleType::ListView).ToString(),
						TEXT("impure_call")});
				}
				continue;
			}

			if (Node->IsA<UK2Node_MacroInstance>())
			{
				++OutUnknownCount;
			}
		}
	}

	/**
	 * Check the parent class for overrides; the generated class also contains local
	 * functions. Inherited declarations are not this Blueprint author's to change.
	 */
	bool ClaireonLintFns_IsOverride(const UBlueprint* Blueprint, const FName FunctionName)
	{
		if (!IsValid(Blueprint) || FunctionName.IsNone())
		{
			return false;
		}
		const UClass* Parent = Blueprint->ParentClass;
		if (!IsValid(Parent))
		{
			return false;
		}
		if (Parent->FindFunctionByName(FunctionName) != nullptr)
		{
			return true;
		}

		// Interface implementations may also appear as ordinary function graphs.
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			if (IsValid(Interface.Interface) && IsValid(Interface.Interface->FindFunctionByName(FunctionName)))
			{
				return true;
			}
		}
		return false;
	}

	/** Evidence array for a side-effect list, capped so one bad function cannot flood a report. */
	TSharedPtr<FJsonValue> ClaireonLintFns_EffectsJson(const TArray<FClaireonLintFnSideEffect>& Effects)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Limit = FMath::Min(Effects.Num(), 8);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("node_guid"), Effects[Index].NodeGuid);
			Obj->SetStringField(TEXT("title"), Effects[Index].Title);
			Obj->SetStringField(TEXT("kind"), Effects[Index].Kind);
			Values.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return MakeShared<FJsonValueArray>(Values);
	}
}
using namespace ClaireonLintFunctionsInternal;

void RunFunctionRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings)
{
	using EKind = ClaireonBlueprintHelpers::EClaireonGraphKind;

	if (!IsValid(Context.Graph))
	{
		return;
	}

	// Require an explicit function kind; interface and delegate graphs also have entry nodes.
	if (!Context.bHasGraphKind || Context.GraphKind != EKind::Function)
	{
		return;
	}

	const UK2Node_FunctionEntry* Entry = ClaireonLintFns_FindEntry(Context.Graph);
	if (!IsValid(Entry))
	{
		return;
	}

	// Construction scripts have fixed signatures and intentionally perform side effects.
	if (Context.Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript)
	{
		return;
	}

	// Interface graphs are declarations without bodies to inspect.
	if (IsValid(Context.Blueprint) && Context.Blueprint->BlueprintType == BPTYPE_Interface)
	{
		return;
	}

	// Inherited declarations are not owned by this Blueprint.
	if (ClaireonLintFns_IsOverride(Context.Blueprint, Context.Graph->GetFName()))
	{
		return;
	}

	const FString FunctionName = Context.Graph->GetName();
	const int32 ExtraFlags = Entry->GetExtraFlags();
	const bool bIsPure = (ExtraFlags & FUNC_BlueprintPure) != 0;

	TArray<FClaireonLintFnSideEffect> Effects;
	int32 UnknownCount = 0;
	ClaireonLintFns_CollectSideEffects(Context.Graph, Effects, UnknownCount);

	// Pure calls are evaluated on demand, so side effects need not run exactly once.
	// Positive evidence remains valid even when other nodes have unknown effects.
	if (bIsPure && Effects.Num() > 0)
	{
		FClaireonLintFinding Finding;
		Finding.Rule = TEXT("pure-function-has-side-effects");
		Finding.Scope = EClaireonLintScope::Functions;
		Finding.Severity = EClaireonLintSeverity::Warning;
		Finding.Confidence = EClaireonLintConfidence::Medium;
		Finding.Target = FunctionName;
		Finding.Evidence = MakeShared<FJsonObject>();
		Finding.Evidence->SetBoolField(TEXT("is_pure"), true);
		Finding.Evidence->SetNumberField(TEXT("side_effect_count"), Effects.Num());
		Finding.Evidence->SetNumberField(TEXT("unresolved_node_count"), UnknownCount);
		Finding.Evidence->SetField(TEXT("side_effects"), ClaireonLintFns_EffectsJson(Effects));
		Finding.Message = FString::Printf(
			TEXT("'%s' is marked pure but performs %d observed side effect(s). A pure function is "
			     "evaluated once per consumer at the point of demand, not once where it appears, so "
			     "these run once per read of the result -- and not at all if the result is unread. "
			     "Either clear pure, or move the side effect out."),
			*FunctionName, Effects.Num());

		// Do not choose whether to remove purity or the side effect for the caller.
		Finding.SuggestedFix = MakeShared<FJsonObject>();
		Finding.SuggestedFix->SetStringField(TEXT("tool"), TEXT("bp_set_function_properties"));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), IsValid(Context.Blueprint)
			? Context.Blueprint->GetPathName() : FString());
		Args->SetStringField(TEXT("function_name"), FunctionName);
		Args->SetStringField(TEXT("is_pure"), TEXT("<false, if the side effects are intended>"));
		Finding.SuggestedFix->SetObjectField(TEXT("args"), Args);

		OutFindings.Add(MoveTemp(Finding));
	}

	const FString Category = Entry->MetaData.Category.ToString();
	if (Category.IsEmpty() || Category.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
	{
		FClaireonLintFinding Finding;
		Finding.Rule = TEXT("function-uncategorized");
		Finding.Scope = EClaireonLintScope::Functions;
		Finding.Severity = EClaireonLintSeverity::Info;
		Finding.Confidence = EClaireonLintConfidence::High;
		Finding.Target = FunctionName;
		Finding.Evidence = MakeShared<FJsonObject>();
		Finding.Evidence->SetStringField(TEXT("category"), Category);
		Finding.Message = FString::Printf(
			TEXT("Function '%s' has no category. Categories are the only grouping a reader gets in "
			     "the My Blueprint tree."),
			*FunctionName);

		Finding.SuggestedFix = MakeShared<FJsonObject>();
		Finding.SuggestedFix->SetStringField(TEXT("tool"), TEXT("bp_set_function_properties"));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), IsValid(Context.Blueprint)
			? Context.Blueprint->GetPathName() : FString());
		Args->SetStringField(TEXT("function_name"), FunctionName);
		Args->SetStringField(TEXT("category"), TEXT("<choose one>"));
		Finding.SuggestedFix->SetObjectField(TEXT("args"), Args);

		OutFindings.Add(MoveTemp(Finding));
	}

	// Suggest purity only with no observed or unknown effects and at least one output.
	if (!bIsPure && Effects.Num() == 0 && UnknownCount == 0)
	{
		// Function outputs are input pins on FunctionResult, not output pins on FunctionEntry.
		int32 OutputCount = 0;
		for (const UEdGraphNode* Node : Context.Graph->Nodes)
		{
			const UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node);
			if (!IsValid(Result))
			{
				continue;
			}
			for (const UEdGraphPin* Pin : Result->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input
					&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					++OutputCount;
				}
			}
		}

		if (OutputCount > 0)
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("function-could-be-pure");
			Finding.Scope = EClaireonLintScope::Functions;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::Medium;
			Finding.Target = FunctionName;
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetBoolField(TEXT("is_pure"), false);
			Finding.Evidence->SetNumberField(TEXT("side_effect_count"), 0);
			Finding.Evidence->SetNumberField(TEXT("unresolved_node_count"), 0);
			Finding.Evidence->SetNumberField(TEXT("output_count"), OutputCount);

			// Native BlueprintPure flags may understate side effects; disclose that blind spot.
			TArray<TSharedPtr<FJsonValue>> Blind;
			Blind.Add(MakeShared<FJsonValueString>(TEXT("native functions whose BlueprintPure flag understates what they touch")));
			Blind.Add(MakeShared<FJsonValueString>(TEXT("latent or delegate-driven work started elsewhere")));
			Finding.Evidence->SetArrayField(TEXT("blind_spots"), Blind);

			Finding.Message = FString::Printf(
				TEXT("'%s' returns %d value(s) and performs no observed side effect, so it could be "
				     "pure -- which removes its exec pins and lets callers read it where they need it. "
				     "Confirm nothing it calls touches state before changing this."),
				*FunctionName, OutputCount);

			Finding.SuggestedFix = MakeShared<FJsonObject>();
			Finding.SuggestedFix->SetStringField(TEXT("tool"), TEXT("bp_set_function_properties"));
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), IsValid(Context.Blueprint)
				? Context.Blueprint->GetPathName() : FString());
			Args->SetStringField(TEXT("function_name"), FunctionName);
			Args->SetBoolField(TEXT("is_pure"), true);
			Finding.SuggestedFix->SetObjectField(TEXT("args"), Args);

			OutFindings.Add(MoveTemp(Finding));
		}
	}
}

}
