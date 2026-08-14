# Plan: Reflect Folder — Type Selection Dialog

## Context
The "Reflect Folder" toolbar button currently scans a folder for JSON files and imports all of them. The user wants a dialog that:
1. Scans all JSON files in the folder
2. Extracts unique `Type` values from each file's exports
3. Shows a modal dialog listing all types found, with:
   - Checkbox per type (ticked by default)
   - "Supported" / "Not Supported" label per type
   - Select All / Deselect All buttons
   - OK / Cancel buttons
4. Only imports files whose primary export type is in the selected set

## Files to Modify

### 1. NEW: `Public/Modules/UI/TypeSelectionDialog.h`
Slate dialog widget (`SCompoundWidget`) that displays the type list and returns the user's selection.

### 2. NEW: `Private/Modules/UI/TypeSelectionDialog.cpp`
Implementation of the dialog.

### 3. MODIFY: `Private/Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.cpp`
Insert the dialog between folder scan and `FImportJob::Enqueue()`.

## Implementation

### Step 1: Create `TypeSelectionDialog.h`

```cpp
#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FTypeEntry {
    FString TypeName;
    bool bSupported;
    bool bSelected;
};

class STypeSelectionDialog : public SCompoundWidget {
public:
    SLATE_BEGIN_ARGS(STypeSelectionDialog) {}
        SLATE_ARGUMENT(TArray<FTypeEntry>, Types)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    TArray<FTypeEntry> GetSelectedTypes() const;

private:
    TSharedPtr<SListView<TSharedPtr<FTypeEntry>>> ListView;
    TArray<TSharedPtr<FTypeEntry>> TypeEntries;

    TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FTypeEntry> Item, const TSharedRef<STableViewBase>& OwnerTable);
    void ToggleAll(bool bSelect);
};
```

### Step 2: Create `TypeSelectionDialog.cpp`

Key behaviors:
- Modal window hosted via `FSlateApplication::Get().AddModalWindow()`
- `OnGenerateRow` renders: checkbox + type name + green "Supported" or red "Not Supported" badge
- Select All / Deselect All buttons toggle `bSelected` on all entries and refresh the list
- OK button closes the window; Cancel discards

### Step 3: Modify `ToolsDropdownBuilder.cpp`

In the `ToolsReflect` lambda (line 57-123), after `IFileManager::Get().FindFilesRecursive(...)`:

1. For each JSON file, call `DeserializeJSON` (from `Utilities/JsonHelpers.h`)
2. For each export in the parsed array, read `GetStringField(TEXT("Type"))` and collect unique types
3. For each unique type, call `CanImport(type)` to determine support status
4. Open the `STypeSelectionDialog` as a modal window
5. If user cancels, return early
6. Build a `TSet<FString>` of selected types
7. Filter `JsonFiles` — only keep files whose first export's Type is in the selected set
8. Proceed with `FImportJob::Enqueue(JsonFiles)`

## Key Dependencies
- `DeserializeJSON` from `Utilities/JsonHelpers.h` (already available)
- `CanImport` from `Importers/Constructor/TypesHelper.h` (already available)
- `FImportJob::Enqueue` from `Importers/Constructor/ImportJob.h` (already used)
- Slate framework (`SWindow`, `SListView`, `SButton`, `SCheckBox`, `STextBlock`)

## Supported vs Unsupported Detection
- Use existing `CanImport(Type, false, nullptr)` from `TypesHelper.h` — it checks:
  - Factory registry (`FindFactoryForAssetType`)
  - Templated importer map (`ImportTypes::Templated`)
  - Class hierarchy (DataAsset, etc.)
- Types that return `true` = "Supported", `false` = "Not Supported"

## Verification
1. Build: `& "D:\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" CustomRig55Editor Win64 Development -Project="C:\Users\Home\Documents\Unreal Projects\Test\CustomRig55.uproject" -WaitMutex -FromMsBuild`
2. Open editor → Reflection toolbar → Reflect Folder
3. Select folder with JSON files → dialog should appear showing types
4. Untick some types → OK → verify only selected types are imported
5. Check logs for correct file/type filtering
