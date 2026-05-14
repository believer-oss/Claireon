// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BehaviorTreeInspect.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonLog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BlackboardData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_BehaviorTreeInspect::GetCategory() const { return TEXT("behaviortree"); }
FString ClaireonTool_BehaviorTreeInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_BehaviorTreeInspect::GetDescription() const
{
	return TEXT("Read the structure of a Behavior Tree asset. Displays the full node hierarchy: "
				"composites, tasks, decorators, services, and blackboard key references. "
				"Use detail_level='summary' for a compact overview or 'full' for complete property details.");
}

TSharedPtr<FJsonObject> ClaireonTool_BehaviorTreeInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the Behavior Tree (e.g. /Game/AI/BT_EnemyBehavior)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for compact overview, 'full' for complete property details (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BehaviorTreeInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	bool bFullDetail = true;
	FString DetailLevel;
	if (Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel))
	{
		bFullDetail = !DetailLevel.Equals(TEXT("summary"), ESearchCase::IgnoreCase);
	}

	FString LoadError;
	UBehaviorTree* BehaviorTree = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(AssetPath, LoadError);
	if (!BehaviorTree)
	{
		return MakeErrorResult(LoadError);
	}

	// Count nodes by walking the tree
	int32 NodeCount = 0;
	if (BehaviorTree->RootNode)
	{
		TFunction<void(const UBTCompositeNode*)> CountNodes;
		CountNodes = [&CountNodes, &NodeCount](const UBTCompositeNode* Composite)
		{
			if (!Composite)
			{
				return;
			}
			++NodeCount; // Count this composite
			for (int32 i = 0; i < Composite->GetChildrenNum(); ++i)
			{
				const FBTCompositeChild& Child = Composite->Children[i];
				if (Child.ChildComposite)
				{
					CountNodes(Child.ChildComposite);
				}
				else if (Child.ChildTask)
				{
					++NodeCount; // Count leaf task
				}
			}
		};
		CountNodes(BehaviorTree->RootNode);
	}

	// Collect blackboard key names
	TArray<TSharedPtr<FJsonValue>> BlackboardKeysArray;
	if (BehaviorTree->BlackboardAsset)
	{
		for (const FBlackboardEntry& Entry : BehaviorTree->BlackboardAsset->Keys)
		{
			BlackboardKeysArray.Add(MakeShared<FJsonValueString>(Entry.EntryName.ToString()));
		}
	}

	// Generate full structure text
	const FString StructureText = ClaireonBehaviorTreeHelpers::FormatBehaviorTreeStructure(BehaviorTree, bFullDetail);
	const FString AssetName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), NodeCount);
	Data->SetArrayField(TEXT("blackboard_keys"), BlackboardKeysArray);
	Data->SetStringField(TEXT("structure"), StructureText);
	if (BehaviorTree->BlackboardAsset)
	{
		Data->SetStringField(TEXT("blackboard_asset"), BehaviorTree->BlackboardAsset->GetPathName());
	}

	const FString Summary = FString::Printf(TEXT("%s: %d nodes"), *AssetName, NodeCount);

	return MakeSuccessResult(Data, Summary);
}
