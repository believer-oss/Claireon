// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionHistory.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionHistory::GetOperation() const { return TEXT("history"); }

FString ClaireonTool_TransactionHistory::GetDescription() const
{
    return TEXT("List recent transactions from the undo buffer. Use filter='claireon' to show only [Claireon]-prefixed entries. Stateless / read-only / non-session: reads the editor-wide transactor history.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionHistory::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("count"), TEXT("Number of entries to return (default 20, max 100)."));
	Builder.AddEnum(TEXT("filter"),
		TEXT("'all' returns all entries, 'claireon' returns only [Claireon]-prefixed entries. Default: 'all'."),
		{TEXT("all"), TEXT("claireon")});
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionHistory::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans);
	if (!TransBuffer)
	{
		return MakeErrorResult(TEXT("Failed to access transaction buffer"));
	}

	int32 Count = 20;
	if (Arguments->HasField(TEXT("count")))
	{
		Count = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("count"))), 1, 100);
	}

	FString Filter = TEXT("all");
	Arguments->TryGetStringField(TEXT("filter"), Filter);
	const bool bFilterClaireon = Filter == TEXT("claireon");

	const int32 BufferSize = TransBuffer->GetQueueLength();
	const int32 UndoCount = TransBuffer->GetUndoCount();

	TArray<TSharedPtr<FJsonValue>> Entries;

	// Iterate from most recent to oldest.
	for (int32 i = BufferSize - 1; i >= 0 && Entries.Num() < Count; --i)
	{
		const FTransaction* Transaction = TransBuffer->GetTransaction(i);
		if (!Transaction)
		{
			continue;
		}

		FString Description = Transaction->GetTitle().ToString();

		if (bFilterClaireon && !Description.StartsWith(TEXT("[Claireon]")))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), i);
		Entry->SetStringField(TEXT("description"), Description);

		// Mark whether this entry is in the "undone" region (available for redo).
		const bool bIsUndone = i >= (BufferSize - UndoCount);
		Entry->SetBoolField(TEXT("is_undone"), bIsUndone);

		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("entries"), Entries);
	Result->SetNumberField(TEXT("total_in_buffer"), BufferSize);
	Result->SetNumberField(TEXT("undo_count"), UndoCount);

	return MakeSuccessResult(Result,
		FString::Printf(TEXT("Returned %d history entries (%d total in buffer)"), Entries.Num(), BufferSize));
}
