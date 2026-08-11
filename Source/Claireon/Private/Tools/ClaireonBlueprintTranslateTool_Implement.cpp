// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateTool_Implement.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/ClaireonBlueprintTranslateHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintTranslateTool_Implement::GetCategory() const { return kBPCategory; }
FString ClaireonBlueprintTranslateTool_Implement::GetOperation() const { return TEXT("translate_implement_node"); }

FString ClaireonBlueprintTranslateTool_Implement::GetDescription() const
{
	return TEXT("Replace a //[BP] tagged node region with user-supplied C++ code in a translation session "
	            "created by blueprint_translate_scaffold. Hash-guarded: fails if the file has been modified "
	            "outside the translator (use the force_implement tool to override). Marks the node "
	            "'implemented' in the session.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintTranslateTool_Implement::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	// Explicit per-field declarations, not a name loop: the loop form could
	// carry no description, so every one of these parameters was undescribed
	// -- invisible in help and in the MCP schema.
	auto AddStringParam = [&Properties](const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		P->SetStringField(TEXT("description"), Description);
		Properties->SetObjectField(Name, P);
	};
	AddStringParam(TEXT("session_id"), TEXT("Session ID returned by the scaffold tool."));
	AddStringParam(TEXT("session_file"), TEXT("Direct path to the session JSON file. Alternative to session_id."));
	AddStringParam(TEXT("blueprint"), TEXT("Blueprint asset path within the session."));
	AddStringParam(TEXT("node_guid"), TEXT("GUID of the node to operate on."));
	AddStringParam(TEXT("code"), TEXT("C++ implementation text to record for this node."));
	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("node_guid")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("code")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonBlueprintTranslateTool_Implement::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	return ClaireonBlueprintTranslateHelpers::DoImplement(Arguments, GetName(), /*bEnforceHashCheck=*/true);
}
