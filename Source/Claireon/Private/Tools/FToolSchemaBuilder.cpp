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

void FToolSchemaBuilder::AddStringOrStringArray(const FString& Name, const FString& Description, bool bRequired)
{
	TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Types;
	Types.Add(MakeShared<FJsonValueString>(TEXT("array")));
	Types.Add(MakeShared<FJsonValueString>(TEXT("string")));
	Prop->SetArrayField(TEXT("type"), Types);

	TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
	Items->SetStringField(TEXT("type"), TEXT("string"));
	Prop->SetObjectField(TEXT("items"), Items);

	Prop->SetStringField(TEXT("description"), Description);
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
	// Consumers require session_id and do not auto-open asset_path. Bases that accept
	// both must declare their own schema, as the BlueprintGraph family does.
	AddString(TEXT("session_id"), TEXT("Session identifier from a previous open operation"), true);
	AddBoolean(TEXT("suppress_output"), TEXT("Return brief status instead of full state. Use for intermediate batch operations."));
}
