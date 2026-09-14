// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonAstralJsonGuard.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ClaireonAstralJsonGuard
{
	namespace Private
	{
		bool IsAstralTool(const FString& Name)
		{
			static const TCHAR* Names[] = {
				TEXT("astral_matrix_author"), TEXT("astral_matrix_inspect"), TEXT("astral_matrix_validate"),
				TEXT("astral_matrix_add_node"), TEXT("astral_matrix_remove_node"), TEXT("astral_matrix_set_property"),
				TEXT("astral_matrix_reparent"), TEXT("astral_matrix_save")
			};
			for (const TCHAR* Candidate : Names)
			{
				if (Name.Equals(Candidate, ESearchCase::IgnoreCase)) { return true; }
			}
			return false;
		}

		enum class ERole : uint8 { Other, Root, Params };
		struct FFrame
		{
			bool bObject = false;
			ERole Role = ERole::Other;
			TSet<FString> Keys;
		};
		struct FScan
		{
			bool bValid = false;
			bool bToolsCall = false;
			bool bAstralName = false;
			FString Collision;
			TSharedPtr<FJsonValue> Id;
		};

		FScan Scan(const FString& Raw)
		{
			FScan Result;
			TArray<FFrame> Stack;
			int32 IdCount = 0;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
			EJsonNotation Notation;
			while (Reader->ReadNext(Notation))
			{
				if (Notation == EJsonNotation::Error) { return Result; }
				if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
				{
					if (Stack.IsEmpty()) { return Result; }
					Stack.Pop();
					continue;
				}
				const FString& Key = Reader->GetIdentifier();
				const bool bMember = !Stack.IsEmpty() && Stack.Last().bObject;
				const ERole ParentRole = bMember ? Stack.Last().Role : ERole::Other;
				if (bMember)
				{
					// Keep decoded spellings until checked; an FJsonObject map has already lost duplicates.
					if (const FString* Previous = Stack.Last().Keys.Find(Key); Previous && Result.Collision.IsEmpty())
					{
						Result.Collision = FString::Printf(TEXT("Ambiguous JSON member names '%s' and '%s' in one object."), **Previous, *Key);
					}
					Stack.Last().Keys.Add(Key);
				}
				if (Notation == EJsonNotation::String)
				{
					if (ParentRole == ERole::Root && Key.Equals(TEXT("method"), ESearchCase::IgnoreCase))
					{
						Result.bToolsCall |= Reader->GetValueAsString().Equals(TEXT("tools/call"), ESearchCase::IgnoreCase);
					}
					if (ParentRole == ERole::Params && Key.Equals(TEXT("name"), ESearchCase::IgnoreCase))
					{
						Result.bAstralName |= IsAstralTool(Reader->GetValueAsString());
					}
				}
				if (ParentRole == ERole::Root && Key.Equals(TEXT("id"), ESearchCase::IgnoreCase))
				{
					++IdCount;
					Result.Id.Reset();
					if (IdCount == 1)
					{
						if (Notation == EJsonNotation::String) { Result.Id = MakeShared<FJsonValueString>(Reader->GetValueAsString()); }
						else if (Notation == EJsonNotation::Number) { Result.Id = MakeShared<FJsonValueNumber>(Reader->GetValueAsNumber()); }
					}
				}
				if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
				{
					FFrame Frame;
					Frame.bObject = Notation == EJsonNotation::ObjectStart;
					if (Frame.bObject)
					{
						Frame.Role = Stack.IsEmpty() ? ERole::Root
							: ParentRole == ERole::Root && Key.Equals(TEXT("params"), ESearchCase::IgnoreCase) ? ERole::Params : ERole::Other;
					}
					Stack.Add(MoveTemp(Frame));
				}
			}
			Result.bValid = Stack.IsEmpty() && Reader->GetErrorMessage().IsEmpty();
			return Result;
		}

		FParseResult Deserialize(const FString& Raw, bool bRpc, bool bAstralArguments)
		{
			FParseResult Result;
			if (bRpc || bAstralArguments)
			{
				const FScan Scanned = Scan(Raw);
				if (Scanned.bValid && !Scanned.Collision.IsEmpty()
					&& (bAstralArguments || (Scanned.bToolsCall && Scanned.bAstralName)))
				{
					Result.RequestId = bRpc ? Scanned.Id : nullptr;
					Result.Error = Scanned.Collision;
					Result.bAmbiguousKeys = true;
					return Result;
				}
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
			if (!FJsonSerializer::Deserialize(Reader, Result.Object) || !Result.Object.IsValid())
			{
				Result.Object.Reset();
				Result.Error = TEXT("Failed to parse JSON object.");
			}
			return Result;
		}
	}

	FParseResult DeserializeRpc(const FString& RawJson) { return Private::Deserialize(RawJson, true, false); }
	FParseResult DeserializeToolArguments(const FString& ToolName, const FString& RawJson)
	{
		return Private::Deserialize(RawJson, false, Private::IsAstralTool(ToolName));
	}
}
