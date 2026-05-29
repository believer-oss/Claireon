// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

#include "ClaireonPathResolver.h"
#include "Tools/ClaireonPropertyUtils.h"

// Runtime audio types
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"

// MetaSound document types (runtime-module only; no Editor modules touched)
#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#endif
#if __has_include("Metasound.h")
#include "Metasound.h" // UMetaSoundPatch
#endif
#if __has_include("MetasoundDocumentInterface.h")
#include "MetasoundDocumentInterface.h" // IMetaSoundDocumentInterface
#endif
#if __has_include("MetasoundFrontendDocument.h")
#include "MetasoundFrontendDocument.h"
#endif

// Editor-only: USoundCueGraphNode lives in AudioEditor/Classes/. Include from Private *.cpp only
// (the public header must not expose editor modules -- see DEPENDENCIES.md "Editor-only / runtime
// module split"). The header has MinimalAPI, so we cast through it to reach the UEdGraphNode
// NodePosX/NodePosY inherited from UEdGraphNode_Base without depending on the AudioEditor class
// being fully exported. We use a generic Cast through UEdGraphNode for positions.
#include "EdGraph/EdGraphNode.h"

// -----------------------------------------------------------------------------
// Local helpers
// -----------------------------------------------------------------------------

namespace
{
	FString AudioHelpers_ToSnakeCase(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 4);
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (FChar::IsUpper(C))
			{
				if (i > 0)
				{
					const TCHAR Prev = In[i - 1];
					const bool bPrevLower = FChar::IsLower(Prev);
					const bool bNextLower = (i + 1 < In.Len()) && FChar::IsLower(In[i + 1]);
					if (bPrevLower || (FChar::IsUpper(Prev) && bNextLower))
					{
						Out.AppendChar(TEXT('_'));
					}
				}
				Out.AppendChar(FChar::ToLower(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}

	/** Enum value -> string name. Returns empty on failure (enum not found / invalid index). */
	FString AudioHelpers_EnumByteToNameString(UEnum* Enum, uint8 Value)
	{
		if (!Enum) return FString();
		return Enum->GetNameStringByValue(static_cast<int64>(Value));
	}

	/** Serialize an FProperty value at ValuePtr into a JSON value. Enums are emitted as name strings. */
	TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Prop, const void* ValuePtr)
	{
		if (!Prop || !ValuePtr) return MakeShared<FJsonValueNull>();

		// TEnumAsByte / ByteProperty with enum metadata -> emit enum name string
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (ByteProp->Enum)
			{
				const uint8 V = ByteProp->GetPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(AudioHelpers_EnumByteToNameString(ByteProp->Enum, V));
			}
			return MakeShared<FJsonValueNumber>(ByteProp->GetPropertyValue(ValuePtr));
		}
		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			const int64 V = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(EnumProp->GetEnum()->GetNameStringByValue(V));
		}

		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}
		if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			if (NumProp->IsFloatingPoint())
			{
				return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(NumProp->GetSignedIntPropertyValue(ValuePtr)));
		}
		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(Obj ? Obj->GetPathName() : FString());
		}

		// Fallback: ExportText_Direct
		FString Exported;
		Prop->ExportText_Direct(Exported, ValuePtr, ValuePtr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}

	/** Dump a struct's FProperties into a JSON object (shallow; nested structs are recursed via GetAllProperties-ish rules below). */
	TSharedPtr<FJsonObject> DumpStructToJson(UScriptStruct* Struct, const void* StructPtr)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Struct || !StructPtr) return Out;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructPtr);

			// Recurse into nested USTRUCT for FStructProperty
			if (FStructProperty* InnerStruct = CastField<FStructProperty>(Prop))
			{
				Out->SetObjectField(Prop->GetName(), DumpStructToJson(InnerStruct->Struct, ValuePtr));
				continue;
			}
			// Array: emit as array of JSON values
			if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
			{
				FScriptArrayHelper Helper(ArrProp, ValuePtr);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
				{
					if (FStructProperty* InnerSP = CastField<FStructProperty>(ArrProp->Inner))
					{
						Arr.Add(MakeShared<FJsonValueObject>(DumpStructToJson(InnerSP->Struct, Helper.GetRawPtr(Idx))));
					}
					else
					{
						Arr.Add(PropertyValueToJson(ArrProp->Inner, Helper.GetRawPtr(Idx)));
					}
				}
				Out->SetArrayField(Prop->GetName(), Arr);
				continue;
			}

			Out->SetField(Prop->GetName(), PropertyValueToJson(Prop, ValuePtr));
		}
		return Out;
	}

	/** Dump a UObject's reflected properties into a JSON object (shallow, instance-level, skips transients). */
	TSharedPtr<FJsonObject> DumpObjectToJson(const UObject* Obj)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Obj) return Out;
		UClass* Cls = Obj->GetClass();
		for (TFieldIterator<FProperty> It(Cls); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Transient)) continue;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

			if (FStructProperty* InnerStruct = CastField<FStructProperty>(Prop))
			{
				Out->SetObjectField(Prop->GetName(), DumpStructToJson(InnerStruct->Struct, ValuePtr));
				continue;
			}
			if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
			{
				FScriptArrayHelper Helper(ArrProp, ValuePtr);
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (int32 Idx = 0; Idx < Helper.Num(); ++Idx)
				{
					if (FStructProperty* InnerSP = CastField<FStructProperty>(ArrProp->Inner))
					{
						Arr.Add(MakeShared<FJsonValueObject>(DumpStructToJson(InnerSP->Struct, Helper.GetRawPtr(Idx))));
					}
					else
					{
						Arr.Add(PropertyValueToJson(ArrProp->Inner, Helper.GetRawPtr(Idx)));
					}
				}
				Out->SetArrayField(Prop->GetName(), Arr);
				continue;
			}
			Out->SetField(Prop->GetName(), PropertyValueToJson(Prop, ValuePtr));
		}
		return Out;
	}
}

// -----------------------------------------------------------------------------
// Kind <-> string round-trip
// -----------------------------------------------------------------------------

FString AudioAssetKindToString(EClaireonAudioAssetKind Kind)
{
	switch (Kind)
	{
	case EClaireonAudioAssetKind::SoundCue:        return TEXT("sound_cue");
	case EClaireonAudioAssetKind::MetaSoundSource: return TEXT("metasound_source");
	case EClaireonAudioAssetKind::MetaSoundPatch:  return TEXT("metasound_patch");
	case EClaireonAudioAssetKind::SoundClass:      return TEXT("sound_class");
	case EClaireonAudioAssetKind::SoundMix:        return TEXT("sound_mix");
	case EClaireonAudioAssetKind::Attenuation:     return TEXT("attenuation");
	case EClaireonAudioAssetKind::Concurrency:     return TEXT("concurrency");
	case EClaireonAudioAssetKind::Unknown:
	default:
		return TEXT("unknown");
	}
}

EClaireonAudioAssetKind AudioAssetKindFromString(FStringView Str)
{
	if (Str.Equals(TEXTVIEW("sound_cue")))         return EClaireonAudioAssetKind::SoundCue;
	if (Str.Equals(TEXTVIEW("metasound_source")))  return EClaireonAudioAssetKind::MetaSoundSource;
	if (Str.Equals(TEXTVIEW("metasound_patch")))   return EClaireonAudioAssetKind::MetaSoundPatch;
	if (Str.Equals(TEXTVIEW("sound_class")))       return EClaireonAudioAssetKind::SoundClass;
	if (Str.Equals(TEXTVIEW("sound_mix")))         return EClaireonAudioAssetKind::SoundMix;
	if (Str.Equals(TEXTVIEW("attenuation")))       return EClaireonAudioAssetKind::Attenuation;
	if (Str.Equals(TEXTVIEW("concurrency")))       return EClaireonAudioAssetKind::Concurrency;
	return EClaireonAudioAssetKind::Unknown;
}

FString ClaireonAudioHelpers::JsonValueToString(const TSharedPtr<FJsonValue>& V)
{
	if (!V.IsValid()) return FString();
	FString S;
	if (V->TryGetString(S)) return S;
	double N;
	bool B;
	if (V->TryGetNumber(N))
	{
		if (FMath::IsFinite(N) && FMath::Floor(N) == N && FMath::Abs(N) < 1e15)
		{
			return FString::Printf(TEXT("%lld"), (int64)N);
		}
		return FString::Printf(TEXT("%g"), N);
	}
	if (V->TryGetBool(B)) return B ? TEXT("true") : TEXT("false");
	return FString();
}

EClaireonAudioAssetKind ResolveAudioAssetKindFromPath(const FString& AssetPath, FString& OutError)
{
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, OutError);
	if (!Obj)
	{
		return EClaireonAudioAssetKind::Unknown;
	}
	return Kind;
}

// -----------------------------------------------------------------------------
// Implementations
// -----------------------------------------------------------------------------

namespace ClaireonAudioHelpers
{
	static EClaireonAudioAssetKind ClassifyAsset(const UObject* Obj)
	{
		if (!Obj) return EClaireonAudioAssetKind::Unknown;
		// MetaSoundSource inherits from USoundWaveProcedural which inherits from USoundBase -
		// check it BEFORE USoundCue (USoundCue is a different USoundBase subclass, not a parent).
		if (Obj->IsA(USoundCue::StaticClass()))          return EClaireonAudioAssetKind::SoundCue;
#if __has_include("MetasoundSource.h")
		if (Obj->IsA(UMetaSoundSource::StaticClass()))   return EClaireonAudioAssetKind::MetaSoundSource;
#endif
#if __has_include("Metasound.h")
		// UMetaSoundPatch is a UObject (NOT a USoundBase subclass), declared in Metasound.h.
		// Implements IMetaSoundDocumentInterface, so it shares the read/edit path with MetaSoundSource.
		if (Obj->IsA(UMetaSoundPatch::StaticClass()))    return EClaireonAudioAssetKind::MetaSoundPatch;
#endif
		if (Obj->IsA(USoundClass::StaticClass()))        return EClaireonAudioAssetKind::SoundClass;
		if (Obj->IsA(USoundMix::StaticClass()))          return EClaireonAudioAssetKind::SoundMix;
		if (Obj->IsA(USoundAttenuation::StaticClass()))  return EClaireonAudioAssetKind::Attenuation;
		if (Obj->IsA(USoundConcurrency::StaticClass()))  return EClaireonAudioAssetKind::Concurrency;
		return EClaireonAudioAssetKind::Unknown;
	}

	UObject* LoadAudioAsset(const FString& AssetPath, EClaireonAudioAssetKind& OutKind, FString& OutError)
	{
		OutKind = EClaireonAudioAssetKind::Unknown;

		ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetPath);
		if (!Resolved.bSuccess)
		{
			OutError = FString::Printf(TEXT("Could not resolve asset path '%s': %s"), *AssetPath, *Resolved.Error);
			return nullptr;
		}

		UObject* Obj = LoadObject<UObject>(nullptr, *Resolved.ResolvedPath.Path);
		if (!Obj)
		{
			OutError = FString::Printf(TEXT("Failed to load audio asset at '%s'"), *Resolved.ResolvedPath.Path);
			return nullptr;
		}

		const EClaireonAudioAssetKind Kind = ClassifyAsset(Obj);
		if (Kind == EClaireonAudioAssetKind::Unknown)
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a supported audio asset (class=%s)"),
				*Resolved.ResolvedPath.Path, *Obj->GetClass()->GetName());
			return nullptr;
		}

		OutKind = Kind;
		return Obj;
	}

	const TMap<FName, UClass*>& GetSoundNodeClassRegistry()
	{
		static TMap<FName, UClass*> Registry;
		static bool bInitialized = false;
		if (!bInitialized)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Cls = *It;
				if (!Cls->IsChildOf(USoundNode::StaticClass())) continue;
				if (Cls == USoundNode::StaticClass()) continue;
				if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

				FString Short = Cls->GetName();
				Short.RemoveFromStart(TEXT("SoundNode"));
				Registry.Add(FName(*AudioHelpers_ToSnakeCase(Short)), Cls);
			}
			bInitialized = true;
		}
		return Registry;
	}

	UClass* ResolveSoundNodeClass(FName ShortName)
	{
		if (const UClass* const* Found = GetSoundNodeClassRegistry().Find(ShortName))
		{
			return const_cast<UClass*>(*Found);
		}
		return nullptr;
	}

	static FString SoundNodeShortName(const USoundNode* Node)
	{
		if (!Node) return FString();
		FString S = Node->GetClass()->GetName();
		S.RemoveFromStart(TEXT("SoundNode"));
		return AudioHelpers_ToSnakeCase(S);
	}

	void FormatSoundCueGraph(const USoundCue* SoundCue, const TSharedRef<FJsonObject>& OutJson,
							 EClaireonAudioDetailLevel DetailLevel, const FString& /*FocusHint*/)
	{
		if (!SoundCue) return;

#if WITH_EDITORONLY_DATA
		const TArray<TObjectPtr<USoundNode>>& Nodes = SoundCue->AllNodes;
#else
		const TArray<TObjectPtr<USoundNode>> Nodes;
#endif

		OutJson->SetStringField(TEXT("asset_path"), SoundCue->GetPathName());
		OutJson->SetNumberField(TEXT("volume_multiplier"), SoundCue->VolumeMultiplier);
		OutJson->SetNumberField(TEXT("pitch_multiplier"), SoundCue->PitchMultiplier);
		OutJson->SetBoolField(TEXT("override_attenuation"), SoundCue->bOverrideAttenuation != 0);

		int32 FirstIndex = INDEX_NONE;
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			if (Nodes[i] == SoundCue->FirstNode)
			{
				FirstIndex = i;
				break;
			}
		}
		OutJson->SetNumberField(TEXT("first_node_index"), FirstIndex);

		TArray<TSharedPtr<FJsonValue>> NodesArr;
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const USoundNode* Node = Nodes[i];
			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetNumberField(TEXT("index"), i);
			NodeJson->SetStringField(TEXT("class"), SoundNodeShortName(Node));

			int32 PosX = 0;
			int32 PosY = 0;
			if (Node && Node->GraphNode)
			{
				PosX = Node->GraphNode->NodePosX;
				PosY = Node->GraphNode->NodePosY;
			}
			NodeJson->SetNumberField(TEXT("pos_x"), PosX);
			NodeJson->SetNumberField(TEXT("pos_y"), PosY);

			TArray<TSharedPtr<FJsonValue>> ChildIdx;
			if (Node)
			{
				for (const TObjectPtr<USoundNode>& Child : Node->ChildNodes)
				{
					int32 Idx = INDEX_NONE;
					for (int32 j = 0; j < Nodes.Num(); ++j)
					{
						if (Nodes[j] == Child) { Idx = j; break; }
					}
					ChildIdx.Add(MakeShared<FJsonValueNumber>(Idx));
				}
			}
			NodeJson->SetArrayField(TEXT("child_indices"), ChildIdx);

			if (DetailLevel == EClaireonAudioDetailLevel::Full && Node)
			{
				NodeJson->SetObjectField(TEXT("properties"), DumpObjectToJson(Node));
			}
			NodesArr.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		OutJson->SetArrayField(TEXT("nodes"), NodesArr);
	}

	void FormatMetaSoundDocument(const UObject* DocumentObject, const TSharedRef<FJsonObject>& OutJson,
								 EClaireonAudioDetailLevel /*DetailLevel*/, const FString& /*FocusHint*/)
	{
		if (!DocumentObject) return;
		OutJson->SetStringField(TEXT("asset_path"), DocumentObject->GetPathName());

#if __has_include("MetasoundFrontendDocument.h") && __has_include("MetasoundDocumentInterface.h")
		// Dispatch via IMetaSoundDocumentInterface so this routine works for both
		// UMetaSoundSource and UMetaSoundPatch (and any future class implementing the interface).
		const IMetaSoundDocumentInterface* DocIFace = Cast<IMetaSoundDocumentInterface>(DocumentObject);
		if (!DocIFace)
		{
			OutJson->SetStringField(TEXT("note"), TEXT("Asset does not implement IMetaSoundDocumentInterface"));
			return;
		}
		const FMetasoundFrontendDocument& Doc = DocIFace->GetConstDocument();
		const FMetasoundFrontendGraphClass& Root = Doc.RootGraph;

		// Inputs from the root class (declared interface of the graph)
		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const FMetasoundFrontendClassInput& In : Root.Interface.Inputs)
		{
			TSharedPtr<FJsonObject> InJ = MakeShared<FJsonObject>();
			InJ->SetStringField(TEXT("name"), In.Name.ToString());
			InJ->SetStringField(TEXT("type_name"), In.TypeName.ToString());
			InputsArr.Add(MakeShared<FJsonValueObject>(InJ));
		}
		OutJson->SetArrayField(TEXT("inputs"), InputsArr);

		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (const FMetasoundFrontendClassOutput& Out : Root.Interface.Outputs)
		{
			TSharedPtr<FJsonObject> OJ = MakeShared<FJsonObject>();
			OJ->SetStringField(TEXT("name"), Out.Name.ToString());
			OJ->SetStringField(TEXT("type_name"), Out.TypeName.ToString());
			OutputsArr.Add(MakeShared<FJsonValueObject>(OJ));
		}
		OutJson->SetArrayField(TEXT("outputs"), OutputsArr);

		// Graph nodes: prefer the default (non-paged) graph. The preset / multi-page API landed in 5.5;
		// we pull from the first available graph page to stay compatible.
		TArray<TSharedPtr<FJsonValue>> NodesArr;
		auto EmitNodes = [&NodesArr](const FMetasoundFrontendGraph& G)
		{
			for (const FMetasoundFrontendNode& N : G.Nodes)
			{
				TSharedPtr<FJsonObject> NJ = MakeShared<FJsonObject>();
				NJ->SetStringField(TEXT("id"), N.GetID().ToString());
				NJ->SetStringField(TEXT("class_id"), N.ClassID.ToString());
				NJ->SetStringField(TEXT("name"), N.Name.ToString());
				NodesArr.Add(MakeShared<FJsonValueObject>(NJ));
			}
		};
		bool bEmitted = false;
		Root.IterateGraphPages([&](const FMetasoundFrontendGraph& G)
		{
			if (!bEmitted)
			{
				EmitNodes(G);
				bEmitted = true;
			}
		});
		OutJson->SetArrayField(TEXT("nodes"), NodesArr);

		// Interfaces set (output format identification)
		TArray<TSharedPtr<FJsonValue>> IFacesArr;
		for (const FMetasoundFrontendVersion& V : Doc.Interfaces)
		{
			IFacesArr.Add(MakeShared<FJsonValueString>(V.Name.ToString()));
		}
		OutJson->SetArrayField(TEXT("interfaces"), IFacesArr);
#else
		OutJson->SetStringField(TEXT("note"), TEXT("MetasoundFrontendDocument.h not available in this build"));
#endif
	}

	void FormatAttenuationSettings(const USoundAttenuation* Attenuation, const TSharedRef<FJsonObject>& OutJson)
	{
		if (!Attenuation) return;
		OutJson->SetStringField(TEXT("asset_path"), Attenuation->GetPathName());
		UScriptStruct* SS = FSoundAttenuationSettings::StaticStruct();
		OutJson->SetObjectField(TEXT("settings"), DumpStructToJson(SS, &Attenuation->Attenuation));
	}

	void FormatConcurrencySettings(const USoundConcurrency* Concurrency, const TSharedRef<FJsonObject>& OutJson)
	{
		if (!Concurrency) return;
		OutJson->SetStringField(TEXT("asset_path"), Concurrency->GetPathName());
		UScriptStruct* SS = FSoundConcurrencySettings::StaticStruct();
		OutJson->SetObjectField(TEXT("settings"), DumpStructToJson(SS, &Concurrency->Concurrency));
	}

	bool SetSoundNodeProperty(USoundNode* Node, FName FieldName, const TSharedPtr<FJsonValue>& ValueJson,
							  FString& OutError)
	{
		if (!Node)
		{
			OutError = TEXT("Node is null");
			return false;
		}
		if (FieldName.IsNone())
		{
			OutError = TEXT("FieldName is None");
			return false;
		}
		// Convert JSON to string for ClaireonPropertyUtils::WritePropertyByPath (reuses existing setter).
		FString AsString;
		if (ValueJson.IsValid())
		{
			if (ValueJson->Type == EJson::String) AsString = ValueJson->AsString();
			else if (ValueJson->Type == EJson::Number) AsString = FString::Printf(TEXT("%g"), ValueJson->AsNumber());
			else if (ValueJson->Type == EJson::Boolean) AsString = ValueJson->AsBool() ? TEXT("true") : TEXT("false");
			else
			{
				// fallback: stringify via exporter
				AsString = FString();
			}
		}
		return ClaireonPropertyUtils::WritePropertyByPath(Node, FieldName.ToString(), AsString, OutError);
	}

	void IterateSoundClassPropertiesStruct(FSoundClassProperties& Props,
										   TFunctionRef<void(FProperty*, void*)> Callback)
	{
		UScriptStruct* SS = FSoundClassProperties::StaticStruct();
		if (!SS) return;
		for (TFieldIterator<FProperty> It(SS); It; ++It)
		{
			FProperty* Prop = *It;
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(&Props);
			Callback(Prop, ValuePtr);
		}
	}
}
