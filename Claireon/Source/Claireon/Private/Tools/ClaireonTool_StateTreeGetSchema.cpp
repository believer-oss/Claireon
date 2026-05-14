// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeGetSchema.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreePropertyBindings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_StateTreeGetSchema::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeGetSchema::GetOperation() const { return TEXT("get_schema"); }

FString ClaireonTool_StateTreeGetSchema::GetDescription() const
{
	return TEXT("Read the State Tree asset's Schema (read-only). Returns schema_class + schema_class_path, a properties map (one entry per UPROPERTY on the Schema UClass with type + ExportText value), and a context_data_descs array (one entry per Context-source FStateTreeBindableStructDesc emitted by VisitGlobalNodes; each record has name, guid, struct, source_type, state_path). Stateless / read-only / non-session. Response size scales with the schema's reflected component count; expect 10-100 records for typical actor schemas.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeGetSchema::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the State Tree (e.g. /Game/AI/ST_FlowprintsPuppet.ST_FlowprintsPuppet)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_StateTreeGetSchema::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString Error;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, Error);
	if (!StateTree)
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(StateTree, Error);
	if (!EditorData)
	{
		return MakeErrorResult(Error);
	}

	UStateTreeSchema* Schema = EditorData->Schema;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	if (!Schema)
	{
		Data->SetField(TEXT("schema_class"), MakeShared<FJsonValueNull>());
		Data->SetField(TEXT("schema_class_path"), MakeShared<FJsonValueNull>());
		Data->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		Data->SetArrayField(TEXT("context_data_descs"), TArray<TSharedPtr<FJsonValue>>());
		return MakeSuccessResult(Data, TEXT("Schema is null on EditorData."));
	}

	UClass* SchemaClass = Schema->GetClass();
	Data->SetStringField(TEXT("schema_class"), SchemaClass->GetName());
	Data->SetStringField(TEXT("schema_class_path"), SchemaClass->GetPathName());

	// Properties map: iterate UPROPERTY fields on Schema's UClass.
	TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(SchemaClass); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		const FString TypeStr = Prop->GetCPPType();
		FString ValueStr;
		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(Schema);
		Prop->ExportText_Direct(ValueStr, PropAddr, PropAddr, Schema, PPF_None);

		TSharedPtr<FJsonObject> PropEntry = MakeShared<FJsonObject>();
		PropEntry->SetStringField(TEXT("type"), TypeStr);
		PropEntry->SetStringField(TEXT("value"), ValueStr);
		PropsJson->SetObjectField(Prop->GetName(), PropEntry);
	}
	Data->SetObjectField(TEXT("properties"), PropsJson);

	// Context-data descs: VisitGlobalNodes filtered to DataSource == Context.
	TArray<TSharedPtr<FJsonValue>> ContextArray;
	EditorData->VisitGlobalNodes(
		[&ContextArray](const UStateTreeState* /*State*/,
						const FStateTreeBindableStructDesc& Desc,
						const FStateTreeDataView /*Value*/) -> EStateTreeVisitor
		{
			if (Desc.DataSource == EStateTreeBindableStructSource::Context)
			{
				ContextArray.Add(MakeShared<FJsonValueObject>(
					ClaireonStateTreeHelpers::EmitBindingSourceRecord(Desc)));
			}
			return EStateTreeVisitor::Continue;
		});
	Data->SetArrayField(TEXT("context_data_descs"), ContextArray);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Read schema '%s'."), *SchemaClass->GetName()));
}
