// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintGetGeneratedClass.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_BlueprintGetGeneratedClass::GetCategory() const { return kBPCategory; }
FString ClaireonTool_BlueprintGetGeneratedClass::GetOperation() const { return TEXT("get_generated_class"); }

FString ClaireonTool_BlueprintGetGeneratedClass::GetDescription() const
{
	return TEXT("Read a Blueprint's GeneratedClass path directly. Stateless / read-only. Use instead of unreal.BlueprintEditorLibrary.generated_class to avoid the intermittent SEH 0xC0000005 access-violation that the engine's editor-property path hits after add_variable + compile (O6). Returns {generated_class_path, parent_class_path, is_skeleton_only}.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintGetGeneratedClass::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"),
		TEXT("Object path to the Blueprint asset (e.g. /Game/Blueprints/BP_MyActor)."),
		true);
	return Builder.Build();
}

FToolResult ClaireonTool_BlueprintGetGeneratedClass::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetPath);
	if (!Resolved.bSuccess)
	{
		return MakeErrorResult(Resolved.Error);
	}
	const FString ResolvedPath = Resolved.ResolvedPath.Path;

	// Load via FSoftObjectPath -- the same surface the editor uses; survives the
	// reflection-path crash because we never invoke the access-checked editor
	// property accessor.
	const FSoftObjectPath Soft(ResolvedPath);
	UObject* Loaded = Soft.TryLoad();
	if (!Loaded)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not load asset at '%s'."), *ResolvedPath));
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset at '%s' is not a UBlueprint (got '%s')."), *ResolvedPath, *Loaded->GetClass()->GetName()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());

	// Prefer GeneratedClass; fall back to SkeletonGeneratedClass when the
	// authoritative class has not been generated yet (a fresh Blueprint that
	// has not been compiled since load). Either is preferable to the
	// crash-prone editor-property path.
	const UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
	bool bIsSkeletonOnly = false;
	if (!GeneratedClass)
	{
		GeneratedClass = Blueprint->SkeletonGeneratedClass.Get();
		bIsSkeletonOnly = (GeneratedClass != nullptr);
	}

	if (!GeneratedClass)
	{
		Data->SetField(TEXT("generated_class_path"), MakeShared<FJsonValueNull>());
		Data->SetField(TEXT("parent_class_path"), MakeShared<FJsonValueNull>());
		Data->SetBoolField(TEXT("is_skeleton_only"), false);
		return MakeSuccessResult(Data, FString::Printf(TEXT("Blueprint '%s' has no GeneratedClass (uncompiled or skeleton-only)."), *Blueprint->GetName()));
	}

	Data->SetStringField(TEXT("generated_class_path"), GeneratedClass->GetPathName());
	if (const UClass* Parent = Blueprint->ParentClass.Get())
	{
		Data->SetStringField(TEXT("parent_class_path"), Parent->GetPathName());
	}
	else
	{
		Data->SetField(TEXT("parent_class_path"), MakeShared<FJsonValueNull>());
	}
	Data->SetBoolField(TEXT("is_skeleton_only"), bIsSkeletonOnly);

	return MakeSuccessResult(Data, FString::Printf(TEXT("%s -> %s"), *Blueprint->GetName(), *GeneratedClass->GetName()));
}
