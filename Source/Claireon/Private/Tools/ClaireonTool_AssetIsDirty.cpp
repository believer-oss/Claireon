// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetIsDirty.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_AssetIsDirty::GetCategory() const  { return TEXT("asset"); }
FString ClaireonTool_AssetIsDirty::GetOperation() const { return TEXT("is_dirty"); }

FString ClaireonTool_AssetIsDirty::GetDescription() const
{
	// Reads UPackage::IsDirty for the asset's package. Stateless.
	return TEXT("Return whether the asset's package has unsaved in-memory edits. "
				"Reads UPackage::IsDirty. Returns false if the package is not loaded. "
				"Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetIsDirty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path (e.g. /Game/Foo/Bar)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetIsDirty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(AssetPath);
	if (!R.bSuccess)
	{
		return MakeErrorResult(R.Error);
	}

	FString PackageNameStr = R.ResolvedPath.Path;
	int32 DotIndex = INDEX_NONE;
	if (PackageNameStr.FindChar(TEXT('.'), DotIndex))
	{
		PackageNameStr = PackageNameStr.Left(DotIndex);
	}

	UPackage* Package = FindPackage(nullptr, *PackageNameStr);
	const bool bExists = Package != nullptr;
	const bool bDirty = bExists && Package->IsDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetBoolField(TEXT("dirty"), bDirty);
	Data->SetBoolField(TEXT("package_loaded"), bExists);

	const FString Summary = bExists
		? FString::Printf(TEXT("%s: %s"), *PackageNameStr, bDirty ? TEXT("DIRTY") : TEXT("clean"))
		: FString::Printf(TEXT("%s: not loaded (treating as clean)"), *PackageNameStr);
	return MakeSuccessResult(Data, Summary);
}
