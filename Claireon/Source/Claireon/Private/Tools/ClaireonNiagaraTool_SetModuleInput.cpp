// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_SetModuleInput.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonNiagaraEditInternal.h"
#include "NiagaraSystem.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Curves/RichCurve.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_SetModuleInput::GetOperation() const { return TEXT("set_module_input"); }

FString ClaireonNiagaraTool_SetModuleInput::GetDescription() const
{
	return TEXT("Set a module input or static switch value. Supports scalar pin values, float curves (JSON array) and color curves (JSON object).");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_SetModuleInput::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("emitter_index"), TEXT("Emitter index."), true);
	Builder.AddString(TEXT("stack"), TEXT("Stack name."), true);
	Builder.AddInteger(TEXT("module_index"), TEXT("Module index within the stack."), true);
	Builder.AddString(TEXT("input_name"), TEXT("Input name (with or without Module. prefix)."), true);
	Builder.AddString(TEXT("value"), TEXT("Value to set. Scalar literal, or JSON for curves/colors."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_SetModuleInput::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString InputName;
	if (!Arguments->TryGetStringField(TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: input_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
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

	const FNiagaraVariable* MatchedVar = nullptr;
	for (const FNiagaraVariable& Var : InputVars)
	{
		if (Var.GetName().ToString().Equals(InputName, ESearchCase::IgnoreCase))
		{
			MatchedVar = &Var;
			break;
		}
	}

	if (!MatchedVar)
	{
		// Not a regular input - check static switch
		FString SwitchName = InputName;
		if (SwitchName.StartsWith(TEXT("Module.")))
		{
			SwitchName = SwitchName.Mid(7);
		}
		UEdGraphPin* SwitchPin = ClaireonNiagaraEditInternal::FindStaticSwitchPin(ModuleNode, FName(*SwitchName));
		if (SwitchPin)
		{
			FString ResolvedValue = Value;
			UNiagaraGraph* SwitchGraph = ModuleNode->GetCalledGraph();
			if (SwitchGraph)
			{
				for (const FNiagaraVariable& SwitchVar : SwitchGraph->FindStaticSwitchInputs())
				{
					if (SwitchVar.GetName().IsEqual(FName(*SwitchName)))
					{
						UEnum* SwitchEnum = SwitchVar.GetType().GetEnum();
						if (SwitchEnum)
						{
							for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e)
							{
								FString DisplayName = SwitchEnum->GetDisplayNameTextByIndex(e).ToString();
								if (DisplayName.Equals(Value, ESearchCase::IgnoreCase))
								{
									ResolvedValue = SwitchEnum->GetNameStringByIndex(e);
									break;
								}
							}
						}
						break;
					}
				}
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Static Switch")));
			System->Modify();
			SwitchPin->Modify();
			SwitchPin->DefaultValue = ResolvedValue;

			ModuleNode->RefreshFromExternalChanges();
			FNiagaraStackGraphUtilities::SynchronizeReferencingMapPinsWithFunctionCall(*ModuleNode);
			ModuleNode->MarkNodeRequiresSynchronization(TEXT("MCP set static switch"), true);
			System->MarkPackageDirty();

			Data->LastOperationStatus = FString::Printf(TEXT("set_module_input -> Set static switch '%s' = '%s' on module '%s'"),
				*SwitchName, *Value, *ModuleNode->GetFunctionName());
			return BuildStateResponse(SessionId, Data);
		}

		FString AvailableInputs;
		for (const FNiagaraVariable& Var : InputVars)
		{
			if (!AvailableInputs.IsEmpty())
			{
				AvailableInputs += TEXT(", ");
			}
			AvailableInputs += Var.GetName().ToString();
		}
		UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
		if (CalledGraph)
		{
			for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
			{
				if (!AvailableInputs.IsEmpty())
				{
					AvailableInputs += TEXT(", ");
				}
				AvailableInputs += SwitchVar.GetName().ToString() + TEXT(" (static switch)");
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Input '%s' not found on module '%s'. Available inputs: %s"),
			*InputName, *ModuleNode->GetFunctionName(), *AvailableInputs));
	}

	FNiagaraTypeDefinition TypeDef = MatchedVar->GetType();

	FString RawInputName = MatchedVar->GetName().ToString();
	if (RawInputName.StartsWith(TEXT("Module.")))
	{
		RawInputName = RawInputName.Mid(7);
	}
	FNiagaraParameterHandle ModuleHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*RawInputName));
	FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, ModuleNode);

	FGuid ScriptVarId;
	UNiagaraScriptSource* FunctionScriptSource = ModuleNode->GetFunctionScriptSource();
	if (FunctionScriptSource && FunctionScriptSource->NodeGraph)
	{
		FNiagaraVariable LookupVar = *MatchedVar;
		TOptional<FNiagaraVariableMetaData> MetaData = FunctionScriptSource->NodeGraph->GetMetaData(LookupVar);
		if (MetaData.IsSet())
		{
			ScriptVarId = MetaData->GetVariableGuid();
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Module Input")));
	System->Modify();

	if (TypeDef.IsDataInterface())
	{
		UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*ModuleNode, AliasedHandle, TypeDef, ScriptVarId, FGuid());

		UNiagaraDataInterface* ExistingDI = nullptr;
		if (OverridePin.LinkedTo.Num() == 1)
		{
			UEdGraphNode* LinkedNode = OverridePin.LinkedTo[0]->GetOwningNode();
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
			{
				FProperty* DIProp = FindFProperty<FProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"));
				if (DIProp)
				{
					UObject* const* DIPtr = DIProp->ContainerPtrToValuePtr<UObject*>(InputNode);
					if (DIPtr)
					{
						ExistingDI = Cast<UNiagaraDataInterface>(*DIPtr);
					}
				}
			}
		}

		if (Value.StartsWith(TEXT("[")))
		{
			TArray<TSharedPtr<FJsonValue>> KeyArray;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
			if (!FJsonSerializer::Deserialize(Reader, KeyArray))
			{
				return MakeErrorResult(TEXT("Failed to parse float curve JSON array. Expected: [{\"time\": 0.0, \"value\": 1.0}, ...]"));
			}

			FRichCurve NewCurve;
			for (const TSharedPtr<FJsonValue>& KeyVal : KeyArray)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (KeyObj.IsValid())
				{
					double Time = KeyObj->GetNumberField(TEXT("time"));
					double Val = KeyObj->GetNumberField(TEXT("value"));
					NewCurve.AddKey(static_cast<float>(Time), static_cast<float>(Val));
				}
			}

			if (UNiagaraDataInterfaceCurve* ExistingCurve = Cast<UNiagaraDataInterfaceCurve>(ExistingDI))
			{
				ExistingCurve->Modify();
				ExistingCurve->Curve = NewCurve;
			}
			else
			{
				if (OverridePin.LinkedTo.Num() > 0)
				{
					OverridePin.BreakAllPinLinks();
				}
				UNiagaraDataInterface* OutDI = nullptr;
				FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverridePin, UNiagaraDataInterfaceCurve::StaticClass(), InputName, OutDI, ScriptVarId);
				if (UNiagaraDataInterfaceCurve* TargetCurve = Cast<UNiagaraDataInterfaceCurve>(OutDI))
				{
					TargetCurve->Curve = NewCurve;
				}
			}
		}
		else if (Value.StartsWith(TEXT("{")) && Value.Contains(TEXT("\"r\"")))
		{
			TSharedPtr<FJsonObject> ColorObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
			if (!FJsonSerializer::Deserialize(Reader, ColorObj) || !ColorObj.IsValid())
			{
				return MakeErrorResult(TEXT("Failed to parse color curve JSON. Expected: {\"r\": [{\"time\": 0, \"value\": 1}, ...], \"g\": [...], \"b\": [...], \"a\": [...]}"));
			}

			auto PopulateChannel = [&](const FString& ChannelName, FRichCurve& OutCurve)
			{
				OutCurve.Reset();
				const TArray<TSharedPtr<FJsonValue>>* ChannelArray = nullptr;
				if (ColorObj->TryGetArrayField(ChannelName, ChannelArray))
				{
					for (const TSharedPtr<FJsonValue>& KeyVal : *ChannelArray)
					{
						const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
						if (KeyObj.IsValid())
						{
							double Time = KeyObj->GetNumberField(TEXT("time"));
							double Val = KeyObj->GetNumberField(TEXT("value"));
							OutCurve.AddKey(static_cast<float>(Time), static_cast<float>(Val));
						}
					}
				}
			};

			if (UNiagaraDataInterfaceColorCurve* ExistingColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(ExistingDI))
			{
				ExistingColorCurve->Modify();
				PopulateChannel(TEXT("r"), ExistingColorCurve->RedCurve);
				PopulateChannel(TEXT("g"), ExistingColorCurve->GreenCurve);
				PopulateChannel(TEXT("b"), ExistingColorCurve->BlueCurve);
				PopulateChannel(TEXT("a"), ExistingColorCurve->AlphaCurve);
			}
			else
			{
				if (OverridePin.LinkedTo.Num() > 0)
				{
					OverridePin.BreakAllPinLinks();
				}
				UNiagaraDataInterface* OutDI = nullptr;
				FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverridePin, UNiagaraDataInterfaceColorCurve::StaticClass(), InputName, OutDI, ScriptVarId);
				if (UNiagaraDataInterfaceColorCurve* TargetColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(OutDI))
				{
					PopulateChannel(TEXT("r"), TargetColorCurve->RedCurve);
					PopulateChannel(TEXT("g"), TargetColorCurve->GreenCurve);
					PopulateChannel(TEXT("b"), TargetColorCurve->BlueCurve);
					PopulateChannel(TEXT("a"), TargetColorCurve->AlphaCurve);
				}
			}
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Data interface type '%s' is not supported. For float curves, use JSON array: [{\"time\": 0, \"value\": 1}, ...]. For color curves, use JSON object with r/g/b/a arrays."),
				*TypeDef.GetName()));
		}

		System->MarkPackageDirty();

		Data->LastOperationStatus = FString::Printf(TEXT("set_module_input -> Set curve '%s' on module '%s'"),
			*InputName, *ModuleNode->GetFunctionName());
		return BuildStateResponse(SessionId, Data);
	}

	// Standard (non-DI) input: set pin default value
	UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*ModuleNode, AliasedHandle, TypeDef, ScriptVarId, FGuid());
	Pin.Modify();
	Pin.DefaultValue = Value;

	if (UNiagaraNode* OwningNode = Cast<UNiagaraNode>(Pin.GetOwningNode()))
	{
		OwningNode->MarkNodeRequiresSynchronization(TEXT("MCP set_module_input"), true);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_module_input -> Set '%s' = '%s' on module '%s'"),
		*InputName, *Value, *ModuleNode->GetFunctionName());
	return BuildStateResponse(SessionId, Data);
}
