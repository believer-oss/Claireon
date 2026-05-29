// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_ListNodeTypes.h"
#include "Tools/FToolSchemaBuilder.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "UObject/UObjectIterator.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_ListNodeTypes::GetOperation() const { return TEXT("list_node_types"); }

FString ClaireonBehaviorTreeTool_ListNodeTypes::GetDescription() const
{
	return TEXT("List available Behavior Tree node classes (composite/task/decorator/service) by "
				"iterating UClass reflection. Read-only, stateless reflection query; no session_id "
				"required and no editor session is opened. Optional category filter narrows the result "
				"to a single subclass family.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_ListNodeTypes::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("category"), TEXT("Optional category filter (composite, task, decorator, service). Empty = all."));
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_ListNodeTypes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Category;
	Arguments->TryGetStringField(TEXT("category"), Category);

	FString Output;

	auto ListSubclasses = [&Output](UClass* BaseClass, const FString& Label)
	{
		Output += FString::Printf(TEXT("=== %s ===\n"), *Label);
		TArray<FString> ClassNames;

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(BaseClass) && !Class->HasAnyClassFlags(CLASS_Abstract) && Class != BaseClass)
			{
				ClassNames.Add(Class->GetName());
			}
		}

		ClassNames.Sort();
		for (const FString& Name : ClassNames)
		{
			Output += FString::Printf(TEXT("  %s\n"), *Name);
		}
		Output += FString::Printf(TEXT("Total: %d\n\n"), ClassNames.Num());
	};

	if (Category.IsEmpty() || Category == TEXT("composite"))
	{
		ListSubclasses(UBTCompositeNode::StaticClass(), TEXT("Composite Nodes"));
	}
	if (Category.IsEmpty() || Category == TEXT("task"))
	{
		ListSubclasses(UBTTaskNode::StaticClass(), TEXT("Task Nodes"));
	}
	if (Category.IsEmpty() || Category == TEXT("decorator"))
	{
		ListSubclasses(UBTDecorator::StaticClass(), TEXT("Decorators"));
	}
	if (Category.IsEmpty() || Category == TEXT("service"))
	{
		ListSubclasses(UBTService::StaticClass(), TEXT("Services"));
	}

	if (Output.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown category: %s. Use 'composite', 'task', 'decorator', or 'service'."), *Category));
	}

	TSharedPtr<FJsonObject> NodeTypesData = MakeShared<FJsonObject>();
	NodeTypesData->SetStringField(TEXT("node_types"), Output);
	NodeTypesData->SetStringField(TEXT("category"), Category.IsEmpty() ? TEXT("all") : Category);
	return MakeSuccessResult(NodeTypesData, FString::Printf(TEXT("Node types for category: %s"), Category.IsEmpty() ? TEXT("all") : *Category));
}
