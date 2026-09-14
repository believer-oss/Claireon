// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_InstructionsRead.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_InstructionsRead::GetCategory() const { return TEXT("instructions"); }
FString ClaireonTool_InstructionsRead::GetOperation() const { return TEXT("read"); }

FString ClaireonTool_InstructionsRead::GetDescription() const
{
	return TEXT("Read one instruction doc's full text by topic slug -- for example "
		"\"blueprint-authoring\" for the judgement-side Blueprint graph rules. Returns the same "
		"bytes the resources/read and prompts/get transports serve for that doc, so a caller "
		"holding only tool access loses nothing. Call instructions_list for the available "
		"topics. Read-only and stateless.");
}

FString ClaireonTool_InstructionsRead::GetFullDescription() const
{
	return TEXT(
		"Returns one instruction doc as served: `topic`, `kind`, the address fields, and `text`."
		"\n\n"
		"BYTE-IDENTICAL TO THE WIRE, by construction rather than by intent. The text comes back "
		"through the same accessors resources/read and prompts/get use, including the same "
		"{{placeholder}} substitution, so the two transports cannot disagree about what the "
		"document says. It is never read off disk -- mcp_reload_content replaces the served "
		"registries wholesale, and a file that fails to parse deliberately keeps its previously "
		"loaded copy, so a disk read would differ from the wire exactly when a doc is mid-edit "
		"and somebody is checking."
		"\n\n"
		"WHY THIS TOOL EXISTS. The docs are otherwise reachable only over transports a "
		"tool-allowlisted caller may not hold: every agent definition under .claude/agents/ is "
		"an explicit allowlist, several grant python_execute, and none grant the harness "
		"resource-reading tools. A pointer naming a claireon:// URI is a link those callers "
		"cannot follow; this is the one they can."
		"\n\n"
		"READ blueprint-authoring BEFORE RESTRUCTURING A GRAPH. bp_lint measures what is "
		"measurable -- counts, spans, thresholds. That doc carries the half that decides what a "
		"count means: its thresholds are defaults measured on one ability graph rather than "
		"invariants, a long reroute is a restructure signal rather than a routing problem, and a "
		"reroute or wire count that RISES right after bp_format is the formatter surfacing "
		"pre-existing structural debt rather than a regression to revert."
		"\n\n"
		"An unknown topic is a clean error naming the topics that do exist, never an empty "
		"success: a typo must not read as an empty document.");
}

FString ClaireonTool_InstructionsRead::GetExampleUsage() const
{
	return TEXT("instructions_read topic=\"blueprint-authoring\"  |  instructions_read topic=\"sequencing\"");
}

FString ClaireonTool_InstructionsRead::GetPatterns() const
{
	return TEXT(
		"**Patterns**\n"
		"- Read `blueprint-authoring` before any non-trivial Blueprint restructuring, and before\n"
		"  acting on a bp_lint result. bp_lint's judgement-reference hint points here.\n"
		"- Do not know the slug? Call instructions_list first; the slug is its `topic` field.\n"
		"\n"
		"**Common pitfalls**\n"
		"- Pass the topic slug, not the title and not the URI. `blueprint-authoring`, not\n"
		"  `Blueprint Authoring` and not the full claireon:// address.\n"
		"- A doc edited on disk is not served until mcp_reload_content runs. Content loads once\n"
		"  at server start, so an edit made this session is invisible until then.\n"
		"\n"
		"**See also**\n"
		"- `instructions_list` for the available topics.\n"
		"- `bp_lint`, whose judgement-reference hint names this topic (and this tool as the\n"
		"  no-resource-access route).\n");
}

TArray<FString> ClaireonTool_InstructionsRead::GetSearchKeywords() const
{
	return {TEXT("instructions"), TEXT("read"), TEXT("docs"), TEXT("guidance"),
	        TEXT("authoring"), TEXT("judgement"), TEXT("rules")};
}

TSharedPtr<FJsonObject> ClaireonTool_InstructionsRead::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("topic"),
		TEXT("Topic slug, as reported by instructions_list's `topic` field (e.g. "
		     "\"blueprint-authoring\"). Not the title, and not the full claireon:// URI."),
		true);
	return Builder.Build();
}

FToolResult ClaireonTool_InstructionsRead::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Topic;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("topic"), Topic) || Topic.IsEmpty())
	{
		return MakeErrorResult(TEXT(
			"Missing required parameter: topic. Call instructions_list for the available topics."));
	}
	Topic.TrimStartAndEndInline();

	for (const auto& Pair : Arguments->Values)
	{
		const FString Key(*Pair.Key);
		if (Key != TEXT("topic"))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Unknown argument: %s. instructions_read takes only `topic`."), *Pair.Key));
		}
	}

	FClaireonModule* Module = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	FClaireonServer* Server = Module ? Module->GetServer() : nullptr;
	if (!Server)
	{
		return MakeErrorResult(TEXT(
			"No Claireon MCP server instance; there is no content registry to read."));
	}

	TArray<FClaireonServer::FInstructionTopic> Topics;
	Server->GetInstructionTopics(Topics);

	// The sorted listing puts resources before prompts for duplicate topics.
	const FClaireonServer::FInstructionTopic* Match = Topics.FindByPredicate(
		[&Topic](const FClaireonServer::FInstructionTopic& Candidate)
		{
			return Candidate.Topic.Equals(Topic, ESearchCase::IgnoreCase);
		});

	if (!Match)
	{
		TArray<FString> Available;
		Available.Reserve(Topics.Num());
		for (const FClaireonServer::FInstructionTopic& Candidate : Topics)
		{
			Available.Add(Candidate.Topic);
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown instruction topic: '%s'. Available: %s."),
			*Topic, *FString::Join(Available, TEXT(", "))));
	}

	FString Text;
	const bool bServed = Match->bIsResource
		? Server->TryGetResourceText(Match->Uri, Text)
		: Server->TryGetPromptText(Match->PromptName, Text);

	if (!bServed)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Instruction topic '%s' is listed but its text could not be served from the "
			     "%s registry. Run mcp_reload_content and retry; if it persists, the doc's "
			     "frontmatter identity and its registry key disagree."),
			*Topic, Match->bIsResource ? TEXT("resource") : TEXT("prompt")));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("topic"), Match->Topic);
	Data->SetStringField(TEXT("kind"), Match->bIsResource ? TEXT("resource") : TEXT("prompt"));
	Data->SetStringField(TEXT("uri"), Match->Uri);
	Data->SetStringField(TEXT("prompt_name"), Match->PromptName);
	Data->SetStringField(TEXT("text"), Text);

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("Instruction '%s' (%d chars), byte-identical to what %s serves."),
		*Match->Topic, Text.Len(),
		Match->bIsResource ? TEXT("resources/read") : TEXT("prompts/get")));
}
