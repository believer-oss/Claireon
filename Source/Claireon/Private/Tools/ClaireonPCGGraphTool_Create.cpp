// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "PCGGraph.h"
#include "UObject/Package.h"

FString ClaireonPCGGraphTool_Create::GetCategory() const { return TEXT("pcg"); }
FString ClaireonPCGGraphTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonPCGGraphTool_Create::GetDescription() const
{
	return TEXT("Create a new, empty UPCGGraph asset at the given /Game/ content path. The graph "
				"starts with only its built-in Input and Output nodes. Non-session and immediate; "
				"errors if an asset already exists at that path. Follow with pcg_open to begin "
				"editing, or pass the path straight to pcg_apply_delta / pcg_apply_spec.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new PCG Graph (must start with /Game/)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonPCGGraphTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path (must start with /Game/)"));
	}
	const FString ObjectName = FPackageName::GetShortName(Canon);

	if (UObject* Existing = LoadObject<UObject>(nullptr, *Canon); IsValid(Existing))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s"), *Canon));
	}

	UPackage* Package = CreatePackage(*Canon);
	if (!IsValid(Package))
	{
		return MakeErrorResult(FString::Printf(TEXT("CreatePackage failed for %s"), *Canon));
	}

	// UPCGGraph constructs its own Input/Output nodes; no editor factory is needed.
	UPCGGraph* NewGraph = NewObject<UPCGGraph>(Package, UPCGGraph::StaticClass(), *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!IsValid(NewGraph))
	{
		return MakeErrorResult(TEXT("NewObject<UPCGGraph> failed"));
	}

	FAssetRegistryModule::AssetCreated(NewGraph);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewGraph->GetPathName());
	Data->SetStringField(TEXT("kind"), TEXT("pcg_graph"));
	Data->SetNumberField(TEXT("node_count"), NewGraph->GetNodes().Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Created PCG Graph at %s"), *NewGraph->GetPathName()));
}
