// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeListNodeTypes.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonLog.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorModule.h"
#include "StateTreeNodeClassCache.h"
#include "StateTreeSchema.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeConsiderationBase.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeNodeBase.h"
#include "Modules/ModuleManager.h"

FString ClaireonTool_StateTreeListNodeTypes::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeListNodeTypes::GetOperation() const { return TEXT("list_node_types"); }

FString ClaireonTool_StateTreeListNodeTypes::GetDescription() const
{
	return TEXT("Enumerate available State Tree node types and their property schemas. Stateless / read-only / non-session: never mutates and requires no open session. Lists tasks, conditions, evaluators, considerations, and property functions. Optionally filter by category, name substring, or schema compatibility with a target asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeListNodeTypes::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// category - required
	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"), TEXT("Node category to list"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("task")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("tasks")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("condition")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("conditions")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("evaluator")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("evaluators")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("consideration")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("considerations")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("property_function")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("property_functions")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("all")));
		CategoryProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	// filter - optional
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"), TEXT("Optional substring filter to match against node type names"));
	Properties->SetObjectField(TEXT("filter"), FilterProp);

	// asset_path - optional
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Optional State Tree asset path to filter node types by schema compatibility"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("category")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace
{
	struct FCategoryInfo
	{
		FString Name;
		const UScriptStruct* BaseStruct;
	};

	FString FormatNodeTypeProperties(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return TEXT("");
		}

		FString Result;
		int32 Count = 0;

		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (Count >= 15)
			{
				Result += TEXT("\n    ...");
				break;
			}

			const FProperty* Prop = *It;
			FString TypeStr = Prop->GetCPPType();

			// Check for Input/Output metadata
			FString MetaStr;
			if (Prop->HasMetaData(TEXT("Input")))
			{
				MetaStr += TEXT(", Input");
			}
			if (Prop->HasMetaData(TEXT("Output")))
			{
				MetaStr += TEXT(", Output");
			}

			Result += FString::Printf(TEXT("\n    %s [%s%s]"), *Prop->GetName(), *TypeStr, *MetaStr);
			Count++;
		}

		return Result;
	}

	FString GetNodeGroup(const FString& StructName)
	{
		// Generic StateTree node grouping. The default returns "Engine" so all
		// authored nodes appear under that bucket; downstream projects can
		// patch this to surface project-specific node families.
		return TEXT("Engine");
	}
} // namespace

IClaireonTool::FToolResult ClaireonTool_StateTreeListNodeTypes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse required parameter
	FString Category;
	if (!Arguments->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: category"));
	}

	// Parse optional parameters
	FString Filter;
	Arguments->TryGetStringField(TEXT("filter"), Filter);

	FString AssetPath;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.statetree.listNodeTypes: category=%s, filter=%s, asset=%s"),
		*Category, *Filter, *AssetPath);

	// Get the StateTree editor module
	FStateTreeEditorModule* EditorModule = FModuleManager::GetModulePtr<FStateTreeEditorModule>("StateTreeEditorModule");
	if (!EditorModule)
	{
		return MakeErrorResult(TEXT("StateTreeEditorModule is not loaded"));
	}

	TSharedPtr<FStateTreeNodeClassCache> ClassCache = EditorModule->GetNodeClassCache();
	if (!ClassCache.IsValid())
	{
		return MakeErrorResult(TEXT("StateTree node class cache is not available (editor may not be fully initialized)"));
	}

	// Optionally load schema for filtering
	const UStateTreeSchema* Schema = nullptr;
	if (!AssetPath.IsEmpty())
	{
		FString Error;
		UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, Error);
		if (StateTree)
		{
			Schema = StateTree->GetSchema();
		}
	}

	// Build category list
	TArray<FCategoryInfo> Categories;
	if (Category == TEXT("task") || Category == TEXT("tasks") || Category == TEXT("all"))
	{
		Categories.Add({ TEXT("Tasks"), FStateTreeTaskBase::StaticStruct() });
	}
	if (Category == TEXT("condition") || Category == TEXT("conditions") || Category == TEXT("all"))
	{
		Categories.Add({ TEXT("Conditions"), FStateTreeConditionBase::StaticStruct() });
	}
	if (Category == TEXT("evaluator") || Category == TEXT("evaluators") || Category == TEXT("all"))
	{
		Categories.Add({ TEXT("Evaluators"), FStateTreeEvaluatorBase::StaticStruct() });
	}
	if (Category == TEXT("consideration") || Category == TEXT("considerations") || Category == TEXT("all"))
	{
		Categories.Add({ TEXT("Considerations"), FStateTreeConsiderationBase::StaticStruct() });
	}
	if (Category == TEXT("property_function") || Category == TEXT("property_functions") || Category == TEXT("all"))
	{
		Categories.Add({ TEXT("Property Functions"), FStateTreePropertyFunctionBase::StaticStruct() });
	}

	if (Categories.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown category: %s. Valid: task|tasks, condition|conditions, evaluator|evaluators, consideration|considerations, property_function|property_functions, all"), *Category));
	}

	FString Output;

	for (const FCategoryInfo& CatInfo : Categories)
	{
		TArray<TSharedPtr<FStateTreeNodeClassData>> AvailableClasses;
		ClassCache->GetStructs(CatInfo.BaseStruct, AvailableClasses);

		// Group by origin
		TMap<FString, TArray<TSharedPtr<FStateTreeNodeClassData>>> GroupedClasses;
		int32 TotalCount = 0;

		for (const TSharedPtr<FStateTreeNodeClassData>& ClassData : AvailableClasses)
		{
			if (!ClassData.IsValid())
			{
				continue;
			}

			UScriptStruct* NodeStruct = ClassData->GetScriptStruct(false);
			if (!NodeStruct)
			{
				continue;
			}

			FString StructName = NodeStruct->GetName();

			// Filter by name substring
			if (!Filter.IsEmpty() && !StructName.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			// Filter by schema compatibility
			if (Schema && !Schema->IsStructAllowed(NodeStruct))
			{
				continue;
			}

			// Skip abstract structs
			if (NodeStruct->HasMetaData(TEXT("Abstract")))
			{
				continue;
			}

			FString Group = GetNodeGroup(StructName);
			GroupedClasses.FindOrAdd(Group).Add(ClassData);
			TotalCount++;
		}

		FString SchemaStr = Schema ? FString::Printf(TEXT(" (filtered by schema: %s)"), *Schema->GetClass()->GetName()) : TEXT("");
		Output += FString::Printf(TEXT("=== Available %s%s ===\n"), *CatInfo.Name, *SchemaStr);

		// Output in a consistent group order
		TArray<FString> GroupOrder = { TEXT("Engine"), TEXT("MyGame"), TEXT("MyGame Sample"), TEXT("MyGame Mob Sample") };
		for (const FString& GroupName : GroupOrder)
		{
			const TArray<TSharedPtr<FStateTreeNodeClassData>>* GroupClasses = GroupedClasses.Find(GroupName);
			if (!GroupClasses || GroupClasses->Num() == 0)
			{
				continue;
			}

			Output += FString::Printf(TEXT("\n--- %s ---\n"), *GroupName);
			for (const TSharedPtr<FStateTreeNodeClassData>& ClassData : *GroupClasses)
			{
				UScriptStruct* NodeStruct = ClassData->GetScriptStruct(false);
				if (!NodeStruct)
					continue;

				Output += FString::Printf(TEXT("\n%s"), *NodeStruct->GetName());

				// Show properties (own properties, not inherited)
				FString Props = FormatNodeTypeProperties(NodeStruct);
				if (!Props.IsEmpty())
				{
					Output += FString::Printf(TEXT("\n  Properties:%s"), *Props);
				}

				// Show instance data type if available
				FStateTreeNodeBase* DefaultNode = reinterpret_cast<FStateTreeNodeBase*>(FMemory::Malloc(NodeStruct->GetStructureSize()));
				NodeStruct->InitializeStruct(DefaultNode);
				const UStruct* InstanceType = DefaultNode->GetInstanceDataType();
				if (InstanceType)
				{
					Output += FString::Printf(TEXT("\n  InstanceData: %s"), *InstanceType->GetName());
					if (const UScriptStruct* InstanceStruct = Cast<const UScriptStruct>(InstanceType))
					{
						FString InstanceProps = FormatNodeTypeProperties(InstanceStruct);
						if (!InstanceProps.IsEmpty())
						{
							Output += InstanceProps;
						}
					}
				}
				NodeStruct->DestroyStruct(DefaultNode);
				FMemory::Free(DefaultNode);

				Output += TEXT("\n");
			}
		}

		Output += FString::Printf(TEXT("\n(%d total %s available)\n\n"), TotalCount, *CatInfo.Name.ToLower());
	}

	return MakeSuccessResult(nullptr, Output);
}
