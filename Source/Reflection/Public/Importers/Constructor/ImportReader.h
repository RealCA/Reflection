/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importer.h"
#include "Dom/JsonValue.h"
#include "Containers/Export.h"

class REFLECTION_API IImportReader {
public:
	static bool ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, bool HideNotifications = false);
	static IImporter* ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, bool HideNotifications = false);
	static IImporter* ImportReference(const FString& File);

	/* Picks the same importer ReadExportAndImport would for an export of this type - factory
	 * delegate first, then UDataAssetImporter for anything inheriting UDataAsset, then the
	 * templated fallback - without creating a package or calling Import(). Shared by
	 * ReadExportAndImport itself and FAssetDependencyRegistry::CreateShells so a type is only
	 * ever mapped to an importer class in one place. */
	static IImporter* CreateImporterForType(const FString& Type, const UClass* Class);
};