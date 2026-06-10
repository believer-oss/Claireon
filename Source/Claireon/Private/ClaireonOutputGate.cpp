// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonOutputGate.h"

#include "ClaireonLog.h"
#include "ClaireonPythonAuditLog.h"
#include "ClaireonSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace ClaireonOutputGateInternal
{
	/** Test-only results-root override; empty means "use ProjectSavedDir/Claireon/Results". */
	static FString GResultsRootOverride;

	/** Sanitise a single path component: replace filesystem-invalid chars with '_'. */
	static FString SanitizeComponent(const FString& In)
	{
		FString Out = In;
		const TCHAR BadChars[] = { '\\', '/', ':', '*', '?', '"', '<', '>', '|' };
		for (TCHAR C : BadChars)
		{
			Out.ReplaceCharInline(C, TCHAR('_'), ESearchCase::CaseSensitive);
		}
		return Out;
	}

	/** Normalise a path to absolute + forward slashes. */
	static FString ToForwardSlashAbsolute(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		Full.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Full;
	}

	/**
	 * Validate that a byte buffer is entirely valid UTF-8.  We accept any byte sequence
	 * already produced by our own JSON/log serialisers as UTF-8; this check is a safety
	 * net for tools that might surface raw binary in Result.Data-derived blobs or in
	 * Logs/UELog.
	 */
	static bool IsValidUtf8(const uint8* Data, int32 Len)
	{
		if (!Data || Len <= 0)
		{
			return true;
		}

		int32 Index = 0;
		while (Index < Len)
		{
			const uint8 Byte = Data[Index];
			int32 Extra = 0;
			uint32 MinValue = 0;
			uint32 Code = 0;

			if ((Byte & 0x80) == 0)
			{
				// ASCII
				++Index;
				continue;
			}
			else if ((Byte & 0xE0) == 0xC0)
			{
				Extra = 1;
				Code = Byte & 0x1F;
				MinValue = 0x80;
			}
			else if ((Byte & 0xF0) == 0xE0)
			{
				Extra = 2;
				Code = Byte & 0x0F;
				MinValue = 0x800;
			}
			else if ((Byte & 0xF8) == 0xF0)
			{
				Extra = 3;
				Code = Byte & 0x07;
				MinValue = 0x10000;
			}
			else
			{
				return false;
			}

			if (Index + Extra >= Len)
			{
				return false;
			}

			for (int32 i = 1; i <= Extra; ++i)
			{
				const uint8 Cont = Data[Index + i];
				if ((Cont & 0xC0) != 0x80)
				{
					return false;
				}
				Code = (Code << 6) | (Cont & 0x3F);
			}

			if (Code < MinValue)
			{
				return false;
			}
			if (Code > 0x10FFFF)
			{
				return false;
			}
			if (Code >= 0xD800 && Code <= 0xDFFF)
			{
				return false;
			}

			Index += 1 + Extra;
		}

		return true;
	}

	/**
	 * UTF-8-boundary-safe prefix of at most MaxBytes.  Given a byte buffer already
	 * validated as UTF-8, returns the largest prefix <= MaxBytes that ends on a codepoint
	 * boundary (per spill-mechanism.md "Preview policy").
	 */
	static FString Utf8PrefixSafe(const uint8* Data, int32 Len, int32 MaxBytes)
	{
		if (Len <= MaxBytes)
		{
			return FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Data), Len));
		}

		int32 Cut = MaxBytes;
		while (Cut > 0 && (Data[Cut] & 0xC0) == 0x80)
		{
			--Cut;
		}
		return FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Data), Cut));
	}

	/** Hex-encode up to 256 bytes as "0x..." for a binary preview. */
	static FString HexPreview(const uint8* Data, int32 Len)
	{
		const int32 Count = FMath::Min(Len, 256);
		FString Out = TEXT("0x");
		Out.Reserve(2 + Count * 2);
		for (int32 i = 0; i < Count; ++i)
		{
			Out += FString::Printf(TEXT("%02X"), Data[i]);
		}
		return Out;
	}

	/**
	 * Spill a single stream to disk using the atomic temp-then-rename dance.  Returns the
	 * populated FClaireonSpillStream; the caller decides whether to record the manifest and
	 * whether to emit an audit entry.
	 *
	 * See CLAIREON_DISK_RESULTS/spill-mechanism.md for the full specification (path layout,
	 * UTF-8 classification, preview policy, ceiling handling, error path).
	 */
	static FClaireonSpillStream SpillOneStream(
		const FString& ToolName,
		const FString& ConversationId,
		const FString& StreamName,
		const TArray<uint8>& RawBytes,
		int64 CeilingBytes)
	{
		FClaireonSpillStream Out;
		Out.Name = StreamName;
		Out.SizeBytes = RawBytes.Num();

		// Apply the per-stream ceiling by truncation.
		TArray<uint8> Bytes;
		if (CeilingBytes > 0 && RawBytes.Num() > CeilingBytes)
		{
			Out.bOverCeiling = true;
			Bytes.Append(RawBytes.GetData(), static_cast<int32>(CeilingBytes));
		}
		else
		{
			Bytes = RawBytes;
		}

		// Classify content -- UTF-8 text vs binary.
		const bool bIsUtf8 = IsValidUtf8(Bytes.GetData(), Bytes.Num());
		FString Extension;
		if (bIsUtf8)
		{
			Extension = (StreamName == TEXT("data")) ? TEXT("json") : TEXT("txt");
			Out.ContentType = (StreamName == TEXT("data")) ? TEXT("application/json") : TEXT("text/plain");
			Out.Preview = Utf8PrefixSafe(Bytes.GetData(), Bytes.Num(), 1024);
		}
		else
		{
			Extension = TEXT("bin");
			Out.ContentType = TEXT("application/octet-stream");
			Out.Preview = HexPreview(Bytes.GetData(), Bytes.Num());
		}

		// Build the final path: <Root>/<conv_id>/<timestamp>-<tool>-<uuid>-<stream>.<ext>
		const FString Root = FClaireonOutputGate::GetResultsRoot();
		const FString ConvDir = Root / SanitizeComponent(ConversationId);
		IFileManager::Get().MakeDirectory(*ConvDir, /*Tree*/ true);

		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
		const FString ShortUuid = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
		const FString BaseName = FString::Printf(TEXT("%s-%s-%s-%s.%s"),
			*Timestamp,
			*SanitizeComponent(ToolName),
			*ShortUuid,
			*StreamName,
			*Extension);

		const FString FinalPath = ConvDir / BaseName;
		const FString TempPath = FinalPath + TEXT(".part");

		// Atomic write: temp file first, then Move to final name.
		bool bWriteOk = false;
		if (bIsUtf8)
		{
			// SaveArrayToFile preserves the exact bytes (including possible newlines);
			// we have already verified the payload is valid UTF-8 so this is equivalent
			// to ForceUTF8WithoutBOM for any well-formed text.
			bWriteOk = FFileHelper::SaveArrayToFile(Bytes, *TempPath);
		}
		else
		{
			bWriteOk = FFileHelper::SaveArrayToFile(Bytes, *TempPath);
		}

		if (!bWriteOk)
		{
			Out.bWriteFailed = true;
			Out.ErrorText = TEXT("Failed to write temp spill file");
			IFileManager::Get().Delete(*TempPath, /*bRequireExists*/ false, /*bEvenIfReadOnly*/ true, /*bQuiet*/ true);
			return Out;
		}

		if (!IFileManager::Get().Move(*FinalPath, *TempPath, /*bReplace*/ true, /*bEvenIfReadOnly*/ false))
		{
			Out.bWriteFailed = true;
			Out.ErrorText = TEXT("Failed to rename temp spill file to final path");
			IFileManager::Get().Delete(*TempPath, /*bRequireExists*/ false, /*bEvenIfReadOnly*/ true, /*bQuiet*/ true);
			return Out;
		}

		Out.AbsolutePath = ToForwardSlashAbsolute(FinalPath);
		return Out;
	}

	/**
	 * Serialise a JSON object to pretty-printed UTF-8 bytes for the spill decision
	 * and the on-disk file. Pretty-printed so line-oriented tools (grep, head)
	 * work on spill files instead of seeing one multi-kilobyte line; the same
	 * bytes feed the threshold check so size_bytes matches the file exactly.
	 */
	static TArray<uint8> SerializeJsonToUtf8Bytes(const TSharedPtr<FJsonObject>& Obj)
	{
		TArray<uint8> Out;
		if (!Obj.IsValid())
		{
			return Out;
		}
		FString Serialized;
		TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&Serialized);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		Writer->Close();

		FTCHARToUTF8 Converter(*Serialized);
		Out.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
		return Out;
	}

	/** Convert an FString to a UTF-8 byte array (no BOM). */
	static TArray<uint8> StringToUtf8Bytes(const FString& In)
	{
		TArray<uint8> Out;
		if (In.IsEmpty())
		{
			return Out;
		}
		FTCHARToUTF8 Converter(*In);
		Out.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
		return Out;
	}

	/** Build the JSON object carried on the envelope for one spilled stream. */
	static TSharedPtr<FJsonObject> StreamManifestToJson(const FClaireonSpillStream& Stream)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Stream.Name);
		Obj->SetStringField(TEXT("absolute_path"), Stream.AbsolutePath);
		Obj->SetNumberField(TEXT("size_bytes"), static_cast<double>(Stream.SizeBytes));
		Obj->SetStringField(TEXT("content_type"), Stream.ContentType);
		Obj->SetStringField(TEXT("preview"), Stream.Preview);
		Obj->SetBoolField(TEXT("over_ceiling"), Stream.bOverCeiling);
		Obj->SetBoolField(TEXT("write_failed"), Stream.bWriteFailed);
		Obj->SetStringField(TEXT("error_text"), Stream.ErrorText);
		return Obj;
	}
}

void FClaireonOutputGate::SetResultsRootOverrideForTests(const FString& InOverridePath)
{
	ClaireonOutputGateInternal::GResultsRootOverride = InOverridePath;
}

FString FClaireonOutputGate::GetResultsRoot()
{
	if (!ClaireonOutputGateInternal::GResultsRootOverride.IsEmpty())
	{
		return ClaireonOutputGateInternal::GResultsRootOverride;
	}
	return FPaths::ProjectSavedDir() / TEXT("Claireon") / TEXT("Results");
}

IClaireonTool::FToolResult FClaireonOutputGate::RouteResult(
	IClaireonTool::FToolResult Result,
	const FString& ToolName,
	const FString& ConversationId,
	EClaireonSpillStreamSet StreamSet)
{
	// Forward to the args-aware overload with an empty Arguments payload.
	return RouteResult(MoveTemp(Result), ToolName, /*Arguments=*/ nullptr, ConversationId, StreamSet);
}

IClaireonTool::FToolResult FClaireonOutputGate::RouteResult(
	IClaireonTool::FToolResult Result,
	const FString& ToolName,
	const TSharedPtr<FJsonObject>& InvocationArguments,
	const FString& ConversationId,
	EClaireonSpillStreamSet StreamSet)
{
	using namespace ClaireonOutputGateInternal;

	const UClaireonSettings* Settings = UClaireonSettings::Get();
	const int64 Threshold = Settings
		? static_cast<int64>(Settings->ResultSpillThresholdBytes)
		: 8192;
	const int64 Ceiling = Settings
		? static_cast<int64>(Settings->ResultSpillMaxBytes)
		: 52428800;

	// force_inline short-circuits the entire spill pipeline. The caller is
	// signalling that it would rather pay the context cost than chase a
	// spill file (e.g. known-small-result queries, agentic short-tail discovery).
	bool bForceInline = false;
	if (InvocationArguments.IsValid())
	{
		InvocationArguments->TryGetBoolField(TEXT("force_inline"), bForceInline);
	}
	if (bForceInline)
	{
		return Result;
	}

	const FString EffectiveConversationId = ConversationId.IsEmpty() ? FString(TEXT("default")) : ConversationId;

	struct FStreamInput
	{
		FString Name;
		TArray<uint8> Bytes;
	};

	TArray<FStreamInput> Inputs;

	if (StreamSet == EClaireonSpillStreamSet::GenericData)
	{
		FStreamInput DataInput;
		DataInput.Name = TEXT("data");
		DataInput.Bytes = SerializeJsonToUtf8Bytes(Result.Data);
		if (DataInput.Bytes.Num() > 0)
		{
			Inputs.Add(MoveTemp(DataInput));
		}
	}
	else
	{
		if (!Result.Logs.IsEmpty())
		{
			FStreamInput StdoutInput;
			StdoutInput.Name = TEXT("stdout");
			StdoutInput.Bytes = StringToUtf8Bytes(Result.Logs);
			if (StdoutInput.Bytes.Num() > 0)
			{
				Inputs.Add(MoveTemp(StdoutInput));
			}
		}
		if (!Result.UELog.IsEmpty())
		{
			FStreamInput UELogInput;
			UELogInput.Name = TEXT("uelog");
			UELogInput.Bytes = StringToUtf8Bytes(Result.UELog);
			if (UELogInput.Bytes.Num() > 0)
			{
				Inputs.Add(MoveTemp(UELogInput));
			}
		}
	}

	TArray<FClaireonSpillStream> SpilledStreams;
	for (const FStreamInput& Input : Inputs)
	{
		if (Input.Bytes.Num() <= Threshold)
		{
			// Under threshold -- stays inline.
			continue;
		}

		FClaireonSpillStream Stream = SpillOneStream(
			ToolName,
			EffectiveConversationId,
			Input.Name,
			Input.Bytes,
			Ceiling);

		// Audit every spill (success, truncated, or failed).
		FClaireonPythonAuditLog::Get().RecordSpill(
			ToolName,
			Stream.Name,
			Stream.SizeBytes,
			Stream.AbsolutePath,
			EffectiveConversationId,
			Stream.bOverCeiling,
			Stream.bWriteFailed,
			Stream.ErrorText);

		SpilledStreams.Add(MoveTemp(Stream));
	}

	if (SpilledStreams.Num() == 0)
	{
		return Result;
	}

	// Build the spill manifest onto Result.Data and clear the spilled inline payloads.
	TSharedPtr<FJsonObject> Manifest = Result.Data;
	if (!Manifest.IsValid())
	{
		Manifest = MakeShared<FJsonObject>();
	}
	Manifest->SetBoolField(TEXT("__mcp_spilled__"), true);

	// Always echo originating tool + invocation arguments on the spill
	// manifest so a downstream reader of a spilled response (or the on-disk
	// spill file, after StreamManifestToJson copies these fields) knows
	// which call wrote it.
	Manifest->SetStringField(TEXT("originating_tool"), ToolName);
	if (InvocationArguments.IsValid())
	{
		Manifest->SetObjectField(TEXT("originating_args"), InvocationArguments);
	}

	TArray<TSharedPtr<FJsonValue>> StreamsJson;
	StreamsJson.Reserve(SpilledStreams.Num());
	for (const FClaireonSpillStream& Stream : SpilledStreams)
	{
		StreamsJson.Add(MakeShared<FJsonValueObject>(StreamManifestToJson(Stream)));
	}
	Manifest->SetArrayField(TEXT("spilled_streams"), StreamsJson);

	// Track which logical fields were stripped from the inline envelope so
	// agents reading data.<field> see a missing-field signal AND a structured
	// hint pointing at the spill file (kill silent-empty-array).
	TArray<TSharedPtr<FJsonValue>> InlineOmittedJson;

	// Build a loud-marker prefix for Summary that names the first successfully-spilled
	// absolute path. Agents who only consume <summary> text now cannot miss the spill.
	FString FirstSpilledPath;

	// Clear inline blobs whose contents are now on disk.
	for (const FClaireonSpillStream& Stream : SpilledStreams)
	{
		if (FirstSpilledPath.IsEmpty() && !Stream.bWriteFailed && !Stream.AbsolutePath.IsEmpty())
		{
			FirstSpilledPath = Stream.AbsolutePath;
		}

		if (StreamSet == EClaireonSpillStreamSet::GenericData && Stream.Name == TEXT("data"))
		{
			// The entire Result.Data blob spilled; preserve only the manifest fields.
			TSharedPtr<FJsonObject> NewData = MakeShared<FJsonObject>();
			NewData->SetBoolField(TEXT("__mcp_spilled__"), true);
			NewData->SetArrayField(TEXT("spilled_streams"), StreamsJson);
			// Keep the originating-call breadcrumbs on the post-spill manifest.
			NewData->SetStringField(TEXT("originating_tool"), ToolName);
			if (InvocationArguments.IsValid())
			{
				NewData->SetObjectField(TEXT("originating_args"), InvocationArguments);
			}
			// Keep small scalar identity fields inline so the agent can still
			// see session_id / state_id / asset_path / kind / status without
			// having to read the spill file. Only scalar (string / number / bool / null)
			// fields under PreservedScalarMaxBytes (matches typical id-field widths) are
			// preserved; nested objects, arrays, and oversized strings go to the spill.
			const int32 PreservedScalarMaxBytes = 512;
			if (Result.Data.IsValid())
			{
				for (const auto& Pair : Result.Data->Values)
				{
					if (!Pair.Value.IsValid()) continue;
					switch (Pair.Value->Type)
					{
					case EJson::Number:
					case EJson::Boolean:
					case EJson::Null:
						NewData->SetField(Pair.Key, Pair.Value);
						break;
					case EJson::String:
					{
						FString S;
						if (Pair.Value->TryGetString(S) && S.Len() <= PreservedScalarMaxBytes)
						{
							NewData->SetField(Pair.Key, Pair.Value);
						}
						break;
					}
					default:
						break;
					}
				}
			}
			Manifest = NewData;
			InlineOmittedJson.Add(MakeShared<FJsonValueString>(TEXT("data")));
		}
		else if (Stream.Name == TEXT("stdout"))
		{
			Result.Logs.Empty();
			InlineOmittedJson.Add(MakeShared<FJsonValueString>(TEXT("logs")));
		}
		else if (Stream.Name == TEXT("uelog"))
		{
			Result.UELog.Empty();
			InlineOmittedJson.Add(MakeShared<FJsonValueString>(TEXT("uelog")));
		}
	}

	// Surface an error_hint pointing at the spill file so agents that
	// read data.<field> and get a missing-field signal also see a structured
	// instruction telling them where the data lives.
	if (FirstSpilledPath.IsEmpty())
	{
		// All spills failed to write; preserve the manifest but skip the hint.
		Manifest->SetArrayField(TEXT("inline_omitted"), InlineOmittedJson);
	}
	else
	{
		Manifest->SetArrayField(TEXT("inline_omitted"), InlineOmittedJson);
		Manifest->SetStringField(
			TEXT("error_hint"),
			FString::Printf(
				TEXT("Response exceeded inline ceiling and was written to disk. ")
				TEXT("The inline payload for the listed fields is NOT on the wire. ")
				TEXT("Read the JSON / text file at spilled_streams[0].absolute_path (%s) ")
				TEXT("or re-call with a smaller scope (e.g. max_nodes=N, format='summary', anchor_node_guid=...)."),
				*FirstSpilledPath));

		// Prepend a loud marker to Summary so consumers that only parse the
		// summary text cannot silently miss the spill (the B2 footgun).
		const FString Marker = FString::Printf(TEXT("[SPILLED -> %s] "), *FirstSpilledPath);
		if (!Result.Summary.StartsWith(TEXT("[SPILLED")))
		{
			Result.Summary = Marker + Result.Summary;
		}
	}

	Result.Data = Manifest;
	return Result;
}

void FClaireonOutputGate::SweepStaleSpills(int32 RetentionDays)
{
	if (RetentionDays < 1)
	{
		RetentionDays = 1;
	}

	const FString Root = GetResultsRoot();
	if (!IFileManager::Get().DirectoryExists(*Root))
	{
		return;
	}

	const FDateTime CutoffTime = FDateTime::UtcNow() - FTimespan::FromDays(RetentionDays);

	// Enumerate immediate subdirectories.
	class FSubdirVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TArray<FString> Subdirs;
		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (bIsDirectory)
			{
				Subdirs.Add(FString(FilenameOrDirectory));
			}
			return true;
		}
	};

	FSubdirVisitor Visitor;
	FPlatformFileManager::Get().GetPlatformFile().IterateDirectory(*Root, Visitor);

	for (const FString& DirPath : Visitor.Subdirs)
	{
		class FRecentMtimeVisitor : public IPlatformFile::FDirectoryVisitor
		{
		public:
			FDateTime Latest;
			FRecentMtimeVisitor() : Latest(FDateTime::MinValue()) {}
			virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
			{
				if (!bIsDirectory)
				{
					const FDateTime Ts = IFileManager::Get().GetTimeStamp(FilenameOrDirectory);
					if (Ts > Latest)
					{
						Latest = Ts;
					}
				}
				return true;
			}
		};

		// Staleness = the newest contained file's mtime; the directory's own
		// mtime is consulted only when the directory holds no files. The dir
		// mtime cannot be a floor when files exist: a partially-failed sweep
		// (locked file) refreshes it by deleting siblings, which would starve
		// the retry that the failure path below promises.
		FRecentMtimeVisitor FileVisitor;
		FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*DirPath, FileVisitor);
		const FDateTime MostRecent = (FileVisitor.Latest != FDateTime::MinValue())
			? FileVisitor.Latest
			: IFileManager::Get().GetTimeStamp(*DirPath);

		if (MostRecent >= CutoffTime)
		{
			continue;
		}

		const bool bDeleted = IFileManager::Get().DeleteDirectory(*DirPath, /*bRequireExists*/ false, /*Tree*/ true);
		if (!bDeleted)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP Sweep] Could not fully delete %s (likely locked file); will retry on next sweep"),
				*DirPath);
		}
	}
}
