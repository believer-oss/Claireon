// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonLog.h"
#include "ClaireonNameResolver.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Create::GetOperation() const { return TEXT("create"); }

FString ClaireonWidgetBPTool_Create::GetDescription() const
{
    return TEXT("Create a new Widget Blueprint asset at the given path and open an editing session. Stateless / non-session entry: writes the new .uasset to disk, then returns a session_id ready for further widgetbp_* operations. Common pitfall: target package directory must already exist.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Create::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Destination asset path (e.g. '/Game/UI/WBP_NewWidget')."), true);
    Builder.AddString(TEXT("parent_class"), TEXT("Parent UUserWidget subclass (default 'UserWidget')."));
    Builder.AddString(TEXT("root_widget_class"), TEXT("Root widget class to seed (default 'CanvasPanel'; use 'none' to skip)."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // create is session-less; it creates the session.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
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

	// Resolve parent class -- must be a subclass of UUserWidget
	ClaireonNameResolver::FNameResolveResult ParentClassResult;
	UClass* ParentClass = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, ParentClassResult);
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
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonWidgetBPEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session (acts as exclusive lock)
	FString FinalAssetPath = NewWBP->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(FinalAssetPath, TEXT("widgetbp_edit"));
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

