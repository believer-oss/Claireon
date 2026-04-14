// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EQSEdit.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FEQSEditToolData> ClaireonTool_EQSEdit::ToolData;
bool ClaireonTool_EQSEdit::bDelegateRegistered = false;

namespace
{
	/**
	 * Get a mutable reference to the Options array on a UEnvQuery.
	 * UEnvQuery::GetOptions() returns const ref; we need write access for editing.
	 * The query has already been marked for modification via Modify() before calling this.
	 */
	TArray<UEnvQueryOption*>& GetOptionsMutable(UEnvQuery* Query)
	{
		// Access the Options UPROPERTY via reflection for safe mutable access
		static FArrayProperty* OptionsProp = nullptr;
		if (!OptionsProp)
		{
			OptionsProp = CastField<FArrayProperty>(FindFProperty<FProperty>(UEnvQuery::StaticClass(), TEXT("Options")));
		}
		check(OptionsProp);
		return *OptionsProp->ContainerPtrToValuePtr<TArray<UEnvQueryOption*>>(Query);
	}

	UClass* ResolveEQSClass(const FString& ClassName, UClass* BaseClass, const FString& BasePrefix, FString& OutError)
	{
		// Try the core resolver first
		ClaireonNameResolver::FNameResolveResult NameResult;
		UClass* FoundClass = ClaireonNameResolver::ResolveClassName(ClassName, BaseClass, NameResult);
		if (FoundClass)
		{
			return FoundClass;
		}

		// Try with domain-specific prefix (e.g., "EnvQueryGenerator_" + "SimpleGrid")
		FoundClass = ClaireonNameResolver::ResolveClassName(BasePrefix + ClassName, BaseClass, NameResult);
		if (FoundClass)
		{
			return FoundClass;
		}

		OutError = FString::Printf(TEXT("Could not resolve EQS class: %s (expected subclass of %s)"), *ClassName, *BaseClass->GetName());
		return nullptr;
	}

	bool SetEQSNodeProperty(UObject* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Node is null");
			return false;
		}

		FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), *PropertyName);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Node->GetClass()->GetName());
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
		const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Node, PPF_None);
		if (!Result)
		{
			OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *PropertyName, *PropertyValue, *Node->GetClass()->GetName());
			return false;
		}

		return true;
	}
} // namespace

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_EQSEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.eqs_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_EQSEdit::GetName() const
{
	return TEXT("claireon.eqs_edit");
}

FString ClaireonTool_EQSEdit::GetDescription() const
{
	return TEXT("Session-based EQS Query editor. Manage options, generators, tests, and properties. Start with 'open', configure, then 'save'.");
}

FString ClaireonTool_EQSEdit::GetFullDescription() const
{
	return TEXT("Session-based EQS Query editor. Supports adding/removing options, generators, tests, "
				"property editing, test reordering, and saving.\n\n"
				"Session operations: open, create_new, close, status\n"
				"Option operations: add_option, remove_option, set_generator\n"
				"Test operations: add_test, remove_test, reorder_tests\n"
				"Property operations: set_node_property\n"
				"Build operations: save");
}

TSharedPtr<FJsonObject> ClaireonTool_EQSEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation
	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("create_new")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_option")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_option")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_generator")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_test")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_test")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("reorder_tests")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_node_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// session_id
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	// params
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	// suppress_output
	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full EQS state."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_EQSEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: operation"));
	}

	// Params sub-object (optional)
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	else
	{
		Params = Arguments;
	}

	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	// Operations that don't need a session
	if (Operation == TEXT("open"))
		return Operation_Open(Params);
	if (Operation == TEXT("create_new"))
		return Operation_CreateNew(Params);

	// All other operations require session_id
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FEQSEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status"))
		return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("add_option"))
		return Operation_AddOption(SessionId, Data, Params);
	if (Operation == TEXT("remove_option"))
		return Operation_RemoveOption(SessionId, Data, Params);
	if (Operation == TEXT("set_generator"))
		return Operation_SetGenerator(SessionId, Data, Params);
	if (Operation == TEXT("add_test"))
		return Operation_AddTest(SessionId, Data, Params);
	if (Operation == TEXT("remove_test"))
		return Operation_RemoveTest(SessionId, Data, Params);
	if (Operation == TEXT("reorder_tests"))
		return Operation_ReorderTests(SessionId, Data, Params);
	if (Operation == TEXT("set_node_property"))
		return Operation_SetNodeProperty(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_EQSEdit::BuildStateResponse(const FString& SessionId, FEQSEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressJson = MakeShared<FJsonObject>();
		SuppressJson->SetStringField(TEXT("session_id"), SessionId);
		SuppressJson->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressJson, StatusMsg);
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->Query->GetPathName());
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonBehaviorTreeHelpers::FormatEQSStructure(Data->Query.Get(), false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), Data->Query->GetPathName());
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResultJson->SetStringField(TEXT("eqs_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResultJson, Summary);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_EQSEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires params.asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UEnvQuery* Query = ClaireonBehaviorTreeHelpers::LoadEQSAsset(AssetPath, Error);
	if (!Query)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EQSEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = Query->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.eqs_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FEQSEditToolData NewData;
	NewData.Query = Query;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatEQSStructure(Query, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}

FToolResult ClaireonTool_EQSEdit::Operation_CreateNew(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create_new' requires params.asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Check that asset does not already exist
	FSoftObjectPath SoftPath(AssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *AssetPath));
	}

	// Create the package and asset
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	FString AssetName = FPackageName::GetShortName(AssetPath);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	UEnvQuery* Query = NewObject<UEnvQuery>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Query)
	{
		return MakeErrorResult(TEXT("Failed to create EQS Query"));
	}

	// Save the new asset
	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	// Register with asset registry
	FAssetRegistryModule::AssetCreated(Query);

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EQSEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open a session for the new asset
	const FString ResolvedAssetPath = Query->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.eqs_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create tool data entry
	FEQSEditToolData NewData;
	NewData.Query = Query;
	NewData.LastOperationStatus = TEXT("Asset created and session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	// Build response
	FString StructureText = ClaireonBehaviorTreeHelpers::FormatEQSStructure(Query, false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("status"), TEXT("Asset created and session opened"));
	ResultJson->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("Created and opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}

FToolResult ClaireonTool_EQSEdit::Operation_Close(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bSaveFirst = false;
	Params->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst)
	{
		Operation_Save(SessionId, Data, MakeShared<FJsonObject>());
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}

FToolResult ClaireonTool_EQSEdit::Operation_Status(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Option Operations
// ============================================================================

FToolResult ClaireonTool_EQSEdit::Operation_AddOption(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString GeneratorClassName;
	if (!Params->TryGetStringField(TEXT("generator_class"), GeneratorClassName) || GeneratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: generator_class"));
	}

	FString Error;
	UClass* GeneratorClass = ResolveEQSClass(GeneratorClassName, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
	if (!GeneratorClass)
	{
		return MakeErrorResult(Error);
	}

	UEnvQuery* Query = Data->Query.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add EQS Option")));
	Query->Modify();

	UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
	NewOption->Generator = NewObject<UEnvQueryGenerator>(NewOption, GeneratorClass);

	GetOptionsMutable(Query).Add(NewOption);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_option â Added option with generator %s (index %d)"),
		*GeneratorClassName, Query->GetOptions().Num() - 1);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EQSEdit::Operation_RemoveOption(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range (0-%d)"), OptionIndex, Query->GetOptions().Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove EQS Option")));
	Query->Modify();

	GetOptionsMutable(Query).RemoveAt(OptionIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_option â Removed option %d"), OptionIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EQSEdit::Operation_SetGenerator(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	FString GeneratorClassName;
	if (!Params->TryGetStringField(TEXT("generator_class"), GeneratorClassName) || GeneratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: generator_class"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	FString Error;
	UClass* GeneratorClass = ResolveEQSClass(GeneratorClassName, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
	if (!GeneratorClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set EQS Generator")));
	Query->Modify();

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];
	Option->Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_generator â Set option %d generator to %s"),
		OptionIndex, *GeneratorClassName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Test Operations
// ============================================================================

FToolResult ClaireonTool_EQSEdit::Operation_AddTest(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	FString TestClassName;
	if (!Params->TryGetStringField(TEXT("test_class"), TestClassName) || TestClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: test_class"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	FString Error;
	UClass* TestClass = ResolveEQSClass(TestClassName, UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), Error);
	if (!TestClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add EQS Test")));
	Query->Modify();

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];
	UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
	Option->Tests.Add(NewTest);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_test â Added %s to option %d (test index %d)"),
		*TestClassName, OptionIndex, Option->Tests.Num() - 1);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EQSEdit::Operation_RemoveTest(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	int32 TestIndex = -1;
	if (!Params->TryGetNumberField(TEXT("test_index"), TestIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: test_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range for option %d (0-%d)"), TestIndex, OptionIndex, Option->Tests.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove EQS Test")));
	Query->Modify();

	Option->Tests.RemoveAt(TestIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_test â Removed test %d from option %d"), TestIndex, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EQSEdit::Operation_ReorderTests(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	int32 TestIndex = -1;
	if (!Params->TryGetNumberField(TEXT("test_index"), TestIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: test_index"));
	}

	int32 NewIndex = -1;
	if (!Params->TryGetNumberField(TEXT("new_index"), NewIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range"), TestIndex));
	}

	if (NewIndex < 0 || NewIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("New index %d out of range"), NewIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reorder EQS Tests")));
	Query->Modify();

	UEnvQueryTest* TestToMove = Option->Tests[TestIndex];
	Option->Tests.RemoveAt(TestIndex);
	Option->Tests.Insert(TestToMove, NewIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("reorder_tests â Moved test %d to index %d in option %d"),
		TestIndex, NewIndex, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Property Operations
// ============================================================================

FToolResult ClaireonTool_EQSEdit::Operation_SetNodeProperty(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 OptionIndex = -1;
	if (!Params->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	FString Target;
	if (!Params->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: target ('generator' or 'test')"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_value"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set EQS Node Property")));
	Query->Modify();

	FString Error;
	if (Target == TEXT("generator"))
	{
		if (!Option->Generator)
		{
			return MakeErrorResult(TEXT("Option has no generator"));
		}
		if (!SetEQSNodeProperty(Option->Generator, PropertyName, PropertyValue, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else if (Target == TEXT("test"))
	{
		int32 TestIndex = -1;
		if (!Params->TryGetNumberField(TEXT("test_index"), TestIndex))
		{
			return MakeErrorResult(TEXT("Missing required parameter: test_index (required when target='test')"));
		}

		if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range for option %d"), TestIndex, OptionIndex));
		}

		if (!SetEQSNodeProperty(Option->Tests[TestIndex], PropertyName, PropertyValue, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid target: '%s'. Use 'generator' or 'test'."), *Target));
	}

	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property â Set '%s' = '%s' on %s (option %d)"),
		*PropertyName, *PropertyValue, *Target, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Build Operations
// ============================================================================

FToolResult ClaireonTool_EQSEdit::Operation_Save(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UEnvQuery* Query = Data->Query.Get();
	UPackage* Package = Query->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save â Saved %s"), *Query->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save â Failed");
		return MakeErrorResult(TEXT("Failed to save EQS package"));
	}
}
