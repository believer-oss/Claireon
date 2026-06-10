// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EnumInspect.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "Engine/UserDefinedEnum.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_EnumInspect::GetCategory() const { return TEXT("enum"); }
FString ClaireonTool_EnumInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_EnumInspect::GetDescription() const
{
	return TEXT("Inspect a UEnum or UUserDefinedEnum and return its full entry table: "
		"per-entry ordinal, raw name, display name, tooltip, and deprecated/hidden flags. "
		"Resolves UDE display names that are not visible through chooser/proxy inspect "
		"(those return raw 'NewEnumeratorN' names for UDEs).");
}

TSharedPtr<FJsonObject> ClaireonTool_EnumInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"),
		TEXT("Enum identifier: full /Script/Module.EnumName, /Game/... package path, or fuzzy enum name (e.g. 'EFSLocoGait')."),
		true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_EnumInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Try direct path load first (handles /Game/... UDE paths and /Script/... native enums).
	UEnum* Enum = nullptr;
	FString ResolutionNote;

	{
		ClaireonPathResolver::FResolveResult PathResolve = ClaireonPathResolver::Resolve(AssetPath);
		if (PathResolve.bSuccess)
		{
			Enum = FindObject<UEnum>(nullptr, *PathResolve.ResolvedPath.Path);
			if (!Enum)
			{
				Enum = LoadObject<UEnum>(nullptr, *PathResolve.ResolvedPath.Path);
			}
		}
	}

	// Fallback to fuzzy name resolution (E-prefix handling, module paths, etc.).
	if (!Enum)
	{
		ClaireonNameResolver::FNameResolveResult NameResolve;
		Enum = ClaireonNameResolver::ResolveEnumName(AssetPath, NameResolve);
		if (!Enum)
		{
			FString Err = NameResolve.Error.IsEmpty()
				? FString::Printf(TEXT("Could not resolve enum '%s'"), *AssetPath)
				: NameResolve.Error;
			if (NameResolve.Candidates.Num() > 0)
			{
				Err += TEXT(" Candidates: ");
				Err += FString::Join(NameResolve.Candidates, TEXT(", "));
			}
			return MakeErrorResult(Err);
		}
		ResolutionNote = NameResolve.ResolutionNote;
	}

	const UUserDefinedEnum* UDE = Cast<UUserDefinedEnum>(Enum);
	const bool bIsUserDefined = (UDE != nullptr);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Enum->GetPathName());
	Data->SetStringField(TEXT("asset_name"), Enum->GetName());
	Data->SetStringField(TEXT("enum_class"), bIsUserDefined ? TEXT("UUserDefinedEnum") : TEXT("UEnum"));
	Data->SetBoolField(TEXT("is_user_defined"), bIsUserDefined);
	if (!ResolutionNote.IsEmpty())
	{
		Data->SetStringField(TEXT("resolution_note"), ResolutionNote);
	}

	const int32 NumDeclared = Enum->NumEnums();
	// NumEnums() includes the trailing _MAX entry by convention; entry_count
	// reports the user-meaningful count. Keep declared_count too in case the
	// caller cares about the raw value (e.g. for bitmask sizing).
	Data->SetNumberField(TEXT("entry_count"), NumDeclared > 0 ? NumDeclared - 1 : 0);
	Data->SetNumberField(TEXT("declared_count"), NumDeclared);

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(NumDeclared);
	for (int32 i = 0; i < NumDeclared; ++i)
	{
		const int64 Value = Enum->GetValueByIndex(i);
		const FString RawName = Enum->GetNameStringByIndex(i);
		const FText DisplayText = Enum->GetDisplayNameTextByIndex(i);

		const bool bIsMax = (i == NumDeclared - 1) && RawName.EndsWith(TEXT("_MAX"));

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("ordinal"), i);
		Entry->SetNumberField(TEXT("value"), static_cast<double>(Value));
		Entry->SetStringField(TEXT("raw_name"), RawName);
		Entry->SetStringField(TEXT("display_name"), DisplayText.ToString());
#if WITH_EDITORONLY_DATA
		const FString Tooltip = Enum->GetToolTipTextByIndex(i).ToString();
		if (!Tooltip.IsEmpty())
		{
			Entry->SetStringField(TEXT("tooltip"), Tooltip);
		}
#endif
		Entry->SetBoolField(TEXT("hidden"), Enum->HasMetaData(TEXT("Hidden"), i));
		Entry->SetBoolField(TEXT("deprecated"), Enum->HasMetaData(TEXT("DeprecatedProperty"), i));
		Entry->SetBoolField(TEXT("is_max_sentinel"), bIsMax);

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("entries"), Entries);

	const FString Summary = FString::Printf(TEXT("Enum '%s' (%s, %d entries)"),
		*Enum->GetName(),
		bIsUserDefined ? TEXT("UDE") : TEXT("native"),
		NumDeclared > 0 ? NumDeclared - 1 : 0);

	return MakeSuccessResult(Data, Summary);
}
