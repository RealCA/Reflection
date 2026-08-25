/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/TypeSelectionDialog.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "Reflection.TypeSelection"

/* Simple row widget — not SMultiColumnTableRow to avoid HeaderRow requirement */
class STypeSelectionRow : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(STypeSelectionRow) {}
		SLATE_ARGUMENT(TSharedPtr<FTypeEntry>, Entry)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs) {
		Entry = InArgs._Entry;

		ChildSlot
		[
			SNew(SBorder)
			.Padding(FMargin(4.0f, 2.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked(Entry->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) {
						Entry->bSelected = (NewState == ECheckBoxState::Checked);
					})
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Entry->TypeName))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Entry->bSupported ? LOCTEXT("Supported", "Supported") : LOCTEXT("NotSupported", "Not Supported"))
					.ColorAndOpacity(Entry->bSupported
						? FSlateColor(FLinearColor(0.2f, 0.9f, 0.3f))
						: FSlateColor(FLinearColor(0.9f, 0.3f, 0.2f)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
			]
		];
	}

private:
	TSharedPtr<FTypeEntry> Entry;
};

/* STypeSelectionDialog ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void STypeSelectionDialog::Construct(const FArguments& InArgs) {
	TypeEntries.Empty();
	for (const FTypeEntry& Entry : InArgs._Types) {
		TypeEntries.Add(MakeShared<FTypeEntry>(Entry));
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(12.0f, 12.0f, 12.0f, 4.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Prompt", "Select asset types to import:"))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(12.0f, 0.0f, 12.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SelectAll", "Select All"))
				.OnClicked_Lambda([this]() {
					OnToggleAll(true);
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("DeselectAll", "Deselect All"))
				.OnClicked_Lambda([this]() {
					OnToggleAll(false);
					return FReply::Handled();
				})
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(12.0f, 0.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(1.0f))
			[
				SAssignNew(ListView, SListView<TSharedPtr<FTypeEntry>>)
				.ListItemsSource(&TypeEntries)
				.OnGenerateRow(this, &STypeSelectionDialog::OnGenerateRow)
				.SelectionMode(ESelectionMode::None)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(12.0f, 8.0f, 12.0f, 12.0f))
		.HAlign(HAlign_Right)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("OK", "OK"))
				.OnClicked_Lambda([this]() {
					OnAccept();
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked_Lambda([this]() {
					OnCancel();
					return FReply::Handled();
				})
			]
		]
	];
}

TSharedRef<ITableRow> STypeSelectionDialog::OnGenerateRow(TSharedPtr<FTypeEntry> Item, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(STableRow<TSharedPtr<FTypeEntry>>, OwnerTable)
		[
			SNew(STypeSelectionRow)
			.Entry(Item)
		];
}

void STypeSelectionDialog::OnToggleAll(bool bSelect) {
	for (TSharedPtr<FTypeEntry>& Entry : TypeEntries) {
		Entry->bSelected = bSelect;
	}
	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

void STypeSelectionDialog::OnAccept() {
	ResultTypes.Empty();
	for (const TSharedPtr<FTypeEntry>& Entry : TypeEntries) {
		ResultTypes.Add(*Entry);
	}

	if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this))) {
		Window->RequestDestroyWindow();
	}
}

void STypeSelectionDialog::OnCancel() {
	ResultTypes.Empty();

	if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this))) {
		Window->RequestDestroyWindow();
	}
}

/* Free function ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

bool ShowTypeSelectionDialog(TArray<FTypeEntry>& InOutTypes) {
	TSharedPtr<STypeSelectionDialog> Dialog;

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("DialogTitle", "Reflect Folder — Select Types"))
		.ClientSize(FVector2D(420, 500))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	Window->SetContent(
		SAssignNew(Dialog, STypeSelectionDialog)
		.Types(InOutTypes)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog.IsValid()) return false;

	InOutTypes = Dialog->GetSelectedTypes();
	return InOutTypes.Num() > 0;
}
