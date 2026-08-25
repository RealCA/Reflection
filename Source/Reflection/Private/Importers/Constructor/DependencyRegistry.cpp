/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/DependencyRegistry.h"

#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Importers/Constructor/TemplatedImporter.h"
#include "Importers/Types/DataAssetImporter.h"
#include "Containers/ExportContainer.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/MissingDependencies.h"
#include "Utilities/SehHelpers.h"
#include "Engine/Compatibility.h"
#include "Engine/Log.h"

FAssetDependencyRegistry& FAssetDependencyRegistry::Get() {
	static FAssetDependencyRegistry Instance;
	return Instance;
}

FSoftObjectPath FAssetDependencyRegistry::BuildObjectPath(const FString& PackagePath) const {
	FString ShortName = PackagePath;
	ShortName.Split(TEXT("/"), nullptr, &ShortName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	if (ShortName.IsEmpty()) {
		return FSoftObjectPath(PackagePath);
	}

	return FSoftObjectPath(PackagePath + TEXT(".") + ShortName);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Phase 1: Scan ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void FAssetDependencyRegistry::ScanFile(const FString& JsonFilePath, const int32 Depth) {
	if (Depth > GMaxDependencyRecursionDepth || JsonFilePath.IsEmpty()) {
		return;
	}

	if (ScannedFiles.Contains(JsonFilePath)) {
    /*crash const FString Normalized = JsonFilePath.Contains(TEXT("\\")) ? JsonFilePath.Replace(TEXT("\\"), TEXT("/")) : JsonFilePath;
	const bool bIsRootFile = RootFilePaths.Contains(Normalized);

	if (ScannedFiles.Contains(Normalized)) {*/
		return;
	}

    ScannedFiles.Add(JsonFilePath);
    OrderedFiles.Add(JsonFilePath);
	/*crash ScannedFiles.Add(Normalized);
	OrderedFiles.Add(Normalized);*/

	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(JsonFilePath, DataObjects) || DataObjects.Num() == 0) {
		UE_LOG(LogReflection, Warning, TEXT("DependencyPlan Step 1/6 Scan: failed to parse \"%s\"."), *JsonFilePath);
		return;
	}

	const TSharedPtr<FJsonObject> RootObj = DataObjects[0]->AsObject();
	if (!RootObj.IsValid() || !RootObj->HasField(TEXT("Package"))) {
		return;
	}

	const FString PackagePath = RootObj->GetStringField(TEXT("Package"));
	if (PackagePath.IsEmpty()) {
		return;
	}

	const FSoftObjectPath ObjectPath = BuildObjectPath(PackagePath);
	if (Entries.Contains(ObjectPath)) {
		return;
	}

	FAssetEntry Entry;
	Entry.PackagePath = PackagePath;
	Entry.ObjectPath = ObjectPath;
	Entry.ClassName = RootObj->HasField(TEXT("Type")) ? RootObj->GetStringField(TEXT("Type")) : FString();
	Entry.JsonPath = JsonFilePath;
	Entry.State = EAssetDependencyState::Internal;

	/* An existing-on-disk dependency must not be shelled: the plan would build an empty shell
	 * at its path that shadows the real asset, and any dependent that imports this batch would
	 * resolve to that shell and get an empty struct type ("Struct unknown (deleted?)"). A
	 * dependency this batch has no JSON for is Internal by definition - it must be imported to
	 * exist at all. A root file is always Internal: the user explicitly asked for it, even
	 * when its UE asset already exists (a re-import). */
	/*crash if (!bIsRootFile && AssetExistsInProject(PackagePath)) {
		Entry.State = EAssetDependencyState::External;
	}*/

	/* A /Game/ SuperStruct means this asset derives from another blueprint (or ControlRig).
	 * Recorded so the Plan preflight can enforce parent-first importing when the parent is
	 * neither in this batch nor already imported into memory. */
	if (RootObj->HasField(TEXT("SuperStruct")) && RootObj->GetObjectField(TEXT("SuperStruct"))->HasField(TEXT("ObjectPath"))) {
		FString ParentObjectPath = RootObj->GetObjectField(TEXT("SuperStruct"))->GetStringField(TEXT("ObjectPath"));
		if (ParentObjectPath.Contains(TEXT("/Game/"))) {
			ParentObjectPath.Split(TEXT("."), &ParentObjectPath, nullptr);
			Entry.ParentPackagePath = ParentObjectPath;
		}
	}

	TSet<FString> Seen;
	TArray<FString> References;
	for (const TSharedPtr<FJsonValue>& Obj : DataObjects) {
		References.Append(CollectGameReferences(Obj, Seen));
	}

	TArray<FString> FilesToScan;

	for (const FString& Ref : References) {
		if (Ref.Equals(PackagePath, ESearchCase::IgnoreCase)) {
			continue;
		}

		Entry.Dependencies.AddUnique(BuildObjectPath(Ref));

		const FString RefFile = ExportPathForGameRef(Ref, JsonFilePath);
		if (!RefFile.IsEmpty() && FPaths::FileExists(RefFile)) {
			FilesToScan.Add(RefFile);
		}
	}

	/* Registered before recursing into what it depends on. ScannedFiles above is what actually
	 * stops a cycle in the file graph from recursing forever; this only needs to be true before
	 * ValidateDependencies and CreateShells run, both of which happen after every file in the
	 * batch has been through this function once. */
	Entries.Add(ObjectPath, MoveTemp(Entry));

	for (const FString& RefFile : FilesToScan) {
		ScanFile(RefFile, Depth + 1);
	}
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Phase 2: Validate ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void FAssetDependencyRegistry::ValidateDependencies() {
	TSet<FSoftObjectPath> AllDependencies;

	for (const TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
		for (const FSoftObjectPath& Dep : Pair.Value.Dependencies) {
			AllDependencies.Add(Dep);
		}
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 2/6 Validate: classifying %d reference(s) outside the batch."), AllDependencies.Num());

	int32 ExternalCount = 0;
	int32 MissingCount = 0;

	for (const FSoftObjectPath& Dep : AllDependencies) {
		/* Already scanned from its own JSON - it's Internal, not something to classify here */
		if (Entries.Contains(Dep)) {
			continue;
		}

		FAssetEntry Entry;
		Entry.ObjectPath = Dep;
		Entry.PackagePath = Dep.GetLongPackageName();

		/* FindObject for memory, FileExists for disk - never LoadObject. A hit on either
		 * means the dependency is usable, so only truly-absent assets are classified Missing. */
		if (AssetExistsInProject(Entry.PackagePath) || AssetExistsOnDisk(Entry.PackagePath)) {
			Entry.State = EAssetDependencyState::External;
			++ExternalCount;
		} else {
			Entry.State = EAssetDependencyState::Missing;
			++MissingCount;
		}

		Entries.Add(Dep, MoveTemp(Entry));
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 2/6 Validate: %d existing (memory/disk), %d missing."), ExternalCount, MissingCount);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Phase 3: Detect circular ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void FAssetDependencyRegistry::DetectCircular() {
	struct FFrame {
		FSoftObjectPath Node;
		int32 ChildCursor = 0;
	};

	TMap<FSoftObjectPath, int32> Index;
	TMap<FSoftObjectPath, int32> LowLink;
	TMap<FSoftObjectPath, bool> OnStack;
	TArray<FSoftObjectPath> Stack;
	int32 NextIndex = 0;

	for (const TPair<FSoftObjectPath, FAssetEntry>& StartPair : Entries) {
		const FSoftObjectPath& Start = StartPair.Key;
		if (Index.Contains(Start)) {
			continue;
		}

		TArray<FFrame> CallStack;
		CallStack.Push({Start, 0});
		Index.Add(Start, NextIndex);
		LowLink.Add(Start, NextIndex);
		NextIndex++;
		Stack.Push(Start);
		OnStack.Add(Start, true);

		while (CallStack.Num() > 0) {
			FFrame& Frame = CallStack.Last();
			const FAssetEntry* NodeEntry = Entries.Find(Frame.Node);
			const TArray<FSoftObjectPath>* Deps = NodeEntry ? &NodeEntry->Dependencies : nullptr;

			if (Deps && Frame.ChildCursor < Deps->Num()) {
				const FSoftObjectPath Child = (*Deps)[Frame.ChildCursor++];

				/* External/Missing entries have no outgoing edges of their own (their JSON, if
				 * any, was never scanned) so they can never be part of a cycle */
				if (!Entries.Contains(Child)) {
					continue;
				}

				if (!Index.Contains(Child)) {
					Index.Add(Child, NextIndex);
					LowLink.Add(Child, NextIndex);
					NextIndex++;
					Stack.Push(Child);
					OnStack.Add(Child, true);
					CallStack.Push({Child, 0});
				} else if (OnStack.FindRef(Child)) {
					LowLink[Frame.Node] = FMath::Min(LowLink[Frame.Node], Index[Child]);
				}

				continue;
			}

			/* Every child has been visited - if this node is the root of its own low-link, it
			 * closes a strongly connected component */
			if (LowLink[Frame.Node] == Index[Frame.Node]) {
				TArray<FSoftObjectPath> Component;
				FSoftObjectPath Popped;

				do {
					Popped = Stack.Pop();
					OnStack[Popped] = false;
					Component.Add(Popped);
				} while (Popped != Frame.Node);

				const bool bSelfLoop = Component.Num() == 1 && Entries.Find(Component[0])
					&& Entries.Find(Component[0])->Dependencies.Contains(Component[0]);

				if (Component.Num() > 1 || bSelfLoop) {
					for (const FSoftObjectPath& Member : Component) {
						if (FAssetEntry* MemberEntry = Entries.Find(Member)) {
							MemberEntry->bIsCircular = true;
						}
					}
				}
			}

			CallStack.Pop();

			if (CallStack.Num() > 0) {
				const FSoftObjectPath& Parent = CallStack.Last().Node;
				LowLink[Parent] = FMath::Min(LowLink[Parent], LowLink[Frame.Node]);
			}
		}
	}

	int32 CircularCount = 0;
	for (const TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
		if (Pair.Value.bIsCircular) {
			++CircularCount;
		}
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 3/6 Circular: %d asset(s) part of a cycle."), CircularCount);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Phase 4: Create shells ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

FUObjectExportContainer* FAssetDependencyRegistry::GetOrBuildContainer(const FString& JsonFilePath) {
	if (FUObjectExportContainer** Existing = FileContainers.Find(JsonFilePath)) {
		return *Existing;
	}

	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(JsonFilePath, DataObjects) || DataObjects.Num() == 0) {
		return nullptr;
	}

	/* Left alive deliberately, same as every other container Reflection builds - see
	 * ImportJob.cpp. Importers keep exports referencing this well past this call. */
	FUObjectExportContainer* Container = new FUObjectExportContainer(DataObjects);
	FileContainers.Add(JsonFilePath, Container);

	return Container;
}

void FAssetDependencyRegistry::CreateShells() {
	int32 ShellCount = 0;

	for (TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
		FAssetEntry& Entry = Pair.Value;

		if (Entry.State != EAssetDependencyState::Internal || Entry.bShellCreated || Entry.JsonPath.IsEmpty() || Entry.bBlocked) {
			continue;
		}

		FUObjectExportContainer* Container = GetOrBuildContainer(Entry.JsonPath);
		if (Container == nullptr) {
			continue;
		}

		const FString ContainerType = Container->GetBlueprintType();

		/* World containers import only the World export; sub-exports (Level, Model, WorldSettings,
		 * etc.) are handled internally by the level importer. Without this, both the Level and World
		 * exports would be shelled and imported as separate assets. */
		const bool bHasWorld = Container->HasWorldType();

		/* A blueprint's first export can be its Default__ CDO, whose "Type" is the generated
		 * class's short name (Foo_C) - not something FindClassByType knows. The container's
		 * resolved blueprint type (BlueprintGeneratedClass / Anim / Widget / RigVM) is what
		 * the importer is registered under, so shelling has to key off that when the container
		 * has one. Without it, ControlRigs and CDO-first blueprints never got a shell, and a
		 * dependent's LoadClass fell back to a disk load mid-batch (the recursive-flush crash). */
		FString ShellType = !ContainerType.IsEmpty() ? ContainerType : Entry.ClassName;

		/* UserDefinedEnums are not shelled. An enum's only content is its names, so nothing
		 * depends on it being built before its dependents - but a shell shadows the real asset
		 * at the same path. When a dependent blueprint imports, resolving its enum properties
		 * finds the populated shell and skips the disk, and a later on-demand load of the same
		 * package deserializes the on-disk asset over the already-populated enum, tripping
		 * UEnum::Serialize's "NumValues == 0" assert. Leaving enums out means the dependent's
		 * resolution LoadObjectByPath hits the disk copy directly (a leaf asset, safe mid-batch). */
		if (ShellType == TEXT("UserDefinedEnum")) {
			continue;
		}

		/* World containers: override shell type to World regardless of Entry.ClassName.
		 * The first JSON element may be a Level/Model/etc., but the World export is what
		 * should be shelled and imported. */
		if (bHasWorld) {
			ShellType = TEXT("World");
		}

		FUObjectExport* Export = !ContainerType.IsEmpty()
			? Container->FindByType(ContainerType)
			: Container->FindByType(bHasWorld ? TEXT("World") : Entry.ClassName);

		if (!Export->IsJsonValid()) {
			continue;
		}

		UClass* Class = FindClassByType(ShellType);
		if (Class == nullptr) {
			continue;
		}

		/* bSkipFullyLoad=true: the creation phase must not load anything from disk.
		 * A batch-internal package's on-disk imports could route back through a
		 * circular pair and trigger a recursive loader flush inside FullyLoad(). */
		UPackage* Package = FAssetUtilities::CreateAssetPackage(Entry.PackagePath, true);
		if (Package == nullptr) {
			UE_LOG(LogReflection, Warning, TEXT("Dependency planning: failed to create package \"%s\"; it will be skipped."), *Entry.PackagePath);
			continue;
		}

		Export->Package = Package;

		IImporter* Importer = IImportReader::CreateImporterForType(ShellType, Class);
		Importer->Initialize(Export, Container);
		Importer->SetSourceFile(Entry.JsonPath);

		/* Builds the empty shell only - every existing CreateAsset override (NewObject for the
		 * templated importer, FKismetEditorUtilities::CreateBlueprint for the blueprint family)
		 * already stops short of deserializing properties, which is exactly what this phase
		 * needs and none of them had to change to provide it. */
		UObject* Shell = Importer->CreateAsset(nullptr);

		if (Shell == nullptr) {
			delete Importer;
			continue;
		}

		Entry.CreatedObject = Shell;
		Entry.Importer = Importer;
		Entry.bShellCreated = true;
		++ShellCount;

		UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 4/6 Shell: \"%s\" (%s)."), *Entry.PackagePath, *Shell->GetClass()->GetName());
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 4/6 Shells: %d shell(s) built."), ShellCount);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Orchestration ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void FAssetDependencyRegistry::Plan(const TArray<FString>& RootFiles) {
	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 1/6 Scan: %d root file(s)."), RootFiles.Num());

	/* Recorded before the scan loop: ScanFile classifies a dependency reached mid-scan as
	 * External when its UE asset already exists on disk, but an explicitly selected file is
	 * always imported - this set is what tells the two apart. */
	//crash RootFilePaths.Empty();

	for (const FString& File : RootFiles) {
		FString Normalized = File;
		if (Normalized.Contains(TEXT("\\"))) {
			Normalized = Normalized.Replace(TEXT("\\"), TEXT("/"));
		}

		//crash RootFilePaths.Add(Normalized);
		ScanFile(Normalized);
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 1/6 Scan: %d unique asset(s) planned."), Entries.Num());

	/* Preflight: a blueprint-family asset whose /Game/ parent is neither populated by this
	 * batch nor already imported into memory must be skipped. Its shell creation would
	 * LoadClass the parent off disk while this batch is still building the child's package,
	 * re-entering the loader on a mid-creation package - the recursive-flush crash. Non-
	 * hierarchy layouts hit this constantly because a dependency's JSON can't be located, so
	 * the parent never gets planned (and therefore never gets a shell). */
	{
		TSet<FString> RootSet;
		for (const FString& File : RootFiles) {
			RootSet.Add(File.Contains(TEXT("\\")) ? File.Replace(TEXT("\\"), TEXT("/")) : File);
		}

		BlockedFiles.Empty();

		/* Re-evaluated from scratch each Plan() call - a file blocked by an earlier (additive)
		 * Enqueue can legitimately become importable once its parent has been imported. */
		for (TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
			Pair.Value.bBlocked = false;
		}

		for (const FString& File : RootFiles) {
			const FString Normalized = File.Contains(TEXT("\\")) ? File.Replace(TEXT("\\"), TEXT("/")) : File;

			FAssetEntry* Entry = nullptr;
			for (TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
				if (Pair.Value.JsonPath.Replace(TEXT("\\"), TEXT("/")).Equals(Normalized, ESearchCase::IgnoreCase)) {
					Entry = &Pair.Value;
					break;
				}
			}

            if (Entry == nullptr || Entry->ParentPackagePath.IsEmpty()) {
			//crash if (Entry == nullptr) {
				continue;
			}

			/* The scan above can reach a root file through another root's dependency walk first
			 * and classify it External because its UE asset exists on disk. It is a selected
			 * root, so it must be imported - restore Internal. */
			/*crash Entry->State = EAssetDependencyState::Internal;

			if (Entry->ParentPackagePath.IsEmpty()) {
				continue;
			}*/

			/* Parent is populated by this batch: its own JSON is one of the root files, so it
			 * gets a shell now and is filled in before the child's deferred compile runs. */
			if (const FAssetEntry* ParentEntry = Entries.Find(BuildObjectPath(Entry->ParentPackagePath))) {
				if (RootSet.Contains(ParentEntry->JsonPath.Replace(TEXT("\\"), TEXT("/")))) {
					continue;
				}
			}

			/* Parent was already fully imported this session (rooted by HandleAssetCreation),
			 * so a StaticLoadObject on it resolves against memory without touching the loader. */
			if (IsAssetFullyImported(Entry->ParentPackagePath)) {
				continue;
			}

			Entry->bBlocked = true;
			BlockedFiles.Add(Normalized, Entry->ParentPackagePath);

			UE_LOG(LogReflection, Warning, TEXT("DependencyPlan Preflight: \"%s\" skipped - import its parent \"%s\" first."),
				*Normalized, *Entry->ParentPackagePath);
		}
	}

	ValidateDependencies();
	DetectCircular();

	/* CreateShells must never FullyLoad a package this batch will populate itself - a circular
	 * pair that already exists on disk would have the loader flush one mid-load ("Flushing
	 * package X recursively") and hand back partially-loaded objects. Registered here so
	 * CreateAssetPackageSafe's bSkipFullyLoad path covers every circular entry during shell
	 * creation; their shells are built fresh, and the import rebuilds them entirely anyway. */
	for (const TPair<FSoftObjectPath, FAssetEntry>& Pair : Entries) {
		if (Pair.Value.bIsCircular) {
			GetKnownCircularPackages().Add(Pair.Value.PackagePath);
		}
	}

	CreateShells();

	bHasPlanned = true;

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan: planning complete, %d entry/entries total."), Entries.Num());
}

FAssetEntry* FAssetDependencyRegistry::Find(const FSoftObjectPath& ObjectPath) {
	return Entries.Find(ObjectPath);
}

FAssetEntry* FAssetDependencyRegistry::FindByPackagePath(const FString& PackagePath) {
	return Find(BuildObjectPath(PackagePath));
}

void FAssetDependencyRegistry::RequestFinalize(TFunction<void()> Finalizer) {
	PendingFinalizers.Add(MoveTemp(Finalizer));
}

void FAssetDependencyRegistry::RequestBrowse(UObject* Asset) {
	/* No plan means no final phase is coming to run the browse later, so fall back to the
	 * immediate sync. BrowseToAssetSafe still swallows any access violation the thumbnail
	 * generation trips over. */
	if (!bHasPlanned) {
		BrowseToAssetSafe(Asset);
		return;
	}

	/* HandleAssetCreation AddToRoot()'s the asset before requesting the browse, so the raw
	 * pointer stays valid until the deferred sync runs. */
	PendingBrowses.Add([Asset]() { BrowseToAssetSafe(Asset); });
}

void FAssetDependencyRegistry::RunFinalPhase() {
	/* A compile access violation poisoned this job: the process is in an
	 * undefined state, and the stub finalizers re-run Construct + compile on
	 * exactly those damaged blueprints (08.24 crash: the finalizer's variable
	 * construction died on a blueprint whose compile had just faulted). Skip
	 * the whole phase - the stubs stay stubs and are rebuilt by the next
	 * import after the auto-clean. */
	if (IsBlueprintCompilePoisoned()) {
		UE_LOG(LogReflection, Error,
			TEXT("DependencyPlan Step 6/6 Finalize: SKIPPED - a compile access violation aborted this job. "
			     "Deferred real imports were not run; re-import after the corrupted asset is cleaned."));
		Reset();
		return;
	}

	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 6/6 Finalize: running %d deferred finalizer(s)."), PendingFinalizers.Num());

	/* Copied out first: a finalizer can itself import something new (a save triggering asset
	 * validation, for instance), and that must not be able to invalidate the array this loop
	 * is walking. */
	TArray<TFunction<void()>> Finalizers = MoveTemp(PendingFinalizers);
	PendingFinalizers.Reset();

	for (const TFunction<void()>& Finalizer : Finalizers) {
		/* A finalizer can itself trip a compile AV - stop the moment the job is
		 * poisoned instead of running more finalizers on damaged state. */
		if (IsBlueprintCompilePoisoned()) {
			UE_LOG(LogReflection, Error, TEXT("DependencyPlan Step 6/6 Finalize: aborted mid-loop (compile access violation)."));
			break;
		}
		if (Finalizer) {
			Finalizer();
		}
	}

	/* Content Browser syncs run last, once every deferred compile in this batch has completed
	 * - the thumbnail generation that SyncBrowserToAssets triggers instantiates an anim
	 * blueprint's graph, which is only safe after the graph was compiled. */
	UE_LOG(LogReflection, Log, TEXT("DependencyPlan Step 6/6 Finalize: syncing %d asset(s) to the Content Browser."), PendingBrowses.Num());

	TArray<TFunction<void()>> Browses = MoveTemp(PendingBrowses);
	PendingBrowses.Reset();

	for (const TFunction<void()>& Browse : Browses) {
		if (Browse) {
			Browse();
		}
	}

	Reset();
}

void FAssetDependencyRegistry::Reset() {
	Entries.Empty();
	ScannedFiles.Empty();
	OrderedFiles.Empty();
	FileContainers.Empty();
	PendingFinalizers.Empty();
	PendingBrowses.Empty();
	BlockedFiles.Empty();
	//crash RootFilePaths.Empty();
	bHasPlanned = false;
}