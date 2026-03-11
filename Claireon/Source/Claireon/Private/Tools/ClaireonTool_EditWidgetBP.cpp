// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditWidgetBP.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "ClaireonTool_EditWidgetBP"

using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FWidgetBPEditToolData> ClaireonTool_EditWidgetBP::ToolData;
bool ClaireonTool_EditWidgetBP::bDelegateRegistered = false;

FString ClaireonTool_EditWidgetBP::GetName() const
{
	return TEXT("editor.widgetbp.edit");
}

FString ClaireonTool_EditWidgetBP::GetCategory() const
{
	return TEXT("widget");
}

FString ClaireonTool_EditWidgetBP::GetDescription() const
{
	return TEXT("Session-based Widget Blueprint editor. Add, remove, and configure widgets, manage properties and bindings, import/export widget hierarchies. Start with 'open' or 'create'.");
}

FString ClaireonTool_EditWidgetBP::GetFullDescription() const
{
	return TEXT("Session-based Widget Blueprint editor. Open a widget blueprint, then use operations to add/remove/move widgets, edit properties, manage bindings, and more. Start with 'open' or 'create'.\n\n"
				"Required params by operation:\n\n"
				"Lifecycle (no session_id needed):\n"
				"- open: params: asset_path (required)\n"
				"- create: params: asset_path (required), parent_class (optional, defaults to \"UserWidget\")\n\n"
				"Lifecycle (session_id required):\n"
				"- get_state, compile, save, close: (no params beyond session_id)\n"
				"- focus: params: widget_name (required)\n\n"
				"Widget CRUD (session_id required):\n"
				"- add_widget: params: widget_class (required), parent_name (optional), widget_name (optional)\n"
				"- remove_widget: params: widget_name (required)\n"
				"- move_widget: params: widget_name (required), new_parent_name (required)\n"
				"- replace_widget: params: widget_name (required), new_widget_class (required)\n"
				"- rename_widget: params: widget_name (required), new_name (required)\n\n"
				"Property operations (session_id required):\n"
				"- set_widget_property: params: widget_name (required), property_name (required), value (required)\n"
				"- set_slot_property: params: widget_name (required), property_name (required), value (required)\n"
				"- get_widget_details: params: widget_name (required)\n\n"
				"Advanced (session_id required):\n"
				"- add_binding: params: widget_name (required), property_name (required), function_name (required)\n"
				"- remove_binding: params: widget_name (required), property_name (required)\n"
				"- list_animations: (no params)\n"
				"- import_widgets: params: widget_text (required)\n"
				"- export_widgets: params: widget_names (required, array of strings)\n"
				"- list_widget_classes: (no params)");
}

TSharedPtr<FJsonObject> ClaireonTool_EditWidgetBP::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation property
	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("create")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("focus")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("compile")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("save")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("close")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("move_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("replace_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("rename_widget")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_widget_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_slot_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_widget_details")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_animations")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("import_widgets")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("export_widgets")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_widget_classes")));
	OperationProp->SetArrayField(TEXT("enum"), OperationEnum);
	OperationProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	Properties->SetObjectField(TEXT("operation"), OperationProp);

	// session_id property
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' or 'create' operation. Required for all operations except 'open' and 'create'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	// params property
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters. See operation descriptions for details."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_EditWidgetBP::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	// Session-less operations
	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}
	if (Operation == TEXT("create"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Create(Params);
	}

	// Session-required operations
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Operation '%s' requires session_id"), *Operation));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FWidgetBPEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	FClaireonSessionManager::Get().TouchSession(SessionId);

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("get_state"))
	{
		return Operation_GetState(SessionId, Data, Params);
	}
	if (Operation == TEXT("focus"))
	{
		return Operation_Focus(SessionId, Data, Params);
	}
	if (Operation == TEXT("compile"))
	{
		return Operation_Compile(SessionId, Data, Params);
	}
	if (Operation == TEXT("save"))
	{
		return Operation_Save(SessionId, Data, Params);
	}
	if (Operation == TEXT("close"))
	{
		return Operation_Close(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_widget"))
	{
		return Operation_AddWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_widget"))
	{
		return Operation_RemoveWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("move_widget"))
	{
		return Operation_MoveWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("replace_widget"))
	{
		return Operation_ReplaceWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("rename_widget"))
	{
		return Operation_RenameWidget(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_widget_property"))
	{
		return Operation_SetWidgetProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_slot_property"))
	{
		return Operation_SetSlotProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("get_widget_details"))
	{
		return Operation_GetWidgetDetails(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_binding"))
	{
		return Operation_AddBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_binding"))
	{
		return Operation_RemoveBinding(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_animations"))
	{
		return Operation_ListAnimations(SessionId, Data, Params);
	}
	if (Operation == TEXT("import_widgets"))
	{
		return Operation_ImportWidgets(SessionId, Data, Params);
	}
	if (Operation == TEXT("export_widgets"))
	{
		return Operation_ExportWidgets(SessionId, Data, Params);
	}
	if (Operation == TEXT("list_widget_classes"))
	{
		return Operation_ListWidgetClasses(SessionId, Data, Params);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_EditWidgetBP::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("editor.widgetbp.edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Session Lifecycle Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. open requires: asset_path (e.g. \"/Game/UI/WBP_MyWidget.WBP_MyWidget\")"));
	}

	// Canonicalize path early to prevent malformed paths from reaching LoadObject
	// (e.g., double slashes cause a fatal error in CreatePackage)
	AssetPath = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path. Path must start with /Game/."));
	}

	// Load the widget blueprint
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!WBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditWidgetBP::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Use the canonical path from the loaded object as the lock key
	FString CanonicalPath = WBP->GetPathName();

	// Open session (acts as exclusive lock)
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(CanonicalPath, TEXT("editor.widgetbp.edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *CanonicalPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create and populate tool data
	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = WBP;
	NewData.bModified = false;
	NewData.LastCommandTime = FDateTime::Now();

	// Set initial focus to root widget if one exists
	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		NewData.FocusedWidget = WBP->WidgetTree->RootWidget->GetFName();
	}

	ToolData.Add(SessionId, NewData);

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Opened session %s for %s"), *SessionId, *AssetPath);

	FWidgetBPEditToolData* LiveData = ToolData.Find(SessionId);
	if (!LiveData)
	{
		return MakeErrorResult(TEXT("Failed to create session after open"));
	}

	return BuildStateResponse(SessionId, LiveData);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Create(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. create requires: asset_path, parent_class (optional, defaults to \"UserWidget\")"));
	}

	// Validate asset path
	FString ValidationError;
	if (!ClaireonWidgetHelpers::ValidateWidgetBPAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Extract parent_class (default: UserWidget)
	FString ParentClassName;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ParentClassName = TEXT("UserWidget");
	}

	// Extract root_widget_class (default: CanvasPanel)
	FString RootWidgetClassStr;
	if (!Params->TryGetStringField(TEXT("root_widget_class"), RootWidgetClassStr))
	{
		RootWidgetClassStr = TEXT("CanvasPanel");
	}

	// Resolve parent class â must be a subclass of UUserWidget
	UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName);
	if (!ParentClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent class '%s' not found"), *ParentClassName));
	}
	if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent class '%s' is not a subclass of UserWidget"), *ParentClassName));
	}

	// Extract package name and asset name from asset path
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewWidgetBlueprint");
		}
	}

	// Delete pre-existing file to avoid partially-loaded errors
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Create: Deleting existing file %s"), *PackageFileName);
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create widget blueprint
	UWidgetBlueprint* NewWBP = CastChecked<UWidgetBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None));

	if (!NewWBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create Widget Blueprint at %s"), *PackageName));
	}

	// Create root widget if requested
	if (!RootWidgetClassStr.IsEmpty() && RootWidgetClassStr != TEXT("none"))
	{
		FString RootClassError;
		UClass* RootClass = ClaireonWidgetHelpers::ResolveWidgetClass(RootWidgetClassStr, RootClassError);
		if (!RootClass)
		{
			// Non-fatal: log warning, continue without root widget
			UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Could not resolve root widget class '%s': %s"), *RootWidgetClassStr, *RootClassError);
		}
		else if (NewWBP->WidgetTree)
		{
			UWidget* Root = NewWBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootClass->GetName()));
			if (Root)
			{
				NewWBP->WidgetTree->RootWidget = Root;
			}
		}
	}

	// Mark package as externally referenceable and dirty
	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(NewWBP);

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditWidgetBP::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session (acts as exclusive lock)
	FString FinalAssetPath = NewWBP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(FinalAssetPath, TEXT("editor.widgetbp.edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *FinalAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create and register tool data
	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = NewWBP;
	NewData.bModified = true;
	NewData.LastCommandTime = FDateTime::Now();

	if (NewWBP->WidgetTree && NewWBP->WidgetTree->RootWidget)
	{
		NewData.FocusedWidget = NewWBP->WidgetTree->RootWidget->GetFName();
	}

	ToolData.Add(SessionId, NewData);

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Created session %s for new WBP %s"), *SessionId, *FinalAssetPath);

	FWidgetBPEditToolData* LiveData = ToolData.Find(SessionId);
	if (!LiveData)
	{
		return MakeErrorResult(TEXT("Failed to create session after create"));
	}

	return BuildStateResponse(SessionId, LiveData);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_GetState(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Focus(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* FoundWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!FoundWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
	}

	Data->FocusedWidget = FName(*WidgetName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Compile(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	FKismetEditorUtilities::CompileBlueprint(WBP);

	const bool bCompileSuccess = (WBP->Status == BS_UpToDate || WBP->Status == BS_UpToDateWithWarnings);
	FString CompileStatus = bCompileSuccess ? TEXT("Success") : TEXT("Failed");

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("compile_status"), CompileStatus);
	ResultJson->SetBoolField(TEXT("success"), bCompileSuccess);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Save(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	UPackage* Package = WBP->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	const bool bSaved = UPackage::SavePackage(Package, WBP, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
	}

	Data->bModified = false;

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResultJson->SetBoolField(TEXT("success"), true);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_Close(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bWasModified = Data->bModified;

	// Remove tool data before closing session (delegate will also try, but be explicit)
	ToolData.Remove(SessionId);
	FClaireonSessionManager::Get().CloseSession(SessionId);

	FString ResultMsg = TEXT("Session closed successfully.");
	if (bWasModified)
	{
		ResultMsg += TEXT(" Warning: Widget Blueprint had unsaved modifications.");
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("message"), ResultMsg);
	if (bWasModified)
	{
		ResultJson->SetBoolField(TEXT("unsaved_changes"), true);
	}

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

// ============================================================================
// Helpers
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::BuildStateResponse(const FString& SessionId, FWidgetBPEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();

	// Serialize widget tree with default options
	FWidgetSerializeOptions Options;
	Options.bIncludeProperties = false;
	Options.MaxDepth = -1;
	TSharedPtr<FJsonObject> TreeData = ClaireonWidgetHelpers::SerializeWidgetTree(WBP, Options);

	// Build response JSON
	TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
	ResponseObj->SetStringField(TEXT("session_id"), SessionId);
	ResponseObj->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResponseObj->SetStringField(TEXT("focused_widget"), Data->FocusedWidget.ToString());
	ResponseObj->SetBoolField(TEXT("modified"), Data->bModified);

	if (TreeData.IsValid())
	{
		ResponseObj->SetObjectField(TEXT("widget_tree"), TreeData);
	}

	FString ResponseString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResponseString);
	FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResponseString);
}

// ============================================================================
// Stage 009: Widget CRUD Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_AddWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	// Extract required widget_class
	FString WidgetClassStr;
	if (!Params->TryGetStringField(TEXT("widget_class"), WidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_class"));
	}

	// Extract optional fields
	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	FString WidgetName;
	Params->TryGetStringField(TEXT("widget_name"), WidgetName);

	const TSharedPtr<FJsonObject>* SlotPropertiesPtr = nullptr;
	TSharedPtr<FJsonObject> SlotProperties;
	if (Params->TryGetObjectField(TEXT("slot_properties"), SlotPropertiesPtr) && SlotPropertiesPtr)
	{
		SlotProperties = *SlotPropertiesPtr;
	}

	// Get WBP and WidgetTree from session
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	// Resolve class
	FString ClassError;
	UClass* ResolvedClass = ClaireonWidgetHelpers::ResolveWidgetClass(WidgetClassStr, ClassError);
	if (!ResolvedClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve widget class '%s': %s"), *WidgetClassStr, *ClassError));
	}

	// Determine parent panel
	UPanelWidget* ParentPanel = nullptr;
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*ParentName));
		if (!ParentWidget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' not found"), *ParentName));
		}
		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' is not a panel widget"), *ParentName));
		}
	}
	else if (Tree->RootWidget)
	{
		// If no parent specified and root is a panel, use root as default parent
		ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
	}
	// If no root exists at all, we will set the new widget as root

	// Transaction
	FScopedTransaction Transaction(LOCTEXT("MCPAddWidget", "MCP: Add Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Determine widget name
	FName FinalWidgetName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

	// Create the widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, ResolvedClass, FinalWidgetName);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create widget of class '%s'"), *WidgetClassStr));
	}

	// Set as root if no root exists
	if (!Tree->RootWidget)
	{
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Add to parent panel
		ClaireonWidgetHelpers::AddChildToPanel(ParentPanel, NewWidget, SlotProperties);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;
	Data->FocusedWidget = NewWidget->GetFName();

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Added widget '%s' (class: %s)"), *NewWidget->GetName(), *ResolvedClass->GetName());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPRemoveWidget", "MCP: Remove Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	if (Tree->RootWidget == Widget)
	{
		Tree->RootWidget = nullptr;
	}
	else
	{
		Tree->RemoveWidget(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Clear focus if the focused widget was removed
	if (Data->FocusedWidget == Widget->GetFName())
	{
		Data->FocusedWidget = Tree->RootWidget ? Tree->RootWidget->GetFName() : NAME_None;
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Removed widget '%s'"), *WidgetName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_MoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewParentName;
	if (!Params->TryGetStringField(TEXT("new_parent_name"), NewParentName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_parent_name"));
	}

	int32 InsertIndex = -1;
	Params->TryGetNumberField(TEXT("index"), InsertIndex);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	UWidget* NewParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewParentName));
	if (!NewParentWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' not found"), *NewParentName));
	}

	UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParentWidget);
	if (!NewParentPanel)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' is not a panel widget"), *NewParentName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPMoveWidget", "MCP: Move Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Remove from old parent if it has one
	if (UPanelWidget* OldParent = Cast<UPanelWidget>(Widget->GetParent()))
	{
		OldParent->RemoveChild(Widget);
	}

	// Add to new parent
	if (InsertIndex >= 0)
	{
		NewParentPanel->InsertChildAt(InsertIndex, Widget);
	}
	else
	{
		NewParentPanel->AddChild(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Moved widget '%s' to parent '%s'"), *WidgetName, *NewParentName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ReplaceWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewWidgetClassStr;
	if (!Params->TryGetStringField(TEXT("new_widget_class"), NewWidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: new_widget_class"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* OldWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!OldWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FString ClassError;
	UClass* NewClass = ClaireonWidgetHelpers::ResolveWidgetClass(NewWidgetClassStr, ClassError);
	if (!NewClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve new widget class '%s': %s"), *NewWidgetClassStr, *ClassError));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPReplaceWidget", "MCP: Replace Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Create new widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, NewClass, NAME_None);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create new widget of class '%s'"), *NewWidgetClassStr));
	}

	// Note old parent before replacing
	UPanelWidget* ParentPanel = Cast<UPanelWidget>(OldWidget->GetParent());

	if (Tree->RootWidget == OldWidget)
	{
		// Replace root
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Use ReplaceChild to maintain slot position
		if (!ParentPanel->ReplaceChild(OldWidget, NewWidget))
		{
			// Fallback: remove old and add new
			ParentPanel->RemoveChild(OldWidget);
			ParentPanel->AddChild(NewWidget);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if needed
	if (Data->FocusedWidget == OldWidget->GetFName())
	{
		Data->FocusedWidget = NewWidget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Replaced widget '%s' with class '%s' -> '%s'"), *WidgetName, *NewWidgetClassStr, *NewWidget->GetName());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RenameWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	// Verify new name doesn't conflict with an existing widget
	if (ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewName)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A widget named '%s' already exists in the tree"), *NewName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPRenameWidget", "MCP: Rename Widget"));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();
	Widget->Modify();

	FName OldFName = Widget->GetFName();

	// Rename the UObject
	Widget->Rename(*NewName, Widget->GetOuter());

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if the focused widget was renamed
	if (Data->FocusedWidget == OldFName)
	{
		Data->FocusedWidget = Widget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Renamed widget '%s' to '%s'"), *WidgetName, *Widget->GetName());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Stage 010: Property Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_SetWidgetProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPSetWidgetProperty", "MCP: Set Widget Property"));
	Widget->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteWidgetProperty(Widget, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_SetSlotProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' has no slot (it may be the root widget)"), *WidgetName));
	}

	FScopedTransaction Transaction(LOCTEXT("MCPSetSlotProperty", "MCP: Set Slot Property"));
	Slot->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteSlotProperty(Slot, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set slot property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_GetWidgetDetails(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("name"), Widget->GetName());
	Details->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());

	// Parent info
	if (UWidget* Parent = Widget->GetParent())
	{
		Details->SetStringField(TEXT("parent_name"), Parent->GetName());
	}
	else
	{
		Details->SetStringField(TEXT("parent_name"), TEXT(""));
	}

	// Panel info
	UPanelWidget* AsPanel = Cast<UPanelWidget>(Widget);
	Details->SetBoolField(TEXT("is_panel"), AsPanel != nullptr);

	if (AsPanel)
	{
		Details->SetNumberField(TEXT("child_count"), AsPanel->GetChildrenCount());

		TArray<TSharedPtr<FJsonValue>> ChildNames;
		for (int32 i = 0; i < AsPanel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = AsPanel->GetChildAt(i))
			{
				ChildNames.Add(MakeShared<FJsonValueString>(Child->GetName()));
			}
		}
		Details->SetArrayField(TEXT("children"), ChildNames);
	}

	// Slot info
	if (UPanelSlot* Slot = Widget->Slot)
	{
		Details->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());

		TSharedPtr<FJsonObject> SlotProps = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			bool bSuccess = false;
			FString PropValue = ClaireonWidgetHelpers::ReadSlotProperty(Slot, Prop->GetName(), bSuccess);
			if (bSuccess)
			{
				SlotProps->SetStringField(Prop->GetName(), PropValue);
			}
		}
		Details->SetObjectField(TEXT("slot_properties"), SlotProps);
	}

	// All editable widget properties
	TSharedPtr<FJsonObject> WidgetProps = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		bool bSuccess = false;
		FString PropValue = ClaireonWidgetHelpers::ReadWidgetProperty(Widget, Prop->GetName(), bSuccess);
		if (bSuccess)
		{
			WidgetProps->SetStringField(Prop->GetName(), PropValue);
		}
	}
	Details->SetObjectField(TEXT("properties"), WidgetProps);

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(Details.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

// ============================================================================
// Stage 011: Advanced Operations
// ============================================================================

FToolResult ClaireonTool_EditWidgetBP::Operation_AddBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetNameStr;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyNameStr;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString FunctionNameStr;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: function_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	// Validate the widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetNameStr));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetNameStr));
	}

	// Build the binding
	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetNameStr;
	Binding.PropertyName = FName(*PropertyNameStr);
	Binding.FunctionName = FName(*FunctionNameStr);
	Binding.Kind = EBindingKind::Function;

	// Remove any pre-existing binding for the same widget+property (only one allowed)
	WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Binding.ObjectName && Existing.PropertyName == Binding.PropertyName;
	});

	WBP->Bindings.Add(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetNameStr);
	ResultObj->SetStringField(TEXT("property_name"), PropertyNameStr);
	ResultObj->SetStringField(TEXT("function_name"), FunctionNameStr);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_RemoveBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetNameStr;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyNameStr;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	int32 RemovedCount = WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Binding)
	{
		return Binding.ObjectName == WidgetNameStr && Binding.PropertyName == FName(*PropertyNameStr);
	});

	if (RemovedCount > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		Data->bModified = true;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetNumberField(TEXT("removed_count"), RemovedCount);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ListAnimations(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TArray<TSharedPtr<FJsonValue>> AnimArray;
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (!Anim)
		{
			continue;
		}

		TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
		AnimObj->SetStringField(TEXT("name"), Anim->GetFName().ToString());
		AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
		AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
		AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("animations"), AnimArray);
	ResultObj->SetNumberField(TEXT("count"), AnimArray.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ImportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetText;
	if (!Params->TryGetStringField(TEXT("widget_text"), WidgetText))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_text"));
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TSet<UWidget*> ImportedWidgets;
	TMap<FName, UWidgetSlotPair*> PastedExtraSlotData;

	FScopedTransaction Transaction(LOCTEXT("MCPImportWidgets", "MCP: Import Widgets"));
	WBP->WidgetTree->SetFlags(RF_Transactional);
	WBP->WidgetTree->Modify();

	FWidgetBlueprintEditorUtils::ImportWidgetsFromText(WBP, WidgetText, ImportedWidgets, PastedExtraSlotData);

	if (ImportedWidgets.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets were imported from the provided text"));
	}

	// If a parent is specified and imported widgets have no parent, add them to the named parent
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*ParentName));
		UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (ParentPanel)
		{
			for (UWidget* Imported : ImportedWidgets)
			{
				if (Imported && !Imported->GetParent())
				{
					ParentPanel->AddChild(Imported);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Collect imported widget names
	TArray<TSharedPtr<FJsonValue>> ImportedNames;
	for (UWidget* Imported : ImportedWidgets)
	{
		if (Imported)
		{
			ImportedNames.Add(MakeShared<FJsonValueString>(Imported->GetName()));
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetArrayField(TEXT("imported_widgets"), ImportedNames);
	ResultObj->SetNumberField(TEXT("count"), ImportedNames.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ExportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* WidgetNamesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("widget_names"), WidgetNamesArray) || !WidgetNamesArray)
	{
		return MakeErrorResult(TEXT("Missing required field: widget_names (array of strings)"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TArray<UWidget*> WidgetsToExport;
	for (const TSharedPtr<FJsonValue>& NameValue : *WidgetNamesArray)
	{
		FString Name = NameValue->AsString();
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*Name));
		if (!Widget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *Name));
		}
		WidgetsToExport.Add(Widget);
	}

	if (WidgetsToExport.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets specified for export"));
	}

	FString ExportedText;
	FWidgetBlueprintEditorUtils::ExportWidgetsToText(WidgetsToExport, ExportedText);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_text"), ExportedText);
	ResultObj->SetNumberField(TEXT("widget_count"), WidgetsToExport.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

FToolResult ClaireonTool_EditWidgetBP::Operation_ListWidgetClasses(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);

	bool bPanelsOnly = false;
	Params->TryGetBoolField(TEXT("panels_only"), bPanelsOnly);

	TArray<UClass*> FoundClasses;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;

		// Must be a non-abstract, non-deprecated UWidget subclass
		if (!Class->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		// panels_only filter
		if (bPanelsOnly && !Class->IsChildOf(UPanelWidget::StaticClass()))
		{
			continue;
		}

		// string filter (case-insensitive match on class name)
		if (!Filter.IsEmpty())
		{
			FString ClassName = Class->GetName();
			if (!ClassName.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FoundClasses.Add(Class);
	}

	// Sort alphabetically by class name
	FoundClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});

	TArray<TSharedPtr<FJsonValue>> ClassArray;
	for (UClass* Class : FoundClasses)
	{
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), Class->GetName());
		ClassObj->SetBoolField(TEXT("is_panel"), Class->IsChildOf(UPanelWidget::StaticClass()));
		ClassArray.Add(MakeShared<FJsonValueObject>(ClassObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("classes"), ClassArray);
	ResultObj->SetNumberField(TEXT("count"), ClassArray.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

#undef LOCTEXT_NAMESPACE
