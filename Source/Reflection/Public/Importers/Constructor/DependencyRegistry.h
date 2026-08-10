/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

class IImporter;
struct FUObjectExportContainer;

/*
 * How a dependency resolved during the validation phase (Utilities/MissingDependencies.h has
 * the pure-JSON scanning this classification is built on top of).
 */
enum class EAssetDependencyState : uint8 {
	/* Not looked at yet */
	Unknown,

	/* Its JSON export is part of the current import batch. Resolving it means reading
	 * AssetEntry::CreatedObject out of the registry - never LoadObject. */
	Internal,

	/* Not part of this batch, but FindObject (no load) already finds it in the project. Safe
	 * to resolve with a load later, since it isn't something this batch is still building. */
	External,

	/* Neither in the batch nor on disk anywhere reachable. Recorded so every importer that
	 * hits this reference reports the same "missing" answer instead of each retrying its own
	 * failed lookup. */
	Missing
};

/*
 * One node in the dependency graph: everything Reflection knows about a single asset before a
 * single UObject is touched.
 */
struct REFLECTION_API FAssetEntry {
	/* /Game/Foo/Bar - the UPackage this asset will live in */
	FString PackagePath;

	/* /Game/Foo/Bar.Bar - PackagePath plus the short asset name, matching what
	 * IImporter::LoadExport builds when it resolves a property reference */
	FSoftObjectPath ObjectPath;

	/* Export "Type" from the JSON - AnimBlueprintGeneratedClass, StaticMesh, etc. */
	FString ClassName;

	/* Absolute path to the JSON export file this entry was scanned from. Empty for External
	 * and Missing entries, since there is nothing in this batch to populate them from. */
	FString JsonPath;

	/* Every other asset this one's JSON references (property values, not just the JSON-level
	 * "sibling export" nesting a blueprint holds) */
	TArray<FSoftObjectPath> Dependencies;

	/* Filled in by the creation phase. Populate-properties and every other importer's
	 * reference resolution reads this pointer and only this pointer - it never changes once
	 * set, so every reference to this asset across the whole batch ends up pointing at the
	 * same UObject. */
	UObject* CreatedObject = nullptr;

	/* The live importer instance that built CreatedObject, kept around so the populate phase
	 * calls Import() on the same instance instead of standing up a second one. Owned by the
	 * registry; see FAssetDependencyRegistry::Reset. */
	IImporter* Importer = nullptr;

	/* Set once DetectCircular has run. An asset can be perfectly valid and still circular -
	 * this only ever changes how it's populated (registry lookups instead of loads), never
	 * whether it imports at all. */
	bool bIsCircular = false;

	/* /Game/ parent class package this asset derives from (its SuperStruct), empty for native
	 * /Script/ parents and non-blueprint assets. Set during ScanFile. */
	FString ParentPackagePath;

	/* The parent above is neither populated in this batch nor already imported into memory, so
	 * importing this asset would have to load the parent off disk mid-creation (the
	 * recursive-flush crash). Set by the Plan preflight; blocked entries are not shelled and
	 * their files are skipped by the caller. */
	bool bBlocked = false;

	EAssetDependencyState State = EAssetDependencyState::Unknown;

	/* Phase 4 has produced CreatedObject for this entry */
	bool bShellCreated = false;

	/* Phase 6 has deserialized this entry's properties into CreatedObject */
	bool bPopulated = false;
};

/*
 * Plans a whole import batch before any package or UObject exists, so that resolving one
 * asset's dependency never has to fall back on LoadObject/LoadPackage for something this same
 * batch is already in the middle of building - the situation that produces "Flushing package
 * recursively", partially-loaded packages, and BlueprintCompilationManager queue errors.
 *
 * Phases, in order:
 *   1. ScanFile / Plan       - read every export's JSON, register it, collect its references.
 *                               Touches nothing but strings and files.
 *   2. ValidateDependencies  - classify every collected reference as Internal (in this batch),
 *                               External (already in the project), or Missing.
 *   3. DetectCircular        - Tarjan over the in-memory graph built by 1-2.
 *   4. CreateShells          - create every package and an empty UObject shell for every
 *                               Internal entry. No property is deserialized here.
 *
 * After Plan() returns, every asset this batch will create already exists as an (empty)
 * UObject with a stable pointer. What ImportReader does after that - deserializing properties
 * and compiling - is the "populate" and "final" phases; this class only covers planning.
 */
class REFLECTION_API FAssetDependencyRegistry {
public:
	static FAssetDependencyRegistry& Get();

	/* Runs phases 1-4 for RootFiles. Safe to call more than once in the same batch (a
	 * dependency import reached from inside property deserialization can extend the same
	 * plan); files already scanned are skipped rather than re-registered. */
	void Plan(const TArray<FString>& RootFiles);

	/* Phase 1: reads JsonFilePath, registers it (and everything it references, recursively,
	 * still without touching any UObject) into the registry. */
	void ScanFile(const FString& JsonFilePath, int32 Depth = 0);

	/* Phase 2 */
	void ValidateDependencies();

	/* Phase 3 */
	void DetectCircular();

	/* Phase 4 */
	void CreateShells();

	/* Phase 5 helper - the only way anything should resolve a same-batch reference */
	FAssetEntry* Find(const FSoftObjectPath& ObjectPath);
	FAssetEntry* FindByPackagePath(const FString& PackagePath);

	/* Preflight result from the last Plan() call: JSON files skipped because their /Game/
	 * parent blueprint must be imported first. Key is the normalized JSON path, value the
	 * parent's package path. */
	const TMap<FString, FString>& GetBlockedFiles() const { return BlockedFiles; }

	/* The explicitly selected JSON files of the last Plan() call, normalized. A file the user
	 * selected is always imported even when its UE asset already exists on disk, so ScanFile
	 * treats a root as Internal no matter what; only dependencies reached from them can fall
	 * back to External (already on disk, used as-is). */
	//crash const TSet<FString>& GetRootFilePaths() const { return RootFilePaths; }

	/* Populate-phase entry point. Returns the container CreateShells built for JsonFilePath,
	 * with every Internal export's Package and Object already filled in - building it on
	 * first use if CreateShells hasn't reached it yet. Always the same instance for the same
	 * path, so the populate phase walks the exact FUObjectExport/UObject pairs the creation
	 * phase made rather than a second, disconnected parse of the same file. */
	FUObjectExportContainer* GetOrBuildContainer(const FString& JsonFilePath);

	/* Every file Plan() has scanned, in scan order. What FImportJob walks during the populate
	 * phase instead of re-discovering files on its own. */
	const TArray<FString>& GetPlannedFiles() const { return OrderedFiles; }

	bool HasPlan() const { return bHasPlanned; }

	/* Phase 7: rather than compiling/saving/PostLoading the moment one asset's properties are
	 * filled in, an importer that needs another asset in the batch to exist first (blueprint
	 * compilation is the main case - see AnimationBlueprintImporter) calls this instead of
	 * doing that work inline. Runs once every export in the batch has been through the
	 * populate phase. */
	void RequestFinalize(TFunction<void()> Finalizer);

	/* Phase 7b: defers the Content Browser sync (BrowseToAsset) to the final phase, after
	 * every deferred compile has run. A freshly created AnimBlueprint is still uncompiled
	 * until its deferred compile executes, and SyncBrowserToAssets renders a thumbnail that
	 * instantiates the anim graph - on a half-built graph that trips pin-link validation and
	 * dereferences dangling nodes (a GC assert). With no plan in flight the browse runs
	 * immediately. */
	void RequestBrowse(UObject* Asset);

	/* Runs every deferred finalizer in the order they were requested, then every deferred
	 * browse, then clears the plan. Called once by the top-level import entry points
	 * (IImportReader::ReadExportsAndImport, FImportJob) after their last export finishes. */
	void RunFinalPhase();

	/* Drops the whole plan. Called by RunFinalPhase; exposed for callers (Cancel) that need to
	 * abandon a batch outright. */
	void Reset();

private:
	TMap<FSoftObjectPath, FAssetEntry> Entries;
	TArray<TFunction<void()>> PendingFinalizers;
	TArray<TFunction<void()>> PendingBrowses;
	TSet<FString> ScannedFiles;
	TArray<FString> OrderedFiles;

	/* Files from the last Plan() whose parent must be imported first (see GetBlockedFiles) */
	TMap<FString, FString> BlockedFiles;

	/* Normalized paths of the files passed to Plan(). See GetRootFilePaths. */
	//crash TSet<FString> RootFilePaths;

	/* One container per scanned file, built lazily by CreateShells (or GetOrBuildContainer,
	 * for a file ScanFile reached but CreateShells hasn't processed yet). Left alive on Reset
	 * the same way FImportJob leaves its own containers alive - importers keep exports
	 * referencing one well past this registry's lifetime for the batch. */
	TMap<FString, FUObjectExportContainer*> FileContainers;

	bool bHasPlanned = false;

	FSoftObjectPath BuildObjectPath(const FString& PackagePath) const;
};