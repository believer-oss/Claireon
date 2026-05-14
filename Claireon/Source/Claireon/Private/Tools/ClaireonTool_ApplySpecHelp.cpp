// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ApplySpecHelp.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FString ResolveCatalogPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetContentDir(), TEXT("ApplySpecCatalog.json"));
	}
}

FString ClaireonTool_ApplySpecHelp::GetName() const
{
	return TEXT("claireon.apply_spec_help");
}

FString ClaireonTool_ApplySpecHelp::GetDescription() const
{
	return TEXT("Returns per-tool spec entry shapes for the eight apply_spec-supporting tools (behaviortree, blackboard, blueprint_edit_graph, eqs, niagara, pcg, statetree, widgetbp). Stateless meta tool, no parameters. Most-common pitfall: assuming spec entries auto-create the asset; only blueprint_edit_graph does, the other seven require the asset to already exist.");
}

TSharedPtr<FJsonObject> ClaireonTool_ApplySpecHelp::GetInputSchema() const
{
	// No parameters; return an object schema with empty properties.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ApplySpecHelp::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	const FString CatalogPath = ResolveCatalogPath();
	if (CatalogPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Claireon plugin not found"));
	}

	if (!FPaths::FileExists(CatalogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("ApplySpecCatalog.json not found at %s"), *CatalogPath));
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *CatalogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to read ApplySpecCatalog.json at %s"), *CatalogPath));
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to parse ApplySpecCatalog.json: %s"), *Reader->GetErrorMessage()));
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), EntriesJson) || !EntriesJson)
	{
		return MakeErrorResult(TEXT("ApplySpecCatalog.json missing 'entries' array"));
	}

	// Build the response: pass through entries[] verbatim, plus _meta and entry_count.
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("entry_count"), EntriesJson->Num());
	Out->SetArrayField(TEXT("entries"), *EntriesJson);

	// Pass-through _meta if present, for source-of-truth attribution.
	const TSharedPtr<FJsonObject>* MetaPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("_meta"), MetaPtr) && MetaPtr && (*MetaPtr).IsValid())
	{
		Out->SetObjectField(TEXT("_meta"), *MetaPtr);
	}

	const FString Summary = FString::Printf(TEXT("apply_spec_help: %d entries"), EntriesJson->Num());
	return MakeSuccessResult(Out, Summary);
}
