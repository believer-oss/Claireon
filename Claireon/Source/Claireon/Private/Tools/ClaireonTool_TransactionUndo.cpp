// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionUndo.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionUndo::GetOperation() const { return TEXT("undo"); }

FString ClaireonTool_TransactionUndo::GetDescription() const
{
	return TEXT("Undo the last N transactions. Returns descriptions of undone entries.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionUndo::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("count"), TEXT("Number of transactions to undo (default 1)."));
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionUndo::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	TArray<TSharedPtr<FJsonValue>> UndoneNames;
	int32 UndoneCount = 0;

	for (int32 i = 0; i < Count; ++i)
	{
		if (!GEditor->UndoTransaction())
		{
			break;
		}
		++UndoneCount;

		// Get the description of what was just undone from the redo buffer position.
		FString Description = TEXT("(unknown)");
		if (UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans))
		{
			int32 UndoBufferNum = TransBuffer->GetUndoCount();
			int32 BufferSize = TransBuffer->GetQueueLength();
			int32 UndoneIndex = BufferSize - UndoBufferNum;
			if (UndoneIndex >= 0 && UndoneIndex < BufferSize)
			{
				const FTransaction* Transaction = TransBuffer->GetTransaction(UndoneIndex);
				if (Transaction)
				{
					Description = Transaction->GetTitle().ToString();
				}
			}
		}
		UndoneNames.Add(MakeShared<FJsonValueString>(Description));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("undone_count"), UndoneCount);
	Result->SetArrayField(TEXT("transactions"), UndoneNames);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Undid %d transaction(s)"), UndoneCount));
}
