// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EngineInfo.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString ClaireonTool_EngineInfo::GetName() const
{
	return TEXT("engine_info");
}

FString ClaireonTool_EngineInfo::GetCategory() const
{
	return TEXT("build");
}

FString ClaireonTool_EngineInfo::GetDescription() const
{
	return TEXT("Get engine path, version, and build information");
}

TSharedPtr<FJsonObject> ClaireonTool_EngineInfo::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_EngineInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const FEngineVersion& EngineVer = FEngineVersion::Current();
	const FString VersionStr = FString::Printf(TEXT("%d.%d.%d"),
		EngineVer.GetMajor(), EngineVer.GetMinor(), EngineVer.GetPatch());

	const FString BuildId = FApp::GetBuildVersion();
	const FString CustomLabel = EngineVer.GetBranch();

	FString Configuration = TEXT("Unknown");
#if UE_BUILD_DEBUG
	Configuration = TEXT("Debug");
#elif UE_BUILD_DEVELOPMENT
	Configuration = TEXT("Development");
#elif UE_BUILD_SHIPPING
	Configuration = TEXT("Shipping");
#elif UE_BUILD_TEST
	Configuration = TEXT("Test");
#endif

	const FString Platform = FPlatformProperties::IniPlatformName();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("version"), VersionStr);
	Data->SetStringField(TEXT("build_id"), BuildId);
	Data->SetStringField(TEXT("custom_label"), CustomLabel);
	Data->SetStringField(TEXT("platform"), Platform);
	Data->SetStringField(TEXT("configuration"), Configuration);

	const FString Summary = FString::Printf(TEXT("UE %s (%s) %s %s"),
		*VersionStr, *BuildId, *Platform, *Configuration);

	return MakeSuccessResult(Data, Summary);
}
