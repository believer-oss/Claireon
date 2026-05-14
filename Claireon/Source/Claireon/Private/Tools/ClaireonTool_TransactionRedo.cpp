// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionRedo.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionRedo::GetOperation() const { return TEXT("redo"); }

FString ClaireonTool_TransactionRedo::GetDescription() const
{
	return TEXT("Redo the last N undone transactions. Returns descriptions of redone entries.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionRedo::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("count"), TEXT("Number of transactions to redo (default 1)."));
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionRedo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	int32 Count = 1;
	if (Arguments->HasField(TEXT("count")))
	{
		Count = FMath::Max(1, static_cast<int32>(Arguments->GetNumberField(TEXT("count"))));
	}

	TArray<TSharedPtr<FJsonValue>> RedoneNames;
	int32 RedoneCount = 0;

	for (int32 i = 0; i < Count; ++i)
	{
		// Before redo, capture the description of what will be redone.
		FString Description = TEXT("(unknown)");
		if (UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans))
		{
			int32 UndoCount = TransBuffer->GetUndoCount();
			if (UndoCount > 0)
			{
				int32 BufferSize = TransBuffer->GetQueueLength();
				int32 RedoIndex = BufferSize - UndoCount;
				if (RedoIndex >= 0 && RedoIndex < BufferSize)
				{
					const FTransaction* Transaction = TransBuffer->GetTransaction(RedoIndex);
					if (Transaction)
					{
						Description = Transaction->GetTitle().ToString();
					}
				}
			}
		}

		if (!GEditor->RedoTransaction())
		{
			break;
		}
		++RedoneCount;
		RedoneNames.Add(MakeShared<FJsonValueString>(Description));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("redone_count"), RedoneCount);
	Result->SetArrayField(TEXT("transactions"), RedoneNames);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Redid %d transaction(s)"), RedoneCount));
}
