// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// File-local helpers shared across the decomposed Niagara edit tools.
// Reimplementations of non-exported Niagara editor utilities (UE 5.5).

#pragma once

#include "CoreMinimal.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterMapHistory.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

namespace ClaireonNiagaraEditInternal
{
	/** Find the ParameterMap input pin on a Niagara node by checking pin types. */
	inline UEdGraphPin* FindParameterMapInputPin(UNiagaraNode& Node)
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
	inline UEdGraphPin* FindParameterMapOutputPin(UNiagaraNode& Node)
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
	 * Walk a module function-call node's already-surfaced Module.* pins. A function-call
	 * node only surfaces a pin once that input has been overridden, so this finds ONLY
	 * overridden inputs. Kept as a fallback for GetModuleInputVariables; not a complete
	 * input list on its own.
	 */
	inline void GetSurfacedModuleInputPins(UNiagaraNodeFunctionCall& ModuleNode, TArray<FNiagaraVariable>& OutInputVars)
	{
		FPinCollectorArray InputPins;
		ModuleNode.GetInputPins(InputPins);
		for (UEdGraphPin* Pin : InputPins)
		{
			if (!Pin)
				continue;
			const FString PinName = Pin->PinName.ToString();
			if (!PinName.StartsWith(TEXT("Module.")))
				continue;
			FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (!PinType.IsValid())
				continue;
			OutInputVars.Emplace(PinType, *PinName);
		}
	}

	/**
	 * Enumerate a module's TRUE input set, including inputs that have never been overridden
	 * (which the surfaced-pin walk alone cannot find).
	 *
	 * The engine's authoritative path (FNiagaraStackGraphUtilities::GetStackFunctionInputs)
	 * builds a parameter-map history over the called graph, but its builder
	 * (TNiagaraParameterMapHistoryBuilder) is not exported to other modules, so we cannot
	 * construct it from here without an engine change. Instead we read the called script
	 * graph's declared parameters directly via the exported UNiagaraGraph::GetAllMetaData()
	 * and keep the Module.* namespace, excluding static switches (handled on a separate path).
	 *
	 * Trade-off vs. the faithful history traversal: this lists every declared Module.*
	 * parameter rather than only those actually read-first-with-no-prior-write. In practice a
	 * module's Module.* parameters are its inputs, so the common knobs (lifetime, count,
	 * color, size, radius, velocity) are all found. Edge cases this may over- or under-report:
	 * Module.* parameters that are written-before-read internals, or inputs surfaced only
	 * through nested dynamic-input graphs. Falls back to the overridden-pin walk if metadata
	 * is unavailable, so behavior never regresses below the old path.
	 */
	inline void GetModuleInputVariables(UNiagaraNodeFunctionCall& ModuleNode, TArray<FNiagaraVariable>& OutInputVars)
	{
		UNiagaraGraph* CalledGraph = ModuleNode.GetCalledGraph();
		if (CalledGraph)
		{
			// Static switches are surfaced separately by callers; exclude them here.
			TSet<FName> StaticSwitchNames;
			for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
			{
				StaticSwitchNames.Add(SwitchVar.GetName());
			}

			for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : CalledGraph->GetAllMetaData())
			{
				const FNiagaraVariable& Var = Pair.Key;
				if (!Var.GetType().IsValid())
					continue;
				if (!Var.GetName().ToString().StartsWith(TEXT("Module.")))
					continue;
				if (StaticSwitchNames.Contains(Var.GetName()))
					continue;
				OutInputVars.AddUnique(Var);
			}

			// Stable ordering: GetAllMetaData is an unordered map.
			OutInputVars.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
			{
				return A.GetName().LexicalLess(B.GetName());
			});
		}

		// Fallback: if metadata enumeration surfaced nothing (e.g. called graph unavailable),
		// fall back to the overridden-pin walk so behavior never regresses below the old path.
		if (OutInputVars.Num() == 0)
		{
			GetSurfacedModuleInputPins(ModuleNode, OutInputVars);
		}
	}

	/**
	 * Local reimplementation of FNiagaraStackGraphUtilities::GetStackFunctionInputOverridePin.
	 * Finds an existing override pin for a module input (read-only lookup).
	 */
	inline UEdGraphPin* FindStackFunctionInputOverridePin(UNiagaraNodeFunctionCall& StackFunctionCall, FNiagaraParameterHandle AliasedInputParameterHandle)
	{
		UEdGraphPin* FuncInputPin = ClaireonNiagaraEditInternal::FindParameterMapInputPin(StackFunctionCall);
		if (FuncInputPin && FuncInputPin->LinkedTo.Num() == 1)
		{
			UEdGraphNode* OverrideNode = FuncInputPin->LinkedTo[0]->GetOwningNode();
			if (OverrideNode)
			{
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

	/**
	 * Find a static switch input pin on a module node by name.
	 * Reimplements UNiagaraNodeFunctionCall::FindStaticSwitchInputPin (not exported).
	 */
	inline UEdGraphPin* FindStaticSwitchPin(UNiagaraNodeFunctionCall* ModuleNode, const FName& VariableName)
	{
		UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
		if (!CalledGraph)
		{
			return nullptr;
		}

		FPinCollectorArray InputPins;
		ModuleNode->GetInputPins(InputPins);
		for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
		{
			if (SwitchVar.GetName().IsEqual(VariableName))
			{
				for (UEdGraphPin* Pin : InputPins)
				{
					if (VariableName.IsEqual(Pin->GetFName()))
					{
						return Pin;
					}
				}
			}
		}
		return nullptr;
	}

	/**
	 * Ensures a Niagara graph has a valid Input -> Output parameter map chain for the given usage.
	 * Creates an input node and wires it to the output node if the chain is broken.
	 */
	inline void EnsureGraphInputOutputChain(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, FGuid UsageId)
	{
		if (!Graph)
		{
			return;
		}

		UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(Usage, UsageId);
		if (!OutputNode)
		{
			return;
		}

		UEdGraphPin* OutputInputPin = ClaireonNiagaraEditInternal::FindParameterMapInputPin(*OutputNode);
		if (!OutputInputPin)
		{
			return;
		}

		UNiagaraNode* CurrentNode = OutputNode;
		bool bFoundInputNode = false;
		while (CurrentNode)
		{
			UEdGraphPin* InputPin = ClaireonNiagaraEditInternal::FindParameterMapInputPin(*CurrentNode);
			if (InputPin && InputPin->LinkedTo.Num() == 1)
			{
				UNiagaraNode* PrevNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
				if (Cast<UNiagaraNodeInput>(PrevNode))
				{
					bFoundInputNode = true;
					break;
				}
				CurrentNode = PrevNode;
			}
			else
			{
				break;
			}
		}

		if (bFoundInputNode)
		{
			return;
		}

		Graph->Modify();

		FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*Graph);
		UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		InputNodeCreator.Finalize();

		UEdGraphPin* InputNodeOutputPin = ClaireonNiagaraEditInternal::FindParameterMapOutputPin(*InputNode);
		if (InputNodeOutputPin && OutputInputPin)
		{
			OutputInputPin->BreakAllPinLinks();
			OutputInputPin->MakeLinkTo(InputNodeOutputPin);
		}
	}

	/** Validate all emitter stack graphs for a newly added emitter. */
	inline void EnsureEmitterGraphChains(UNiagaraSystem* System, int32 EmitterIndex)
	{
		if (!System)
		{
			return;
		}

		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
		{
			return;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handles[EmitterIndex].GetEmitterData();
		if (!EmitterData)
		{
			return;
		}

		UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
		if (!ScriptSource || !ScriptSource->NodeGraph)
		{
			return;
		}

		UNiagaraGraph* Graph = ScriptSource->NodeGraph;

		EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::EmitterSpawnScript, EmitterData->EmitterSpawnScriptProps.Script->GetUsageId());
		EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::EmitterUpdateScript, EmitterData->EmitterUpdateScriptProps.Script->GetUsageId());
		EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::ParticleSpawnScript, EmitterData->SpawnScriptProps.Script->GetUsageId());
		EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::ParticleUpdateScript, EmitterData->UpdateScriptProps.Script->GetUsageId());
	}
}
