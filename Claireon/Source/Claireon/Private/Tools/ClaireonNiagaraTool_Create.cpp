// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonNiagaraTool_Create::GetDescription() const
{
	return TEXT("Create a new Niagara System asset and open an edit session.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Destination object path (e.g. /Game/FX/NS_Test)."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires 'asset_path'"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(AssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *AssetPath));
	}

	const FString& PackagePath = ResolveResult.ResolvedPath.PackagePath;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
	}

	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, *FPackageName::GetShortName(PackagePath), RF_Public | RF_Standalone);
	if (!System)
	{
		return MakeErrorResult(TEXT("Failed to create Niagara System"));
	}

	UNiagaraSystemFactoryNew::InitializeSystem(System, true);

	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (!ClaireonSafeExec::DidLastExecutionCrash())
	{
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	}

	FAssetRegistryModule::AssetCreated(System);

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = System->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, NiagaraSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FNiagaraEditToolData NewData;
	NewData.System = System;
	NewData.LastOperationStatus = TEXT("System created and session opened");
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	FNiagaraEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
