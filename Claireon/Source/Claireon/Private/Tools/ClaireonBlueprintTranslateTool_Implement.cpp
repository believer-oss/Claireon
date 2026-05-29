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
	for (const TCHAR* Field : { TEXT("session_id"), TEXT("session_file"), TEXT("blueprint"), TEXT("node_guid"), TEXT("code") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		Properties->SetObjectField(Field, P);
	}
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
