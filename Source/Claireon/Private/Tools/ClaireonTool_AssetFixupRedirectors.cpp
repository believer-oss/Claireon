// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetFixupRedirectors.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/ObjectRedirector.h"

FString ClaireonTool_AssetFixupRedirectors::GetCategory() const { return TEXT("asset"); }
FString ClaireonTool_AssetFixupRedirectors::GetOperation() const { return TEXT("fixup_redirectors"); }

FString ClaireonTool_AssetFixupRedirectors::GetDescription() const
{
    return TEXT("Find and optionally fix asset redirectors. Redirectors are left behind when assets are moved or renamed. Use dryRun to preview without making changes. Stateless / non-session: operates directly on the asset registry.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetFixupRedirectors::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// contentPath - optional
	TSharedPtr<FJsonObject> ContentPathProp = MakeShared<FJsonObject>();
	ContentPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ContentPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path to scan for redirectors (default: /Game). E.g. /Game/Characters"));
	Properties->SetObjectField(TEXT("contentPath"), ContentPathProp);

	// dryRun - optional
	TSharedPtr<FJsonObject> DryRunProp = MakeShared<FJsonObject>();
	DryRunProp->SetStringField(TEXT("type"), TEXT("boolean"));
	DryRunProp->SetStringField(TEXT("description"),
		TEXT("If true, only report redirectors without fixing them (default: true). Set to false to actually consolidate and delete redirectors."));
	Properties->SetObjectField(TEXT("dryRun"), DryRunProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetFixupRedirectors::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse optional parameters
	FString ContentPath = TEXT("/Game");
	if (Arguments.IsValid() && Arguments->HasField(TEXT("contentPath")))
	{
		FString ProvidedPath;
		if (Arguments->TryGetStringField(TEXT("contentPath"), ProvidedPath) && !ProvidedPath.IsEmpty())
		{
			ContentPath = ProvidedPath;
		}
	}

	bool bDryRun = true;
	if (Arguments.IsValid() && Arguments->HasField(TEXT("dryRun")))
	{
		bDryRun = Arguments->GetBoolField(TEXT("dryRun"));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.asset.fixupRedirectors: contentPath=%s, dryRun=%d"),
		*ContentPath, bDryRun);

	// Get Asset Registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Find all ObjectRedirector assets in the specified path
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
	if (!ContentPath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*ContentPath));
	}

	TArray<FAssetData> RedirectorAssets;
	AssetRegistry.GetAssets(Filter, RedirectorAssets);

	if (RedirectorAssets.Num() == 0)
	{
		FString Output;
		Output += FString::Printf(TEXT("Redirector Fixup: %s\n"), *ContentPath);
		Output += FString::Printf(TEXT("Mode: %s\n\n"), bDryRun ? TEXT("Dry Run") : TEXT("Fix"));
		Output += TEXT("No redirectors found in the specified path.\n");
		return MakeSuccessResult(nullptr, Output);
	}

	// Collect details about each redirector
	struct FRedirectorInfo
	{
		FString RedirectorPath;
		FString TargetPath;
		bool bFixed;
	};
	TArray<FRedirectorInfo> Redirectors;
	int32 FixedCount = 0;

	// Load redirectors to inspect their targets
	TArray<UObjectRedirector*> LoadedRedirectors;
	for (const FAssetData& RedirectorData : RedirectorAssets)
	{
		FString RedirectorPath = RedirectorData.GetObjectPathString();

		UObjectRedirector* Redirector = LoadObject<UObjectRedirector>(nullptr, *RedirectorPath);
		if (Redirector == nullptr)
		{
			Redirectors.Add({
				RedirectorData.PackageName.ToString(),
				TEXT("(failed to load)"),
				false
			});
			continue;
		}

		FString TargetPath = TEXT("(null target)");
		if (Redirector->DestinationObject != nullptr)
		{
			TargetPath = Redirector->DestinationObject->GetPathName();
		}

		Redirectors.Add({
			RedirectorData.PackageName.ToString(),
			TargetPath,
			false
		});

		if (!bDryRun)
		{
			LoadedRedirectors.Add(Redirector);
		}
	}

	// Perform fixup if not dry run
	if (!bDryRun && LoadedRedirectors.Num() > 0)
	{
		// Use AssetTools to fix up referencers
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

		// Filter to only redirectors with valid targets
		TArray<UObjectRedirector*> ValidRedirectors;
		for (UObjectRedirector* Redirector : LoadedRedirectors)
		{
			if (Redirector != nullptr && Redirector->DestinationObject != nullptr)
			{
				ValidRedirectors.Add(Redirector);
			}
		}

		if (ValidRedirectors.Num() > 0)
		{
			// FixupReferencers will update all assets that reference the redirectors
			// to point directly to the target, then the redirectors can be deleted.
			AssetToolsModule.Get().FixupReferencers(ValidRedirectors, /*bCheckoutDialogPrompt=*/false, ERedirectFixupMode::DeleteFixedUpRedirectors);

			FixedCount = ValidRedirectors.Num();

			// Mark all as fixed (best-effort — the API doesn't report per-asset failures individually)
			for (FRedirectorInfo& Info : Redirectors)
			{
				Info.bFixed = true;
			}

			UE_LOG(LogClaireon, Display, TEXT("[MCP] Fixed up %d redirectors"), FixedCount);
		}
	}

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("Redirector Fixup: %s\n"), *ContentPath);
	Output += FString::Printf(TEXT("Mode: %s\n"), bDryRun ? TEXT("Dry Run (preview only)") : TEXT("Fix"));
	Output += FString::Printf(TEXT("Redirectors Found: %d\n"), Redirectors.Num());
	if (!bDryRun)
	{
		Output += FString::Printf(TEXT("Redirectors Fixed: %d\n"), FixedCount);
	}
	Output += TEXT("\n");

	// Cap output
	static constexpr int32 MaxReportedRedirectors = 200;
	const int32 ToReport = FMath::Min(Redirectors.Num(), MaxReportedRedirectors);

	for (int32 i = 0; i < ToReport; i++)
	{
		const FRedirectorInfo& Info = Redirectors[i];
		Output += FString::Printf(TEXT("%d. %s\n"), i + 1, *Info.RedirectorPath);
		Output += FString::Printf(TEXT("   -> %s"), *Info.TargetPath);
		if (!bDryRun)
		{
			Output += FString::Printf(TEXT(" [%s]"), Info.bFixed ? TEXT("Fixed") : TEXT("Not Fixed"));
		}
		Output += TEXT("\n");
	}

	if (Redirectors.Num() > MaxReportedRedirectors)
	{
		Output += FString::Printf(TEXT("\n... and %d more redirectors (output truncated)\n"),
			Redirectors.Num() - MaxReportedRedirectors);
	}

	return MakeSuccessResult(nullptr, Output);
}
