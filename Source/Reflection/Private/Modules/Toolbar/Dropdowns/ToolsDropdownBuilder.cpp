/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ToolsDropdownBuilder.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/ImportJob.h"
#include "Importers/Constructor/ImportReader.h"

#if ENGINE_UE4
#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"
#endif

#include "Engine/EngineUtilities.h"
#include "Modules/UI/StyleModule.h"

#include "Modules/Toolbar/Tools/ClearImportData.h"
#include "Modules/Toolbar/Tools/FixUpAssetData.h"
#include "Utilities/Dialog.h"
#include "Utilities/ImportWithHierarchy.h"
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
				FText::FromString("Import all JSON files in a folder. Existing assets can be kept or replaced."),
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

						SpawnYesNoPrompt(
							"Replace Existing Assets?",
							FString::Printf(TEXT("Found %d JSON file(s).\n\nYes = Replace all (re-import everything)\nNo = Add only new (skip assets already in project)"), JsonFiles.Num()),
							[JsonFiles](bool bReplaceAll) {
								if (bReplaceAll) {
									/* Replace all: import everything */
									FImportJob::Enqueue(JsonFiles);
									return;
								}

								/* Add only new: filter out files whose UE asset already exists */
								TArray<FString> Filtered;
								int32 Skipped = 0;

								for (const FString& File : JsonFiles) {
									const FString Package = GetPackagePathFromJson(File);
									if (!Package.IsEmpty() && AssetExistsInProject(Package)) {
										++Skipped;
										continue;
									}
									Filtered.Add(File);
								}

								if (Filtered.Num() == 0) {
									AppendNotification(
										FText::FromString("Nothing to Import"),
										FText::FromString(FString::Printf(TEXT("All %d asset(s) already exist in the project."), Skipped)),
										4.0f,
										SNotificationItem::CS_Success,
										true,
										350.0f
									);
									return;
								}

								UE_LOG(LogReflection, Log, TEXT("Reflect Folder: %d new / %d existing, importing %d"),
									Filtered.Num(), Skipped, Filtered.Num());

								FImportJob::Enqueue(Filtered);
							}
						);
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
