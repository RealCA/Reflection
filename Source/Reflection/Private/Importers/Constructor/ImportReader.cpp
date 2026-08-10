/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportReader.h"

#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Settings/Runtime.h"
#include "Styling/SlateIconFinder.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/DependencyRegistry.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/MissingDependencies.h"
#include "Modules/Cloud/Remote.h"

IImporter* IImportReader::CreateImporterForType(const FString& Type, const UClass* Class) {
	const bool InheritsDataAsset = Class != nullptr && Class->IsChildOf(UDataAsset::StaticClass());

	if (const FImporterFactoryDelegate* Factory = FindFactoryForAssetType(Type)) {
		if (IImporter* Importer = (*Factory)()) {
			return Importer;
		}
	}

	if (InheritsDataAsset) {
		return new IDataAssetImporter();
	}

	return new ITemplatedImporter<UObject>();
}

bool IImportReader::ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, const bool HideNotifications) {
	/* Importers resolve references through the Cloud while they deserialize, and those requests
	 * have nowhere to put a callback, so they get waited on. The scope is what keeps the editor
	 * drawn and cancellable for as long as this import needs the Cloud. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "CloudImporting", "Reflecting {0}"),
		FText::FromString(FPaths::GetCleanFilename(File))
	));

	/* Only a genuine on-disk JSON file can be planned ahead of time - a hidden, nested call
	 * (Cloud's ConstructAsset, or a dependency reached from inside property deserialization)
	 * hands in an in-memory export array with no file behind File to scan, and must not try to
	 * run - or worse, finish and reset - a plan some outer, still-in-progress batch owns. */
	FAssetDependencyRegistry& Registry = FAssetDependencyRegistry::Get();
	const bool bOwnsPlan = !HideNotifications && FPaths::FileExists(File);

	/* No dependency scan, validation or shelling here. Single-file and nested imports build
	 * the container directly and run the final phase when they own the batch; only the explicit
	 * "Import with Hierarchy" tool plans ahead of time (through FImportJob::Enqueue). */
	FUObjectExportContainer* Container = bOwnsPlan ? Registry.GetOrBuildContainer(File) : nullptr;
	if (Container == nullptr) {
		Container = new FUObjectExportContainer(Exports);
	}

	/* The blueprint importers rebuild the whole blueprint - functions, structs, subobjects - from
	 * the main export and a compile, so the nested exports (Function, ScriptStruct, Default__ CDO,
	 * etc.) must not be reflected on their own. Anim and Widget blueprints export under their own
	 * *GeneratedClass names, so all of them have to be detected, not just BlueprintGeneratedClass. */
	const FString ContainerType = Container->GetBlueprintType();

	for (FUObjectExport* Export : Container->Exports) {
		if (!ContainerType.IsEmpty()) {
			if (Export->GetType() != ContainerType) continue;
		}
		
		if (IImporter* Importer = ReadExportAndImport(Container, Export, File, HideNotifications)) OutImporter = Importer;
	}

	/* Final phase: PostLoad, blueprint/ControlRig/AnimBlueprint compiles, and saves that the
	 * populate phase deferred (FAssetDependencyRegistry::RequestFinalize) all run now, once
	 * every export in the batch has a fully populated shell to reference. */
	if (bOwnsPlan) {
		Registry.RunFinalPhase();
	}

	return true;
}

IImporter* IImportReader::ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, const bool HideNotifications) {
	const FString Type = Export->GetType().ToString();
	FString Name = Export->GetName().ToString();

	const bool IsBlueprint = Type.Contains("BlueprintGeneratedClass");

	/* BlueprintGeneratedClass is post-fixed with _C */
	if (IsBlueprint) {
		Name.Split("_C", &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	const UClass* Class = FindClassByType(Type);
	
	if (Class == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("No UClass found for export type \"%s\" (\"%s\"); skipping."), *Type, *Name);
		return nullptr;
	}

	/* Check if this export can be imported */
	if (!CanImport(Type, false, Class)) {
		UE_LOG(LogReflection, Warning, TEXT("Type \"%s\" (\"%s\") is not in the importable list; skipping."), *Type, *Name);
		return nullptr;
	}

	/* A blueprint-family export whose /Game/ parent is neither populated by this batch nor
	 * already imported into memory must not be built - its import would LoadClass the parent
	 * off disk while this batch is mid-way through creating the child (the recursive-flush
	 * crash). Enforced only while a hierarchy plan is running; regular imports just import
	 * what was selected. */
	if (FAssetDependencyRegistry::Get().HasPlan()
		&& Export->JsonObject.IsValid() && Export->JsonObject->HasField(TEXT("SuperStruct"))
		&& Export->JsonObject->GetObjectField(TEXT("SuperStruct"))->HasField(TEXT("ObjectPath"))) {
		FString ParentObjectPath = Export->JsonObject->GetObjectField(TEXT("SuperStruct"))->GetStringField(TEXT("ObjectPath"));

		if (ParentObjectPath.Contains(TEXT("/Game/"))) {
			ParentObjectPath.Split(TEXT("."), &ParentObjectPath, nullptr);

			const FAssetEntry* ParentEntry = FAssetDependencyRegistry::Get().FindByPackagePath(ParentObjectPath);

			if ((ParentEntry == nullptr || !ParentEntry->bShellCreated) && !IsAssetFullyImported(ParentObjectPath)) {
				UE_LOG(LogReflection, Warning, TEXT("Skipping \"%s\": import its parent blueprint \"%s\" first."), *Name, *ParentObjectPath);
				return nullptr;
			}
		}
	}

	/* Convert from relative path to full path */
	if (FPaths::IsRelative(File)) File = FPaths::ConvertRelativePathToFull(File);

	/* Planning already ran for this export if it's part of a batch that went through
	 * FAssetDependencyRegistry::Plan (ImportJob and the file-path branch of ReadExportsAndImport
	 * both do this before any export is visited): the package exists, the shell exists, and
	 * Importer is the very instance that built it. Reusing both means nothing here calls
	 * LoadObject or forces a FullyLoad a second time for this export, and Import() below fills
	 * in the same UObject every other export in the batch already resolved its reference to -
	 * IImporter::Create<T>() hands back AssetExport->Object once CreateAsset has set it, rather
	 * than building a second, disconnected instance. */
	UPackage* LocalPackage = Export->Package;
	IImporter* Importer = nullptr;

	if (LocalPackage != nullptr) {
		if (FAssetEntry* PlannedEntry = FAssetDependencyRegistry::Get().FindByPackagePath(LocalPackage->GetName())) {
			Importer = PlannedEntry->Importer;
		}
	}

	if (Importer == nullptr) {
		/* Nothing planned this export ahead of time - a single dependency import reached from
		 * inside property deserialization (IImporter::LoadExport's DownloadWrapper path, for an
		 * asset that was never part of a plan), or a Cloud response that never had a JSON file
		 * on disk to plan from in the first place. Falls back to the original behaviour: create
		 * a package and stand up a fresh importer instance, with no dependency scanning. */
		FString FailureReason;
		LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

		if (LocalPackage == nullptr) {
			/* Try fixing our Export Directory Settings using the provided File directory if local package not found */
			UReflectionSettings* PluginSettings = GetSettings();

			GReflectionRuntime.Update();
			LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

			if (LocalPackage == nullptr) {
				FString ExportDirectoryCache = GReflectionRuntime.ExportDirectory.Path;

				if (FString DirectoryPathFix; File.Split(TEXT("Output/Exports/"), &DirectoryPathFix, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
					DirectoryPathFix = DirectoryPathFix + TEXT("Output/Exports");

					GReflectionRuntime.ExportDirectory.Path = DirectoryPathFix;
					SavePluginSettings(PluginSettings);

					/* Retry creating the asset package */
					LocalPackage = FAssetUtilities::CreateAssetPackage(Name, File, FailureReason);

					/* Undo the change if unsuccessful */
					if (LocalPackage == nullptr) {
						GReflectionRuntime.ExportDirectory.Path = ExportDirectoryCache;

						SavePluginSettings(PluginSettings);
					}
				}
			}
		}

		if (LocalPackage == nullptr) {
			AppendNotification(
				FText::FromString("Failed: " + Type),
				FText::FromString(FailureReason),
				4.0f,
				FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
				SNotificationItem::CS_Fail,
				false,
				350.0f
			);

			return nullptr;
		}

		Importer = CreateImporterForType(Type, Class);

		Export->Package = LocalPackage;
		Importer->Initialize(Export, Container);
		Importer->SetSourceFile(File);
	}

	/* Import the asset ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	bool Successful = false; {
		try {
			Successful = Importer->Import();
		} catch (const char* Exception) {
			UE_LOG(LogReflection, Error, TEXT("Importer exception: %s"), *FString(Exception));
		}
	}

	if (HideNotifications) {
		return Importer;
	}

	FString ClassIconType = Type;

	if (Type.Contains("GeneratedClass")) {
		Type.Split("GeneratedClass", &ClassIconType, nullptr);
	}

	if (Successful) {
		UE_LOG(LogReflection, Log, TEXT("Successfully reflected \"%s\" as \"%s\""), *Name, *Type);

		/* Successful Notification */
		AppendNotification(
			FText::FromString("Reflected: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + ClassIconType)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Success,
			false,
			350.0f
		);
	} else {
		/* Failed Notification */
		AppendNotification(
			FText::FromString("Failed: " + Name),
			FText::FromString(Type),
			2.0f,
			FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + ClassIconType)), TEXT("ClassThumbnail")),
			SNotificationItem::CS_Fail,
			false,
			350.0f
		);
	}

	return Importer;
}

IImporter* IImportReader::ImportReference(const FString& File) {
	FString FilePath = File;
	if (FilePath.Contains("\\")) {
		FilePath = File.Replace(TEXT("\\"), TEXT("/"));
	}
	
	TArray<TSharedPtr<FJsonValue>> DataObjects; {
		DeserializeJSON(FilePath, DataObjects);
	}

	IImporter* Importer = nullptr;
	ReadExportsAndImport(DataObjects, FilePath, Importer);
	
	return Importer;
}