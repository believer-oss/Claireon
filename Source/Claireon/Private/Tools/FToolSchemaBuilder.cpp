// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FToolSchemaBuilder.h"

FToolSchemaBuilder::FToolSchemaBuilder()
{
	Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Properties = MakeShared<FJsonObject>();
}

void FToolSchemaBuilder::AddString(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("string"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddNumber(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("number"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddInteger(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("integer"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddBoolean(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("boolean"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddObject(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("object"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddArray(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("array"));
	Prop->SetStringField(TEXT("description"), Description);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

void FToolSchemaBuilder::AddEnum(const FString& Name, const FString& Description, const TArray<FString>& Values, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
	Prop->SetStringField(TEXT("type"), TEXT("string"));
	Prop->SetStringField(TEXT("description"), Description);
	TArray<TSharedPtr<FJsonValue>> EnumValues;
	for (const FString& Val : Values)
	{
		EnumValues.Add(MakeShared<FJsonValueString>(Val));
	}
	Prop->SetArrayField(TEXT("enum"), EnumValues);
	Properties->SetObjectField(Name, Prop);
	if (bRequired)
	{
		RequiredFields.Add(MakeShared<FJsonValueString>(Name));
	}
}

TSharedPtr<FJsonObject> FToolSchemaBuilder::Build()
{
	Schema->SetObjectField(TEXT("properties"), Properties);
	if (RequiredFields.Num() > 0)
	{
		Schema->SetArrayField(TEXT("required"), RequiredFields);
	}
	return Schema;
}

void FToolSchemaBuilder::AddSessionParams()
{
	// Deliberately session_id-REQUIRED and no asset_path (P2-1, verified
	// 2026-08-12): every non-Blueprint edit base that consumes this helper
	// (statetree, niagara, pcg, material, widgetbp, input, ...) resolves
	// session_id ONLY -- their RequireSession has no asset_path auto-open.
	// Declaring asset_path here would make ~180 tools accept a parameter
	// their bodies ignore, which the P1-2 argument gate exists to prevent.
	// A tool whose base DOES accept both (the BlueprintGraph family's
	// BeginSessionOp) must declare the canonical two-param shape itself --
	// see ClaireonBlueprintGraphTool_Close::GetInputSchema.
	AddString(TEXT("session_id"), TEXT("Session identifier from a previous open operation"), true);
	AddBoolean(TEXT("suppress_output"), TEXT("Return brief status instead of full state. Use for intermediate batch operations."));
}
