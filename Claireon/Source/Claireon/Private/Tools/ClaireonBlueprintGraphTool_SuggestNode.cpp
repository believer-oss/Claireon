// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SuggestNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_SuggestNode::GetName() const
{
    return TEXT("claireon.blueprint_graph_suggest_node");
}

FString ClaireonBlueprintGraphTool_SuggestNode::GetDescription() const
{
    return TEXT("Suggest Blueprint authoring patterns matching an intent string. Stateless / read-only / non-session: never mutates and requires no open session. Reads BPAuthoringPatterns.json from the Claireon plugin content directory and ranks matches by keyword overlap. Use during planning to discover the right node sequence.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SuggestNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("intent"), TEXT("Intent or C++ expression shape to match against the authoring-pattern catalog."), true);
    Builder.AddInteger(TEXT("top_k"), TEXT("Number of matches to return (default 5, max 20)."));
    return Builder.Build();
}

namespace ClaireonSuggestNode
{
	static const int32 GTopKDefault = 5;
	static const int32 GTopKMax = 20;

	static const TCHAR* const GKeys[] = {
		TEXT("intent"),
		TEXT("cpp_expr_shape"),
		TEXT("bp_node_type"),
		TEXT("function_class"),
		TEXT("function_name"),
		TEXT("macro_library"),
		TEXT("macro_name"),
		TEXT("pin_defaults"),
		TEXT("priority"),
		TEXT("disambiguator"),
		TEXT("translator_ref")
	};

	struct FCatalogEntry
	{
		TSharedPtr<FJsonObject> Raw;  // original entry, all 11 keys preserved
		FString Intent;
		FString IntentLower;
		FString CppExprShape;
		int32 Priority = 0;
		int32 FileOrder = 0;
	};

	struct FCache
	{
		FString ResolvedPath;
		FDateTime LastModified = FDateTime::MinValue();
		TArray<FCatalogEntry> Entries;
		bool bValid = false;
	};

	static FCache& GetCache()
	{
		static FCache Cache;
		return Cache;
	}

	static FString ResolveCatalogPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		return FPaths::Combine(Plugin->GetContentDir(), TEXT("BPAuthoringPatterns.json"));
	}

	static bool LoadCatalog(FString& OutError)
	{
		FCache& Cache = GetCache();
		const FString Path = ResolveCatalogPath();
		if (Path.IsEmpty())
		{
			OutError = TEXT("Could not resolve Claireon plugin content directory");
			return false;
		}

		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("BPAuthoringPatterns.json not found at %s"), *Path);
			Cache.bValid = false;
			Cache.Entries.Empty();
			return false;
		}

		const FDateTime Modified = IFileManager::Get().GetTimeStamp(*Path);
		if (Cache.bValid && Cache.ResolvedPath == Path && Cache.LastModified == Modified)
		{
			return true;
		}

		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to read BPAuthoringPatterns.json at %s"), *Path);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse BPAuthoringPatterns.json: %s"), *Reader->GetErrorMessage());
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
		if (!Root->TryGetArrayField(TEXT("entries"), EntriesJson) || !EntriesJson)
		{
			OutError = TEXT("BPAuthoringPatterns.json missing 'entries' array");
			return false;
		}

		Cache.Entries.Reset(EntriesJson->Num());
		int32 Order = 0;
		for (const TSharedPtr<FJsonValue>& Val : *EntriesJson)
		{
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
			{
				continue;
			}
			FCatalogEntry Entry;
			Entry.Raw = *ObjPtr;
			Entry.FileOrder = Order++;

			FString IntentVal;
			if (Entry.Raw->TryGetStringField(TEXT("intent"), IntentVal))
			{
				Entry.Intent = IntentVal;
				Entry.IntentLower = IntentVal.ToLower();
			}

			FString CppVal;
			if (Entry.Raw->TryGetStringField(TEXT("cpp_expr_shape"), CppVal))
			{
				Entry.CppExprShape = CppVal;
			}

			double PriorityVal = 0.0;
			if (Entry.Raw->TryGetNumberField(TEXT("priority"), PriorityVal))
			{
				Entry.Priority = static_cast<int32>(PriorityVal);
			}

			Cache.Entries.Add(MoveTemp(Entry));
		}

		Cache.ResolvedPath = Path;
		Cache.LastModified = Modified;
		Cache.bValid = true;
		return true;
	}

	static TSharedPtr<FJsonObject> CloneEntryWithAllKeys(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const TCHAR* Key : GKeys)
		{
			const TSharedPtr<FJsonValue> Field = Source->TryGetField(Key);
			if (Field.IsValid())
			{
				Out->SetField(Key, Field);
			}
			else
			{
				Out->SetField(Key, MakeShared<FJsonValueNull>());
			}
		}
		return Out;
	}

	static void SortByPriorityThenOrder(TArray<const FCatalogEntry*>& InOut)
	{
		InOut.Sort([](const FCatalogEntry& A, const FCatalogEntry& B)
		{
			if (A.Priority != B.Priority)
			{
				return A.Priority > B.Priority;
			}
			return A.FileOrder < B.FileOrder;
		});
	}
}

FToolResult ClaireonBlueprintGraphTool_SuggestNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // suggest_node is stateless; it does a read-only pattern lookup.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	using namespace ClaireonSuggestNode;

	FString Intent;
	if (!Params->TryGetStringField(TEXT("intent"), Intent) || Intent.IsEmpty())
	{
		return MakeErrorResult(TEXT("suggest_node requires 'intent' (non-empty string)"));
	}

	int32 TopK = GTopKDefault;
	double TopKRaw = 0.0;
	if (Params->TryGetNumberField(TEXT("top_k"), TopKRaw))
	{
		TopK = static_cast<int32>(TopKRaw);
		if (TopK < 1)
		{
			TopK = 1;
		}
	}
	if (TopK > GTopKMax)
	{
		TopK = GTopKMax;
	}

	FString LoadError;
	if (!LoadCatalog(LoadError))
	{
		return MakeErrorResult(LoadError);
	}

	const FCache& Cache = GetCache();
	const FString IntentLower = Intent.ToLower();

	// Strategy 1: exact cpp_expr_shape match against the intent argument
	TArray<const FCatalogEntry*> Matched;
	for (const FCatalogEntry& E : Cache.Entries)
	{
		if (!E.CppExprShape.IsEmpty() && E.CppExprShape.Equals(Intent, ESearchCase::CaseSensitive))
		{
			Matched.Add(&E);
		}
	}

	// Strategy 2: exact intent match (case-insensitive for robustness)
	if (Matched.Num() == 0)
	{
		for (const FCatalogEntry& E : Cache.Entries)
		{
			if (!E.Intent.IsEmpty() && E.IntentLower == IntentLower)
			{
				Matched.Add(&E);
			}
		}
	}

	// Strategy 3: case-insensitive substring match on intent
	if (Matched.Num() == 0)
	{
		for (const FCatalogEntry& E : Cache.Entries)
		{
			if (!E.IntentLower.IsEmpty() && E.IntentLower.Contains(IntentLower))
			{
				Matched.Add(&E);
			}
		}
	}

	SortByPriorityThenOrder(Matched);

	const int32 ResultCount = FMath::Min(TopK, Matched.Num());
	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	MatchesArray.Reserve(ResultCount);
	for (int32 Idx = 0; Idx < ResultCount; ++Idx)
	{
		MatchesArray.Add(MakeShared<FJsonValueObject>(CloneEntryWithAllKeys(Matched[Idx]->Raw)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("matches"), MatchesArray);
	Data->SetNumberField(TEXT("count"), ResultCount);

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Data.ToSharedRef(), Writer);

	const FString Summary = FString::Printf(TEXT("suggest_node(intent='%s'): %d match(es) (of %d candidates, top_k=%d)\n%s"),
		*Intent, ResultCount, Matched.Num(), TopK, *Serialized);

	return MakeSuccessResult(Data, Summary);
}


#undef LOCTEXT_NAMESPACE
