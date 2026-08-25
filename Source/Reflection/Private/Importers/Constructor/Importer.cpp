/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Importer.h"

#include "Settings/ReflectionSettings.h"

#include "Misc/MessageDialog.h"

/* ~~~~~~~~~~~~~ Templated Engine Classes ~~~~~~~~~~~~~ */
#include "Engine/Log.h"
#include "Engine/EngineUtilities.h"
/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

UObject* IImporter::CreateAsset(UObject* CreatedAsset) {
	if (CreatedAsset) {
		AssetExport->Object = CreatedAsset;
    
		return CreatedAsset;
	}

	return nullptr;
}

void IImporter::Save() const {
	const UReflectionSettings* Settings = GetSettings();

	UPackage* Pkg = GetPackage();
	UE_LOG(LogReflection, Log, TEXT("IImporter::Save: Package=%s SaveAssets=%s"),
		Pkg ? *Pkg->GetName() : TEXT("NULL"),
		Settings->AssetSettings.SaveAssets ? TEXT("true") : TEXT("false"));

	if (Pkg == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("IImporter::Save: Package is null"));
		return;
	}

	if (Settings->AssetSettings.SaveAssets) {
		SavePackage(GetPackage());
	}
}

bool IImporter::OnAssetCreation(UObject* Asset) const {
	UE_LOG(LogReflection, Log, TEXT("IImporter::OnAssetCreation: START for '%s'"),
		Asset ? *Asset->GetName() : TEXT("NULL"));

	const bool Synced = HandleAssetCreation(Asset, GetPackage());
	UE_LOG(LogReflection, Log, TEXT("IImporter::OnAssetCreation: HandleAssetCreation returned %s"), Synced ? TEXT("true") : TEXT("false"));

	if (Synced) {
		UE_LOG(LogReflection, Log, TEXT("IImporter::OnAssetCreation: Calling Save..."));
		Save();
		UE_LOG(LogReflection, Log, TEXT("IImporter::OnAssetCreation: Save done"));
	}

	UE_LOG(LogReflection, Log, TEXT("IImporter::OnAssetCreation: END (Synced=%s)"), Synced ? TEXT("true") : TEXT("false"));
	return Synced;
}
