// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetParameterValue.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetParameterValue::GetOperation() const { return TEXT("set_parameter_value"); }

FString ClaireonNiagaraTool_SetParameterValue::GetDescription() const
{
	return TEXT("Set the value of an exposed User.* parameter on the Niagara System.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetParameterValue::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("name"), TEXT("Parameter name (User. prefix added automatically)."), true);
	Builder.AddString(TEXT("value"), TEXT("String-encoded value: scalar literal, 'X, Y, Z' or 'R, G, B, A'."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetParameterValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Arguments->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	UNiagaraSystem* System = Data->System.Get();

	TArray<FNiagaraVariable> UserParams;
	System->GetExposedParameters().GetUserParameters(UserParams);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& Param : UserParams)
	{
		if (Param.GetName().ToString().Equals(FullName, ESearchCase::IgnoreCase))
		{
			FoundVar = &Param;
			break;
		}
	}

	if (!FoundVar)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parameter '%s' not found in exposed parameters"), *FullName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Parameter Value")));
	System->Modify();

	FNiagaraTypeDefinition TypeDef = FoundVar->GetType();
	FNiagaraVariable MutableVar = *FoundVar;

	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		float FloatVal = FCString::Atof(*Value);
		MutableVar.SetValue(FloatVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		int32 IntVal = FCString::Atoi(*Value);
		MutableVar.SetValue(IntVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool BoolVal;
		BoolVal.SetValue(Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"));
		MutableVar.SetValue(BoolVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
	{
		FVector3f Vec;
		FString CleanValue = Value;
		CleanValue.ReplaceInline(TEXT("("), TEXT(""));
		CleanValue.ReplaceInline(TEXT(")"), TEXT(""));
		TArray<FString> Components;
		CleanValue.ParseIntoArray(Components, TEXT(","));
		if (Components.Num() >= 3)
		{
			Vec.X = FCString::Atof(*Components[0].TrimStartAndEnd());
			Vec.Y = FCString::Atof(*Components[1].TrimStartAndEnd());
			Vec.Z = FCString::Atof(*Components[2].TrimStartAndEnd());
		}
		MutableVar.SetValue(Vec);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		FLinearColor Color;
		FString CleanValue = Value;
		CleanValue.ReplaceInline(TEXT("("), TEXT(""));
		CleanValue.ReplaceInline(TEXT(")"), TEXT(""));
		CleanValue.ReplaceInline(TEXT("R="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("G="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("B="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("A="), TEXT(""));
		TArray<FString> Components;
		CleanValue.ParseIntoArray(Components, TEXT(","));
		if (Components.Num() >= 3)
		{
			Color.R = FCString::Atof(*Components[0].TrimStartAndEnd());
			Color.G = FCString::Atof(*Components[1].TrimStartAndEnd());
			Color.B = FCString::Atof(*Components[2].TrimStartAndEnd());
			Color.A = Components.Num() >= 4 ? FCString::Atof(*Components[3].TrimStartAndEnd()) : 1.0f;
		}
		MutableVar.SetValue(Color);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set value for parameter type: %s"), *TypeDef.GetName()));
	}

	System->GetExposedParameters().SetParameterData(MutableVar.GetData(), MutableVar, true);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_parameter_value -> Set '%s' = '%s'"),
		*FullName, *Value);
	return BuildStateResponse(SessionId, Data);
}
