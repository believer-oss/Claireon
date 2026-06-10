// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetFindActorsByLabel.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"

FString ClaireonTool_AssetFindActorsByLabel::GetCategory() const  { return TEXT("asset"); }
FString ClaireonTool_AssetFindActorsByLabel::GetOperation() const { return TEXT("find_actors_by_label"); }

FString ClaireonTool_AssetFindActorsByLabel::GetDescription() const
{
	// Asset Registry's tag indexing for external-actor descriptors does not
	// reliably populate ActorLabel; explicit query against the FAssetData.TagsAndValues
	// map is required (the value is stored if at all, but not exposed as a primary key).
	return TEXT("Find external-actor descriptors whose AActor::GetActorLabel() matches the "
				"given label. Iterates Asset Registry entries for the /Game/__ExternalActors__/ "
				"prefix (or a custom prefix) and reads the ActorLabel tag value. "
				"Returns [{path, class, actor_label}, ...]. Stateless / read-only.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetFindActorsByLabel::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	auto MkProp = [](const TCHAR* Type, const TCHAR* Desc) {
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	};

	Properties->SetObjectField(TEXT("label"),  MkProp(TEXT("string"),  TEXT("Actor label to match (case-insensitive; exact match unless contains=true).")));
	Properties->SetObjectField(TEXT("contains"), MkProp(TEXT("boolean"), TEXT("If true, match by substring instead of exact equality. Default false.")));
	Properties->SetObjectField(TEXT("prefix"), MkProp(TEXT("string"),  TEXT("Optional content prefix to scan (default /Game). External-actor descriptors live under /Game/__ExternalActors__/<MapName>/.")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("label")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetFindActorsByLabel::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Label, Prefix = TEXT("/Game");
	bool bContains = false;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: label"));
	}
	Arguments->TryGetBoolField(TEXT("contains"), bContains);
	Arguments->TryGetStringField(TEXT("prefix"), Prefix);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.PackagePaths.Add(*Prefix);
	Filter.bRecursivePaths = true;
	// We only want actor-derived descriptors. Restrict to AActor + descendants.
	Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Actor")));
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Hits;
	AR.GetAssets(Filter, Hits);

	const FString LabelLower = Label.ToLower();

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	for (const FAssetData& AD : Hits)
	{
		FString ActorLabel;
		// FAssetData stores AActor::ActorLabel under tag "ActorLabel" (see UE_ENGINE_PROFILE_ACTOR_LABEL_TAG).
		if (!AD.GetTagValue(FName(TEXT("ActorLabel")), ActorLabel) || ActorLabel.IsEmpty())
		{
			continue;
		}
		const FString ActorLabelLower = ActorLabel.ToLower();
		const bool bMatch = bContains
			? ActorLabelLower.Contains(LabelLower)
			: (ActorLabelLower == LabelLower);
		if (!bMatch)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), AD.GetObjectPathString());
		Entry->SetStringField(TEXT("class"), AD.AssetClassPath.GetAssetName().ToString());
		Entry->SetStringField(TEXT("actor_label"), ActorLabel);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("label"), Label);
	Data->SetStringField(TEXT("prefix"), Prefix);
	Data->SetBoolField(TEXT("contains"), bContains);
	Data->SetArrayField(TEXT("matches"), ResultsArr);
	Data->SetNumberField(TEXT("scanned"), Hits.Num());

	const FString Summary = FString::Printf(TEXT("%d actor(s) with label %s '%s' (scanned %d)"),
		ResultsArr.Num(),
		bContains ? TEXT("containing") : TEXT("=="),
		*Label,
		Hits.Num());
	return MakeSuccessResult(Data, Summary);
}
