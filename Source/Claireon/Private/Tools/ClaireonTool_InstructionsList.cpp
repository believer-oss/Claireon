// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_InstructionsList.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_InstructionsList::GetCategory() const { return TEXT("instructions"); }
FString ClaireonTool_InstructionsList::GetOperation() const { return TEXT("list"); }

FString ClaireonTool_InstructionsList::GetDescription() const
{
	return TEXT("List the instruction docs this server serves, each with its topic slug, title, "
		"one-line summary, and the URI or prompt name that addresses it over the wire. These "
		"docs carry the judgement-side authoring rules no lint rule can measure. Available to "
		"any caller with tool access, including one that cannot read MCP resources. Read-only "
		"and stateless.");
}

FString ClaireonTool_InstructionsList::GetFullDescription() const
{
	return TEXT(
		"Enumerates every instruction doc currently served out of the Claireon plugin's "
		"Content/MCP/Instructions tree. One row per topic: `topic` is the bare slug you pass "
		"to instructions_read, `title` and `summary` come from the doc's own frontmatter, and "
		"`kind` is either \"resource\" or \"prompt\"."
		"\n\n"
		"WHY THIS TOOL EXISTS. The instruction docs are served over the MCP resources/ and "
		"prompts/ transports, and a caller can easily hold tool access without either one. "
		"Every agent definition under .claude/agents/ is an explicit tool allowlist; several "
		"grant python_execute and none grant the harness resource-reading tools. So a pointer "
		"naming claireon://instructions/blueprint-authoring is, for those callers, a link they "
		"cannot follow. This pair is the tool-reachable path to the same text."
		"\n\n"
		"KIND IS ALWAYS PRESENT, and it decides which of the two address fields is populated: "
		"a resource row carries `uri` and an empty `prompt_name`, a prompt row the reverse. "
		"The URI is the registry key verbatim and is never rebuilt from the topic, so a `uri` "
		"reported here is one resources/read answers for. Branch on `kind` rather than on a "
		"field being empty."
		"\n\n"
		"Rows are sorted by topic, so two calls serialize identically -- the underlying "
		"registries iterate in filesystem-scan order. The set is derived from the live "
		"registries rather than a hardcoded list, so a doc added to the tree appears here "
		"after the next mcp_reload_content without a code change.");
}

FString ClaireonTool_InstructionsList::GetExampleUsage() const
{
	return TEXT("instructions_list()  |  then: instructions_read topic=\"blueprint-authoring\"");
}

FString ClaireonTool_InstructionsList::GetPatterns() const
{
	return TEXT(
		"**Patterns**\n"
		"- Call this first when you do not know the topic slug, then instructions_read for the\n"
		"  doc you want. The slug is the `topic` field, not the title.\n"
		"- Before a non-trivial Blueprint restructuring, read `blueprint-authoring`. bp_lint\n"
		"  measures what is measurable; that doc carries the judgement half.\n"
		"\n"
		"**Common pitfalls**\n"
		"- An empty `uri` is not a defect. It means the doc is served as a prompt rather than a\n"
		"  resource, and `prompt_name` is its address. Read `kind`, not the empty field.\n"
		"- A doc edited on disk is not served until mcp_reload_content runs. Content loads once\n"
		"  at server start.\n"
		"\n"
		"**See also**\n"
		"- `instructions_read` to read one topic's text.\n"
		"- `mcp_reload_content` after editing a doc under Plugins/Claireon/Content/MCP.\n");
}

TArray<FString> ClaireonTool_InstructionsList::GetSearchKeywords() const
{
	return {TEXT("instructions"), TEXT("list"), TEXT("docs"), TEXT("guidance"),
	        TEXT("topics"), TEXT("authoring"), TEXT("judgement")};
}

TSharedPtr<FJsonObject> ClaireonTool_InstructionsList::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
	return Schema;
}

FToolResult ClaireonTool_InstructionsList::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (Arguments.IsValid())
	{
		for (const auto& Pair : Arguments->Values)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Unknown argument: %s. instructions_list takes no parameters."), *Pair.Key));
		}
	}

	FClaireonModule* Module = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	FClaireonServer* Server = Module ? Module->GetServer() : nullptr;
	if (!Server)
	{
		return MakeErrorResult(TEXT(
			"No Claireon MCP server instance; there is no content registry to enumerate."));
	}

	TArray<FClaireonServer::FInstructionTopic> Topics;
	Server->GetInstructionTopics(Topics);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Topics.Num());
	for (const FClaireonServer::FInstructionTopic& Topic : Topics)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("topic"), Topic.Topic);
		Row->SetStringField(TEXT("title"), Topic.Title);
		Row->SetStringField(TEXT("summary"), Topic.Summary);
		Row->SetStringField(TEXT("kind"), Topic.bIsResource ? TEXT("resource") : TEXT("prompt"));
		// Emit both fields; kind identifies which one is populated.
		Row->SetStringField(TEXT("uri"), Topic.Uri);
		Row->SetStringField(TEXT("prompt_name"), Topic.PromptName);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("topics"), Rows);

	FToolResult Result = MakeSuccessResult(Data, FString::Printf(
		TEXT("%d instruction topic(s). Read one with instructions_read(topic=...)."),
		Topics.Num()));

	// Disclose duplicate slugs because reads prefer resources over prompts.
	for (int32 Index = 1; Index < Topics.Num(); ++Index)
	{
		if (Topics[Index].Topic == Topics[Index - 1].Topic)
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("Topic '%s' is served as both a resource and a prompt; instructions_read "
				     "resolves the resource. Address the other one over prompts/get directly."),
				*Topics[Index].Topic));
		}
	}

	if (Topics.Num() == 0)
	{
		Result.Warnings.Add(TEXT(
			"No instruction docs are registered. Check that Plugins/Claireon/Content/MCP/Instructions "
			"is present and that each file's frontmatter parses, then run mcp_reload_content."));
	}

	return Result;
}
