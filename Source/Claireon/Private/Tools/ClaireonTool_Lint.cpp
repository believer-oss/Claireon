// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_Lint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory, FindToolData
#include "Tools/ClaireonTool_SearchInBlueprintsIndexStatus.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonExecTopology.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_Lint::GetCategory() const { return kBPCategory; }
FString ClaireonTool_Lint::GetOperation() const { return TEXT("lint"); }

TArray<FString> ClaireonTool_Lint::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("lint"), TEXT("hygiene"), TEXT("audit"),
	        TEXT("readable"), TEXT("reroute"), TEXT("knot"), TEXT("layout"),
	        TEXT("geometry"), TEXT("wire"), TEXT("check"), TEXT("review"),
	        TEXT("island"), TEXT("column"), TEXT("overlap"), TEXT("rail")};
}

FString ClaireonTool_Lint::GetDescription() const
{
	return TEXT("Inspect a Blueprint for readability defects: converging execution, entry-pin wires, "
		"long or backward wires, dead casts, uncategorised variables, overlapping or off-rail "
		"islands. Read-only -- never mutates. Omitting scope skips the geometry rules; pass "
		"scope=layout or scope=all. exec-join reports once per graph and classifies each join; "
		"extraction RELOCATES a join to its gateway call.");
}


TSharedPtr<FJsonObject> ClaireonTool_Lint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
	Builder.AddString(TEXT("session_id"), TEXT("Session id; lints the session's in-memory Blueprint including unsaved edits."), false);
	Builder.AddString(TEXT("graph_name"), TEXT("Restrict the GRAPH-LOCAL rules to one graph. Omit to lint every graph. A name that matches no graph is an error, not an empty result. Does NOT scope the variables scope: a member variable belongs to the Blueprint, not to a graph, so variable findings are always Blueprint-wide -- see variable_scope in the response."), false);

	Builder.AddStringOrStringArray(TEXT("scope"), TEXT("Scopes to run: variables, graph, layout, functions, hygiene, or all. Omit for the default set, which excludes layout. An explicitly EMPTY list is an error, not the default -- omit the field to get the default. `functions` checks a function DECLARATION (purity against observed side effects, categorisation); it does NOT select function graphs -- graph/hygiene/layout already run over every graph, and graph_name is the graph selector."), false);
	Builder.AddStringOrStringArray(TEXT("rules"), TEXT("Allowlist of rule ids."), false);
	Builder.AddStringOrStringArray(TEXT("exclude_rules"), TEXT("Rule ids to drop. Applied after rules, before severity_min."), false);

	Builder.AddEnum(TEXT("severity_min"), TEXT("Drop findings below this severity. bp_lint is report-only and has just these two levels; anything else is an error rather than a silent default."), {TEXT("info"), TEXT("warning")}, false);
	return Builder.Build();
}

namespace ClaireonTool_Lint_Internal
{
	enum class ECl6807ListField : uint8
	{
		Absent,
		Parsed,
		WrongType,
	};

	/** Read a string array or comma-separated string, distinguishing absent and invalid values. */
	static ECl6807ListField Cl6807Lint_ReadListField(
		const TSharedPtr<FJsonObject>& Arguments,
		const TCHAR* Field,
		TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Arguments.IsValid())
		{
			return ECl6807ListField::Absent;
		}
		// TryGetField avoids key-conversion incompatibilities between UE 5.5 and 5.8.
		const TSharedPtr<FJsonValue> FoundValue = Arguments->TryGetField(Field);
		const TSharedPtr<FJsonValue>* Found = FoundValue.IsValid() ? &FoundValue : nullptr;
		if (!Found || !Found->IsValid())
		{
			return ECl6807ListField::Absent;
		}

		// Check the JSON type directly; TryGetStringField coerces non-string values.
		switch ((*Found)->Type)
		{
		case EJson::Array:
		{
			for (const TSharedPtr<FJsonValue>& Element : (*Found)->AsArray())
			{
				if (!Element.IsValid() || Element->Type != EJson::String)
				{
					return ECl6807ListField::WrongType;
				}
				FString Value = Element->AsString();
				Value.TrimStartAndEndInline();
				if (!Value.IsEmpty())
				{
					OutValues.Add(Value);
				}
			}
			return ECl6807ListField::Parsed;
		}
		case EJson::String:
		{
			TArray<FString> Parts;
			(*Found)->AsString().ParseIntoArray(Parts, TEXT(","), true);
			for (FString& Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					OutValues.Add(Part);
				}
			}
			return ECl6807ListField::Parsed;
		}
		default:
			return ECl6807ListField::WrongType;
		}
	}

	/** Whether the scope has a rule runner. */
	static bool Cl6807Lint_ScopeHasRunner(EClaireonLintScope /*Scope*/)
	{
		// All current scopes have runners. Update this check when adding a scope without one.
		return true;
	}
}

bool ClaireonTool_Lint::ResolveScopes(const TSharedPtr<FJsonObject>& Arguments,
                                     TSet<EClaireonLintScope>& OutScopes,
                                     FString& OutError)
{
	TArray<FString> Names;
	const ClaireonTool_Lint_Internal::ECl6807ListField ScopeField =
		ClaireonTool_Lint_Internal::Cl6807Lint_ReadListField(Arguments, TEXT("scope"), Names);

	if (ScopeField == ClaireonTool_Lint_Internal::ECl6807ListField::WrongType)
	{
		OutError = TEXT("scope must be a list of strings or a comma-separated string, e.g. [\"layout\"] or \"graph,layout\".");
		return false;
	}

	if (ScopeField == ClaireonTool_Lint_Internal::ECl6807ListField::Absent)
	{
		// Only an absent scope selects defaults. An explicitly empty scope is an error.
		OutScopes.Add(EClaireonLintScope::Variables);
		OutScopes.Add(EClaireonLintScope::Graph);
		OutScopes.Add(EClaireonLintScope::Functions);
		OutScopes.Add(EClaireonLintScope::Hygiene);
		return true;
	}

	if (Names.Num() == 0)
	{
		OutError = TEXT("scope was supplied but names no scope. An empty list is not the default -- "
		                "OMIT scope to get the default set (variables, graph, functions, hygiene), or "
		                "name at least one of: variables, graph, layout, functions, hygiene, all.");
		return false;
	}

	for (const FString& Name : Names)
	{
		if (Name.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			OutScopes.Add(EClaireonLintScope::Variables);
			OutScopes.Add(EClaireonLintScope::Graph);
			OutScopes.Add(EClaireonLintScope::Layout);
			OutScopes.Add(EClaireonLintScope::Functions);
			OutScopes.Add(EClaireonLintScope::Hygiene);
			continue;
		}

		EClaireonLintScope Parsed;
		if (!ClaireonLint::ParseScope(Name, Parsed))
		{
			OutError = FString::Printf(
				TEXT("Unknown scope '%s'. Valid: variables, graph, layout, functions, hygiene, all."), *Name);
			return false;
		}
		OutScopes.Add(Parsed);
	}

	if (OutScopes.Num() == 0)
	{
		OutError = TEXT("scope was supplied but resolved to nothing.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> ClaireonTool_Lint::FindingToJson(const FClaireonLintFinding& Finding)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("rule"), Finding.Rule);
	Obj->SetNumberField(TEXT("rule_version"), Finding.RuleVersion);
	Obj->SetStringField(TEXT("severity"), ClaireonLint::ToString(Finding.Severity));
	Obj->SetStringField(TEXT("confidence"), ClaireonLint::ToString(Finding.Confidence));
	Obj->SetStringField(TEXT("scope"), ClaireonLint::ToString(Finding.Scope));

	// graph_name is always present and empty for Blueprint-wide findings.
	Obj->SetStringField(TEXT("graph_name"), Finding.GraphName);
	Obj->SetStringField(TEXT("target"), Finding.Target);
	Obj->SetStringField(TEXT("message"), Finding.Message);
	Obj->SetObjectField(TEXT("evidence"), Finding.Evidence.IsValid() ? Finding.Evidence : MakeShared<FJsonObject>());
	if (Finding.SuggestedFix.IsValid())
	{
		Obj->SetObjectField(TEXT("suggested_fix"), Finding.SuggestedFix);
	}
	return Obj;
}

IClaireonTool::FToolResult ClaireonTool_Lint::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath, SessionId;
	const bool bHasAsset = Arguments.IsValid() && Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();
	const bool bHasSession = Arguments.IsValid() && Arguments->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty();
	if (bHasAsset == bHasSession)
	{
		return MakeErrorResult(bHasAsset
			? TEXT("Supply exactly one of asset_path or session_id, not both.")
			: TEXT("Supply one of asset_path or session_id."));
	}

	UBlueprint* Blueprint = nullptr;
	if (bHasAsset)
	{
		Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!IsValid(Blueprint))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint at path: %s"), *AssetPath));
		}
	}
	else
	{
		FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
		if (!Data)
		{
			return MakeErrorResult(FString::Printf(TEXT("No open session with id '%s'."), *SessionId));
		}
		Blueprint = Data->Blueprint.Get();
		if (!IsValid(Blueprint))
		{
			return MakeErrorResult(FString::Printf(TEXT("Session '%s' no longer references a valid Blueprint."), *SessionId));
		}
	}

	TSet<EClaireonLintScope> Scopes;
	FString ScopeError;
	if (!ResolveScopes(Arguments, Scopes, ScopeError))
	{
		return MakeErrorResult(ScopeError);
	}

	TArray<FString> ScopesWithoutRunner;
	bool bAnyScopeHasRunner = false;
	for (const EClaireonLintScope Scope : Scopes)
	{
		if (ClaireonTool_Lint_Internal::Cl6807Lint_ScopeHasRunner(Scope))
		{
			bAnyScopeHasRunner = true;
		}
		else
		{
			ScopesWithoutRunner.Add(ClaireonLint::ToString(Scope));
		}
	}
	ScopesWithoutRunner.Sort();
	if (!bAnyScopeHasRunner)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Every requested scope has no rules yet (%s), so nothing ran. An empty result "
			     "here would be indistinguishable from a clean Blueprint. Request a scope that "
			     "has rules: variables, graph, layout, hygiene, or all."),
			*FString::Join(ScopesWithoutRunner, TEXT(", "))));
	}

	FString RequestedGraph;
	const bool bOneGraph = Arguments->TryGetStringField(TEXT("graph_name"), RequestedGraph) && !RequestedGraph.IsEmpty();

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);

	// Reject an unmatched graph name before running any rules.
	if (bOneGraph)
	{
		TArray<FString> GraphNames;
		int32 NameMatches = 0;
		for (const UEdGraph* Graph : Graphs)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			GraphNames.Add(Graph->GetName());
			if (Graph->GetName().Equals(RequestedGraph, ESearchCase::IgnoreCase))
			{
				++NameMatches;
			}
		}
		if (NameMatches == 0)
		{
			GraphNames.Sort();
			return MakeErrorResult(FString::Printf(
				TEXT("No graph named '%s' on %s, so nothing was linted. Available graphs: %s."),
				*RequestedGraph, *Blueprint->GetPathName(),
				GraphNames.Num() > 0 ? *FString::Join(GraphNames, TEXT(", ")) : TEXT("(none)")));
		}
	}

	// Use the same collection-based graph kinds as the readers; nested graphs have no kind.
	TMap<const UEdGraph*, ClaireonBlueprintHelpers::EClaireonGraphKind> GraphKinds;
	for (const ClaireonBlueprintHelpers::FGraphEnumerationEntry& KindEntry :
		ClaireonBlueprintHelpers::EnumerateTopLevelGraphs(Blueprint))
	{
		if (IsValid(KindEntry.Graph))
		{
			GraphKinds.Add(KindEntry.Graph, KindEntry.GraphKind);
		}
	}

	int32 GraphsLinted = 0;
	TArray<FClaireonLintFinding> Findings;

	// Variable rules also emit hygiene findings; filter by each finding's scope afterward.
	if (Scopes.Contains(EClaireonLintScope::Variables) || Scopes.Contains(EClaireonLintScope::Hygiene))
	{
		FClaireonLintContext VarContext;
		ClaireonLint::RunVariableRules(VarContext, Blueprint, Findings);
	}

	for (UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			continue;
		}
		if (bOneGraph && !Graph->GetName().Equals(RequestedGraph, ESearchCase::IgnoreCase))
		{
			continue;
		}
		++GraphsLinted;
		const int32 FirstOfThisGraph = Findings.Num();

		// Compute knot-transparent joins once per graph.
		const TArray<FClaireonExecJoin> ExecJoins = ClaireonExecTopology::FindExecJoins(Graph);

		FClaireonLintContext Context;
		Context.Graph = Graph;
		Context.GraphName = Graph->GetName();
		Context.Blueprint = Blueprint;
		Context.ExecJoins = &ExecJoins;
		if (const ClaireonBlueprintHelpers::EClaireonGraphKind* FoundKind = GraphKinds.Find(Graph))
		{
			Context.GraphKind = *FoundKind;
			Context.bHasGraphKind = true;
		}

		if (Scopes.Contains(EClaireonLintScope::Graph))
		{
			ClaireonLint::RunGraphRules(Context, Findings);
		}
		if (Scopes.Contains(EClaireonLintScope::Hygiene))
		{
			ClaireonLint::RunHygieneRules(Context, Findings);
		}
		if (Scopes.Contains(EClaireonLintScope::Layout))
		{
			ClaireonLint::RunLayoutRules(Context, Findings);
		}
		if (Scopes.Contains(EClaireonLintScope::Functions))
		{
			ClaireonLint::RunFunctionRules(Context, Findings);
		}

		for (int32 Index = FirstOfThisGraph; Index < Findings.Num(); ++Index)
		{
			Findings[Index].GraphName = Context.GraphName;
		}
	}

	// Apply the rule allowlist, exclusions, then minimum severity.
	TSet<FString> Allow;
	TSet<FString> Deny;
	{
		TArray<FString> Values;
		if (ClaireonTool_Lint_Internal::Cl6807Lint_ReadListField(Arguments, TEXT("rules"), Values)
			== ClaireonTool_Lint_Internal::ECl6807ListField::WrongType)
		{
			return MakeErrorResult(TEXT("rules must be a list of strings or a comma-separated string."));
		}
		Allow.Append(Values);

		if (ClaireonTool_Lint_Internal::Cl6807Lint_ReadListField(Arguments, TEXT("exclude_rules"), Values)
			== ClaireonTool_Lint_Internal::ECl6807ListField::WrongType)
		{
			return MakeErrorResult(TEXT("exclude_rules must be a list of strings or a comma-separated string."));
		}
		Deny.Append(Values);
	}

	// Read the JSON type directly to reject coercible non-string severities.
	FString SeverityMin;
	if (Arguments.IsValid())
	{
		if (const TSharedPtr<FJsonValue>* Found = Arguments->Values.Find(TEXT("severity_min"));
			Found && Found->IsValid() && (*Found)->Type != EJson::Null)
		{
			if ((*Found)->Type != EJson::String)
			{
				return MakeErrorResult(TEXT("severity_min must be a string: info or warning."));
			}
			SeverityMin = (*Found)->AsString();
			SeverityMin.TrimStartAndEndInline();
			if (!SeverityMin.IsEmpty()
				&& !SeverityMin.Equals(TEXT("info"), ESearchCase::IgnoreCase)
				&& !SeverityMin.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Unknown severity_min '%s'. bp_lint is report-only and has exactly two "
					     "severities: info | warning. Omit the field to keep everything."),
					*SeverityMin));
			}
		}
	}
	const bool bWarningOnly = SeverityMin.Equals(TEXT("warning"), ESearchCase::IgnoreCase);

	Findings.RemoveAll([&](const FClaireonLintFinding& F)
	{
		if (!Scopes.Contains(F.Scope)) { return true; }
		if (Allow.Num() > 0 && !Allow.Contains(F.Rule)) { return true; }
		if (Deny.Contains(F.Rule)) { return true; }
		if (bWarningOnly && F.Severity != EClaireonLintSeverity::Warning) { return true; }
		return false;
	});

	// Stable sorting preserves input order for findings with identical scalar keys but different evidence.
	Findings.StableSort([](const FClaireonLintFinding& A, const FClaireonLintFinding& B)
	{
		// Group by graph, with Blueprint-wide findings first.
		if (A.GraphName != B.GraphName)   { return A.GraphName < B.GraphName; }
		if (A.Rule != B.Rule)             { return A.Rule < B.Rule; }
		if (A.Target != B.Target)         { return A.Target < B.Target; }
		if (A.Scope != B.Scope)           { return A.Scope < B.Scope; }
		if (A.Severity != B.Severity)     { return A.Severity < B.Severity; }
		if (A.Confidence != B.Confidence) { return A.Confidence < B.Confidence; }
		if (A.RuleVersion != B.RuleVersion) { return A.RuleVersion < B.RuleVersion; }
		return A.Message < B.Message;
	});

	TArray<TSharedPtr<FJsonValue>> FindingValues;
	TMap<FString, int32> CountsByRule;
	for (const FClaireonLintFinding& Finding : Findings)
	{
		FindingValues.Add(MakeShared<FJsonValueObject>(FindingToJson(Finding)));
		CountsByRule.FindOrAdd(Finding.Rule)++;
	}

	TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : CountsByRule)
	{
		Counts->SetNumberField(Pair.Key, Pair.Value);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Data->SetArrayField(TEXT("findings"), FindingValues);
	Data->SetObjectField(TEXT("counts_by_rule"), Counts);

	Data->SetNumberField(TEXT("graphs_linted"), GraphsLinted);

	// graph_name filters graph-local rules; member-variable rules still examine the whole Blueprint.
	Data->SetStringField(TEXT("graph_filter"), bOneGraph ? RequestedGraph : FString());
	{
		TSharedPtr<FJsonObject> ScopeKinds = MakeShared<FJsonObject>();
		ScopeKinds->SetStringField(TEXT("variables"), TEXT("blueprint"));
		ScopeKinds->SetStringField(TEXT("graph"), TEXT("graph"));
		ScopeKinds->SetStringField(TEXT("hygiene"), TEXT("graph"));
		ScopeKinds->SetStringField(TEXT("layout"), TEXT("graph"));

		ScopeKinds->SetStringField(TEXT("functions"), TEXT("graph"));
		Data->SetObjectField(TEXT("scope_subjects"), ScopeKinds);
		Data->SetBoolField(TEXT("graph_filter_applies_to_all_scopes"), false);
	}

	{
		TArray<TSharedPtr<FJsonValue>> UnimplementedValues;
		for (const FString& Name : ScopesWithoutRunner)
		{
			UnimplementedValues.Add(MakeShared<FJsonValueString>(Name));
		}
		Data->SetArrayField(TEXT("scopes_not_implemented"), UnimplementedValues);
	}

	// scopes_run is the sorted resolved request, including any scopes without runners.
	TArray<FString> ScopeNames;
	for (const EClaireonLintScope Scope : Scopes)
	{
		ScopeNames.Add(ClaireonLint::ToString(Scope));
	}
	ScopeNames.Sort();

	TArray<TSharedPtr<FJsonValue>> ScopeValues;
	for (const FString& Name : ScopeNames)
	{
		ScopeValues.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("scopes_run"), ScopeValues);

	// Omit index_status if it cannot be read; index failure does not invalidate lint findings.
	{
		ClaireonTool_SearchInBlueprintsIndexStatus StatusTool;
		const IClaireonTool::FToolResult StatusResult = StatusTool.Execute(MakeShared<FJsonObject>());
		if (!StatusResult.bIsError && StatusResult.Data.IsValid())
		{
			Data->SetObjectField(TEXT("index_status"), StatusResult.Data);
		}
	}


	// Tier 0 retains the finding counts already present in the summary text.

	FString Summary = FString::Printf(
		TEXT("%d finding(s) across %d rule(s) over %d graph(s). Report-only; nothing was modified."),
		Findings.Num(), CountsByRule.Num(), GraphsLinted);

	if (ScopesWithoutRunner.Num() > 0)
	{
		Summary += FString::Printf(
			TEXT(" Scope(s) with no rules yet, which contributed nothing: %s."),
			*FString::Join(ScopesWithoutRunner, TEXT(", ")));
	}

	if (bOneGraph)
	{
		const bool bAnyBlueprintWide = Findings.ContainsByPredicate([](const FClaireonLintFinding& F)
		{
			return F.Scope == EClaireonLintScope::Variables;
		});
		if (bAnyBlueprintWide)
		{
			Summary += FString::Printf(
				TEXT(" graph_name='%s' scoped the graph, hygiene and layout rules; variable findings "
				     "are Blueprint-wide -- see scope_subjects."),
				*RequestedGraph);
		}
	}
	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.AddHint(MakeResourceHint(
		TEXT("claireon://instructions/blueprint-authoring"),
		TEXT("Judgement-side rules for these findings; read before restructuring "
		     "(no resource access? call instructions_read(\"blueprint-authoring\")). "
		     "Thresholds are defaults measured on one ability graph, not invariants. "
		     "A rising reroute or wire count immediately after bp_format is the formatter "
		     "surfacing structural debt, not a regression. Do not judge a layout pass by any "
		     "count: bp_format converges in one call, so re-run it after fixing the structural "
		     "cause -- knots that survive that mean the fix missed the cause."),
		FName(TEXT("claireon.lint.judgement-reference"))));
	return Result;
}
