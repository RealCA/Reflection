/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/SkeletalMesh/SkeletalMeshImporter.h"

#include "Engine/Log.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"
#include "Utilities/PSKXMesh.h"
#include "Utilities/UEModelMesh.h"

#if ENGINE_UE5
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshModel.h"
#endif

UObject* ISkeletalMeshImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<USkeletalMesh>(GetPackage(), USkeletalMesh::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool ISkeletalMeshImporter::Import() {
	USkeletalMesh* SkeletalMesh = Create<USkeletalMesh>();
	if (SkeletalMesh == nullptr) {
		return false;
	}

	const FString SourceDirectory = FPaths::GetPath(GetSourceFile());
	const FString AssetName = GetAssetName();

	/* Prefer .uemodel - it carries native morph target deltas cleanly and
	 * doesn't have PSKX's shared-point ambiguity issues. Only fall back to
	 * ActorX for older exports that don't have a .uemodel sibling. */
	const FString UEModelCandidate = SourceDirectory / (AssetName + TEXT(".uemodel"));
	if (FPaths::FileExists(UEModelCandidate)) {
		FUEModelMeshData ParsedData;
		if (!ImportFromUEModel(UEModelCandidate, SkeletalMesh, ParsedData)) {
			return false;
		}

		const bool bCreated = OnAssetCreation(SkeletalMesh);

#if ENGINE_UE5
		ApplyJSONProperties(SkeletalMesh);
		SkeletalMesh->CalculateInvRefMatrices();
		ApplyMorphTargets(SkeletalMesh, ParsedData);

		const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
		const int32 LODVertices = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].NumVertices : -1;
		const int32 LODSections = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].Sections.Num() : -1;
		const int32 LODActiveBones = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].ActiveBoneIndices.Num() : -1;
		const bool bRenderData = SkeletalMesh->GetResourceForRendering() != nullptr;
		UE_LOG(LogReflection, Log, TEXT("Mesh state after import: RefSkeleton=%d, RefBasesInvMatrix=%d, LOD0 verts=%d, sections=%d, activeBones=%d, renderData=%s, skeleton=%s"),
			SkeletalMesh->GetRefSkeleton().GetNum(),
			SkeletalMesh->GetRefBasesInvMatrix().Num(),
			LODVertices,
			LODSections,
			LODActiveBones,
			bRenderData ? TEXT("yes") : TEXT("no"),
			SkeletalMesh->GetSkeleton() ? *SkeletalMesh->GetSkeleton()->GetName() : TEXT("none"));
#endif

		return bCreated;
	}

	/* The ActorX (.pskx) binary FModel writes next to the JSON holds all the
	 * geometry; the export switches to .pskx once a LOD passes 65536 vertices. */
	/*const TArray<FString> PSKXCandidates = {
		SourceDirectory / (AssetName + TEXT(".pskx")),
		SourceDirectory / (AssetName + TEXT(".psk"))
	};

	for (const FString& Candidate : PSKXCandidates) {
		if (!FPaths::FileExists(Candidate)) {
			continue;
		}

		FPSKXMeshData ParsedData;
		if (!ImportFromPSKX(Candidate, SkeletalMesh, ParsedData)) {
			return false;
		}

		const bool bCreated = OnAssetCreation(SkeletalMesh);

#if ENGINE_UE5
		ApplyJSONProperties(SkeletalMesh);
		SkeletalMesh->CalculateInvRefMatrices();
		ApplyMorphTargets(SkeletalMesh, ParsedData);

		const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
		const int32 LODVertices = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].NumVertices : -1;
		const int32 LODSections = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].Sections.Num() : -1;
		const int32 LODActiveBones = (ImportedModel && ImportedModel->LODModels.Num() > 0) ? ImportedModel->LODModels[0].ActiveBoneIndices.Num() : -1;
		const bool bRenderData = SkeletalMesh->GetResourceForRendering() != nullptr;
		UE_LOG(LogReflection, Log, TEXT("Mesh state after import: RefSkeleton=%d, RefBasesInvMatrix=%d, LOD0 verts=%d, sections=%d, activeBones=%d, renderData=%s, skeleton=%s"),
			SkeletalMesh->GetRefSkeleton().GetNum(),
			SkeletalMesh->GetRefBasesInvMatrix().Num(),
			LODVertices,
			LODSections,
			LODActiveBones,
			bRenderData ? TEXT("yes") : TEXT("no"),
			SkeletalMesh->GetSkeleton() ? *SkeletalMesh->GetSkeleton()->GetName() : TEXT("none"));
#endif

		return bCreated;
	}*/

	UE_LOG(LogReflection, Error, TEXT("No mesh binary (.uemodel/.pskx/.psk) found next to \"%s\""), *GetSourceFile());
	return false;
}

bool ISkeletalMeshImporter::ImportFromUEModel(const FString& FilePath, USkeletalMesh* SkeletalMesh, FUEModelMeshData& OutData) const {
	/* ScaleFactor = 1.0: UE5 is already centimeters, matching the source data -
	 * only use 0.01 if you're deliberately replicating the Blender addon's
	 * cm->m convention. */
	if (!ReadUEModelMeshData(FilePath, OutData, 1.0f)) {
		UE_LOG(LogReflection, Error, TEXT("Could not parse UEModel mesh \"%s\""), *FilePath);
		return false;
	}

	if (!BuildSkeletalMeshFromUEModel(SkeletalMesh, OutData)) {
		UE_LOG(LogReflection, Error, TEXT("Could not build skeletal mesh from \"%s\""), *FilePath);
		return false;
	}

	const int32 MorphCount = OutData.LODs.IsValidIndex(0) ? OutData.LODs[0].MorphTargets.Num() : 0;
	const int32 BoneCount = OutData.Skeleton.Bones.Num();
	const int32 VertCount = OutData.LODs.IsValidIndex(0) ? OutData.LODs[0].Vertices.Num() : 0;
	UE_LOG(LogReflection, Log, TEXT("Imported %d vertices, %d bones and %d morph targets from \"%s\""),
		VertCount, BoneCount, MorphCount, *FilePath);

	return true;
}

bool ISkeletalMeshImporter::ImportFromPSKX(const FString& FilePath, USkeletalMesh* SkeletalMesh, FPSKXMeshData& OutData) const {
	if (!ReadPSKXMeshData(FilePath, OutData)) {
		UE_LOG(LogReflection, Error, TEXT("Could not parse ActorX mesh \"%s\""), *FilePath);
		return false;
	}

	if (!BuildSkeletalMeshFromPSKX(SkeletalMesh, OutData)) {
		UE_LOG(LogReflection, Error, TEXT("Could not build skeletal mesh from \"%s\""), *FilePath);
		return false;
	}

	UE_LOG(LogReflection, Log, TEXT("Imported %d points, %d wedges, %d faces, %d bones and %d morph targets from \"%s\""),
		OutData.Points.Num(), OutData.Wedges.Num(), OutData.Faces.Num(), OutData.Bones.Num(), OutData.MorphTargets.Num(), *FilePath);

	return true;
}

#if ENGINE_UE5
void ISkeletalMeshImporter::ApplyJSONProperties(USkeletalMesh* SkeletalMesh) {
	if (SkeletalMesh == nullptr) {
		return;
	}

	const TSharedPtr<FJsonObject> Properties = GetAssetData();

	/* Properties.Skeleton -> loaded USkeleton. */
	if (Properties.IsValid() && Properties->HasField(TEXT("Skeleton"))) {
		const TSharedPtr<FJsonObject> SkeletonReference = Properties->GetObjectField(TEXT("Skeleton"));

		/* The ObjectPath carries an in-package object index ("...Skeleton.2"); strip
		 * it before loading so LoadObjectByPath can resolve the actual asset. */
		FString SkeletonPath = SkeletonReference->GetStringField(TEXT("ObjectPath"));
		SkeletonPath.Split(TEXT("."), &SkeletonPath, nullptr);
		USkeleton* Skeleton = LoadObjectByPath<USkeleton>(SkeletonPath);

		if (Skeleton != nullptr) {
			SkeletalMesh->SetSkeleton(Skeleton);
			UE_LOG(LogReflection, Log, TEXT("Assigned skeleton \"%s\""), *Skeleton->GetName());
		} else {
			UE_LOG(LogReflection, Warning, TEXT("Skeleton \"%s\" referenced by JSON could not be loaded"), *SkeletonPath);
		}
	}

	/* Top-level SkeletalMaterials -> material slot assignment. */
	const TSharedPtr<FJsonObject> Export = GetAssetExport();
	if (!Export.IsValid() || !Export->HasField(TEXT("SkeletalMaterials"))) {
		return;
	}

	TArray<FSkeletalMaterial>& MeshMaterials = SkeletalMesh->GetMaterials();
	const TArray<TSharedPtr<FJsonValue>>& JsonMaterials = Export->GetArrayField(TEXT("SkeletalMaterials"));

	for (int32 MaterialIndex = 0; MaterialIndex < JsonMaterials.Num(); MaterialIndex++) {
		const TSharedPtr<FJsonObject> MaterialEntry = JsonMaterials[MaterialIndex]->AsObject();
		if (!MaterialEntry.IsValid() || !MaterialEntry->HasField(TEXT("Material"))) {
			continue;
		}

		const FString JsonSlotName = MaterialEntry->GetStringField(TEXT("MaterialSlotName"));
		const TSharedPtr<FJsonObject> MaterialReference = MaterialEntry->GetObjectField(TEXT("Material"));

		/* The binary's material names are the material asset names (MI_Eye_Normal_L),
		 * while the JSON renames the slots (Eye_L). Extract the asset name so the
		 * two can be paired up. */
		FString MaterialAssetName;
		{
			FString ObjectNameField = MaterialReference->GetStringField(TEXT("ObjectName"));
			if (ObjectNameField.Split(TEXT("'"), nullptr, &MaterialAssetName)) {
				MaterialAssetName.RemoveFromEnd(TEXT("'"));
			}
		}

		TObjectPtr<UMaterialInterface> Material = nullptr;
		LoadExport<UMaterialInterface>(&MaterialReference, Material);
		if (Material == nullptr) {
			UE_LOG(LogReflection, Warning, TEXT("Material \"%s\" for slot \"%s\" could not be loaded"),
				*MaterialAssetName, *JsonSlotName);
			continue;
		}

		/* The slots usually line up by index (both come from the same section list);
		 * verify the name, then fall back to a by-name search. */
		FSkeletalMaterial* TargetSlot = nullptr;
		if (MeshMaterials.IsValidIndex(MaterialIndex)) {
			FSkeletalMaterial& Candidate = MeshMaterials[MaterialIndex];
			const FName CandidateName = !Candidate.ImportedMaterialSlotName.IsNone()
				? Candidate.ImportedMaterialSlotName : Candidate.MaterialSlotName;
			if (CandidateName == FName(*MaterialAssetName) || CandidateName == FName(*JsonSlotName)) {
				TargetSlot = &Candidate;
			}
		}

		if (TargetSlot == nullptr) {
			TargetSlot = MeshMaterials.FindByPredicate([&MaterialAssetName, &JsonSlotName](const FSkeletalMaterial& MeshMaterial) {
				return MeshMaterial.ImportedMaterialSlotName == FName(*MaterialAssetName)
					|| MeshMaterial.MaterialSlotName == FName(*MaterialAssetName)
					|| MeshMaterial.ImportedMaterialSlotName == FName(*JsonSlotName)
					|| MeshMaterial.MaterialSlotName == FName(*JsonSlotName);
			});
		}

		if (TargetSlot != nullptr) {
			TargetSlot->MaterialInterface = Material;
			TargetSlot->MaterialSlotName = FName(*JsonSlotName);
			if (TargetSlot->ImportedMaterialSlotName.IsNone()) {
				TargetSlot->ImportedMaterialSlotName = FName(*MaterialAssetName);
			}
			UE_LOG(LogReflection, Log, TEXT("Assigned material \"%s\" to slot \"%s\""),
				*Material->GetName(), *JsonSlotName);
		} else {
			UE_LOG(LogReflection, Warning, TEXT("No material slot \"%s\" on the mesh for \"%s\""),
				*JsonSlotName, *MaterialAssetName);
		}
	}
}

void ISkeletalMeshImporter::ApplyMorphTargets(USkeletalMesh* SkeletalMesh, const FPSKXMeshData& Data) {
	if (SkeletalMesh == nullptr || Data.MorphTargets.IsEmpty()) {
		return;
	}

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel();
	if (ImportedResource == nullptr || ImportedResource->LODModels.IsEmpty()) {
		UE_LOG(LogReflection, Warning, TEXT("Skipping morph targets for \"%s\": no built LOD model"), *SkeletalMesh->GetName());
		return;
	}

	FSkeletalMeshLODModel& LODModel = ImportedResource->LODModels[0];
	const TArray<int32>& MeshToImportVertexMap = LODModel.MeshToImportVertexMap;

	/* Reverse map: PSKX shared-point index -> every render vertex the Build()
	 * step expanded from it (a point can back multiple wedges/render verts). */
	TMultiMap<uint32, int32> RenderVerticesByPoint;
	RenderVerticesByPoint.Reserve(MeshToImportVertexMap.Num());
	for (int32 RenderIndex = 0; RenderIndex < MeshToImportVertexMap.Num(); RenderIndex++) {
		RenderVerticesByPoint.Add(static_cast<uint32>(MeshToImportVertexMap[RenderIndex]), RenderIndex);
	}

	int32 RegisteredCount = 0;
	for (const FPSKXMorphTarget& SourceMorph : Data.MorphTargets) {
		if (SourceMorph.Deltas.IsEmpty()) {
			continue;
		}

		UMorphTarget* MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, FName(*SourceMorph.Name), RF_Public);

		TArray<FMorphTargetDelta> Deltas;
		Deltas.Reserve(SourceMorph.Deltas.Num());

		TArray<int32> RenderIndices;
		for (const FPSKXMorphVertex& SourceDelta : SourceMorph.Deltas) {
			RenderIndices.Reset();
			RenderVerticesByPoint.MultiFind(SourceDelta.PointIndex, RenderIndices);

			for (const int32 RenderIndex : RenderIndices) {
				FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
				Delta.PositionDelta = SourceDelta.PositionDelta;
				Delta.TangentZDelta = SourceDelta.TangentZDelta;
				Delta.SourceIdx = RenderIndex;
			}
		}

		if (Deltas.IsEmpty()) {
			UE_LOG(LogReflection, Warning, TEXT("Morph target \"%s\" had no matching render vertices; skipped"), *SourceMorph.Name);
			continue;
		}

		MorphTarget->GetMorphLODModels().SetNum(FMath::Max(MorphTarget->GetMorphLODModels().Num(), 1));
		MorphTarget->PopulateDeltas(Deltas, 0, LODModel.Sections);

		SkeletalMesh->RegisterMorphTarget(MorphTarget, false);
		RegisteredCount++;
	}

	if (RegisteredCount > 0) {
		SkeletalMesh->InitMorphTargetsAndRebuildRenderData();
		SkeletalMesh->MarkPackageDirty();
		SkeletalMesh->GetOnMeshChanged().Broadcast();
	}

	UE_LOG(LogReflection, Log, TEXT("Registered %d/%d morph targets on \"%s\""),
		RegisteredCount, Data.MorphTargets.Num(), *SkeletalMesh->GetName());
}

void ISkeletalMeshImporter::ApplyMorphTargets(USkeletalMesh* SkeletalMesh, const FUEModelMeshData& Data, const int32 LODIndex) {
	if (SkeletalMesh == nullptr || !Data.LODs.IsValidIndex(LODIndex) || Data.LODs[LODIndex].MorphTargets.IsEmpty()) {
		return;
	}

	const FUEModelLOD& LOD = Data.LODs[LODIndex];

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel();
	if (ImportedResource == nullptr || ImportedResource->LODModels.IsEmpty()) {
		UE_LOG(LogReflection, Warning, TEXT("Skipping morph targets for \"%s\": no built LOD model"), *SkeletalMesh->GetName());
		return;
	}

	FSkeletalMeshLODModel& LODModel = ImportedResource->LODModels[0];
	const TArray<int32>& MeshToImportVertexMap = LODModel.MeshToImportVertexMap;

	/* Same reverse-map approach as the PSKX path - .uemodel morph deltas are
	 * indexed against the same vertex-index space as ImportData.Points. */
	TMultiMap<uint32, int32> RenderVerticesByPoint;
	RenderVerticesByPoint.Reserve(MeshToImportVertexMap.Num());
	for (int32 RenderIndex = 0; RenderIndex < MeshToImportVertexMap.Num(); RenderIndex++) {
		RenderVerticesByPoint.Add(static_cast<uint32>(MeshToImportVertexMap[RenderIndex]), RenderIndex);
	}

	int32 RegisteredCount = 0;
	for (const FUEModelMorphTarget& SourceMorph : LOD.MorphTargets) {
		if (SourceMorph.Deltas.IsEmpty()) {
			continue;
		}

		UMorphTarget* MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, FName(*SourceMorph.Name), RF_Public);

		TArray<FMorphTargetDelta> Deltas;
		Deltas.Reserve(SourceMorph.Deltas.Num());

		TArray<int32> RenderIndices;
		for (const FUEModelMorphVertex& SourceDelta : SourceMorph.Deltas) {
			RenderIndices.Reset();
			RenderVerticesByPoint.MultiFind(SourceDelta.VertexIndex, RenderIndices);

			for (const int32 RenderIndex : RenderIndices) {
				FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
				Delta.PositionDelta = SourceDelta.PositionDelta;
				Delta.TangentZDelta = SourceDelta.TangentZDelta;
				Delta.SourceIdx = RenderIndex;
			}
		}

		if (Deltas.IsEmpty()) {
			UE_LOG(LogReflection, Warning, TEXT("Morph target \"%s\" had no matching render vertices; skipped"), *SourceMorph.Name);
			continue;
		}

		MorphTarget->GetMorphLODModels().SetNum(FMath::Max(MorphTarget->GetMorphLODModels().Num(), 1));
		MorphTarget->PopulateDeltas(Deltas, 0, LODModel.Sections);

		SkeletalMesh->RegisterMorphTarget(MorphTarget, false);
		RegisteredCount++;
	}

	if (RegisteredCount > 0) {
		SkeletalMesh->InitMorphTargetsAndRebuildRenderData();
		SkeletalMesh->MarkPackageDirty();
		SkeletalMesh->GetOnMeshChanged().Broadcast();
	}

	UE_LOG(LogReflection, Log, TEXT("Registered %d/%d morph targets on \"%s\""),
		RegisteredCount, LOD.MorphTargets.Num(), *SkeletalMesh->GetName());
}
#endif
