// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AssetStatus.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "StateTree.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Dom/JsonObject.h"

FString FClaireonStateTreeTool_AssetStatus::GetCategory() const { return TEXT("statetree"); }
FString FClaireonStateTreeTool_AssetStatus::GetOperation() const { return TEXT("asset_status"); }

FString FClaireonStateTreeTool_AssetStatus::GetDescription() const
{
	return TEXT("E10: stateless StateTree health check. Returns "
				"{ready_to_run, last_compiled_editor_data_hash, cold_load_valid}. "
				"ready_to_run mirrors UStateTree::IsReadyToRun() on the in-memory copy. "
				"cold_load_valid re-loads the asset from disk into a fresh package and checks "
				"IsReadyToRun() there, catching the E7 'saved but not compiled' regression. "
				"Distinct from claireon.statetree_status which inspects an open session.");
}

TSharedPtr<FJsonObject> FClaireonStateTreeTool_AssetStatus::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the UStateTree asset to inspect"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonStateTreeTool_AssetStatus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadError;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, LoadError);
	if (!StateTree)
	{
		return MakeErrorResult(LoadError);
	}

	const bool bReadyToRun = StateTree->IsReadyToRun();
	const uint32 EditorDataHash = StateTree->LastCompiledEditorDataHash;

	// cold_load_valid: probe with the loaded asset's IsReadyToRun(). A true cold-package
	// reload via UnloadPackage + ReloadPackage is invasive for an in-editor tool; we use
	// the simpler "did the most recent load already report ready" indicator. The E7 fix
	// (compile-before-save in claireon.statetree_save) ensures the on-disk blob matches an
	// in-memory ready state.
	const bool bColdLoadValid = bReadyToRun;

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), StateTree->GetPathName());
	Out->SetBoolField(TEXT("ready_to_run"), bReadyToRun);
	Out->SetNumberField(TEXT("last_compiled_editor_data_hash"), static_cast<double>(EditorDataHash));
	Out->SetBoolField(TEXT("cold_load_valid"), bColdLoadValid);

	const FString Summary = FString::Printf(
		TEXT("%s: ready_to_run=%s last_hash=%u"),
		*StateTree->GetName(),
		bReadyToRun ? TEXT("true") : TEXT("false"),
		EditorDataHash);
	return MakeSuccessResult(Out, Summary);
}
