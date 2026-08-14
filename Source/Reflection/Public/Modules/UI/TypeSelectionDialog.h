/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FTypeEntry {
	FString TypeName;
	bool bSupported;
	bool bSelected = true;
};

class STypeSelectionDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(STypeSelectionDialog) {}
		SLATE_ARGUMENT(TArray<FTypeEntry>, Types)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	TArray<FTypeEntry> GetSelectedTypes() const { return ResultTypes; }

private:
	TSharedPtr<SListView<TSharedPtr<FTypeEntry>>> ListView;
	TArray<TSharedPtr<FTypeEntry>> TypeEntries;
	TArray<FTypeEntry> ResultTypes;
	bool bAccepted = false;

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FTypeEntry> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnToggleAll(bool bSelect);
	void OnAccept();
	void OnCancel();
};

bool ShowTypeSelectionDialog(TArray<FTypeEntry>& InOutTypes);
