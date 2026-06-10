// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_InputInspect.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonLog.h"
#include "InputAction.h"
#include "InputMappingContext.h"

FString ClaireonTool_InputInspect::GetCategory() const { return TEXT("input"); }
FString ClaireonTool_InputInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_InputInspect::GetDescription() const
{
	return TEXT("Read the structure of an Enhanced Input asset (Input Action or Input Mapping Context). "
				"Auto-detects the asset type from the path. Shows value type, triggers, modifiers, "
				"and key mappings.");
}

TSharedPtr<FJsonObject> ClaireonTool_InputInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal asset path to an Input Action or Input Mapping Context (e.g. /Game/Input/Actions/IA_BasicAttack)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"),
		TEXT("Level of detail: 'full' (default) shows all properties, 'summary' shows compact overview."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_InputInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);
	bool bSummaryOnly = (DetailLevel == TEXT("summary"));

	FString LoadError;
	UObject* Asset = ClaireonEnhancedInputHelpers::LoadInputAsset(AssetPath, LoadError);
	if (!Asset)
	{
		return MakeErrorResult(LoadError);
	}

	FString AssetType;
	FString FormattedOutput;

	if (UInputAction* IA = Cast<UInputAction>(Asset))
	{
		AssetType = TEXT("input_action");
		FormattedOutput = ClaireonEnhancedInputHelpers::FormatInputAction(IA, bSummaryOnly);
	}
	else if (UInputMappingContext* IMC = Cast<UInputMappingContext>(Asset))
	{
		AssetType = TEXT("mapping_context");
		FormattedOutput = ClaireonEnhancedInputHelpers::FormatMappingContext(IMC, bSummaryOnly);
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Asset at %s is neither an Input Action nor an Input Mapping Context (type: %s)"),
			*AssetPath, *Asset->GetClass()->GetName()));
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	ResultJson->SetStringField(TEXT("asset_type"), AssetType);
	ResultJson->SetStringField(TEXT("view"), FormattedOutput);

	return MakeSuccessResult(ResultJson,
		FString::Printf(TEXT("Inspected %s: %s"), *AssetType, *Asset->GetName()));
}
