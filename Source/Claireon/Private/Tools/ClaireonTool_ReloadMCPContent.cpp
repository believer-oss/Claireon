// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ReloadMCPContent.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

FString ClaireonTool_ReloadMCPContent::GetCategory() const { return TEXT("mcp"); }
FString ClaireonTool_ReloadMCPContent::GetOperation() const { return TEXT("reload_content"); }

FString ClaireonTool_ReloadMCPContent::GetDescription() const
{
	return TEXT("Refresh the served MCP prompt and resource registries by rescanning the Claireon plugin's Content/MCP tree from disk, so an edited instruction doc is served without an editor restart. A file that fails to parse keeps its previously loaded copy. Stateless / non-session.");
}

FString ClaireonTool_ReloadMCPContent::GetPatterns() const
{
	return TEXT(
		"**Patterns**\n"
		"- Call after editing any file under Plugins/Claireon/Content/MCP, then re-read the\n"
		"  resource to confirm the new text.\n"
		"\n"
		"**Common pitfalls**\n"
		"- No client notification is sent. This transport has no server-to-client channel, so a\n"
		"  client holding a cached resources/list must re-read it to see additions or removals.\n"
		"- `failed_files` is not an error. Those files yielded no entry; any registry key they\n"
		"  previously defined is listed in `carried_forward` and still serves its last good text.\n"
		"\n"
		"**See also**\n"
		"- `mcp_resources_list` / `resources/read` for what is currently served.\n");
}

TSharedPtr<FJsonObject> ClaireonTool_ReloadMCPContent::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ReloadMCPContent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (Arguments.IsValid())
	{
		for (const auto& Pair : Arguments->Values)
		{
			return MakeErrorResult(FString::Printf(TEXT("Unknown argument: %s"), *Pair.Key));
		}
	}

	FClaireonModule* Module = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	FClaireonServer* Server = Module ? Module->GetServer() : nullptr;
	if (!Server)
	{
		return MakeErrorResult(TEXT(
			"No Claireon MCP server instance; there is no content registry to reload."));
	}

	const FClaireonServer::FMCPContentLoadReport Report = Server->ReloadMCPContent();

	if (!Report.FatalError.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Content rescan did not run (%s); the previously loaded content is still served."),
			*Report.FatalError));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("prompts"), Report.NumPrompts);
	Data->SetNumberField(TEXT("resources"), Report.NumResources);

	TArray<TSharedPtr<FJsonValue>> FailedArray;
	for (const FString& Failed : Report.FailedFiles)
	{
		FailedArray.Add(MakeShared<FJsonValueString>(Failed));
	}
	Data->SetArrayField(TEXT("failed_files"), FailedArray);

	TArray<TSharedPtr<FJsonValue>> CarriedArray;
	for (const FString& Key : Report.CarriedForwardKeys)
	{
		CarriedArray.Add(MakeShared<FJsonValueString>(Key));
	}
	Data->SetArrayField(TEXT("carried_forward"), CarriedArray);

	FToolResult Result = MakeSuccessResult(Data, FString::Printf(
		TEXT("Reloaded MCP content: %d prompt(s), %d resource(s)"),
		Report.NumPrompts, Report.NumResources));

	for (const FString& Key : Report.CarriedForwardKeys)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("'%s' kept its previously loaded content: the file defining it failed to load this pass."),
			*Key));
	}
	for (const FString& Failed : Report.FailedFiles)
	{
		Result.Warnings.Add(FString::Printf(TEXT("Yielded no entry: %s"), *Failed));
	}

	return Result;
}
