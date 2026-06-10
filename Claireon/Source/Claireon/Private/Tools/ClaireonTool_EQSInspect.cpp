// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EQSInspect.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonLog.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/Items/EnvQueryItemType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString FormatContextClass(TSubclassOf<UEnvQueryContext> ContextClass)
	{
		if (!ContextClass)
		{
			return TEXT("(none)");
		}
		FString Name = ContextClass->GetName();
		Name.RemoveFromStart(TEXT("EnvQueryContext_"));
		return Name;
	}

	FString FormatNodeProperties(const UObject* Node, const FString& Indent)
	{
		if (!Node)
		{
			return FString();
		}

		FString Output;
		const UClass* NodeClass = Node->GetClass();

		// Walk UProperties and export non-default values
		for (TFieldIterator<FProperty> PropIt(NodeClass, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;

			// Skip properties that aren't useful for inspection
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			// Skip internal UE properties
			const FString PropName = Prop->GetName();
			if (PropName.StartsWith(TEXT("b")) && PropName.Len() > 1 && FChar::IsUpper(PropName[1]))
			{
				// Include bool properties -- they're often useful
			}
			else if (PropName == TEXT("UpdateInterval") || PropName == TEXT("VerNum") || PropName == TEXT("TestOrder"))
			{
				continue;
			}

			// Export the value
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
			FString ValueStr;
			Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

			if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()"))
			{
				// Truncate very long values
				if (ValueStr.Len() > 120)
				{
					ValueStr = ValueStr.Left(117) + TEXT("...");
				}
				Output += FString::Printf(TEXT("%s%s = %s\n"), *Indent, *PropName, *ValueStr);
			}
		}

		return Output;
	}
} // anonymous namespace

FString ClaireonTool_EQSInspect::GetCategory() const { return TEXT("eqs"); }
FString ClaireonTool_EQSInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_EQSInspect::GetDescription() const
{
	return TEXT("Read the structure of an Environment Query System (EQS) asset. "
				"Displays all options (generators + tests), context class references, "
				"scoring functions, and filter settings. Useful for identifying "
				"blackboard-based vs Trajan-based context references.");
}

TSharedPtr<FJsonObject> ClaireonTool_EQSInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the EQS query asset (e.g. /Game/AI/EQS/EQS_CombatWaiting_Strafe)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for structure overview, 'full' for complete property details (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_EQSInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	bool bFullDetail = true;
	FString DetailLevel;
	if (Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel))
	{
		bFullDetail = !DetailLevel.Equals(TEXT("summary"), ESearchCase::IgnoreCase);
	}

	FString LoadError;
	UEnvQuery* Query = ClaireonBehaviorTreeHelpers::LoadEQSAsset(AssetPath, LoadError);
	if (!Query)
	{
		return MakeErrorResult(LoadError);
	}

	// Build generators and tests arrays
	TArray<TSharedPtr<FJsonValue>> GeneratorsArray;
	TArray<TSharedPtr<FJsonValue>> TestsArray;

	const TArray<UEnvQueryOption*>& Options = Query->GetOptions();
	for (int32 OptionIdx = 0; OptionIdx < Options.Num(); ++OptionIdx)
	{
		const UEnvQueryOption* Option = Options[OptionIdx];
		if (!Option)
		{
			continue;
		}

		// Generator
		if (Option->Generator)
		{
			TSharedPtr<FJsonObject> GenObj = MakeShared<FJsonObject>();
			GenObj->SetNumberField(TEXT("option_index"), OptionIdx);
			GenObj->SetStringField(TEXT("name"), Option->Generator->GetName());
			FString GenTypeName = Option->Generator->GetClass()->GetName();
			GenTypeName.RemoveFromStart(TEXT("EnvQueryGenerator_"));
			GenObj->SetStringField(TEXT("type"), GenTypeName);

			if (bFullDetail)
			{
				GenObj->SetStringField(TEXT("params"), FormatNodeProperties(Option->Generator, TEXT("")));
			}

			GeneratorsArray.Add(MakeShared<FJsonValueObject>(GenObj));
		}

		// Tests
		for (int32 TestIdx = 0; TestIdx < Option->Tests.Num(); ++TestIdx)
		{
			const UEnvQueryTest* Test = Option->Tests[TestIdx];
			if (!Test)
			{
				continue;
			}

			TSharedPtr<FJsonObject> TestObj = MakeShared<FJsonObject>();
			TestObj->SetNumberField(TEXT("option_index"), OptionIdx);
			TestObj->SetNumberField(TEXT("test_index"), TestIdx);
			TestObj->SetStringField(TEXT("name"), Test->GetName());
			FString TestTypeName = Test->GetClass()->GetName();
			TestTypeName.RemoveFromStart(TEXT("EnvQueryTest_"));
			TestObj->SetStringField(TEXT("type"), TestTypeName);

			if (bFullDetail)
			{
				TestObj->SetStringField(TEXT("params"), FormatNodeProperties(Test, TEXT("")));
			}

			TestsArray.Add(MakeShared<FJsonValueObject>(TestObj));
		}
	}

	// Generate full structure text
	const FString StructureText = ClaireonBehaviorTreeHelpers::FormatEQSStructure(Query, bFullDetail);
	const FString AssetName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("generators"), GeneratorsArray);
	Data->SetArrayField(TEXT("tests"), TestsArray);
	Data->SetStringField(TEXT("structure"), StructureText);

	const FString Summary = FString::Printf(TEXT("%s: %d generator(s), %d test(s)"),
		*AssetName, GeneratorsArray.Num(), TestsArray.Num());

	return MakeSuccessResult(Data, Summary);
}
