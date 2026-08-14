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
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Modules/UI/TypeSelectionDialog.h"
#include "Utilities/Dialog.h"
#include "Utilities/ImportWithHierarchy.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/MissingDependencies.h"

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
				FText::FromString("Reflect Folder of JSON Files"),
				FText::FromString("Import JSON files from a folder. Choose which asset types to import."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.BspMode"),

				FUIAction(
					FExecuteAction::CreateLambda([] {
						TArray<FString> JsonFiles;

						for (const FString& Folder : OpenFolderDialog("Folder of JSON files")) {
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

						/* Scan all JSON files to collect unique types */
						TMap<FString, bool> TypeSupportMap;

						for (const FString& File : JsonFiles) {
							TArray<TSharedPtr<FJsonValue>> DataObjects;
							if (!DeserializeJSON(File, DataObjects)) continue;

							for (const TSharedPtr<FJsonValue>& Value : DataObjects) {
								const TSharedPtr<FJsonObject> Obj = Value->AsObject();
								if (!Obj.IsValid()) continue;

								const FString Type = Obj->GetStringField(TEXT("Type"));
								if (!Type.IsEmpty() && !TypeSupportMap.Contains(Type)) {
									TypeSupportMap.Add(Type, CanImport(Type));
								}
							}
						}

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

						/* Filter files: keep only files whose first export's Type is selected */
						TArray<FString> Filtered;
						for (const FString& File : JsonFiles) {
							TArray<TSharedPtr<FJsonValue>> DataObjects;
							if (!DeserializeJSON(File, DataObjects)) continue;

							for (const TSharedPtr<FJsonValue>& Value : DataObjects) {
								const TSharedPtr<FJsonObject> Obj = Value->AsObject();
								if (!Obj.IsValid()) continue;

								const FString Type = Obj->GetStringField(TEXT("Type"));
								if (SelectedTypes.Contains(Type)) {
									Filtered.Add(File);
									break;
								}
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
