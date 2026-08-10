/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportJob.h"

#include "Editor.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/DependencyRegistry.h"
//crash #include "Utilities/ImportWithHierarchy.h"
#include "Engine/Log.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Remote.h"

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

			/* The whole batch was already planned in Enqueue - scanned, validated, checked for
			 * circular references, and shelled - before this loop took its first step, so this
			 * asks the registry for the exact container CreateShells built rather than parsing
			 * the file a second time into a disconnected copy. */
			FUObjectExportContainer* Container = FAssetDependencyRegistry::Get().GetOrBuildContainer(FilePath);

			if (Container == nullptr) {
				UE_LOG(LogReflection, Warning, TEXT("Nothing to reflect from \"%s\"."), *FilePath);
				GJob->ImportedFiles++;

				continue;
			}

			/* Parsed, but nothing in it the container recognised as an export, so it is finished
			 * the moment it is opened. Returning it would step straight off the end. */
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

		/* Dependency fetches inside these exports still block, and this keeps them painted and
		 * cancellable while they do. Scoped to the slice, so no progress dialog is ever held
		 * across frames. */
		const FBlockingRequestScope BlockingScope(FText::FromString(TEXT("Reflecting from the Cloud")));

		do {
			if (!OpenPendingFile()) {
				return false;
			}

			FUObjectExport* Export = GJob->Container->Exports[GJob->ExportIndex++];

			if (Export == nullptr) continue;
			if (!GJob->BlueprintType.IsEmpty() && Export->GetType() != GJob->BlueprintType) continue;

			IImportReader::ReadExportAndImport(GJob->Container, Export, GJob->CurrentFile);

			KeepContainerObjects();
		} while (FPlatformTime::Seconds() - SliceStart < SliceBudgetSeconds);

		return true;
	}

	void FinishJob() {
		if (GEditor != nullptr) {
			GEditor->GetTimerManager()->ClearTimer(GJob->Timer);
		}

		RemoveNotification(GJob->Notification);

		/* Final phase: every deferred compile/PostLoad/save the populate phase queued up
		 * (FAssetDependencyRegistry::RequestFinalize) now runs, with every shell in the whole
		 * batch fully populated - including ones a circular reference would otherwise have
		 * needed only partially loaded to reach. */
		FAssetDependencyRegistry::Get().RunFinalPhase();

		const int32 Count = GJob->ImportedFiles;

		/* Dropped before the notification below, so a handler that starts another import does not
		 * find this one still standing */
		GJob.Reset();

		UE_LOG(LogReflection, Log, TEXT("Reflection finished: %d file(s)."), Count);

		AppendNotification(
			FText::FromString(FString::Printf(TEXT("Reflected %d file(s)"), Count)),
			FText::FromString(TEXT("")),
			4.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_Success,
			false,
			310.0f
		);
	}

	void TickJob() {
		if (!GJob.IsValid()) {
			return;
		}

		/* Building assets while the game is running in the editor is asking for trouble, so the
		 * job waits it out rather than racing PIE. Said out loud, because a progress line that
		 * stops moving on its own reads exactly like the hang this is meant to avoid. */
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
}

void FImportJob::Enqueue(const TArray<FString>& Files, bool bUseHierarchy) {
	if (Files.Num() == 0) {
		return;
	}

	/* A batch import has no inherent order - a folder scan comes back alphabetically, a file
	 * dialog in selection order - and importing a struct or blueprint before the asset it
	 * references leaves that reference unresolved: a UDS member whose struct type was still
	 * absent imports as "Struct unknown (deleted?)". Re-order any multi-file import so every
	 * dependency comes first, pulling in missing transitive dependencies and leaving existing
	 * on-disk ones to resolve from disk. A single file needs none of this - its dependencies
	 * load on demand when the import hits them. */
	/*crash TArray<FString> Batch = Files;

	if (!bUseHierarchy && Files.Num() > 1) {
		TSet<FString> Scanned;
		TSet<FString> OrderedSet;
		TArray<FString> Ordered;

		for (const FString& File : Files) {
			Ordered.Append(CollectHierarchyImportOrder(File, Scanned, OrderedSet, /* bSkipExistingDeps */ /*crash true));
	    }

		if (Ordered.Num() > 0) {
			Batch = MoveTemp(Ordered);
			bUseHierarchy = true;
		}
	}*/

	/* No editor loop to slice against, so there is nothing to hand a frame back to */
	if (GEditor == nullptr) {
		for (const FString& File : Files) {
		//crash for (const FString& File : Batch) {
			IImportReader::ImportReference(File);
		}

		return;
	}

	/* Scans, validates, checks for circular references, and shells every file in this call
	 * before a single export is imported - see FAssetDependencyRegistry. Only the explicit
 	 * "Import with Hierarchy" tool needs any of that; regular single-file, batch and folder
 	 * imports skip it and build the container for each file on demand, like the plugin did
 	 * before the hierarchy feature existed. */
	 /*crash before a single export is imported - see FAssetDependencyRegistry. The explicit "Import
	 * with Hierarchy" tool asks for it directly; a multi-file batch reaches it through the
	 * dependency-ordered reorder above. A single file skips it and builds the container on
	 * demand, loading dependencies as its import runs into them. */
	FAssetDependencyRegistry& Registry = FAssetDependencyRegistry::Get();

	if (bUseHierarchy) {
		Registry.Plan(Files);
		//Registry.Plan(Batch);
	}

	/* A file whose /Game/ parent blueprint must be imported first is dropped here - importing
	 * it would load the parent off disk while this batch is mid-way through creating the
	 * child's package (the recursive-flush crash). Only the hierarchy preflight flags files,
	 * so this only ever drops anything when a hierarchy plan is running. */
	TArray<FString> ImportableFiles = Files;
	//TArray<FString> ImportableFiles = Batch;
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

	/* The export in flight is left to finish; only what has not been started is dropped */
	GJob->Files.SetNum(GJob->FileIndex);

	if (GJob->Container != nullptr) {
		GJob->ExportIndex = GJob->Container->Exports.Num();
	}
}