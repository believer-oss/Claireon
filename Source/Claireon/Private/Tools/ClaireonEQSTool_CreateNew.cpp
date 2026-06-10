// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_CreateNew.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_CreateNew::GetOperation() const { return TEXT("create_new"); }

FString ClaireonEQSTool_CreateNew::GetDescription() const
{
	return TEXT("Create a new EQS Query asset at the given asset_path and open an editing session on it.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_CreateNew::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path where the new EQS Query asset will be created. Must not already exist."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_CreateNew::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Check that asset does not already exist
	FSoftObjectPath SoftPath(AssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *AssetPath));
	}

	// Create the package and asset. Use the package-prefix form from the resolver so
	// FPackageName::GetShortName returns just the leaf asset name (not the
	// object-path form with trailing `.AssetName`).
	const FString& PackagePath = ResolveResult.ResolvedPath.PackagePath;
	FString AssetName = FPackageName::GetShortName(PackagePath);

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
	}

	UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Query)
	{
		return MakeErrorResult(TEXT("Failed to create EQS Query"));
	}

	// Save the new asset
	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	// Register with asset registry
	FAssetRegistryModule::AssetCreated(Query);

	EnsureDelegateRegistered();

	// Open a session for the new asset
	const FString ResolvedAssetPath = Query->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, EQSSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create tool data entry
	FEQSEditToolData NewData;
	NewData.Query = Query;
	NewData.LastOperationStatus = TEXT("Asset created and session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	// Build response
	FString StructureText = ClaireonBehaviorTreeHelpers::FormatEQSStructure(Query, false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("status"), TEXT("Asset created and session opened"));
	ResultJson->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("Created and opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}
