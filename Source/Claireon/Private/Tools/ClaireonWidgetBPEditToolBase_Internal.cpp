// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPEditToolBase_Internal.h"
#include "ClaireonNameResolver.h"
#include "WidgetBlueprint.h"
#include "MVVMPropertyPath.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "Types/MVVMFieldVariant.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace ClaireonWidgetBPInternal
{
bool ParseBindingMode(const FString& ModeStr, EMVVMBindingMode& OutMode)
{
	if (ModeStr == TEXT("OneWayToDestination"))
	{
		OutMode = EMVVMBindingMode::OneWayToDestination;
		return true;
	}
	if (ModeStr == TEXT("OneWayToSource"))
	{
		OutMode = EMVVMBindingMode::OneWayToSource;
		return true;
	}
	if (ModeStr == TEXT("TwoWay"))
	{
		OutMode = EMVVMBindingMode::TwoWay;
		return true;
	}
	if (ModeStr == TEXT("OneTimeToDestination"))
	{
		OutMode = EMVVMBindingMode::OneTimeToDestination;
		return true;
	}
	if (ModeStr == TEXT("OneTimeToSource"))
	{
		OutMode = EMVVMBindingMode::OneTimeToSource;
		return true;
	}
	return false;
}

bool ParseCreationType(const FString& TypeStr, EMVVMBlueprintViewModelContextCreationType& OutType)
{
	if (TypeStr == TEXT("Manual"))
	{
		OutType = EMVVMBlueprintViewModelContextCreationType::Manual;
		return true;
	}
	if (TypeStr == TEXT("CreateInstance"))
	{
		OutType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
		return true;
	}
	if (TypeStr == TEXT("GlobalViewModelCollection"))
	{
		OutType = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection;
		return true;
	}
	if (TypeStr == TEXT("PropertyPath"))
	{
		OutType = EMVVMBlueprintViewModelContextCreationType::PropertyPath;
		return true;
	}
	if (TypeStr == TEXT("Resolver"))
	{
		OutType = EMVVMBlueprintViewModelContextCreationType::Resolver;
		return true;
	}
	return false;
}

bool ResolvePropertyPath(
	UWidgetBlueprint* WBP,
	FMVVMBlueprintPropertyPath& Path,
	UClass* StartClass,
	const FString& PropertyPathStr,
	FString& OutError)
{
	TArray<FString> Segments;
	PropertyPathStr.ParseIntoArray(Segments, TEXT("."), true);

	if (Segments.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Property path is empty"));
		return false;
	}

	UStruct* CurrentStruct = StartClass;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		if (!CurrentStruct)
		{
			OutError = FString::Printf(TEXT("Cannot resolve segment '%s' -- no struct context at depth %d"), *Segments[i], i);
			return false;
		}

		const FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Segments[i]));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *Segments[i], *CurrentStruct->GetName());
			return false;
		}

		UE::MVVM::FMVVMConstFieldVariant FieldVariant(Prop);
		if (i == 0)
		{
			Path.SetPropertyPath(WBP, FieldVariant);
		}
		else
		{
			Path.AppendPropertyPath(WBP, FieldVariant);
		}

		// Advance current struct for next segment
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			CurrentStruct = StructProp->Struct;
		}
		else if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			CurrentStruct = ObjProp->PropertyClass;
		}
		else
		{
			// Primitive -- no further nesting possible
			CurrentStruct = nullptr;
		}
	}

	return true;
}

const UFunction* ResolveConversionFunction(
	UWidgetBlueprint* WBP,
	const FString& FunctionNameStr,
	FString& OutError)
{
	// 1. Full path
	const UFunction* Func = FindObject<UFunction>(nullptr, *FunctionNameStr);
	if (Func)
	{
		if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
		{
			return Func;
		}
		OutError = FString::Printf(TEXT("Function '%s' found but is not a valid MVVM conversion function"), *FunctionNameStr);
		return nullptr;
	}

	// 2. Class::Function format
	FString ClassName, FuncName;
	if (FunctionNameStr.Split(TEXT("::"), &ClassName, &FuncName))
	{
		ClaireonNameResolver::FNameResolveResult ClassResult;
		UClass* FoundClass = ClaireonNameResolver::ResolveClassName(ClassName, nullptr, ClassResult);
		if (FoundClass)
		{
			ClaireonNameResolver::FNameResolveResult FuncResult;
			Func = ClaireonNameResolver::ResolveFunctionName(FoundClass, FuncName, FuncResult);
			if (Func)
			{
				if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
				{
					return Func;
				}
				OutError = FString::Printf(TEXT("Function '%s::%s' found but is not a valid MVVM conversion function"), *ClassName, *FuncName);
				return nullptr;
			}
		}
	}

	// 3. Self-context: search WBP generated class hierarchy
	if (WBP->GeneratedClass)
	{
		ClaireonNameResolver::FNameResolveResult SelfFuncResult;
		Func = ClaireonNameResolver::ResolveFunctionName(WBP->GeneratedClass, FunctionNameStr, SelfFuncResult);
		if (Func)
		{
			if (UMVVMBlueprintViewConversionFunction::IsValidConversionFunction(WBP, Func))
			{
				return Func;
			}
			OutError = FString::Printf(TEXT("Function '%s' found on self but is not a valid MVVM conversion function"), *FunctionNameStr);
			return nullptr;
		}
	}

	OutError = FString::Printf(TEXT("Conversion function '%s' not found"), *FunctionNameStr);
	return nullptr;
}
} // namespace ClaireonWidgetBPInternal
