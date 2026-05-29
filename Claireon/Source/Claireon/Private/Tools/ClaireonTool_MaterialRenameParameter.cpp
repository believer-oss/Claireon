// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialRenameParameter.h"
#include "Tools/ClaireonMaterialHelpers.h"

#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "ScopedTransaction.h"

FString ClaireonTool_MaterialRenameParameter::GetCategory() const  { return TEXT("material"); }
FString ClaireonTool_MaterialRenameParameter::GetOperation() const { return TEXT("rename_parameter"); }

FString ClaireonTool_MaterialRenameParameter::GetDescription() const
{
	// Walks Material->GetExpressions(), finds the expression whose ParameterName
	// matches old_name (case-insensitive), and rewrites it. The change is applied
	// inside a transaction; the material is recompiled by the caller (or via a
	// follow-up material_compile call).
	return TEXT("Rename a parameter expression on a UMaterial. Walks expressions, "
				"finds the one whose ParameterName matches `old_name` (case-insensitive), "
				"and rewrites it to `new_name`. Wraps the change in an Undo transaction "
				"and marks the package dirty. Caller should follow up with material_compile + "
				"asset save. Refuses to run while PIE is active.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialRenameParameter::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	auto MkProp = [](const TCHAR* Type, const TCHAR* Desc) {
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	};

	Properties->SetObjectField(TEXT("asset_path"), MkProp(TEXT("string"), TEXT("Unreal asset path to the UMaterial. Alias: `path`.")));
	Properties->SetObjectField(TEXT("path"),       MkProp(TEXT("string"), TEXT("Alias for asset_path.")));
	Properties->SetObjectField(TEXT("old_name"),   MkProp(TEXT("string"), TEXT("Current parameter name on a Scalar/Vector/Texture/StaticSwitch/RVT/StaticComponentMask expression.")));
	Properties->SetObjectField(TEXT("new_name"),   MkProp(TEXT("string"), TEXT("New parameter name.")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("old_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("new_name")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MaterialRenameParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}
	FString AssetPath, OldName, NewName;
	Arguments->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path (or path)"));
	}
	if (!Arguments->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: old_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	FString LoadError;
	UMaterial* Material = ClaireonMaterialHelpers::LoadMaterialAsset(AssetPath, LoadError);
	if (!Material)
	{
		return MakeErrorResult(LoadError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Material Parameter")));

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();
	UMaterialExpression* Hit = nullptr;
	int32 HitIndex = INDEX_NONE;
	for (int32 i = 0; i < Expressions.Num(); ++i)
	{
		UMaterialExpression* Expr = Expressions[i];
		if (!Expr || !Expr->HasAParameterName())
		{
			continue;
		}
		if (Expr->GetParameterName().ToString().Equals(OldName, ESearchCase::IgnoreCase))
		{
			Hit = Expr;
			HitIndex = i;
			break;
		}
	}

	if (!Hit)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("No parameter expression named '%s' on %s"), *OldName, *Material->GetName()));
	}

	// Refuse no-op renames (avoids spurious package-dirty).
	if (Hit->GetParameterName().ToString().Equals(NewName, ESearchCase::CaseSensitive))
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("Parameter is already named '%s'; nothing to do"), *NewName));
	}

	Hit->Modify();
	Hit->SetParameterName(FName(*NewName));
	Material->Modify();
	Material->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("old_name"), OldName);
	Data->SetStringField(TEXT("new_name"), NewName);
	Data->SetNumberField(TEXT("expression_index"), HitIndex);

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("Renamed parameter #%d '%s' -> '%s' on %s"),
		HitIndex, *OldName, *NewName, *Material->GetName()));
}
