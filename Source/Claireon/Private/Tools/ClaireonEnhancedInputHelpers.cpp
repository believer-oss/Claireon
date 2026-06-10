// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "EnhancedInputLibrary.h"
#include "EnhancedInputModule.h"
#include "InputCoreTypes.h"

// ============================================================================
// Asset Loading
// ============================================================================

UObject* ClaireonEnhancedInputHelpers::LoadInputAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	if (Cast<UInputAction>(LoadedObj) || Cast<UInputMappingContext>(LoadedObj))
	{
		return LoadedObj;
	}

	OutError = FString::Printf(
		TEXT("Asset at %s is neither an Input Action nor an Input Mapping Context (actual type: %s)"),
		*ResolvedPath, *LoadedObj->GetClass()->GetName());
	return nullptr;
}

// ============================================================================
// Value Type Helpers
// ============================================================================

FString ClaireonEnhancedInputHelpers::ValueTypeToString(EInputActionValueType ValueType)
{
	switch (ValueType)
	{
	case EInputActionValueType::Boolean: return TEXT("Boolean");
	case EInputActionValueType::Axis1D:  return TEXT("Axis1D (float)");
	case EInputActionValueType::Axis2D:  return TEXT("Axis2D (Vector2D)");
	case EInputActionValueType::Axis3D:  return TEXT("Axis3D (Vector)");
	default: return TEXT("Unknown");
	}
}

bool ClaireonEnhancedInputHelpers::ParseValueType(const FString& TypeStr, EInputActionValueType& OutType, FString& OutError)
{
	FString Lower = TypeStr.ToLower();
	if (Lower == TEXT("bool") || Lower == TEXT("boolean") || Lower == TEXT("digital"))
	{
		OutType = EInputActionValueType::Boolean;
		return true;
	}
	if (Lower == TEXT("float") || Lower == TEXT("axis1d") || Lower == TEXT("1d"))
	{
		OutType = EInputActionValueType::Axis1D;
		return true;
	}
	if (Lower == TEXT("2d") || Lower == TEXT("axis2d") || Lower == TEXT("vector2d"))
	{
		OutType = EInputActionValueType::Axis2D;
		return true;
	}
	if (Lower == TEXT("3d") || Lower == TEXT("axis3d") || Lower == TEXT("vector"))
	{
		OutType = EInputActionValueType::Axis3D;
		return true;
	}
	OutError = FString::Printf(TEXT("Unknown value type: %s (expected: bool, float, 2d, 3d)"), *TypeStr);
	return false;
}

// ============================================================================
// Formatting Helpers
// ============================================================================

FString ClaireonEnhancedInputHelpers::FormatObjectProperties(const UObject* Object, const FString& Indent)
{
	if (!Object)
	{
		return TEXT("");
	}

	FString Output;
	for (TFieldIterator<FProperty> PropIt(Object->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (!Property)
		{
			continue;
		}

		// Skip transient, deprecated, and hidden properties
		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}
		if (Property->HasMetaData(TEXT("Hidden")))
		{
			continue;
		}
		// Skip object pointers (like Outer, Class) that are not useful for display
		if (CastField<FObjectProperty>(Property))
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		Property->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

		Output += FString::Printf(TEXT("%s%s: %s\n"), *Indent, *Property->GetName(), *ValueStr);
	}
	return Output;
}

FString ClaireonEnhancedInputHelpers::FormatTriggers(const TArray<TObjectPtr<UInputTrigger>>& Triggers, bool bSummaryOnly, const FString& Indent)
{
	if (Triggers.Num() == 0)
	{
		return FString::Printf(TEXT("%sTriggers: (none)\n"), *Indent);
	}

	FString Output;
	Output += FString::Printf(TEXT("%sTriggers (%d):\n"), *Indent, Triggers.Num());
	for (int32 i = 0; i < Triggers.Num(); ++i)
	{
		const UInputTrigger* Trigger = Triggers[i];
		if (!Trigger)
		{
			Output += FString::Printf(TEXT("%s  [%d] (null)\n"), *Indent, i);
			continue;
		}
		Output += FString::Printf(TEXT("%s  [%d] %s\n"), *Indent, i, *Trigger->GetClass()->GetName());
		if (!bSummaryOnly)
		{
			Output += FormatObjectProperties(Trigger, Indent + TEXT("      "));
		}
	}
	return Output;
}

FString ClaireonEnhancedInputHelpers::FormatModifiers(const TArray<TObjectPtr<UInputModifier>>& Modifiers, bool bSummaryOnly, const FString& Indent)
{
	if (Modifiers.Num() == 0)
	{
		return FString::Printf(TEXT("%sModifiers: (none)\n"), *Indent);
	}

	FString Output;
	Output += FString::Printf(TEXT("%sModifiers (%d):\n"), *Indent, Modifiers.Num());
	for (int32 i = 0; i < Modifiers.Num(); ++i)
	{
		const UInputModifier* Modifier = Modifiers[i];
		if (!Modifier)
		{
			Output += FString::Printf(TEXT("%s  [%d] (null)\n"), *Indent, i);
			continue;
		}
		Output += FString::Printf(TEXT("%s  [%d] %s\n"), *Indent, i, *Modifier->GetClass()->GetName());
		if (!bSummaryOnly)
		{
			Output += FormatObjectProperties(Modifier, Indent + TEXT("      "));
		}
	}
	return Output;
}

FString ClaireonEnhancedInputHelpers::FormatInputAction(const UInputAction* Action, bool bSummaryOnly)
{
	if (!Action)
	{
		return TEXT("(null Input Action)\n");
	}

	FString Output;
	Output += TEXT("=== Input Action ===\n");
	Output += FString::Printf(TEXT("Name: %s\n"), *Action->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *Action->GetPathName());
	Output += FString::Printf(TEXT("Value Type: %s\n"), *ValueTypeToString(Action->ValueType));
	Output += FString::Printf(TEXT("Description: %s\n"), *Action->ActionDescription.ToString());

	if (!bSummaryOnly)
	{
		Output += FString::Printf(TEXT("Trigger When Paused: %s\n"), Action->bTriggerWhenPaused ? TEXT("true") : TEXT("false"));
		Output += FString::Printf(TEXT("Consume Input: %s\n"), Action->bConsumeInput ? TEXT("true") : TEXT("false"));
		Output += FString::Printf(TEXT("Reserve All Mappings: %s\n"), Action->bReserveAllMappings ? TEXT("true") : TEXT("false"));

		// AccumulationBehavior
		FString AccumStr;
		switch (Action->AccumulationBehavior)
		{
		case EInputActionAccumulationBehavior::TakeHighestAbsoluteValue: AccumStr = TEXT("TakeHighestAbsoluteValue"); break;
		case EInputActionAccumulationBehavior::Cumulative: AccumStr = TEXT("Cumulative"); break;
		default: AccumStr = TEXT("Unknown"); break;
		}
		Output += FString::Printf(TEXT("Accumulation Behavior: %s\n"), *AccumStr);
	}

	Output += TEXT("\n");
	Output += FormatTriggers(Action->Triggers, bSummaryOnly, TEXT(""));
	Output += FormatModifiers(Action->Modifiers, bSummaryOnly, TEXT(""));

	return Output;
}

FString ClaireonEnhancedInputHelpers::FormatMapping(const FEnhancedActionKeyMapping& Mapping, int32 Index, bool bSummaryOnly)
{
	FString Output;
	Output += FString::Printf(TEXT("  [%d] Action: %s | Key: %s\n"),
		Index,
		Mapping.Action ? *Mapping.Action->GetPathName() : TEXT("(none)"),
		*Mapping.Key.GetFName().ToString());

	if (!bSummaryOnly)
	{
		Output += FormatTriggers(Mapping.Triggers, bSummaryOnly, TEXT("      "));
		Output += FormatModifiers(Mapping.Modifiers, bSummaryOnly, TEXT("      "));
	}

	return Output;
}

FString ClaireonEnhancedInputHelpers::FormatMappingContext(const UInputMappingContext* IMC, bool bSummaryOnly)
{
	if (!IMC)
	{
		return TEXT("(null Input Mapping Context)\n");
	}

	FString Output;
	Output += TEXT("=== Input Mapping Context ===\n");
	Output += FString::Printf(TEXT("Name: %s\n"), *IMC->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *IMC->GetPathName());
	Output += FString::Printf(TEXT("Description: %s\n"), *IMC->ContextDescription.ToString());

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	Output += FString::Printf(TEXT("Mappings (%d):\n"), Mappings.Num());

	for (int32 i = 0; i < Mappings.Num(); ++i)
	{
		Output += FormatMapping(Mappings[i], i, bSummaryOnly);
	}

	return Output;
}

// ============================================================================
// Class Resolution
// ============================================================================

UClass* ClaireonEnhancedInputHelpers::ResolveModifierClass(const FString& ClassName, FString& OutError)
{
	auto ValidateClass = [](UClass* Class) -> bool
	{
		return Class && Class->IsChildOf(UInputModifier::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract);
	};

	// Try direct name
	UClass* Result = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with UInputModifier prefix (e.g. "DeadZone" -> "UInputModifierDeadZone")
	FString WithPrefix = TEXT("UInputModifier") + ClassName;
	Result = FindFirstObject<UClass>(*WithPrefix, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with InputModifier prefix without U (e.g. "DeadZone" -> "InputModifierDeadZone")
	FString WithoutU = TEXT("InputModifier") + ClassName;
	Result = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with just U prefix (e.g. user typed "InputModifierDeadZone" -> "UInputModifierDeadZone")
	if (!ClassName.StartsWith(TEXT("U")))
	{
		FString WithU = TEXT("U") + ClassName;
		Result = FindFirstObject<UClass>(*WithU, EFindFirstObjectOptions::NativeFirst);
		if (ValidateClass(Result))
		{
			return Result;
		}
	}

	OutError = FString::Printf(TEXT("Could not resolve modifier class: %s. Try short names like 'DeadZone', 'Scalar', 'Negate', 'Swizzle', 'FOVScaling'."), *ClassName);
	return nullptr;
}

UClass* ClaireonEnhancedInputHelpers::ResolveTriggerClass(const FString& ClassName, FString& OutError)
{
	auto ValidateClass = [](UClass* Class) -> bool
	{
		return Class && Class->IsChildOf(UInputTrigger::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract);
	};

	// Try direct name
	UClass* Result = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with UInputTrigger prefix (e.g. "Down" -> "UInputTriggerDown")
	FString WithPrefix = TEXT("UInputTrigger") + ClassName;
	Result = FindFirstObject<UClass>(*WithPrefix, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with InputTrigger prefix without U (e.g. "Down" -> "InputTriggerDown")
	FString WithoutU = TEXT("InputTrigger") + ClassName;
	Result = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
	if (ValidateClass(Result))
	{
		return Result;
	}

	// Try with just U prefix
	if (!ClassName.StartsWith(TEXT("U")))
	{
		FString WithU = TEXT("U") + ClassName;
		Result = FindFirstObject<UClass>(*WithU, EFindFirstObjectOptions::NativeFirst);
		if (ValidateClass(Result))
		{
			return Result;
		}
	}

	OutError = FString::Printf(TEXT("Could not resolve trigger class: %s. Try short names like 'Down', 'Pressed', 'Released', 'Hold', 'HoldAndRelease', 'Tap', 'Pulse', 'ChordAction'."), *ClassName);
	return nullptr;
}

// ============================================================================
// Object Creation
// ============================================================================

UInputModifier* ClaireonEnhancedInputHelpers::CreateModifier(UObject* Outer, UClass* ModifierClass)
{
	if (!Outer || !ModifierClass)
	{
		return nullptr;
	}
	return NewObject<UInputModifier>(Outer, ModifierClass, NAME_None, RF_Transactional);
}

UInputTrigger* ClaireonEnhancedInputHelpers::CreateTrigger(UObject* Outer, UClass* TriggerClass)
{
	if (!Outer || !TriggerClass)
	{
		return nullptr;
	}
	return NewObject<UInputTrigger>(Outer, TriggerClass, NAME_None, RF_Transactional);
}

// ============================================================================
// Property Manipulation
// ============================================================================

bool ClaireonEnhancedInputHelpers::SetObjectProperty(UObject* Object, const FString& PropertyName,
	const FString& PropertyValue, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Object is null");
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Object->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Object, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *PropertyName, *PropertyValue, *Object->GetClass()->GetName());
		return false;
	}

	return true;
}

// ============================================================================
// IMC Helpers
// ============================================================================

TArray<FEnhancedActionKeyMapping>& ClaireonEnhancedInputHelpers::GetMappingsMutable(UInputMappingContext* IMC)
{
	static FArrayProperty* MappingsProp = nullptr;
	if (!MappingsProp)
	{
		MappingsProp = CastField<FArrayProperty>(
			FindFProperty<FProperty>(UInputMappingContext::StaticClass(), TEXT("Mappings")));
	}
	check(MappingsProp);
	return *MappingsProp->ContainerPtrToValuePtr<TArray<FEnhancedActionKeyMapping>>(IMC);
}

void ClaireonEnhancedInputHelpers::NotifyMappingContextModified(UInputMappingContext* IMC)
{
	if (!IMC)
	{
		return;
	}
	IEnhancedInputModule::Get().GetLibrary()->RequestRebuildControlMappingsUsingContext(IMC);
}

// ============================================================================
// Key Resolution
// ============================================================================

bool ClaireonEnhancedInputHelpers::ResolveKey(const FString& KeyName, FKey& OutKey, FString& OutError)
{
	FKey TestKey(*KeyName);
	if (!TestKey.IsValid() || !EKeys::GetKeyDetails(TestKey).IsValid())
	{
		OutError = FString::Printf(TEXT("Unknown key name: %s"), *KeyName);
		return false;
	}
	OutKey = TestKey;
	return true;
}
