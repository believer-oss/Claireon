// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetParent.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialEditingLibrary.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetParent::GetOperation() const { return TEXT("instance_set_parent"); }

FString ClaireonMaterialInstanceTool_SetParent::GetDescription() const
{
	return TEXT("Reparent a UMaterialInstanceConstant to a new UMaterialInterface. Rejects cycles.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetParent::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parent_path"), TEXT("Path to the new parent UMaterialInterface."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetParent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParentPath;
	if (!Arguments->TryGetStringField(TEXT("parent_path"), ParentPath) || ParentPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parent_path"));
	}

	FSoftObjectPath SoftPath(ParentPath);
	UMaterialInterface* NewParent = Cast<UMaterialInterface>(SoftPath.TryLoad());
	if (!NewParent)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load parent material '%s'"), *ParentPath));
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();

	// Cycle detection
	{
		UMaterialInterface* Cursor = NewParent;
		int32 Safety = 0;
		while (Cursor && Safety++ < 64)
		{
			if (Cursor == Instance)
			{
				return MakeErrorResult(TEXT("Setting parent would create a cycle"));
			}
			if (UMaterialInstance* AsInstance = Cast<UMaterialInstance>(Cursor))
			{
				Cursor = AsInstance->Parent;
			}
			else
			{
				break;
			}
		}
	}

	Instance->Modify();
	Instance->SetParentEditorOnly(NewParent);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set parent to %s"), *ParentPath);
	return BuildStateResponse(SessionId, Data);
}
