// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RemoveModule.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonNiagaraEditInternal.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RemoveModule::GetOperation() const { return TEXT("remove_module"); }

FString ClaireonNiagaraTool_RemoveModule::GetDescription() const
{
	return TEXT("Remove a module from an emitter stack by index, reconnecting the parameter map.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_RemoveModule::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddString(TEXT("stack"), TEXT("Stack name."), true);
	Builder.AddInteger(TEXT("module_index"), TEXT("Module index within the stack."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_RemoveModule::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString RemovedName = ModuleNodes[ModuleIndex]->GetFunctionName();

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Module")));
	System->Modify();

	UNiagaraNodeFunctionCall* ModuleToRemove = ModuleNodes[ModuleIndex];
	UEdGraphPin* ModuleInputPin = ClaireonNiagaraEditInternal::FindParameterMapInputPin(*ModuleToRemove);
	UEdGraphPin* ModuleOutputPin = ClaireonNiagaraEditInternal::FindParameterMapOutputPin(*ModuleToRemove);

	UEdGraphPin* UpstreamOutputPin = nullptr;
	UEdGraphNode* OverrideNode = nullptr;
	if (ModuleInputPin && ModuleInputPin->LinkedTo.Num() == 1)
	{
		UEdGraphPin* LinkedPin = ModuleInputPin->LinkedTo[0];
		UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
		UNiagaraNode* LinkedNiagaraNode = Cast<UNiagaraNode>(LinkedNode);
		if (LinkedNiagaraNode)
		{
			UEdGraphPin* OverrideInputPin = ClaireonNiagaraEditInternal::FindParameterMapInputPin(*LinkedNiagaraNode);
			if (OverrideInputPin && OverrideInputPin->LinkedTo.Num() == 1)
			{
				UpstreamOutputPin = OverrideInputPin->LinkedTo[0];
				OverrideNode = LinkedNode;
			}
			else
			{
				UpstreamOutputPin = LinkedPin;
			}
		}
	}

	UEdGraphPin* DownstreamInputPin = nullptr;
	if (ModuleOutputPin && ModuleOutputPin->LinkedTo.Num() == 1)
	{
		DownstreamInputPin = ModuleOutputPin->LinkedTo[0];
	}

	ModuleToRemove->BreakAllNodeLinks();

	if (OverrideNode)
	{
		OverrideNode->BreakAllNodeLinks();
	}

	if (UpstreamOutputPin && DownstreamInputPin)
	{
		UpstreamOutputPin->MakeLinkTo(DownstreamInputPin);
	}

	if (OverrideNode)
	{
		OverrideNode->DestroyNode();
	}
	ModuleToRemove->DestroyNode();

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_module -> Removed '%s' (index %d) from %s stack"),
		*RemovedName, ModuleIndex, *StackName);
	return BuildStateResponse(SessionId, Data);
}
