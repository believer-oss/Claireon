// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "ClaireonNameResolver.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Create::GetCategory() const { return TEXT("statetree"); }
FString ClaireonStateTreeTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonStateTreeTool_Create::GetDescription() const
{
	return TEXT("Create a new UStateTree asset at asset_path with the supplied schema class. Stateless / non-session: writes the new .uasset and returns {asset_path, schema_class, schema_class_path}. UStateTreeFactory::StateTreeSchemaClass is not EditAnywhere, so this tool is the only Python-reachable path for non-default schemas. Follow with statetree_open to begin editing.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Destination asset path for the new State Tree (e.g. /Game/AI/ST_NewTree). Must not already exist."), true);
	Builder.AddString(TEXT("schema_class_path"), TEXT("Object path or class name of a UStateTreeSchema subclass (e.g. /Script/StateTreeModule.StateTreeSchema or a project-specific schema)."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString SchemaClassPath;
	if (!Arguments->TryGetStringField(TEXT("schema_class_path"), SchemaClassPath) || SchemaClassPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: schema_class_path"));
	}

	// Resolve asset_path to canonical form.
	const ClaireonPathResolver::FResolveResult ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	const FString ResolvedAssetPath = ResolveResult.ResolvedPath.Path;
	const FString PackagePath = ResolveResult.ResolvedPath.PackagePath;

	// Refuse if an asset already exists at the resolved path.
	FSoftObjectPath SoftPath(ResolvedAssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *ResolvedAssetPath));
	}

	// Resolve schema_class_path -- must be a non-abstract subclass of UStateTreeSchema.
	ClaireonNameResolver::FNameResolveResult ClassResolve;
	UClass* SchemaClass = ClaireonNameResolver::ResolveClassName(SchemaClassPath, UStateTreeSchema::StaticClass(), ClassResolve);
	if (!SchemaClass)
	{
		FString ErrText = FString::Printf(TEXT("Schema class '%s' not found or is not a subclass of UStateTreeSchema."), *SchemaClassPath);
		if (!ClassResolve.Error.IsEmpty())
		{
			ErrText += TEXT(" ");
			ErrText += ClassResolve.Error;
		}
		return MakeErrorResult(ErrText);
	}
	if (SchemaClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		return MakeErrorResult(FString::Printf(TEXT("Schema class '%s' is abstract / deprecated and cannot be instantiated."), *SchemaClass->GetPathName()));
	}

	// Asset name = leaf of PackagePath.
	const FString AssetName = FPackageName::GetShortName(PackagePath);

	// Configure factory before invoking AssetTools.CreateAsset -- this bypasses the
	// missing EditAnywhere restriction on StateTreeSchemaClass (the restriction only
	// affects the reflection / set_editor_property path; direct C++ writes via the
	// public setter work fine).
	UStateTreeFactory* Factory = NewObject<UStateTreeFactory>();
	if (!Factory)
	{
		return MakeErrorResult(TEXT("Failed to construct UStateTreeFactory."));
	}
	Factory->SetSchemaClass(SchemaClass);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UStateTree::StaticClass(), Factory);
	UStateTree* NewStateTree = Cast<UStateTree>(NewAsset);
	if (!NewStateTree)
	{
		return MakeErrorResult(FString::Printf(TEXT("AssetTools.CreateAsset returned null for State Tree '%s' (schema '%s'). Verify the schema class is valid and the package path is writable."), *ResolvedAssetPath, *SchemaClass->GetPathName()));
	}

	// Confirm the resulting asset has the requested schema -- belt-and-braces in case
	// the factory silently fell back. UStateTreeFactory::FactoryCreateNew is the only
	// path to populate EditorData->Schema, so if SchemaClass mismatches we report it.
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(NewStateTree->EditorData);
	UClass* ActualSchemaClass = (EditorData && EditorData->Schema) ? EditorData->Schema->GetClass() : nullptr;
	if (!ActualSchemaClass || !ActualSchemaClass->IsChildOf(SchemaClass))
	{
		const FString ActualName = ActualSchemaClass ? ActualSchemaClass->GetPathName() : TEXT("<null>");
		return MakeErrorResult(FString::Printf(TEXT("Created State Tree '%s' but EditorData->Schema is '%s', not the requested '%s'."), *NewStateTree->GetPathName(), *ActualName, *SchemaClass->GetPathName()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewStateTree->GetPathName());
	Data->SetStringField(TEXT("schema_class"), ActualSchemaClass->GetName());
	Data->SetStringField(TEXT("schema_class_path"), ActualSchemaClass->GetPathName());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Created State Tree '%s' with schema '%s'."), *NewStateTree->GetName(), *ActualSchemaClass->GetName()));
}
