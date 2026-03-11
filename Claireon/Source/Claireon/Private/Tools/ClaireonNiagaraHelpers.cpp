// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonLog.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraTypes.h"

// ============================================================================
// Asset Loading
// ============================================================================

UNiagaraSystem* ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Asset path is empty");
		return nullptr;
	}

	FSoftObjectPath SoftPath(AssetPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *AssetPath);
		return nullptr;
	}

	UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadedObj);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a Niagara System (actual type: %s)"), *AssetPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return System;
}

// ============================================================================
// Formatting Helpers
// ============================================================================

FString ClaireonNiagaraHelpers::FormatObjectProperties(const UObject* Object, const FString& Indent)
{
	if (!Object)
	{
		return FString();
	}

	FString Output;
	const UClass* ObjClass = Object->GetClass();

	for (TFieldIterator<FProperty> PropIt(ObjClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		const FString PropName = Prop->GetName();

		// Skip internal UE properties
		if (PropName == TEXT("UpdateInterval") || PropName == TEXT("VerNum"))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

		if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()"))
		{
			if (ValueStr.Len() > 120)
			{
				ValueStr = ValueStr.Left(117) + TEXT("...");
			}
			Output += FString::Printf(TEXT("%s%s = %s\n"), *Indent, *PropName, *ValueStr);
		}
	}

	return Output;
}

FString ClaireonNiagaraHelpers::GetRendererTypeName(const UNiagaraRendererProperties* Renderer)
{
	if (!Renderer)
	{
		return TEXT("Unknown");
	}

	if (Renderer->IsA<UNiagaraSpriteRendererProperties>())
	{
		return TEXT("Sprite Renderer");
	}
	if (Renderer->IsA<UNiagaraMeshRendererProperties>())
	{
		return TEXT("Mesh Renderer");
	}
	if (Renderer->IsA<UNiagaraRibbonRendererProperties>())
	{
		return TEXT("Ribbon Renderer");
	}
	if (Renderer->IsA<UNiagaraLightRendererProperties>())
	{
		return TEXT("Light Renderer");
	}

	// Fallback: use class name with cleanup
	FString ClassName = Renderer->GetClass()->GetName();
	ClassName.RemoveFromStart(TEXT("Niagara"));
	ClassName.RemoveFromEnd(TEXT("Properties"));
	return ClassName;
}

FString ClaireonNiagaraHelpers::FormatRendererProperties(const UNiagaraRendererProperties* Renderer, int32 RendererIndex, const FString& Indent)
{
	if (!Renderer)
	{
		return FString();
	}

	FString Output;
	Output += FString::Printf(TEXT("%s[Renderer %d] %s (%s)\n"), *Indent, RendererIndex, *GetRendererTypeName(Renderer), *Renderer->GetClass()->GetName());

	// Iterate UPROPERTY fields and export non-default/non-transient values
	const FString PropIndent = Indent + TEXT("  ");
	for (TFieldIterator<FProperty> PropIt(Renderer->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Renderer);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

		if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()"))
		{
			if (ValueStr.Len() > 120)
			{
				ValueStr = ValueStr.Left(117) + TEXT("...");
			}
			Output += FString::Printf(TEXT("%s%s = %s\n"), *PropIndent, *Prop->GetName(), *ValueStr);
		}
	}

	return Output;
}

FString ClaireonNiagaraHelpers::FormatEmitterStructure(const FNiagaraEmitterHandle& EmitterHandle, int32 EmitterIndex, bool bFullDetail)
{
	FString Output;

	Output += FString::Printf(TEXT("--- Emitter %d: %s ---\n"), EmitterIndex, *EmitterHandle.GetName().ToString());
	Output += FString::Printf(TEXT("  Enabled: %s\n"), EmitterHandle.GetIsEnabled() ? TEXT("true") : TEXT("false"));

	FVersionedNiagaraEmitterData* EmitterData = EmitterHandle.GetEmitterData();
	if (!EmitterData)
	{
		Output += TEXT("  (no emitter data)\n\n");
		return Output;
	}

	// Sim target
	const ENiagaraSimTarget SimTarget = EmitterData->SimTarget;
	Output += FString::Printf(TEXT("  Sim Target: %s\n"), SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("CPU") : TEXT("GPU"));

	// Local space
	Output += FString::Printf(TEXT("  Local Space: %s\n"), EmitterData->bLocalSpace ? TEXT("true") : TEXT("false"));

	// Renderers
	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (Renderers.Num() > 0)
	{
		Output += FString::Printf(TEXT("  Renderers: %d\n"), Renderers.Num());
		for (int32 RendererIdx = 0; RendererIdx < Renderers.Num(); ++RendererIdx)
		{
			if (Renderers[RendererIdx])
			{
				Output += FormatRendererProperties(Renderers[RendererIdx], RendererIdx, TEXT("    "));
			}
		}
	}
	else
	{
		Output += TEXT("  Renderers: (none)\n");
	}

	// Additional non-default properties if full detail
	if (bFullDetail)
	{
		// EmitterData is a struct, not a UObject — format properties from the UNiagaraEmitter instead
		UNiagaraEmitter* Emitter = EmitterHandle.GetInstance().Emitter;
		if (Emitter)
		{
			FString ExtraProps = FormatObjectProperties(Emitter, TEXT("    "));
			if (!ExtraProps.IsEmpty())
			{
				Output += TEXT("  Properties:\n");
				Output += ExtraProps;
			}
		}
	}

	Output += TEXT("\n");
	return Output;
}

FString ClaireonNiagaraHelpers::FormatUserParameters(const UNiagaraSystem* System)
{
	if (!System)
	{
		return FString();
	}

	FString Output;

	TArray<FNiagaraVariable> UserParams;
	System->GetExposedParameters().GetUserParameters(UserParams);

	if (UserParams.Num() == 0)
	{
		return TEXT("=== User Parameters ===\n  (none)\n");
	}

	Output += TEXT("=== User Parameters ===\n");
	for (const FNiagaraVariable& Param : UserParams)
	{
		Output += FString::Printf(TEXT("  %s : %s"), *Param.GetName().ToString(), *Param.GetType().GetName());

		// Try to display values for common types
		if (Param.IsDataAllocated())
		{
			const uint8* Data = Param.GetData();
			if (Data)
			{
				if (Param.GetType() == FNiagaraTypeDefinition::GetFloatDef() && Param.GetSizeInBytes() >= sizeof(float))
				{
					const float Value = *reinterpret_cast<const float*>(Data);
					Output += FString::Printf(TEXT(" = %.4f"), Value);
				}
				else if (Param.GetType() == FNiagaraTypeDefinition::GetIntDef() && Param.GetSizeInBytes() >= sizeof(int32))
				{
					const int32 Value = *reinterpret_cast<const int32*>(Data);
					Output += FString::Printf(TEXT(" = %d"), Value);
				}
				else if (Param.GetType() == FNiagaraTypeDefinition::GetBoolDef() && Param.GetSizeInBytes() >= sizeof(FNiagaraBool))
				{
					const FNiagaraBool Value = *reinterpret_cast<const FNiagaraBool*>(Data);
					Output += FString::Printf(TEXT(" = %s"), Value.GetValue() ? TEXT("true") : TEXT("false"));
				}
				else if (Param.GetType() == FNiagaraTypeDefinition::GetVec3Def() && Param.GetSizeInBytes() >= sizeof(FVector3f))
				{
					const FVector3f Value = *reinterpret_cast<const FVector3f*>(Data);
					Output += FString::Printf(TEXT(" = (%.2f, %.2f, %.2f)"), Value.X, Value.Y, Value.Z);
				}
				else if (Param.GetType() == FNiagaraTypeDefinition::GetColorDef() && Param.GetSizeInBytes() >= sizeof(FLinearColor))
				{
					const FLinearColor Value = *reinterpret_cast<const FLinearColor*>(Data);
					Output += FString::Printf(TEXT(" = (R=%.2f, G=%.2f, B=%.2f, A=%.2f)"), Value.R, Value.G, Value.B, Value.A);
				}
			}
		}

		Output += TEXT("\n");
	}

	return Output;
}

FString ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(const UNiagaraSystem* System, bool bFullDetail)
{
	if (!System)
	{
		return TEXT("(null Niagara System)");
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Niagara System: %s ===\n"), *System->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *System->GetPathName());

	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	Output += FString::Printf(TEXT("Emitters: %d\n"), EmitterHandles.Num());

	// System-level properties
	Output += FString::Printf(TEXT("Warmup Time: %.2f\n"), System->GetWarmupTime());
	Output += FString::Printf(TEXT("Fixed Bounds: %s\n"), System->GetFixedBounds().IsValid ? TEXT("Yes") : TEXT("No"));

	Output += TEXT("\n");

	// Per-emitter structure
	for (int32 EmitterIdx = 0; EmitterIdx < EmitterHandles.Num(); ++EmitterIdx)
	{
		Output += FormatEmitterStructure(EmitterHandles[EmitterIdx], EmitterIdx, bFullDetail);
	}

	// User parameters
	Output += FormatUserParameters(System);

	return Output;
}

// ============================================================================
// Class Resolution
// ============================================================================

UClass* ClaireonNiagaraHelpers::ResolveRendererClass(const FString& TypeName, FString& OutError)
{
	// Map shorthand names to full class names
	static const TMap<FString, FString> ShorthandMap = {
		{ TEXT("Sprite"),    TEXT("NiagaraSpriteRendererProperties") },
		{ TEXT("Mesh"),      TEXT("NiagaraMeshRendererProperties") },
		{ TEXT("Ribbon"),    TEXT("NiagaraRibbonRendererProperties") },
		{ TEXT("Light"),     TEXT("NiagaraLightRendererProperties") },
		{ TEXT("Decal"),     TEXT("NiagaraDecalRendererProperties") },
		{ TEXT("Component"), TEXT("NiagaraComponentRendererProperties") },
	};

	// Check shorthand map first (case-insensitive)
	for (const auto& Pair : ShorthandMap)
	{
		if (Pair.Key.Equals(TypeName, ESearchCase::IgnoreCase))
		{
			UClass* FoundClass = FindFirstObject<UClass>(*Pair.Value, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass)
			{
				return FoundClass;
			}
		}
	}

	// Try the raw input as a class name
	UClass* FoundClass = FindFirstObject<UClass>(*TypeName, EFindFirstObjectOptions::NativeFirst);
	if (FoundClass && FoundClass->IsChildOf(UNiagaraRendererProperties::StaticClass()))
	{
		return FoundClass;
	}

	// Try with U prefix stripped
	if (TypeName.StartsWith(TEXT("U")))
	{
		FString WithoutU = TypeName.Mid(1);
		FoundClass = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(UNiagaraRendererProperties::StaticClass()))
		{
			return FoundClass;
		}
	}

	OutError = FString::Printf(TEXT("Could not resolve renderer class: %s. "
		"Valid shorthand names: Sprite, Mesh, Ribbon, Light, Decal, Component. "
		"Or use the full class name (e.g., NiagaraSpriteRendererProperties)."), *TypeName);
	return nullptr;
}

// ============================================================================
// Property Setting
// ============================================================================

bool ClaireonNiagaraHelpers::SetObjectProperty(UObject* Object, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
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
