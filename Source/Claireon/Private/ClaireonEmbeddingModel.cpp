// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonEmbeddingModel.h"

// NNE headers included here (private, not in the public interface).
// NNE.h pulls in NNERuntime.h and the GetRuntime<T> template.
// NNERuntimeCPU.h pulls in NNERuntimeRunSync.h for IModelInstanceCPU / RunSync.
// NNEModelData.h is needed for UNNEModelData (a UObject).
// NNETypes.h gives FTensorDesc / FTensorShape / ENNETensorDataType.
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNEModelData.h"
#include "NNETypes.h"

#include "ClaireonLog.h"
#include "ClaireonWordPieceTokenizer.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

// File-local discriminator on anonymous-namespace helpers to avoid unity-batch
// symbol collisions (memory feedback_anon_namespace_unity_collision).
namespace ClaireonEmbeddingModel_Private
{
	// The ORT CPU runtime name registered by the NNERuntimeORT plugin
	// (UNNERuntimeORTCpu::GetRuntimeName() == "NNERuntimeORTCpu").
	static const FString GRuntimeName = TEXT("NNERuntimeORTCpu");

	// Known BERT input tensor names (sentence-transformers / BGE ONNX exports).
	static bool ClaireonEmb_NameIs(const FString& A, const TCHAR* B)
	{
		return A.Equals(B, ESearchCase::IgnoreCase);
	}
}

// ---------------------------------------------------------------------------
// Per-model metadata literals (ground truth: the two MODEL_INFO.json files).
// ---------------------------------------------------------------------------

FClaireonEmbedderMeta FClaireonEmbedderMeta::BGE()
{
	FClaireonEmbedderMeta M;
	M.ModelDir    = TEXT("bge-small-en-v1.5-int8");
	M.ModelFile   = TEXT("model.onnx");
	M.VocabFile   = TEXT("vocab.txt");
	M.Dim         = 384;
	M.SeqLen      = 64;
	M.Pooling     = EPooling::Cls;        // BGE retrieval uses CLS-token pooling.
	// Query-side instruction (s2p retrieval); applied ONLY to queries, not docs.
	M.QueryPrefix = TEXT("Represent this sentence for searching relevant passages: ");
	M.DocPrefix   = TEXT("");
	M.bNormalize  = true;
	return M;
}

// ---------------------------------------------------------------------------
// FClaireonEmbeddingModel
// ---------------------------------------------------------------------------

// Special members defined here (not inline in the header) so the
// TStrongObjectPtr<UNNEModelData> member instantiates against the COMPLETE
// UNNEModelData type (NNEModelData.h is included above). See the header note.
FClaireonEmbeddingModel::FClaireonEmbeddingModel() = default;
FClaireonEmbeddingModel::~FClaireonEmbeddingModel() = default;
FClaireonEmbeddingModel::FClaireonEmbeddingModel(FClaireonEmbeddingModel&&) = default;
FClaireonEmbeddingModel& FClaireonEmbeddingModel::operator=(FClaireonEmbeddingModel&&) = default;

bool FClaireonEmbeddingModel::Load(const FClaireonEmbedderMeta& InMeta)
{
	using namespace UE::NNE;
	using namespace ClaireonEmbeddingModel_Private;

	// Reset any prior session/state so Load() is idempotent.
	ModelInstance.Reset();
	ModelData.Reset();
	InputElemTypes.Reset();
	InputIdxInputIds = INDEX_NONE;
	InputIdxAttentionMask = INDEX_NONE;
	InputIdxTokenTypeIds = INDEX_NONE;
	bOutputIsLastHiddenState = true;
	Meta = InMeta;

	// 1. Resolve the model path under the Claireon plugin base dir.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] Claireon plugin not found; semantic search disabled."));
		return false;
	}

	const FString ModelPath = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Models"), Meta.ModelDir, Meta.ModelFile);

	TArray64<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *ModelPath))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] Failed to load ONNX model '%s'; semantic search disabled."),
			*ModelPath);
		return false;
	}

	// 2. Obtain the ORT CPU runtime. Absent runtime -> caller falls back to lexical.
	TWeakInterfacePtr<INNERuntimeCPU> Runtime = GetRuntime<INNERuntimeCPU>(GRuntimeName);
	if (!Runtime.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] NNE runtime '%s' unavailable; semantic search disabled "
				 "(lexical fallback)."), *GRuntimeName);
		return false;
	}

	// 3. Import the raw bytes into a UNNEModelData UObject.
	//    LIFETIME: UNNEModelData is a UObject and this class is plain C++, so it
	//    cannot be a UPROPERTY. We root it via TStrongObjectPtr for the lifetime
	//    of this model (a raw UObject* would be GC'd out from under the runtime).
	UNNEModelData* RawModelData = NewObject<UNNEModelData>();
	if (!RawModelData)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EmbeddingModel] NewObject<UNNEModelData> failed."));
		return false;
	}
	ModelData.Reset(RawModelData);
	ModelData->Init(TEXT("onnx"), Bytes);
	Bytes.Empty(); // UNNEModelData copied the data; drop our copy.

	// 4. Create the CPU model + a single inference instance.
	TSharedPtr<IModelCPU> Model = Runtime->CreateModelCPU(ModelData.Get());
	if (!Model.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] CreateModelCPU failed for '%s' (opset/quant op unsupported "
				 "by ORT?); semantic search disabled."), *ModelPath);
		ModelData.Reset();
		return false;
	}

	TSharedPtr<IModelInstanceCPU> Instance = Model->CreateModelInstanceCPU();
	if (!Instance.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] CreateModelInstanceCPU failed for '%s'."), *ModelPath);
		ModelData.Reset();
		return false;
	}

	// 5. Inspect the declared input tensor descs to learn names + element types.
	//    The model decides int32 vs int64 ids; we marshal to the exact type later.
	const TConstArrayView<FTensorDesc> InputDescs = Instance->GetInputTensorDescs();
	InputElemTypes.SetNumUninitialized(InputDescs.Num());
	for (int32 i = 0; i < InputDescs.Num(); ++i)
	{
		const FTensorDesc& Desc = InputDescs[i];
		InputElemTypes[i] = static_cast<uint8>(Desc.GetDataType());

		const FString& Name = Desc.GetName();
		if (ClaireonEmb_NameIs(Name, TEXT("input_ids")))
		{
			InputIdxInputIds = i;
		}
		else if (ClaireonEmb_NameIs(Name, TEXT("attention_mask")))
		{
			InputIdxAttentionMask = i;
		}
		else if (ClaireonEmb_NameIs(Name, TEXT("token_type_ids")))
		{
			InputIdxTokenTypeIds = i;
		}

		UE_LOG(LogClaireon, Verbose,
			TEXT("[EmbeddingModel] input[%d] name='%s' type=%d rank=%d"),
			i, *Name, (int32)Desc.GetDataType(), Desc.GetShape().Rank());
	}

	// If the names are unconventional but there are exactly two inputs, assume
	// the canonical (input_ids, attention_mask) order so we can still run.
	if (InputIdxInputIds == INDEX_NONE && InputDescs.Num() >= 1)
	{
		InputIdxInputIds = 0;
	}
	if (InputIdxAttentionMask == INDEX_NONE && InputDescs.Num() >= 2 && InputIdxAttentionMask != InputIdxInputIds)
	{
		InputIdxAttentionMask = 1;
	}

	// 6. Inspect the output descs: detect raw last_hidden_state [1,SeqLen,Dim]
	//    (needs pooling) vs an already-pooled sentence embedding [1,Dim].
	const TConstArrayView<FTensorDesc> OutputDescs = Instance->GetOutputTensorDescs();
	if (OutputDescs.Num() > 0)
	{
		// Prefer a named pooled output if present (some exports add one).
		int32 ChosenOutput = 0;
		for (int32 i = 0; i < OutputDescs.Num(); ++i)
		{
			const FString& OName = OutputDescs[i].GetName();
			if (ClaireonEmb_NameIs(OName, TEXT("sentence_embedding")) ||
				ClaireonEmb_NameIs(OName, TEXT("pooler_output")))
			{
				ChosenOutput = i;
				break;
			}
		}
		const int32 OutRank = OutputDescs[ChosenOutput].GetShape().Rank();
		// rank 3 -> [batch, seq, dim] = last_hidden_state (pool here).
		// rank 2 -> [batch, dim] = already pooled (use directly).
		bOutputIsLastHiddenState = (OutRank >= 3);

		for (int32 i = 0; i < OutputDescs.Num(); ++i)
		{
			UE_LOG(LogClaireon, Verbose,
				TEXT("[EmbeddingModel] output[%d] name='%s' type=%d rank=%d"),
				i, *OutputDescs[i].GetName(), (int32)OutputDescs[i].GetDataType(),
				OutputDescs[i].GetShape().Rank());
		}
	}

	// 7. Set the fixed input shape [1, SeqLen] for every declared input and cache.
	TArray<FTensorShape> InputShapes;
	InputShapes.Reserve(InputDescs.Num());
	const uint32 SeqLenU = static_cast<uint32>(Meta.SeqLen);
	for (int32 i = 0; i < InputDescs.Num(); ++i)
	{
		// All BERT inputs are [batch=1, seq] int tensors.
		const uint32 Dims[2] = { 1u, SeqLenU };
		InputShapes.Add(FTensorShape::Make(MakeArrayView(Dims, 2)));
	}

	if (Instance->SetInputTensorShapes(InputShapes) != IModelInstanceCPU::ESetInputTensorShapesStatus::Ok)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] SetInputTensorShapes failed for '%s'."), *ModelPath);
		ModelData.Reset();
		return false;
	}

	ModelInstance = Instance;

	UE_LOG(LogClaireon, Log,
		TEXT("[EmbeddingModel] Loaded '%s' (Dim=%d SeqLen=%d pooling=%s inputs=%d "
			 "token_type_ids=%s output=%s)."),
		*Meta.ModelDir, Meta.Dim, Meta.SeqLen,
		(Meta.Pooling == FClaireonEmbedderMeta::EPooling::Mean) ? TEXT("mean") : TEXT("cls"),
		InputDescs.Num(),
		(InputIdxTokenTypeIds != INDEX_NONE) ? TEXT("yes") : TEXT("no"),
		bOutputIsLastHiddenState ? TEXT("last_hidden_state") : TEXT("pooled"));

	return true;
}

bool FClaireonEmbeddingModel::IsReady() const
{
	return ModelInstance.IsValid();
}

int32 FClaireonEmbeddingModel::Dim() const
{
	return ModelInstance.IsValid() ? Meta.Dim : 0;
}

bool FClaireonEmbeddingModel::Embed(const FString& Text, bool bIsQuery, TArray<float>& OutVec)
{
	OutVec.Reset();

	if (!IsReady())
	{
		return false;
	}

	// Embed(text) needs a tokenizer (owned by the embedding index, borrowed here).
	// Returns false until a tokenizer is set AND it is ready.
	// EmbedTokenIds() is the tokenizer-independent path for NNE inference validation.
	if (Tokenizer == nullptr || !Tokenizer->IsReady())
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[EmbeddingModel] Embed(text) has no ready tokenizer; "
				 "use EmbedTokenIds() for the tokenizer-independent smoke path."));
		return false;
	}

	// Apply the model's query/doc prefix, tokenize, then run the shared NNE path.
	const FString Prefixed = (bIsQuery ? Meta.QueryPrefix : Meta.DocPrefix) + Text;
	TArray<int32> Ids;
	TArray<int32> Mask;
	Tokenizer->Encode(Prefixed, Meta.SeqLen, Ids, Mask);
	if (Ids.Num() == 0)
	{
		return false;
	}
	return EmbedTokenIds(Ids, Mask, OutVec);
}

bool FClaireonEmbeddingModel::EmbedTokenIds(
	TConstArrayView<int32> Ids, TConstArrayView<int32> Mask, TArray<float>& OutVec)
{
	using namespace UE::NNE;

	OutVec.Reset();

	if (!IsReady())
	{
		return false;
	}

	const int32 SeqLen = Meta.SeqLen;
	const int32 Dim = Meta.Dim;
	if (SeqLen <= 0 || Dim <= 0 || InputIdxInputIds == INDEX_NONE)
	{
		return false;
	}

	// Build padded/truncated int buffers at the model's declared element type.
	// ORT BERT exports typically declare int64 inputs; some declare int32. We
	// match the declared type exactly (a mismatch silently yields garbage).
	const ENNETensorDataType IdType =
		static_cast<ENNETensorDataType>(InputElemTypes[InputIdxInputIds]);

	const bool bInt64 = (IdType == ENNETensorDataType::Int64);
	const bool bInt32 = (IdType == ENNETensorDataType::Int32);
	if (!bInt64 && !bInt32)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingModel] Unsupported input_ids element type %d (expected Int32/Int64)."),
			(int32)IdType);
		return false;
	}

	auto FillBuffer = [SeqLen, bInt64](TConstArrayView<int32> Src, TArray<uint8>& OutBuf)
	{
		const int32 ElemSize = bInt64 ? (int32)sizeof(int64) : (int32)sizeof(int32);
		OutBuf.SetNumZeroed(SeqLen * ElemSize);
		const int32 N = FMath::Min(SeqLen, Src.Num());
		if (bInt64)
		{
			int64* Dst = reinterpret_cast<int64*>(OutBuf.GetData());
			for (int32 i = 0; i < N; ++i) { Dst[i] = static_cast<int64>(Src[i]); }
		}
		else
		{
			int32* Dst = reinterpret_cast<int32*>(OutBuf.GetData());
			for (int32 i = 0; i < N; ++i) { Dst[i] = Src[i]; }
		}
	};

	TArray<uint8> IdBuf;
	TArray<uint8> MaskBuf;
	TArray<uint8> TypeBuf;   // zeroed token_type_ids (also reused for any extra input).
	FillBuffer(Ids, IdBuf);
	FillBuffer(Mask, MaskBuf);
	// token_type_ids are all zeros for single-sentence input. Always materialise a
	// zero row so any extra/unexpected declared input can bind to it too.
	FillBuffer(TConstArrayView<int32>(), TypeBuf);

	// Assemble input bindings in the model's declared input order.
	TArray<FTensorBindingCPU> InputBindings;
	InputBindings.SetNum(InputElemTypes.Num());
	const int32 ElemSize = bInt64 ? (int32)sizeof(int64) : (int32)sizeof(int32);
	const uint64 RowBytes = static_cast<uint64>(SeqLen) * ElemSize;

	InputBindings[InputIdxInputIds] = FTensorBindingCPU{ IdBuf.GetData(), RowBytes };
	if (InputIdxAttentionMask != INDEX_NONE)
	{
		InputBindings[InputIdxAttentionMask] = FTensorBindingCPU{ MaskBuf.GetData(), RowBytes };
	}
	if (InputIdxTokenTypeIds != INDEX_NONE)
	{
		InputBindings[InputIdxTokenTypeIds] = FTensorBindingCPU{ TypeBuf.GetData(), RowBytes };
	}
	// Any other (unexpected) declared input binds to the same zeroed row.
	for (int32 i = 0; i < InputBindings.Num(); ++i)
	{
		if (InputBindings[i].Data == nullptr)
		{
			InputBindings[i] = FTensorBindingCPU{ TypeBuf.GetData(), RowBytes };
		}
	}

	// Output buffer. last_hidden_state is [1, SeqLen, Dim] fp32; pooled is [1, Dim].
	const int32 OutFloats = bOutputIsLastHiddenState ? (SeqLen * Dim) : Dim;
	TArray<float> RawOut;
	RawOut.SetNumZeroed(OutFloats);
	const FTensorBindingCPU OutputBinding{ RawOut.GetData(), static_cast<uint64>(OutFloats) * sizeof(float) };

	if (ModelInstance->RunSync(InputBindings, MakeArrayView(&OutputBinding, 1))
		!= IModelInstanceCPU::ERunSyncStatus::Ok)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EmbeddingModel] RunSync failed."));
		return false;
	}

	// Pool to a Dim-vector.
	OutVec.SetNumZeroed(Dim);
	if (!bOutputIsLastHiddenState)
	{
		// Already a pooled sentence embedding [1, Dim].
		FMemory::Memcpy(OutVec.GetData(), RawOut.GetData(), Dim * sizeof(float));
	}
	else if (Meta.Pooling == FClaireonEmbedderMeta::EPooling::Cls)
	{
		// CLS pooling: token 0's hidden state (rows are [seq, dim]).
		FMemory::Memcpy(OutVec.GetData(), RawOut.GetData(), Dim * sizeof(float));
	}
	else
	{
		// Mean pooling over masked (mask==1) positions.
		int32 Counted = 0;
		const int32 MaskN = Mask.Num();
		for (int32 t = 0; t < SeqLen; ++t)
		{
			const bool bActive = (t < MaskN) ? (Mask[t] != 0) : false;
			if (!bActive)
			{
				continue;
			}
			const float* Row = RawOut.GetData() + static_cast<int64>(t) * Dim;
			for (int32 d = 0; d < Dim; ++d)
			{
				OutVec[d] += Row[d];
			}
			++Counted;
		}
		if (Counted == 0)
		{
			// No active tokens (empty mask) -> fall back to token 0 to avoid NaN.
			FMemory::Memcpy(OutVec.GetData(), RawOut.GetData(), Dim * sizeof(float));
			Counted = 1;
		}
		const float InvCount = 1.0f / static_cast<float>(Counted);
		for (int32 d = 0; d < Dim; ++d)
		{
			OutVec[d] *= InvCount;
		}
	}

	// L2-normalize so dot-product == cosine downstream.
	if (Meta.bNormalize)
	{
		double SumSq = 0.0;
		for (int32 d = 0; d < Dim; ++d)
		{
			SumSq += static_cast<double>(OutVec[d]) * static_cast<double>(OutVec[d]);
		}
		const float Norm = static_cast<float>(FMath::Sqrt(SumSq));
		if (Norm > SMALL_NUMBER)
		{
			const float InvNorm = 1.0f / Norm;
			for (int32 d = 0; d < Dim; ++d)
			{
				OutVec[d] *= InvNorm;
			}
		}
	}

	return true;
}
