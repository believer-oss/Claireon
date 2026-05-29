// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_DumpGraph.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAudioHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#undef CLAIREON_HAS_METASOUND_DOC_API
#define CLAIREON_HAS_METASOUND_DOC_API 0
#if __has_include("MetasoundDocumentInterface.h") && __has_include("MetasoundFrontendDocument.h")
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#undef CLAIREON_HAS_METASOUND_DOC_API
#define CLAIREON_HAS_METASOUND_DOC_API 1
#endif

FString FClaireonMetaSoundTool_DumpGraph::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_DumpGraph::GetOperation() const { return TEXT("dump_graph"); }

FString FClaireonMetaSoundTool_DumpGraph::GetDescription() const
{
	return TEXT("Stateless dump of a MetaSound asset's graph (UMetaSoundSource or UMetaSoundPatch). "
				"Walks IMetaSoundDocumentInterface to emit "
				"{nodes:[{id, class_id, name}], edges:[{from_node, from_vertex, to_node, to_vertex}], "
				"inputs:[{name, type_name}], outputs:[{name, type_name}], interfaces:[...]}. "
				"D5: \"is this asset what we expected\" without blind probes via the Python builder. "
				"D6 workaround: bypasses the broken Python tuple shapes "
				"(find_graph_input_node, get_node_output_data, find_or_begin_building all return "
				"tuples that Python users must destructure -- this tool reads the document directly "
				"and emits flat JSON).");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_DumpGraph::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to a UMetaSoundSource or UMetaSoundPatch asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_DumpGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !CLAIREON_HAS_METASOUND_DOC_API
	return MakeErrorResult(TEXT("MetaSound document API not available on this engine branch"));
#else
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString LoadError;
	UObject* Asset = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, LoadError);
	if (!Asset) return MakeErrorResult(LoadError);
	if (Kind != EClaireonAudioAssetKind::MetaSoundSource && Kind != EClaireonAudioAssetKind::MetaSoundPatch)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a MetaSound asset: %s"), *AssetPath));
	}

	const IMetaSoundDocumentInterface* DocIFace = Cast<IMetaSoundDocumentInterface>(Asset);
	if (!DocIFace)
	{
		return MakeErrorResult(TEXT("Asset does not implement IMetaSoundDocumentInterface"));
	}
	const FMetasoundFrontendDocument& Doc = DocIFace->GetConstDocument();
	const FMetasoundFrontendGraphClass& Root = Doc.RootGraph;

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	Out->SetStringField(TEXT("kind"), AudioAssetKindToString(Kind));

	// Inputs
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FMetasoundFrontendClassInput& In : Root.Interface.Inputs)
		{
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), In.Name.ToString());
			J->SetStringField(TEXT("type_name"), In.TypeName.ToString());
			Arr.Add(MakeShared<FJsonValueObject>(J));
		}
		Out->SetArrayField(TEXT("inputs"), Arr);
	}

	// Outputs
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FMetasoundFrontendClassOutput& O : Root.Interface.Outputs)
		{
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("name"), O.Name.ToString());
			J->SetStringField(TEXT("type_name"), O.TypeName.ToString());
			Arr.Add(MakeShared<FJsonValueObject>(J));
		}
		Out->SetArrayField(TEXT("outputs"), Arr);
	}

	// Nodes + Edges -- iterate the first non-paged graph page (preset / multi-page API >= 5.5)
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	TArray<TSharedPtr<FJsonValue>> EdgesArr;
	bool bEmitted = false;
	Root.IterateGraphPages([&](const FMetasoundFrontendGraph& G)
	{
		if (bEmitted) return;
		for (const FMetasoundFrontendNode& N : G.Nodes)
		{
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("id"), N.GetID().ToString());
			J->SetStringField(TEXT("class_id"), N.ClassID.ToString());
			J->SetStringField(TEXT("name"), N.Name.ToString());
			NodesArr.Add(MakeShared<FJsonValueObject>(J));
		}
		for (const FMetasoundFrontendEdge& E : G.Edges)
		{
			TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetStringField(TEXT("from_node"), E.FromNodeID.ToString());
			J->SetStringField(TEXT("from_vertex"), E.FromVertexID.ToString());
			J->SetStringField(TEXT("to_node"), E.ToNodeID.ToString());
			J->SetStringField(TEXT("to_vertex"), E.ToVertexID.ToString());
			EdgesArr.Add(MakeShared<FJsonValueObject>(J));
		}
		bEmitted = true;
	});
	Out->SetArrayField(TEXT("nodes"), NodesArr);
	Out->SetArrayField(TEXT("edges"), EdgesArr);

	// Interfaces declared on this document
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FMetasoundFrontendVersion& V : Doc.Interfaces)
		{
			Arr.Add(MakeShared<FJsonValueString>(V.Name.ToString()));
		}
		Out->SetArrayField(TEXT("interfaces"), Arr);
	}

	const FString Summary = FString::Printf(TEXT("Dumped %s: %d nodes, %d edges, %d inputs, %d outputs"),
		*Asset->GetName(), NodesArr.Num(), EdgesArr.Num(),
		Root.Interface.Inputs.Num(), Root.Interface.Outputs.Num());
	return MakeSuccessResult(Out, Summary);
#endif
}
