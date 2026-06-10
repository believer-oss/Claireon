// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_GetModuleInputs.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonNiagaraEditInternal.h"
#include "NiagaraSystem.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraGraph.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_GetModuleInputs::GetOperation() const { return TEXT("get_module_inputs"); }

FString ClaireonNiagaraTool_GetModuleInputs::GetDescription() const
{
    return TEXT("List inputs (and static switches) for a specific module in a stack, including override status and current values. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_GetModuleInputs::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddString(TEXT("stack"), TEXT("Stack name."), true);
	Builder.AddInteger(TEXT("module_index"), TEXT("Module index within the stack."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_GetModuleInputs::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	int32 EmitterIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString StackName;
	if (!Arguments->TryGetStringField(TEXT("stack"), StackName) || StackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: stack"));
	}

	int32 ModuleIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("module_index"), ModuleIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: module_index"));
	}

	UNiagaraSystem* System = Data->System.Get();

	ENiagaraScriptUsage Usage;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	if (!ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error))
	{
		return MakeErrorResult(Error);
	}

	if (ModuleIndex < 0 || ModuleIndex >= ModuleNodes.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Module index %d out of range. Stack '%s' has %d modules (0-%d)"),
			ModuleIndex, *StackName, ModuleNodes.Num(), ModuleNodes.Num() - 1));
	}

	UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];

	TArray<FNiagaraVariable> InputVars;
	ClaireonNiagaraEditInternal::GetModuleInputVariables(*ModuleNode, InputVars);

	FString Output;
	Output += FString::Printf(TEXT("=== Module Inputs: %s ===\n"), *ModuleNode->GetFunctionName());
	Output += FString::Printf(TEXT("Stack: %s, Module Index: %d\n\n"), *StackName, ModuleIndex);

	if (InputVars.Num() == 0)
	{
		Output += TEXT("  (no inputs)\n");
	}

	for (int32 i = 0; i < InputVars.Num(); ++i)
	{
		const FNiagaraVariable& Var = InputVars[i];
		const FString VarName = Var.GetName().ToString();
		const FString TypeName = Var.GetType().GetName();

		FString RawName = Var.GetName().ToString();
		if (RawName.StartsWith(TEXT("Module.")))
		{
			RawName = RawName.Mid(7);
		}
		FNiagaraParameterHandle ModuleHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*RawName));
		FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, ModuleNode);

		UEdGraphPin* OverridePin = ClaireonNiagaraEditInternal::FindStackFunctionInputOverridePin(*ModuleNode, AliasedHandle);

		bool bIsOverridden = (OverridePin != nullptr);
		FString ValueStr = TEXT("(default)");

		if (bIsOverridden && OverridePin)
		{
			ValueStr = OverridePin->DefaultValue;
			if (ValueStr.IsEmpty())
			{
				ValueStr = TEXT("(empty)");
			}
		}

		Output += FString::Printf(TEXT("  [%d] %s : %s = %s%s\n"),
			i, *VarName, *TypeName, *ValueStr,
			bIsOverridden ? TEXT(" (overridden)") : TEXT(""));
	}

	UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
	if (CalledGraph)
	{
		TArray<FNiagaraVariable> SwitchVars = CalledGraph->FindStaticSwitchInputs();
		if (SwitchVars.Num() > 0)
		{
			Output += TEXT("\n  --- Static Switches ---\n");
			for (int32 i = 0; i < SwitchVars.Num(); ++i)
			{
				const FNiagaraVariable& SwitchVar = SwitchVars[i];
				UEdGraphPin* SwitchPin = ClaireonNiagaraEditInternal::FindStaticSwitchPin(ModuleNode, SwitchVar.GetName());
				FString SwitchValue = SwitchPin ? SwitchPin->DefaultValue : TEXT("(not set)");

				FString DisplayValue = SwitchValue;
				UEnum* SwitchEnum = SwitchVar.GetType().GetEnum();
				if (SwitchEnum)
				{
					for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e)
					{
						if (SwitchEnum->GetNameStringByIndex(e) == SwitchValue)
						{
							DisplayValue = SwitchEnum->GetDisplayNameTextByIndex(e).ToString();
							break;
						}
					}
				}

				FString ValidValues;
				if (SwitchEnum)
				{
					for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e)
					{
						if (!ValidValues.IsEmpty())
							ValidValues += TEXT(", ");
						ValidValues += FString::Printf(TEXT("%s (%s)"),
							*SwitchEnum->GetDisplayNameTextByIndex(e).ToString(),
							*SwitchEnum->GetNameStringByIndex(e));
					}
				}

				Output += FString::Printf(TEXT("  [S%d] %s = %s (static switch)%s\n"),
					i, *SwitchVar.GetName().ToString(), *DisplayValue,
					ValidValues.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\n        Valid: %s"), *ValidValues));
			}
		}
	}

	Data->LastOperationStatus = FString::Printf(TEXT("get_module_inputs -> Listed %d inputs for '%s'"),
		InputVars.Num(), *ModuleNode->GetFunctionName());

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(RespData, Output);
}
