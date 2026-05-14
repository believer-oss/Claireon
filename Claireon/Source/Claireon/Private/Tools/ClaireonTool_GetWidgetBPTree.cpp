// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetWidgetBPTree.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

FString ClaireonTool_GetWidgetBPTree::GetName() const
{
	return TEXT("claireon.widgetbp_get_tree");
}

FString ClaireonTool_GetWidgetBPTree::GetDescription() const
{
	return TEXT("Inspect a Widget Blueprint's widget tree hierarchy by asset_path. Stateless / read-only / non-session: never mutates and requires no open session. Returns widget names, classes, slot properties, and optionally full widget properties, bindings, MVVM bindings, and animations. Use the in-session claireon.widgetbp_get_state when an editing session is already open.");
}

TSharedPtr<FJsonObject> ClaireonTool_GetWidgetBPTree::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Widget Blueprint asset path (e.g., /Game/UI/WBP_HUD)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// depth - optional
	TSharedPtr<FJsonObject> DepthProp = MakeShared<FJsonObject>();
	DepthProp->SetStringField(TEXT("type"), TEXT("integer"));
	DepthProp->SetStringField(TEXT("description"), TEXT("Max depth to traverse (-1 = unlimited, default)"));
	Properties->SetObjectField(TEXT("depth"), DepthProp);

	// include_properties - optional
	TSharedPtr<FJsonObject> IncludePropsProp = MakeShared<FJsonObject>();
	IncludePropsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludePropsProp->SetStringField(TEXT("description"), TEXT("Include widget properties (default: false)"));
	Properties->SetObjectField(TEXT("include_properties"), IncludePropsProp);

	// include_bindings - optional
	TSharedPtr<FJsonObject> IncludeBindingsProp = MakeShared<FJsonObject>();
	IncludeBindingsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeBindingsProp->SetStringField(TEXT("description"), TEXT("Include property bindings (default: false)"));
	Properties->SetObjectField(TEXT("include_bindings"), IncludeBindingsProp);

	// include_animations - optional
	TSharedPtr<FJsonObject> IncludeAnimsProp = MakeShared<FJsonObject>();
	IncludeAnimsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeAnimsProp->SetStringField(TEXT("description"), TEXT("Include animation list (default: false)"));
	Properties->SetObjectField(TEXT("include_animations"), IncludeAnimsProp);

	// include_mvvm_bindings - optional
	TSharedPtr<FJsonObject> IncludeMVVMProp = MakeShared<FJsonObject>();
	IncludeMVVMProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeMVVMProp->SetStringField(TEXT("description"), TEXT("Include MVVM ViewModel contexts and bindings (default: false)"));
	Properties->SetObjectField(TEXT("include_mvvm_bindings"), IncludeMVVMProp);

	// widget_name - optional
	TSharedPtr<FJsonObject> WidgetNameProp = MakeShared<FJsonObject>();
	WidgetNameProp->SetStringField(TEXT("type"), TEXT("string"));
	WidgetNameProp->SetStringField(TEXT("description"), TEXT("Only return subtree rooted at this widget"));
	Properties->SetObjectField(TEXT("widget_name"), WidgetNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

ClaireonTool_GetWidgetBPTree::FToolResult ClaireonTool_GetWidgetBPTree::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!WBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	FWidgetSerializeOptions Options;
	Arguments->TryGetNumberField(TEXT("depth"), Options.MaxDepth);
	Arguments->TryGetBoolField(TEXT("include_properties"), Options.bIncludeProperties);
	Arguments->TryGetBoolField(TEXT("include_bindings"), Options.bIncludeBindings);
	Arguments->TryGetBoolField(TEXT("include_animations"), Options.bIncludeAnimations);
	Arguments->TryGetBoolField(TEXT("include_mvvm_bindings"), Options.bIncludeMVVMBindings);

	FString WidgetNameFilter;
	if (Arguments->TryGetStringField(TEXT("widget_name"), WidgetNameFilter) && !WidgetNameFilter.IsEmpty())
	{
		Options.FilterWidgetName = FName(*WidgetNameFilter);
	}

	TSharedPtr<FJsonObject> TreeObj = ClaireonWidgetHelpers::SerializeWidgetTree(WBP, Options);

	// Count widgets
	int32 WidgetCount = 0;
	FString RootWidgetClass;
	if (TreeObj.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
		if (TreeObj->TryGetArrayField(TEXT("widgets"), WidgetsArray) && WidgetsArray)
		{
			WidgetCount = WidgetsArray->Num();
		}
		TreeObj->TryGetStringField(TEXT("root_class"), RootWidgetClass);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("root_widget"), RootWidgetClass);
	Data->SetNumberField(TEXT("widget_count"), WidgetCount);
	if (TreeObj.IsValid())
	{
		Data->SetObjectField(TEXT("tree"), TreeObj);
	}

	// Extract asset short name for summary
	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const FString Summary = FString::Printf(TEXT("%s: %d widgets (root: %s)"),
		*AssetName, WidgetCount, *RootWidgetClass);

	return MakeSuccessResult(Data, Summary);
}
