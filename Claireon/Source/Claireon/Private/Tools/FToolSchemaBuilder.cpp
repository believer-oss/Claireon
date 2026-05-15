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
	AddString(TEXT("session_id"), TEXT("Session identifier from a previous open operation"), true);
	AddBoolean(TEXT("suppress_output"), TEXT("Return brief status instead of full state. Use for intermediate batch operations."));
}
