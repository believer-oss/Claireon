// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"

#include "ClaireonAudioHelpers.generated.h"

// Runtime-only audio forward declarations (editor-only types are NOT exposed here by design -
// see Docs/llm/archive/audio-tools/AUDIO_TOOLS_DEPENDENCIES.md "Editor-only / runtime module split" section).
class UClass;
class UObject;
class USoundCue;
class USoundNode;
class USoundClass;
class USoundMix;
class USoundAttenuation;
class USoundConcurrency;
class UMetaSoundSource;
class UMetaSoundPatch;
struct FSoundClassProperties;

/**
 * Classification of audio asset kinds the audio_* tools operate on.
 * String form used in JSON output is snake_case lowercase (see AudioAssetKindToString).
 */
UENUM()
enum class EClaireonAudioAssetKind : uint8
{
	Unknown,
	SoundCue,
	MetaSoundSource,
	MetaSoundPatch,
	SoundClass,
	SoundMix,
	Attenuation,
	Concurrency,
};

/**
 * Detail level used by audio formatters when emitting JSON. Defined locally for the audio tool
 * surface; mirrors the summary/full distinction used elsewhere in Claireon.
 */
enum class EClaireonAudioDetailLevel : uint8
{
	Summary,
	Full,
};

/** Round-trips EClaireonAudioAssetKind to snake_case strings. */
FString AudioAssetKindToString(EClaireonAudioAssetKind Kind);
EClaireonAudioAssetKind AudioAssetKindFromString(FStringView Str);

/**
 * Path-to-kind classifier (no session, no mutation, no asset retention). Resolves AssetPath via
 * ClaireonPathResolver, loads the UObject, classifies into EClaireonAudioAssetKind, and returns the
 * kind. Returns EClaireonAudioAssetKind::Unknown and writes to OutError on failure.
 * Migrated from the bundled audio_edit Operation_Open per AUDIO_DECOMPOSE_SESSION_REGISTRY.md
 * (M-ResolveAudioAssetKindFromPath). Used by per-cohort _open tools to validate the asset kind
 * matches the cohort before opening a session.
 */
EClaireonAudioAssetKind ResolveAudioAssetKindFromPath(const FString& AssetPath, FString& OutError);

/**
 * Helper functions for the audio_inspect / audio_edit / audio_apply tools.
 * Parallels ClaireonNiagaraHelpers. All signatures here are the stable surface; implementations
 * land incrementally across stages 000-006.
 */
namespace ClaireonAudioHelpers
{
	/** Resolves AssetPath via ClaireonPathResolver::Resolve, loads the UObject, classifies into OutKind.
	 *  Returns nullptr and writes to OutError on failure (unresolved path, load failure, unsupported class). */
	UObject* LoadAudioAsset(const FString& AssetPath, EClaireonAudioAssetKind& OutKind, FString& OutError);

	/** Convert a JSON value to string form for property writes (matches the bundled tool's
	 *  JsonValueToString helper). Numbers are formatted as integers when finite-and-whole. */
	FString JsonValueToString(const TSharedPtr<FJsonValue>& V);

	/** Reflection-populated short-name -> USoundNode subclass map. Lazy-initialized on first call.
	 *  Short names: strip "SoundNode" prefix, convert to snake_case. e.g. USoundNodeWavePlayer -> "wave_player". */
	const TMap<FName, UClass*>& GetSoundNodeClassRegistry();

	/** Map lookup. Returns nullptr if ShortName is not registered. */
	UClass* ResolveSoundNodeClass(FName ShortName);

	/** Walks SoundCue->AllNodes, emits node type, reflected properties, parent->child wiring via ChildNodes,
	 *  and EdGraph position via Cast<USoundCueGraphNode>(Node->GraphNode)->NodePosX / NodePosY. */
	void FormatSoundCueGraph(const USoundCue* SoundCue, const TSharedRef<FJsonObject>& OutJson,
							 EClaireonAudioDetailLevel DetailLevel, const FString& FocusHint);

	/** Uses IMetaSoundDocumentInterface::GetConstDocument() and Metasound::Frontend graph-handle API.
	 *  Emits graph nodes, inputs, outputs, output format GUID. Read-only. Accepts any UObject that
	 *  implements IMetaSoundDocumentInterface (UMetaSoundSource, UMetaSoundPatch) [D1/D2]. */
	void FormatMetaSoundDocument(const UObject* DocumentObject, const TSharedRef<FJsonObject>& OutJson,
								 EClaireonAudioDetailLevel DetailLevel, const FString& FocusHint);

	/** Dumps FSoundAttenuationSettings via reflection: shape, distance model, spatialization toggle,
	 *  focus settings, reverb send, submix send. */
	void FormatAttenuationSettings(const USoundAttenuation* Attenuation, const TSharedRef<FJsonObject>& OutJson);

	/** Dumps FSoundConcurrencySettings via reflection. ResolutionRule emits as its enum name (e.g. "StopOldest"),
	 *  not integer. */
	void FormatConcurrencySettings(const USoundConcurrency* Concurrency, const TSharedRef<FJsonObject>& OutJson);

	/** Reflection-based property setter. Wraps ClaireonPropertyUtils::SetObjectProperty so PreEditChange /
	 *  PostEditChangeProperty fire. */
	bool SetSoundNodeProperty(USoundNode* Node, FName FieldName, const TSharedPtr<FJsonValue>& ValueJson,
							  FString& OutError);

	/** Iterates FSoundClassProperties UScriptStruct via reflection (D6). For each FProperty* on the struct,
	 *  invokes Callback(FProperty*, void* ValuePtr). ValuePtr points into the caller's instance of the struct. */
	void IterateSoundClassPropertiesStruct(FSoundClassProperties& Props,
										   TFunctionRef<void(FProperty*, void*)> Callback);
}
