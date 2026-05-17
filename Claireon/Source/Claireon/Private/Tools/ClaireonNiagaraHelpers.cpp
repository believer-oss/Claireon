// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
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
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScript.h"
#include "NiagaraParameterMapHistory.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ============================================================================
// File-local helpers for graph traversal (replacing non-exported engine functions)
// ============================================================================

namespace ClaireonNiagaraHelpersInternal
{

/** Find the ParameterMap input pin on a Niagara node by checking pin types. */
UEdGraphPin* FindParameterMapInputPin(UNiagaraNode& Node)
{
	FPinCollectorArray InputPins;
	Node.GetInputPins(InputPins);
	for (UEdGraphPin* Pin : InputPins)
	{
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Find the ParameterMap output pin on a Niagara node by checking pin types. */
UEdGraphPin* FindParameterMapOutputPin(UNiagaraNode& Node)
{
	FPinCollectorArray OutputPins;
	Node.GetOutputPins(OutputPins);
	for (UEdGraphPin* Pin : OutputPins)
	{
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/**
 * Local reimplementation of FNiagaraStackGraphUtilities::GetStackFunctionInputOverridePin
 * which is not exported from NiagaraEditor. Finds an existing override pin for a module input.
 */
UEdGraphPin* FindStackFunctionInputOverridePin(UNiagaraNodeFunctionCall& StackFunctionCall, FNiagaraParameterHandle AliasedInputParameterHandle)
{
	// Find the ParameterMapSet node connected to the function call's parameter map input
	UEdGraphPin* FuncInputPin = FindParameterMapInputPin(StackFunctionCall);
	if (FuncInputPin && FuncInputPin->LinkedTo.Num() == 1)
	{
		UEdGraphNode* OverrideNode = FuncInputPin->LinkedTo[0]->GetOwningNode();
		if (OverrideNode)
		{
			// Search the override node's input pins for one matching the aliased handle name
			for (UEdGraphPin* Pin : OverrideNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinName == AliasedInputParameterHandle.GetParameterHandleString())
				{
					return Pin;
				}
			}
		}
	}
	return nullptr;
}

}  // namespace ClaireonNiagaraHelpersInternal

// ============================================================================
// Asset Loading
// ============================================================================

UNiagaraSystem* ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(const FString& AssetPath, FString& OutError)
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

	UNiagaraSystem* System = Cast<UNiagaraSystem>(LoadedObj);
	if (!System)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a Niagara System (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
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

FString ClaireonNiagaraHelpers::FormatEmitterStructure(const UNiagaraSystem* System, const FNiagaraEmitterHandle& EmitterHandle, int32 EmitterIndex, bool bFullDetail)
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

	// Module stacks (Stage 009)
	if (System)
	{
		Output += TEXT("  Stacks:\n");

		static const TPair<FString, ENiagaraScriptUsage> StackDefs[] = {
			{ TEXT("EmitterSpawn"),   ENiagaraScriptUsage::EmitterSpawnScript },
			{ TEXT("EmitterUpdate"),  ENiagaraScriptUsage::EmitterUpdateScript },
			{ TEXT("ParticleSpawn"), ENiagaraScriptUsage::ParticleSpawnScript },
			{ TEXT("ParticleUpdate"), ENiagaraScriptUsage::ParticleUpdateScript },
		};

		for (const auto& StackDef : StackDefs)
		{
			Output += FString::Printf(TEXT("    %s:\n"), *StackDef.Key);

			TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
			FString StackError;
			if (GetOrderedModuleNodes(const_cast<UNiagaraSystem*>(System), EmitterIndex, StackDef.Value, ModuleNodes, StackError))
			{
				if (ModuleNodes.Num() == 0)
				{
					Output += TEXT("      (empty)\n");
				}
				else
				{
					for (int32 ModIdx = 0; ModIdx < ModuleNodes.Num(); ++ModIdx)
					{
						Output += FString::Printf(TEXT("      [%d] %s\n"), ModIdx, *FormatModuleInfo(ModuleNodes[ModIdx], true));
					}
				}
			}
			else
			{
				Output += TEXT("      (unavailable)\n");
			}
		}
	}

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
		Output += FormatEmitterStructure(System, EmitterHandles[EmitterIdx], EmitterIdx, bFullDetail);
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
			ClaireonNameResolver::FNameResolveResult ShorthandResult;
			UClass* FoundClass = ClaireonNameResolver::ResolveClassName(Pair.Value, UNiagaraRendererProperties::StaticClass(), ShorthandResult);
			if (FoundClass)
			{
				return FoundClass;
			}
		}
	}

	// Try the raw input as a class name via the core resolver
	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* FoundClass = ClaireonNameResolver::ResolveClassName(TypeName, UNiagaraRendererProperties::StaticClass(), NameResult);
	if (FoundClass)
	{
		return FoundClass;
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

// ============================================================================
// Stack Resolution + Graph Traversal (Stage 001)
// ============================================================================

bool ClaireonNiagaraHelpers::ResolveStackName(const FString& StackName, ENiagaraScriptUsage& OutUsage, FString& OutError)
{
	if (StackName.Equals(TEXT("EmitterSpawn"), ESearchCase::IgnoreCase))
	{
		OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
		return true;
	}
	if (StackName.Equals(TEXT("EmitterUpdate"), ESearchCase::IgnoreCase))
	{
		OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
		return true;
	}
	if (StackName.Equals(TEXT("ParticleSpawn"), ESearchCase::IgnoreCase))
	{
		OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
		return true;
	}
	if (StackName.Equals(TEXT("ParticleUpdate"), ESearchCase::IgnoreCase))
	{
		OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
		return true;
	}
	if (StackName.Equals(TEXT("ParticleEvent"), ESearchCase::IgnoreCase))
	{
		OutUsage = ENiagaraScriptUsage::ParticleEventScript;
		return true;
	}

	OutError = TEXT("Invalid stack name. Valid: EmitterSpawn, EmitterUpdate, ParticleSpawn, ParticleUpdate, ParticleEvent");
	return false;
}

UNiagaraNodeOutput* ClaireonNiagaraHelpers::GetStackOutputNode(UNiagaraSystem* System, int32 EmitterIndex, ENiagaraScriptUsage Usage, FString& OutError)
{
	if (!System)
	{
		OutError = TEXT("System is null");
		return nullptr;
	}

	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	if (EmitterIndex < 0 || EmitterIndex >= EmitterHandles.Num())
	{
		OutError = FString::Printf(TEXT("Emitter index %d out of range (system has %d emitters)"), EmitterIndex, EmitterHandles.Num());
		return nullptr;
	}

	const FNiagaraEmitterHandle& Handle = EmitterHandles[EmitterIndex];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		OutError = FString::Printf(TEXT("Emitter %d ('%s') has no emitter data"), EmitterIndex, *Handle.GetName().ToString());
		return nullptr;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (!ScriptSource)
	{
		OutError = FString::Printf(TEXT("Emitter %d ('%s') has no script source (GraphSource is null or not UNiagaraScriptSource)"), EmitterIndex, *Handle.GetName().ToString());
		return nullptr;
	}

	UNiagaraGraph* Graph = ScriptSource->NodeGraph;
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("Emitter %d ('%s') script source has no NodeGraph"), EmitterIndex, *Handle.GetName().ToString());
		return nullptr;
	}

	UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(Usage, FGuid());
	if (!OutputNode)
	{
		OutError = FString::Printf(TEXT("Emitter %d ('%s') has no output node for the specified stack usage"), EmitterIndex, *Handle.GetName().ToString());
		return nullptr;
	}

	return OutputNode;
}

bool ClaireonNiagaraHelpers::GetOrderedModuleNodes(UNiagaraSystem* System, int32 EmitterIndex, ENiagaraScriptUsage Usage, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes, FString& OutError)
{
	UNiagaraNodeOutput* OutputNode = GetStackOutputNode(System, EmitterIndex, Usage, OutError);
	if (!OutputNode)
	{
		return false;
	}

	// Walk backward from the output node along ParameterMap pin connections,
	// collecting UNiagaraNodeFunctionCall nodes in execution order.
	// This reimplements FNiagaraStackGraphUtilities::GetOrderedModuleNodes
	// which is not exported from NiagaraEditor.
	OutModuleNodes.Reset();
	UNiagaraNode* PreviousNode = OutputNode;
	while (PreviousNode)
	{
		UEdGraphPin* InputPin = ClaireonNiagaraHelpersInternal::FindParameterMapInputPin(*PreviousNode);
		if (InputPin && InputPin->LinkedTo.Num() == 1)
		{
			UNiagaraNode* CurrentNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
			if (!CurrentNode)
			{
				break;
			}

			UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(CurrentNode);
			if (ModuleNode)
			{
				OutModuleNodes.Insert(ModuleNode, 0);
			}
			PreviousNode = CurrentNode;
		}
		else
		{
			PreviousNode = nullptr;
		}
	}
	return true;
}

UNiagaraScript* ClaireonNiagaraHelpers::ResolveModuleScript(const FString& ModuleNameOrPath, FString& OutError)
{
	if (ModuleNameOrPath.IsEmpty())
	{
		OutError = TEXT("Module name or path is empty");
		return nullptr;
	}

	// Full asset path: load directly
	if (ModuleNameOrPath.StartsWith(TEXT("/")))
	{
		FSoftObjectPath SoftPath(ModuleNameOrPath);
		UObject* LoadedObj = SoftPath.TryLoad();
		if (!LoadedObj)
		{
			OutError = FString::Printf(TEXT("Failed to load module at path: %s"), *ModuleNameOrPath);
			return nullptr;
		}

		UNiagaraScript* Script = Cast<UNiagaraScript>(LoadedObj);
		if (!Script)
		{
			OutError = FString::Printf(TEXT("Asset at %s is not a UNiagaraScript (actual type: %s)"), *ModuleNameOrPath, *LoadedObj->GetClass()->GetName());
			return nullptr;
		}

		return Script;
	}

	// Short name: search asset registry for UNiagaraScript assets matching the name
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// First pass: exact match (case-insensitive)
	for (const FAssetData& Asset : AssetList)
	{
		if (Asset.AssetName.ToString().Equals(ModuleNameOrPath, ESearchCase::IgnoreCase))
		{
			UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset());
			if (Script)
			{
				return Script;
			}
		}
	}

	// Second pass: contains match (case-insensitive)
	UNiagaraScript* BestMatch = nullptr;
	FString BestMatchName;
	int32 BestMatchLen = MAX_int32;

	for (const FAssetData& Asset : AssetList)
	{
		const FString AssetName = Asset.AssetName.ToString();
		if (AssetName.Contains(ModuleNameOrPath, ESearchCase::IgnoreCase))
		{
			// Prefer shorter names (more specific matches)
			if (AssetName.Len() < BestMatchLen)
			{
				UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset());
				if (Script)
				{
					BestMatch = Script;
					BestMatchName = AssetName;
					BestMatchLen = AssetName.Len();
				}
			}
		}
	}

	if (BestMatch)
	{
		return BestMatch;
	}

	OutError = FString::Printf(TEXT("Could not find a Niagara module matching '%s'. Use list_modules to discover available modules, or provide a full asset path."), *ModuleNameOrPath);
	return nullptr;
}

FString ClaireonNiagaraHelpers::FormatModuleInfo(UNiagaraNodeFunctionCall* ModuleNode, bool bIncludeInputs)
{
	if (!ModuleNode)
	{
		return TEXT("(null module node)");
	}

	FString Output = ModuleNode->GetFunctionName();

	if (bIncludeInputs)
	{
		// Enumerate Module.* input pins using exported graph APIs (GetStackFunctionInputs is
		// not exported from NiagaraEditor in UE 5.5 — no NIAGARAEDITOR_API on those overloads).
		TArray<FNiagaraVariable> InputVars;
		FPinCollectorArray InputPins;
		ModuleNode->GetInputPins(InputPins);
		for (UEdGraphPin* Pin : InputPins)
		{
			if (!Pin) continue;
			const FString PinName = Pin->PinName.ToString();
			if (!PinName.StartsWith(TEXT("Module."))) continue;
			FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType.IsValid())
				InputVars.Emplace(PinType, *PinName);
		}

		if (InputVars.Num() > 0)
		{
			Output += TEXT(" (");
			const int32 MaxInputs = FMath::Min(InputVars.Num(), 3);
			for (int32 i = 0; i < MaxInputs; ++i)
			{
				if (i > 0)
				{
					Output += TEXT(", ");
				}

				const FNiagaraVariable& Var = InputVars[i];
				FString ValueStr;

				if (Var.IsDataAllocated())
				{
					const uint8* Data = Var.GetData();
					if (Data)
					{
						if (Var.GetType() == FNiagaraTypeDefinition::GetFloatDef() && Var.GetSizeInBytes() >= sizeof(float))
						{
							ValueStr = FString::Printf(TEXT("%.4f"), *reinterpret_cast<const float*>(Data));
						}
						else if (Var.GetType() == FNiagaraTypeDefinition::GetIntDef() && Var.GetSizeInBytes() >= sizeof(int32))
						{
							ValueStr = FString::Printf(TEXT("%d"), *reinterpret_cast<const int32*>(Data));
						}
						else if (Var.GetType() == FNiagaraTypeDefinition::GetBoolDef() && Var.GetSizeInBytes() >= sizeof(FNiagaraBool))
						{
							const FNiagaraBool BoolVal = *reinterpret_cast<const FNiagaraBool*>(Data);
							ValueStr = BoolVal.GetValue() ? TEXT("true") : TEXT("false");
						}
					}
				}

				if (ValueStr.IsEmpty())
				{
					Output += FString::Printf(TEXT("%s:%s"), *Var.GetName().ToString(), *Var.GetType().GetName());
				}
				else
				{
					Output += FString::Printf(TEXT("%s=%s"), *Var.GetName().ToString(), *ValueStr);
				}
			}

			if (InputVars.Num() > MaxInputs)
			{
				Output += FString::Printf(TEXT(", +%d more"), InputVars.Num() - MaxInputs);
			}

			Output += TEXT(")");
		}
	}

	return Output;
}
