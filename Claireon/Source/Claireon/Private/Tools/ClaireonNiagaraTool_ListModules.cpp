// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_ListModules.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraScript.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_ListModules::GetOperation() const { return TEXT("list_modules"); }

FString ClaireonNiagaraTool_ListModules::GetDescription() const
{
	return TEXT("List available Niagara module scripts (session-less).");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_ListModules::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("query"), TEXT("Optional substring filter for module name."));
	Builder.AddString(TEXT("stack"), TEXT("Optional stack filter (emitter_spawn/emitter_update/particle_spawn/particle_update/system_spawn/system_update)."));
	Builder.AddInteger(TEXT("max_results"), TEXT("Maximum number of results (default 20)."));
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_ListModules::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Query;
	Arguments->TryGetStringField(TEXT("query"), Query);

	FString Stack;
	Arguments->TryGetStringField(TEXT("stack"), Stack);

	int32 MaxResults = 20;
	Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
	if (MaxResults <= 0)
	{
		MaxResults = 20;
	}

	if (!Stack.IsEmpty())
	{
		ENiagaraScriptUsage StackUsage;
		FString StackError;
		if (!ClaireonNiagaraHelpers::ResolveStackName(Stack, StackUsage, StackError))
		{
			return MakeErrorResult(StackError);
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FAssetData& Asset : AssetList)
	{
		if (ResultArray.Num() >= MaxResults)
		{
			break;
		}

		FString UsageStr;
		bool bIsModule = false;
		if (Asset.GetTagValue(GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage), UsageStr))
		{
			bIsModule = UsageStr == TEXT("Module") || UsageStr == TEXT("ENiagaraScriptUsage::Module")
				|| UsageStr == FString::FromInt(static_cast<int32>(ENiagaraScriptUsage::Module));
		}

		if (!bIsModule)
		{
			continue;
		}

		const FString AssetName = Asset.AssetName.ToString();

		if (!Query.IsEmpty() && !AssetName.Contains(Query, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Available Modules (%d results) ===\n"), ResultArray.Num());
	for (int32 i = 0; i < ResultArray.Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Entry = ResultArray[i]->AsObject();
		Output += FString::Printf(TEXT("  [%d] %s\n       %s\n"),
			i,
			*Entry->GetStringField(TEXT("name")),
			*Entry->GetStringField(TEXT("asset_path")));
	}

	if (ResultArray.Num() == 0)
	{
		Output += TEXT("  (no modules found matching query)\n");
	}

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetArrayField(TEXT("modules"), ResultArray);
	return MakeSuccessResult(RespData, Output);
}
