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
	if (Package == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("SavePackage: Package is null, skipping."));
		return;
	}

	const FString PackageName = Package->GetName();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	const bool bFileExists = IFileManager::Get().FileExists(*PackageFileName);

	UE_LOG(LogReflection, Log, TEXT("SavePackage: '%s'"), *PackageName);
	UE_LOG(LogReflection, Log, TEXT("  FileName: %s"), *PackageFileName);
	UE_LOG(LogReflection, Log, TEXT("  File exists before save: %s"), bFileExists ? TEXT("YES") : TEXT("NO"));
	UE_LOG(LogReflection, Log, TEXT("  Package IsDirty: %s"), Package->IsDirty() ? TEXT("YES") : TEXT("NO"));

#if ENGINE_UE5
	FSavePackageArgs SaveArgs; {
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
	}

	const bool bSuccess = UPackage::SavePackage(Package, nullptr, *PackageFileName, SaveArgs);
	UE_LOG(LogReflection, Log, TEXT("  SavePackage result: %s"), bSuccess ? TEXT("SUCCESS") : TEXT("FAILED"));
#else
	UPackage::SavePackage(Package, nullptr, RF_Standalone, *PackageFileName);
#endif

	const bool bFileExistsAfter = IFileManager::Get().FileExists(*PackageFileName);
	UE_LOG(LogReflection, Log, TEXT("  File exists after save: %s"), bFileExistsAfter ? TEXT("YES") : TEXT("NO"));

	if (bFileExistsAfter) {
		const int64 FileSize = IFileManager::Get().FileSize(*PackageFileName);
		UE_LOG(LogReflection, Log, TEXT("  File size: %lld bytes"), FileSize);
	}
}

inline bool HandleAssetCreation(UObject* Asset, UPackage* Package) {
	if (Asset == nullptr || Package == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("HandleAssetCreation: skipping null asset or package. Asset=%p Package=%p"), Asset, Package);
		return false;
	}

	UE_LOG(LogReflection, Log, TEXT("HandleAssetCreation: START for '%s' (class=%s)"), *Asset->GetName(), *Asset->GetClass()->GetName());
	UE_LOG(LogReflection, Log, TEXT("  Asset outer: %s"), *Asset->GetOuter()->GetFullName());
	UE_LOG(LogReflection, Log, TEXT("  Asset outermost: %s"), *Asset->GetOutermost()->GetName());
	UE_LOG(LogReflection, Log, TEXT("  Package param: %s"), *Package->GetName());
	UE_LOG(LogReflection, Log, TEXT("  Asset outermost == Package: %s"),
		Asset->GetOutermost() == Package ? TEXT("YES") : TEXT("NO - MISMATCH!"));

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

	UE_LOG(LogReflection, Log, TEXT("  Calling FAssetRegistryModule::AssetCreated..."));
	FAssetRegistryModule::AssetCreated(Asset);
	UE_LOG(LogReflection, Log, TEXT("  AssetCreated done"));

	const bool bMarkedDirty = Asset->MarkPackageDirty();
	UE_LOG(LogReflection, Log, TEXT("  MarkPackageDirty: %s"), bMarkedDirty ? TEXT("YES") : TEXT("NO"));

	Package->SetDirtyFlag(true);
	UE_LOG(LogReflection, Log, TEXT("  Package dirty flag set: %s"), Package->IsDirty() ? TEXT("YES") : TEXT("NO"));

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->InitResource();
	}

	UE_LOG(LogReflection, Log, TEXT("  Running PostEditChange..."));
	Asset->PostEditChange();
	UE_LOG(LogReflection, Log, TEXT("  PostEditChange done"));

#if ENGINE_UE5
	FAssetCompilingManager::Get().FinishCompilationForObjects({Asset});
	UE_LOG(LogReflection, Log, TEXT("  Async compilation finished for '%s'"), *Asset->GetName());
#endif

	Asset->AddToRoot();
	UE_LOG(LogReflection, Log, TEXT("  AddToRoot done"));

	const bool bHasPlan = FAssetDependencyRegistry::Get().HasPlan();
	UE_LOG(LogReflection, Log, TEXT("  HasPlan: %s"), bHasPlan ? TEXT("YES") : TEXT("NO"));

	if (bHasPlan) {
		UE_LOG(LogReflection, Log, TEXT("  In a dependency plan; skipping FullyLoad (deferred to final phase)."));
	} else if (Asset->IsA<UWorld>()) {
		UE_LOG(LogReflection, Log, TEXT("  UWorld detected; skipping FullyLoad."));
	} else {
		UE_LOG(LogReflection, Log, TEXT("  NOT in a dependency plan; calling FullyLoad."));
		Package->FullyLoad();
	}

	UE_LOG(LogReflection, Log, TEXT("  Requesting browse..."));
	FAssetDependencyRegistry::Get().RequestBrowse(Asset);
	UE_LOG(LogReflection, Log, TEXT("  RequestBrowse done"));

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->Resource = nullptr;
	}

	UE_LOG(LogReflection, Log, TEXT("HandleAssetCreation: END for '%s'"), *Asset->GetName());
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
