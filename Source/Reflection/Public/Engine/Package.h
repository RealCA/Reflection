/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "VectorField/VectorFieldStatic.h"
#include "UObject/ObjectRedirector.h"

#include "Utilities/ContentBrowser.h"
#include "Utilities/Dialog.h"
#include "Importers/Constructor/DependencyRegistry.h"

#if ENGINE_UE5
#include "AssetCompilingManager.h"
#include "UObject/SavePackage.h"
#endif

/* Follows an object redirector to whatever it points at.*/
inline UObject* ResolveRedirector(UObject* Object) {
	/* Redirectors chain. The cap only exists so a self referential one cannot spin forever. */
	for (int32 Depth = 0; Depth < 16; ++Depth) {
		const UObjectRedirector* Redirector = Cast<UObjectRedirector>(Object);

		if (Redirector == nullptr) {
			return Object;
		}

		Object = Redirector->DestinationObject;
	}

	return nullptr;
}

/* Loads an asset by package path, following any redirector left behind by a rename. Every load
 * the importer does by path goes through here, so none of them can hand back a redirector. */
template <typename T>
T* LoadObjectByPath(const FString& Path) {
	return Cast<T>(ResolveRedirector(StaticLoadObject(T::StaticClass(), nullptr, *Path)));
}

inline void SavePackage(UPackage* Package) {
	const FString PackageName = Package->GetName();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

#if ENGINE_UE5
	FSavePackageArgs SaveArgs; {
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
	}

	UPackage::SavePackage(Package, nullptr, *PackageFileName, SaveArgs);
#else
	UPackage::SavePackage(Package, nullptr, RF_Standalone, *PackageFileName);
#endif
}

inline bool HandleAssetCreation(UObject* Asset, UPackage* Package) {
	if (Asset == nullptr || Package == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("HandleAssetCreation: skipping null asset or package."));
		return false;
	}

	{
		/* User Failsafe.... */
		const UPackage* AssetOutermostPackage = Asset->GetOutermost();
		const FString PackageName = AssetOutermostPackage->GetName();

		const FString Path = FPackageName::GetLongPackagePath(PackageName);
		if (!Path.StartsWith(TEXT("/")) || Path.Len() < 2) {
			SpawnPrompt("Failsafe", "Here's some reasons why:\n\n- You didn't export it from FModel\n- Reflected it from a random path, not in Exports/.../\n\nPlease reflect it again next time at the proper location.");

			return false;
		}
	}

	FAssetRegistryModule::AssetCreated(Asset);
	if (!Asset->MarkPackageDirty()) return false;

	Package->SetDirtyFlag(true);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->InitResource();
	}

	UE_LOG(LogReflection, Log, TEXT("HandleAssetCreation: \"%s\" registered, running PostEditChange."), *Asset->GetName());

	Asset->PostEditChange();

#if ENGINE_UE5
	/* PostEditChange() can kick off async asset compilation (USkeletalMesh::Build()
	 * queues a background build and returns immediately). Wait for the compilation
	 * so the asset is fully built before it gets saved, and nothing below races the
	 * still-running task. */
	FAssetCompilingManager::Get().FinishCompilationForObjects({Asset});
	UE_LOG(LogReflection, Log, TEXT("HandleAssetCreation: async compilation finished for \"%s\"."), *Asset->GetName());
#endif

	Asset->AddToRoot();

	/* FullyLoad() re-enters the loader on a package this batch is still populating - the shell
	 * (CreateAssetPackageSafe) already fully loaded it, or deliberately skipped it for a circular
	 * dependency, where re-loading would run a reference fix-up over the freshly imported - and
	 * not yet compiled - anim graph, tripping pin-link validation and walking dangling object
	 * pointers (the IsValidLowLevel assert in GarbageCollection.cpp). Deferred with the rest of
	 * the batch finalization; only the standalone, non-batch path loads here. */
	if (FAssetDependencyRegistry::Get().HasPlan()) {
		UE_LOG(LogReflection, Log, TEXT("HandleAssetCreation: \"%s\" in a dependency plan; skipping FullyLoad (deferred to final phase)."), *Asset->GetName());
	} else {
		Package->FullyLoad();
	}

	/* Deferred to the batch final phase when a dependency plan is running: SyncBrowserToAssets
	 * renders a thumbnail, which instantiates the anim graph of a freshly created - and not yet
	 * compiled - AnimBlueprint, tripping pin-link validation and a GC assert on the dangling
	 * nodes. BrowseToAssetSafe swallows any access violation that still slips through. */
	FAssetDependencyRegistry::Get().RequestBrowse(Asset);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->Resource = nullptr;
	}

	/* PostLoad() is a deserialization hook; calling it on a freshly created,
	 * in-memory asset is wrong. For USkeletalMesh it even destroys the async build
	 * task PostEditChange just queued (USkinnedAsset::PostLoad reassigns AsyncTask),
	 * asserting in the FAsyncTask destructor. Importers that genuinely need extra
	 * initialization call PostLoad() themselves. */

	return true;
}

inline FString GetAssetPath(const UObject* Object) {
	if (!Object) {
		return FString();
	}

	if (const UPackage* Package = Object->GetOutermost()) {
		return Package->GetName();
	}

	return FString();
}

inline void MoveToTransientPackageAndRename(UObject* Object) {
	if (Object) {
		Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
		Object->SetFlags(RF_Transient);
	}
}

inline void MoveToTransientPackagesAndRename(TArray<UObject*> Objects) {
	for (UObject* Object : Objects) {
		MoveToTransientPackageAndRename(Object);
	}
}
