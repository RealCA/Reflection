/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"
#include "Utilities/PSKXMesh.h"
#include "Utilities/UEModelMesh.h"

class USkeletalMesh;
class USkeleton;

/* Imports a skeletal mesh from the binary FModel writes next to the skeletal
 * mesh's JSON export - .uemodel where available, falling back to ActorX
 * (.pskx/.psk) for older exports. The JSON drives asset creation and the
 * import flow either way; all the geometry, bones and skinning live in the
 * binary. */
class ISkeletalMeshImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* OutData carries the parsed binary data back out so morph targets (which
	 * need the raw per-vertex deltas) can be applied once the async Build()
	 * triggered by OnAssetCreation has populated the render buffers. */
	bool ImportFromUEModel(const FString& FilePath, USkeletalMesh* SkeletalMesh, FUEModelMeshData& OutData) const;
	bool ImportFromPSKX(const FString& FilePath, USkeletalMesh* SkeletalMesh, FPSKXMeshData& OutData) const;

#if ENGINE_UE5
	/* Applies the sibling JSON export's Skeleton and SkeletalMaterials references
	 * to the freshly built mesh (the binary only carries geometry). */
	void ApplyJSONProperties(USkeletalMesh* SkeletalMesh);

	/* Registers morph targets (blend shapes) decoded from the mesh binary.
	 * Must run after OnAssetCreation's Build() has populated
	 * LODModel.MeshToImportVertexMap, which maps render vertices back to the
	 * shared vertex-index space the morph deltas are indexed against. */
	void ApplyMorphTargets(USkeletalMesh* SkeletalMesh, const FPSKXMeshData& Data);
	void ApplyMorphTargets(USkeletalMesh* SkeletalMesh, const FUEModelMeshData& Data, int32 LODIndex = 0);
#endif
};

REGISTER_IMPORTER(ISkeletalMeshImporter, {
	"SkeletalMesh"
}, "Skeletal Assets");
