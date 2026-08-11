// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintTranslateTool_ForceImplement.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/ClaireonBlueprintTranslateHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintTranslateTool_ForceImplement::GetCategory() const { return kBPCategory; }
FString ClaireonBlueprintTranslateTool_ForceImplement::GetOperation() const { return TEXT("translate_force_implement_node"); }

FString ClaireonBlueprintTranslateTool_ForceImplement::GetDescription() const
{
	return TEXT("Replace a //[BP] tagged node region with user-supplied C++ code WITHOUT the file-hash guard. "
	            "Use when the user has knowingly edited the file outside the translator and wants to overwrite "
	            "the region anyway. Marks the node 'implemented' in the session.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintTranslateTool_ForceImplement::GetInputSchema() const
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

FToolResult ClaireonBlueprintTranslateTool_ForceImplement::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	return ClaireonBlueprintTranslateHelpers::DoImplement(Arguments, GetName(), /*bEnforceHashCheck=*/false);
}
