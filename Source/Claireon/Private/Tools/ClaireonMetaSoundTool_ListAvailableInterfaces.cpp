// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_ListAvailableInterfaces.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#undef CLAIREON_HAS_METASOUND_SEARCH_API
#define CLAIREON_HAS_METASOUND_SEARCH_API 0
#if __has_include("MetasoundFrontendSearchEngine.h") && __has_include("MetasoundFrontendDocument.h")
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundFrontendDocument.h"
#undef CLAIREON_HAS_METASOUND_SEARCH_API
#define CLAIREON_HAS_METASOUND_SEARCH_API 1
#endif

FString FClaireonMetaSoundTool_ListAvailableInterfaces::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_ListAvailableInterfaces::GetOperation() const { return TEXT("list_available_interfaces"); }

FString FClaireonMetaSoundTool_ListAvailableInterfaces::GetDescription() const
{
	return TEXT("Return all registered MetaSound frontend interface names (e.g. 'UE.Spatialization', "
				"'UE.Source.Stereo'). Stateless; no session required. Use to discover the correct name "
				"before metasound_add_interface (D4). Returns {interfaces:[{name, major_version, "
				"minor_version}, ...]}.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_ListAvailableInterfaces::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddBoolean(TEXT("include_all_versions"),
		TEXT("If true, include all registered versions (incl. deprecated). Default false."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_ListAvailableInterfaces::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !CLAIREON_HAS_METASOUND_SEARCH_API
	return MakeErrorResult(TEXT("MetaSound frontend search engine not available on this engine branch"));
#elif !WITH_EDITORONLY_DATA
	return MakeErrorResult(TEXT("FindAllInterfaces requires WITH_EDITORONLY_DATA"));
#else
	bool bIncludeAllVersions = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetBoolField(TEXT("include_all_versions"), bIncludeAllVersions);
	}

	using namespace Metasound::Frontend;
	ISearchEngine& Engine = ISearchEngine::Get();
	const TArray<FMetasoundFrontendInterface> All = Engine.FindAllInterfaces(bIncludeAllVersions);

	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(All.Num());
	for (const FMetasoundFrontendInterface& IF : All)
	{
		TSharedPtr<FJsonObject> IJ = MakeShared<FJsonObject>();
		IJ->SetStringField(TEXT("name"), IF.Version.Name.ToString());
		IJ->SetNumberField(TEXT("major_version"), IF.Version.Number.Major);
		IJ->SetNumberField(TEXT("minor_version"), IF.Version.Number.Minor);
		Arr.Add(MakeShared<FJsonValueObject>(IJ));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("interfaces"), Arr);
	return MakeSuccessResult(Out, FString::Printf(TEXT("%d registered MetaSound interface(s)"), Arr.Num()));
#endif
}
