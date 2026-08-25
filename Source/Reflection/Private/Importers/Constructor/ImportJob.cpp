/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportJob.h"

#include "Editor.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/DependencyRegistry.h"
#include "Importers/Types/Blueprint/BlueprintStubFactory.h"

//crash #include "Utilities/ImportWithHierarchy.h"
#include "Engine/Log.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Remote.h"
#include "Utilities/SehHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/MetaData.h"

namespace {
	/* How long one tick may spend importing. Plenty of single exports run well past this and
	 * cannot be divided any further; the budget is here so that the small ones are not spread
	 * over a frame each for no reason. */
	constexpr double SliceBudgetSeconds = 0.008;

	/* Effectively "every tick". The timer manager treats a zero or negative rate as an error, so
	 * this is the smallest honest way to ask for one step per frame. */
	constexpr float StepRate = 0.001f;

	struct FJobState {
		TArray<FString> Files;
		int32 FileIndex = 0;

		/* The file currently being walked ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		FString CurrentFile;
		FUObjectExportContainer* Container = nullptr;
		int32 ExportIndex = 0;

		/* A file containing a BlueprintGeneratedClass imports only that, the same way
		 * IImportReader::ReadExportsAndImport decides it */
		FString BlueprintType;

		int32 ImportedFiles = 0;

		/* Full frames happen between slices, which means garbage collection does too. The objects
		 * an export builds are reachable from a raw pointer in the container and not much else, so
		 * the job holds them itself until it is done. */
		TArray<TStrongObjectPtr<UObject>> Keep;
		TSet<UObject*> Kept;

		FTimerHandle Timer;
		TWeakPtr<SNotificationItem> Notification;
	};

	TUniquePtr<FJobState> GJob;

	bool IsPlayingInEditor() {
		if (GEditor == nullptr) {
			return false;
		}

		for (const FWorldContext& WorldContext : GEditor->GetWorldContexts()) {
			if (WorldContext.World() && WorldContext.World()->WorldType == EWorldType::PIE) {
				return true;
			}
		}

		return false;
	}

	void UpdateNotification() {
		const TSharedPtr<SNotificationItem> Item = GJob->Notification.Pin();
		if (!Item.IsValid()) {
			return;
		}

		Item->SetText(FText::FromString(FString::Printf(
			TEXT("Reflecting %d/%d: %s"),
			GJob->ImportedFiles + 1,
			GJob->Files.Num(),
			*FPaths::GetCleanFilename(GJob->CurrentFile)
		)));
	}

	/* Holds on to whatever the last export built, along with anything else the container has
	 * filled in along the way */
	void KeepContainerObjects() {
		if (GJob->Container == nullptr) {
			return;
		}

		for (const FUObjectExport* Export : GJob->Container->Exports) {
			if (Export == nullptr || Export->Object == nullptr) {
				continue;
			}

			if (GJob->Kept.Contains(Export->Object)) {
				continue;
			}

			GJob->Kept.Add(Export->Object);
			GJob->Keep.Add(TStrongObjectPtr<UObject>(Export->Object));
		}
	}

	/* Moves on to the next file when the current one is spent. False once there is nothing left. */
	bool OpenPendingFile() {
		if (GJob->Container != nullptr && GJob->Container->Exports.IsValidIndex(GJob->ExportIndex)) {
			return true;
		}

		/* The file that just finished */
		if (GJob->Container != nullptr) {
			GJob->Container = nullptr;
			GJob->ImportedFiles++;
		}

		while (GJob->Files.IsValidIndex(GJob->FileIndex)) {
			FString FilePath = GJob->Files[GJob->FileIndex++];

			if (FilePath.Contains("\\")) {
				FilePath = FilePath.Replace(TEXT("\\"), TEXT("/"));
			}

			FUObjectExportContainer* Container = FAssetDependencyRegistry::Get().GetOrBuildContainer(FilePath);

			if (Container == nullptr) {
				UE_LOG(LogReflection, Warning, TEXT("Nothing to reflect from \"%s\"."), *FilePath);
				GJob->ImportedFiles++;

				continue;
			}

			if (Container->Exports.Num() == 0) {
				UE_LOG(LogReflection, Warning, TEXT("No exports found in \"%s\"."), *FilePath);
				GJob->ImportedFiles++;

				continue;
			}

			GJob->Container = Container;
			GJob->CurrentFile = FilePath;
			GJob->ExportIndex = 0;
			GJob->BlueprintType = Container->GetBlueprintType();

			UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 5/6 Populate: \"%s\" (%d export(s))."), *FilePath, Container->Exports.Num());

			UpdateNotification();

			return true;
		}

		return false;
	}

	/* One frame's worth of importing. False once the job is done. */
	bool StepJob() {
		const double SliceStart = FPlatformTime::Seconds();

		const FBlockingRequestScope BlockingScope(FText::FromString(TEXT("Reflecting from the Cloud")));

		do {
			/* A guarded compile caught an access violation earlier in this job:
			 * the process is in an undefined state and continuing churns it into
			 * a delayed editor crash (08.24: the progress-dialog teardown died
			 * on corrupted Slate state minutes after the swallowed AV). Abort
			 * cleanly instead. */
			if (IsBlueprintCompilePoisoned()) {
				UE_LOG(LogReflection, Error,
					TEXT("Import aborted: a blueprint compile hit an access violation earlier in this job. "
					     "Delete the corrupted asset it named, then re-import."));
				return false;
			}

			if (!OpenPendingFile()) {
				return false;
			}

			FUObjectExport* Export = GJob->Container->Exports[GJob->ExportIndex++];

			if (Export == nullptr) continue;
			if (!GJob->BlueprintType.IsEmpty() && Export->GetType() != GJob->BlueprintType) continue;

			const double ExportStart = FPlatformTime::Seconds();
			IImportReader::ReadExportAndImport(GJob->Container, Export, GJob->CurrentFile);
			/* Per-export duration - the 08.25 session hid 80-200s stalls between
			 * Populate lines and there was no way to tell which export ate it. */
			UE_LOG(LogReflection, Log, TEXT("[Timing] %s :: %s took %.2fs"),
				*FPaths::GetCleanFilename(GJob->CurrentFile),
				*Export->GetName().ToString(),
				FPlatformTime::Seconds() - ExportStart);

			KeepContainerObjects();
		} while (FPlatformTime::Seconds() - SliceStart < SliceBudgetSeconds);

		return true;
	}

	void FinishJob() {
		if (GEditor != nullptr) {
			GEditor->GetTimerManager()->ClearTimer(GJob->Timer);
		}

		RemoveNotification(GJob->Notification);

		FAssetDependencyRegistry::Get().RunFinalPhase();

		const int32 Count = GJob->ImportedFiles;
		const bool bPoisoned = IsBlueprintCompilePoisoned();

		FBlueprintStubFactory::ClearStubImports();
		GJob.Reset();

		ResetBlueprintCompilePoison();

		UE_LOG(LogReflection, Log, TEXT("Reflection finished: %d file(s)."), Count);

		AppendNotification(
			FText::FromString(bPoisoned
				? TEXT("Reflection ABORTED - blueprint compile access violation")
				: FString::Printf(TEXT("Reflected %d file(s)"), Count)),
			FText::FromString(bPoisoned
				? TEXT("Delete the corrupted asset named in the log, then re-import")
				: TEXT("")),
			8.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			bPoisoned ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success,
			false,
			310.0f
		);
	}

	void TickJob() {
		if (!GJob.IsValid()) {
			return;
		}

		if (IsPlayingInEditor()) {
			if (const TSharedPtr<SNotificationItem> Item = GJob->Notification.Pin()) {
				Item->SetText(FText::FromString(TEXT("Reflection paused while in Play In Editor")));
			}

			return;
		}

		if (!StepJob()) {
			FinishJob();

			return;
		}

		UpdateNotification();
	}

	/* SEH wrapper (C2712-clean: no C++ locals). Deletes a stale asset file;
	 * never let a file op take the editor down. */
	static bool TryDeleteAssetFileRaw(const TCHAR* DiskFile) {
		__try {
			return IFileManager::Get().Delete(DiskFile);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	/* Auto-clean (plan 013): a compile access violation records its blueprint's
	 * package to Saved/CorruptedImports.txt. On the NEXT enqueue, a recorded
	 * package is deleted ONLY when every guard holds:
	 *   1. the on-disk asset carries the "ReflectionStub" metadata tag - proof
	 *      it is a plugin-created dependency stub, not a real import, not a
	 *      foreign asset;
	 *   2. the stub's own source JSON is in THIS run's resolved queue - the
	 *      deletion is always immediately followed by a fresh re-import.
	 * Real imports (ReflectionImport tag), foreign assets and anything not
	 * being re-imported are never touched - only logged. */
	static void AutoCleanCorruptedImports(const TArray<FString>& ResolvedFiles) {
		const FString RecordPath = FPaths::ProjectSavedDir() / TEXT("CorruptedImports.txt");
		TArray<FString> Packages;
		if (!FFileHelper::LoadFileToStringArray(Packages, *RecordPath)) {
			return;
		}

		// Consume the record regardless of outcome: each entry gets one clean attempt.
		IFileManager::Get().Delete(*RecordPath);

		auto Normalize = [](const FString& In) {
			FString S = In.Replace(TEXT("\\"), TEXT("/"));
			return S;
		};

		for (const FString& RawPackage : Packages) {
			const FString PackagePath = RawPackage.TrimStartAndEnd();
			if (PackagePath.IsEmpty()) continue;

			const FString AssetPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
			UBlueprint* Asset = LoadObject<UBlueprint>(nullptr, *AssetPath);
			if (!Asset) {
				UE_LOG(LogReflection, Log, TEXT("AutoClean: %s is not a blueprint package - left alone."), *PackagePath);
				continue;
			}

			/* Provenance lives in the package metadata (FMetaData), set at import time. */
			FString StubSource;
			if (UPackage* AssetPkg = Asset->GetPackage()) {
				if (const FString* Tagged = AssetPkg->GetMetaData().FindValue(Asset, TEXT("ReflectionStub"))) {
					StubSource = *Tagged;
				}
			}
			if (StubSource.IsEmpty()) {
				UE_LOG(LogReflection, Warning,
					TEXT("AutoClean: %s is not a plugin-created stub (no ReflectionStub tag) - NEVER touched."),
					*PackagePath);
				continue;
			}

			const FString NormalizedStubSource = Normalize(StubSource);
			bool bInQueue = false;
			for (const FString& QueueFile : ResolvedFiles) {
				if (Normalize(QueueFile).Equals(NormalizedStubSource, ESearchCase::IgnoreCase)) {
					bInQueue = true;
					break;
				}
			}
			if (!bInQueue) {
				UE_LOG(LogReflection, Warning,
					TEXT("AutoClean: corrupted stub %s (from %s) is not in this import batch - left for the next run that re-imports it."),
					*PackagePath, *StubSource);
				continue;
			}

			if (UPackage* Pkg = FindPackage(nullptr, *PackagePath)) {
				Pkg->SetDirtyFlag(false);
			}
			const FString DiskFile = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
			if (TryDeleteAssetFileRaw(*DiskFile)) {
				UE_LOG(LogReflection, Log, TEXT("AutoClean: deleted stale stub %s (will be re-imported fresh from %s)"),
					*PackagePath, *StubSource);
			} else {
				UE_LOG(LogReflection, Warning, TEXT("AutoClean: could not delete %s - the re-import will overwrite it."), *DiskFile);
			}
		}
	}
}

void FImportJob::Enqueue(const TArray<FString>& Files, bool bUseHierarchy) {
	if (Files.Num() == 0) {
		return;
	}

	/* Resolve BP dependencies: prepend dependency JSONs before the main files */
	TArray<FString> ResolvedFiles;
	TSet<FString> Added;
	for (const FString& File : Files) {
		TArray<FString> Deps = FBlueprintStubFactory::ResolveDependencies(File);
		for (int32 i = Deps.Num() - 1; i >= 0; --i) {
			if (!Added.Contains(Deps[i])) {
				ResolvedFiles.Insert(Deps[i], 0);
				Added.Add(Deps[i]);
			}
		}
		if (!Added.Contains(File)) {
			ResolvedFiles.Add(File);
			Added.Add(File);
		}
	}

	/* The files the user actually picked import as real blueprints, never as stubs.
	 * A file registered as a dependency stub in an earlier batch is unregistered here
	 * so importing the real file replaces the stub instead of re-stubbing it. */
	for (const FString& File : Files) {
		FBlueprintStubFactory::UnregisterStubImport(File);
	}

	/* Self-healing (plan 013): remove stale stub assets a previous aborted run
	 * recorded as compile-faulting, before this job starts. Guarded to tagged
	 * stubs being re-imported in this very batch. */
	AutoCleanCorruptedImports(ResolvedFiles);

	UE_LOG(LogReflection, Log, TEXT("Import queue: %d files (%d deps resolved)"), ResolvedFiles.Num(), ResolvedFiles.Num() - Files.Num());

	/* No editor loop to slice against, so there is nothing to hand a frame back to */
	if (GEditor == nullptr) {
		for (const FString& File : ResolvedFiles) {
			IImportReader::ImportReference(File);
		}

		return;
	}

	FAssetDependencyRegistry& Registry = FAssetDependencyRegistry::Get();

	if (bUseHierarchy) {
		Registry.Plan(ResolvedFiles);
	}

	TArray<FString> ImportableFiles = ResolvedFiles;
	if (bUseHierarchy) {
		FString SkippedNames;
		FString ParentNames;
		TSet<FString> ParentsSeen;

		for (int32 i = ImportableFiles.Num() - 1; i >= 0; --i) {
			FString Normalized = ImportableFiles[i];
			if (Normalized.Contains(TEXT("\\"))) {
				Normalized = Normalized.Replace(TEXT("\\"), TEXT("/"));
			}

			if (const FString* Parent = Registry.GetBlockedFiles().Find(Normalized)) {
				if (!SkippedNames.IsEmpty()) SkippedNames += TEXT(", ");
				SkippedNames += FPaths::GetCleanFilename(ImportableFiles[i]);

				if (!ParentsSeen.Contains(*Parent)) {
					ParentsSeen.Add(*Parent);
					if (!ParentNames.IsEmpty()) ParentNames += TEXT(", ");
					ParentNames += FPaths::GetBaseFilename(*Parent);
				}

				ImportableFiles.RemoveAt(i);
			}
		}

		if (!SkippedNames.IsEmpty()) {
			UE_LOG(LogReflection, Warning, TEXT("Import skipped %s - import their parent(s) first: %s."), *SkippedNames, *ParentNames);

			AppendNotification(
				FText::FromString(TEXT("Import Parent First")),
				FText::FromString(FString::Printf(
					TEXT("Skipped: %s\n\nParent(s) to import first: %s\n\nUse \"Import with Hierarchy\", or import the parent JSON(s) first and re-run."),
					*SkippedNames,
					*ParentNames
				)),
				10.0f,
				SNotificationItem::CS_Fail,
				true,
				500.0f
			);
		}
	}

	if (ImportableFiles.Num() == 0) {
		return;
	}

	if (GJob.IsValid()) {
		GJob->Files.Append(ImportableFiles);
		UpdateNotification();

		return;
	}

	GJob = MakeUnique<FJobState>();
	GJob->Files = ImportableFiles;

	/* Fresh job: clear any poison left by an earlier aborted run. */
	ResetBlueprintCompilePoison();

	GJob->Notification = AppendNotificationWithHandler(
		FText::FromString(TEXT("Reflecting")),
		FText::FromString(TEXT("")),
		999.0f,
		FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
		SNotificationItem::CS_Pending,
		false,
		340.0f
	);

	GEditor->GetTimerManager()->SetTimer(
		GJob->Timer,
		FTimerDelegate::CreateStatic(&TickJob),
		StepRate,
		true
	);
}

bool FImportJob::IsRunning() {
	return GJob.IsValid();
}

void FImportJob::Cancel() {
	if (!GJob.IsValid()) {
		return;
	}

	GJob->Files.SetNum(GJob->FileIndex);

	if (GJob->Container != nullptr) {
		GJob->ExportIndex = GJob->Container->Exports.Num();
	}
}
