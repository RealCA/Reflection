/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/ImportJob.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/TypesHelper.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"
#include "Modules/UI/StyleModule.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Modules/Toolbar/Tools/DumpBlueprintDebug.h"
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Modules/UI/TypeSelectionDialog.h"
#include "Utilities/Dialog.h"
#include "Utilities/ImportWithHierarchy.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/MissingDependencies.h"

/* Extract the first export's real UE type from a JSON file. Some exporters put the
 * export Name (e.g. "AB_CharCreation_C") in the "Type" field instead of the actual class
 * type. When Type ends with _C, we read the "Class" field which has the real type, e.g.
 * "AnimBlueprintGeneratedClass'/Game/.../AB_CharCreation.AB_CharCreation_C'". */
static FString ExtractFirstTypeFromFile(const FString& FilePath) {
	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(FilePath, DataObjects) || DataObjects.Num() == 0) {
		return FString();
	}

	const TSharedPtr<FJsonObject> Obj = DataObjects[0]->AsObject();
	if (!Obj.IsValid()) return FString();

	FString Type;
	if (Obj->HasTypedField<EJson::String>(TEXT("Type"))) {
		Type = Obj->GetStringField(TEXT("Type"));
	}

	if (Type.IsEmpty()) return FString();

	/* If Type ends with _C, it's a blueprint name, not a real UE type. Extract the actual
	 * type from the "Class" field: "AnimBlueprintGeneratedClass'/Game/...'" → "AnimBlueprintGeneratedClass" */
	if (Type.EndsWith(TEXT("_C"))) {
		if (Obj->HasTypedField<EJson::String>(TEXT("Class"))) {
			const FString ClassStr = Obj->GetStringField(TEXT("Class"));
			const int32 QuoteIdx = ClassStr.Find(TEXT("'"), ESearchCase::CaseSensitive);
			if (QuoteIdx != INDEX_NONE) {
				return ClassStr.Mid(0, QuoteIdx);
			}
		}
		FString Base = Type;
		Base.RemoveFromEnd(TEXT("_C"));
		return Base;
	}

	return Type;
}

/* Read / write the .type_index cache file. Format: one line per file, "RelativePath|Type" */
static TMap<FString, FString> LoadTypeIndex(const FString& FolderPath) {
	TMap<FString, FString> Index;
	const FString IndexPath = FPaths::Combine(FolderPath, TEXT(".type_index"));

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *IndexPath)) return Index;

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, /* bCullEmpty */ true);

	for (const FString& Line : Lines) {
		FString RelPath, TypeName;
		if (Line.Split(TEXT("|"), &RelPath, &TypeName)) {
			Index.Add(RelPath, TypeName);
		}
	}

	return Index;
}

static void SaveTypeIndex(const FString& FolderPath, const TMap<FString, FString>& Index) {
	const FString IndexPath = FPaths::Combine(FolderPath, TEXT(".type_index"));

	FString Content;
	for (const auto& Pair : Index) {
		Content += Pair.Key + TEXT("|") + Pair.Value + TEXT("\n");
	}

	FFileHelper::SaveStringToFile(Content, *IndexPath);
}

void IToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.AddSubMenu(
		FText::FromString("Asset Tools"),
		FText::FromString("Tools bundled"),
		FNewMenuDelegate::CreateLambda([this](FMenuBuilder& InnerMenuBuilder) {
			InnerMenuBuilder.BeginSection("ReflectionToolsSection", FText::FromString("Tools"));
			{
				InnerMenuBuilder.AddMenuEntry(
					FText::FromString("Clear Import Data"),
					FText::FromString(""),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

					FUIAction(
						FExecuteAction::CreateLambda([] {
							TToolClearImportData* Tool = new TToolClearImportData();
							Tool->Execute();
						})
					),
					NAME_None
				);

			InnerMenuBuilder.AddMenuEntry(
				FText::FromString("Fixup Asset Data"),
				FText::FromString(""),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

				FUIAction(
					FExecuteAction::CreateLambda([] {
						TToolFixUpAssetData* Tool = new TToolFixUpAssetData();
						Tool->Execute();
					})
				),
				NAME_None
			);

			InnerMenuBuilder.AddMenuEntry(
				FText::FromString("Dump Blueprint Debug Data"),
				FText::FromString("Per-graph editor text exports + JSON report (import diagnostics, bytecode statement map, unwired pins) for the selected Blueprint."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

				FUIAction(
					FExecuteAction::CreateLambda([] {
						TToolDumpBlueprintDebug* Tool = new TToolDumpBlueprintDebug();
						Tool->Execute();
					})
				),
				NAME_None
			);

			InnerMenuBuilder.AddMenuEntry(
				FText::FromString("Reflect Folder of JSON Files"),
				FText::FromString("Import JSON files from a folder. Choose which asset types to import."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

				FUIAction(
					FExecuteAction::CreateLambda([] {
						TArray<FString> JsonFiles;
						FString SelectedFolder;

						for (const FString& Folder : OpenFolderDialog("Folder of JSON files")) {
							SelectedFolder = Folder;
							IFileManager::Get().FindFilesRecursive(
								JsonFiles,
								*Folder,
								TEXT("*.json"),
								true,
								true,
								/* bClearFileNames */ false
							);
						}

						if (JsonFiles.Num() == 0) return;

						/* Load existing type index cache */
						TMap<FString, FString> TypeIndex = LoadTypeIndex(SelectedFolder);
						TMap<FString, bool> TypeSupportMap;
						TArray<FString> FilesToScan;
						int32 CacheHits = 0;

						for (const FString& File : JsonFiles) {
						const FString* CachedType = TypeIndex.Find(File);

						if (CachedType && !CachedType->IsEmpty()) {
							/* Cache hit */
							++CacheHits;
							if (!TypeSupportMap.Contains(*CachedType)) {
								TypeSupportMap.Add(*CachedType, CanImport(*CachedType));
							}
						} else {
							FilesToScan.Add(File);
						}
						}

						UE_LOG(LogReflection, Log, TEXT("Reflect Folder: %d cache hits, %d files to scan"),
							CacheHits, FilesToScan.Num());

						/* Scan files not in cache */
						for (const FString& File : FilesToScan) {
							const FString Type = ExtractFirstTypeFromFile(File);
							if (Type.IsEmpty()) {
								UE_LOG(LogReflection, Warning, TEXT("Reflect Folder: no type found in \"%s\""), *FPaths::GetCleanFilename(File));
								continue;
							}

							TypeIndex.Add(File, Type);

							if (!TypeSupportMap.Contains(Type)) {
								TypeSupportMap.Add(Type, CanImport(Type));
							}
						}

						/* Save updated index */
						SaveTypeIndex(SelectedFolder, TypeIndex);

						if (TypeSupportMap.Num() == 0) return;

						/* Build type entries for the dialog */
						TArray<FTypeEntry> TypeEntries;
						for (const auto& Pair : TypeSupportMap) {
							FTypeEntry Entry;
							Entry.TypeName = Pair.Key;
							Entry.bSupported = Pair.Value;
							Entry.bSelected = true;
							TypeEntries.Add(Entry);
						}

						/* Show type selection dialog */
						if (!ShowTypeSelectionDialog(TypeEntries)) return;

						/* Build selected types set */
						TSet<FString> SelectedTypes;
						for (const FTypeEntry& Entry : TypeEntries) {
							if (Entry.bSelected) {
								SelectedTypes.Add(Entry.TypeName);
							}
						}

						if (SelectedTypes.Num() == 0) return;

						/* Filter files using the index — no re-parsing needed */
						TArray<FString> Filtered;
						for (const FString& File : JsonFiles) {
							const FString* CachedType = TypeIndex.Find(File);

							if (CachedType && SelectedTypes.Contains(*CachedType)) {
								Filtered.Add(File);
							}
						}

						if (Filtered.Num() == 0) return;

						UE_LOG(LogReflection, Log, TEXT("Reflect Folder: %d types selected, %d files matched"),
							SelectedTypes.Num(), Filtered.Num());

						FImportJob::Enqueue(Filtered);
					})
				),
				NAME_None
			);

			InnerMenuBuilder.AddMenuEntry(
				FText::FromString("Import with Hierarchy"),
				FText::FromString("Import a JSON file and all missing dependencies in order. The selected file is always re-imported."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

				FUIAction(
					FExecuteAction::CreateLambda([] {
						const TArray<FString> Files = OpenFileDialog("Select a JSON File", "JSON Files|*.json");
						if (Files.Num() == 0) return;

						/* Pre-flight: validate hierarchy of the selected file before scanning */
						FString HierarchyError;
						if (!ValidateExportHierarchy(Files[0], HierarchyError)) {
							AppendNotification(
								FText::FromString("Wrong Export Hierarchy"),
								FText::FromString(HierarchyError),
								10.0f,
								SNotificationItem::CS_Fail,
								true,
								500.0f
							);
							return;
						}

						/* bSkipExistingDeps=true: skip deps that already exist in the project,
						 * but the selected file (Depth 0) is always included for re-import */
						const TArray<FString> Order = GetHierarchyImportOrder(Files[0], /* bSkipExistingDeps */ true);

						if (Order.Num() == 0) {
							AppendNotification(
								FText::FromString("Nothing to Import"),
								FText::FromString("All dependencies already exist in the project."),
								4.0f,
								SNotificationItem::CS_Success,
								true,
								400.0f
							);
							return;
						}

						if (Order.Num() == 1) {
							/* No missing dependencies, just import the selected file */
							FImportJob::Enqueue(Files);
							return;
						}

						UE_LOG(LogReflection, Log, TEXT("Import with Hierarchy: %d files in order"), Order.Num());

						AppendNotification(
							FText::FromString(FString::Printf(TEXT("Importing with Hierarchy (%d files)"), Order.Num())),
							FText::FromString(FPaths::GetCleanFilename(Files[0])),
							4.0f,
							FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
							SNotificationItem::CS_Pending,
							false,
							400.0f
						);

						FImportJob::Enqueue(Order, true);
					})
				),
				NAME_None
			);

				InnerMenuBuilder.EndSection();
			}
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "DeveloperTools.MenuIcon")
	);
}
