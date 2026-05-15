// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_RemoveFoliageType.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_RemoveFoliageType::GetName() const
{
	return TEXT("claireon.foliage_remove_foliage_type");
}

FString ClaireonFoliageTool_RemoveFoliageType::GetDescription() const
{
	return TEXT("Remove a foliage type and all its instances from the session's foliage actor.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_RemoveFoliageType::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("foliage_type"), TEXT("Name or asset path of the foliage type to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_RemoveFoliageType::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if WITH_EDITOR
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString TypeName;
	if (!Arguments->TryGetStringField(TEXT("foliage_type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: foliage_type"));
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UFoliageType* FoundType = FindFoliageTypeInActor(IFA, TypeName);
	if (!FoundType)
	{
		return MakeErrorResult(FString::Printf(TEXT("Foliage type '%s' not found in this level"), *TypeName));
	}

	IFA->RemoveFoliageType(&FoundType, 1);

	Data->LastOperationStatus = FString::Printf(TEXT("Removed foliage type: %s"), *TypeName);
	return BuildStateResponse(SessionId, Data);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}
