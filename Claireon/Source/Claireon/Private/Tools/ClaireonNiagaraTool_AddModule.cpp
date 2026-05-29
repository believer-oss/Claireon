// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_AddModule.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraScript.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_AddModule::GetOperation() const { return TEXT("add_module"); }

FString ClaireonNiagaraTool_AddModule::GetDescription() const
{
    return TEXT("Add a module script to an emitter stack at the specified index. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_AddModule::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddString(TEXT("stack"), TEXT("Stack name (emitter_spawn/emitter_update/particle_spawn/particle_update)."), true);
	Builder.AddString(TEXT("module"), TEXT("Module short name or full asset path."), true);
	Builder.AddInteger(TEXT("index"), TEXT("Optional insertion index within the stack."));
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_AddModule::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ModuleName;
	if (!Arguments->TryGetStringField(TEXT("module"), ModuleName) || ModuleName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: module"));
	}

	int32 Index = INDEX_NONE;
	Arguments->TryGetNumberField(TEXT("index"), Index);

	UNiagaraSystem* System = Data->System.Get();

	ENiagaraScriptUsage Usage;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	UNiagaraScript* Script = ClaireonNiagaraHelpers::ResolveModuleScript(ModuleName, Error);
	if (!Script)
	{
		return MakeErrorResult(Error);
	}

	UNiagaraNodeOutput* OutputNode = ClaireonNiagaraHelpers::GetStackOutputNode(System, EmitterIndex, Usage, Error);
	if (!OutputNode)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Module")));
	System->Modify();

	UNiagaraNodeFunctionCall* NewModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *OutputNode, Index);

	System->MarkPackageDirty();

	if (!NewModuleNode)
	{
		return MakeErrorResult(TEXT("AddScriptModuleToStack returned null - module could not be added"));
	}

	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error);

	int32 NewIndex = ModuleNodes.IndexOfByKey(NewModuleNode);

	Data->LastOperationStatus = FString::Printf(TEXT("add_module -> Added '%s' to %s stack at index %d"),
		*NewModuleNode->GetFunctionName(), *StackName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}
