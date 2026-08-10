/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/PSKXMesh.h"

#include "Engine/Compatibility.h"
#include "Misc/FileHelper.h"

#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshRenderData.h"

#if ENGINE_UE5
#include "ImportUtils/SkeletalMeshImportUtils.h"
#include "SkeletalMeshTypes.h"
#endif

/* The ActorX mesh layout as written by FModel's ActorXMesh.cs. All integers are
 * little-endian, every chunk starts with a fixed 20 byte chunk id followed by an
 * int32 version/type flag, an int32 data size and an int32 data count. */
namespace {

struct FPSKXReader {
	const uint8* Buffer = nullptr;
	int32 Size = 0;
	int32 Pos = 0;

	bool ReadBytes(void* Out, const int32 Count) {
		if (Pos + Count > Size) {
			return false;
		}

		FMemory::Memcpy(Out, Buffer + Pos, Count);
		Pos += Count;

		return true;
	}

	int32 ReadInt32() {
		int32 Value = 0;
		ReadBytes(&Value, sizeof(int32));
		return Value;
	}

	uint32 ReadUInt32() {
		uint32 Value = 0;
		ReadBytes(&Value, sizeof(uint32));
		return Value;
	}

	uint16 ReadUInt16() {
		uint16 Value = 0;
		ReadBytes(&Value, sizeof(uint16));
		return Value;
	}

	uint8 ReadUInt8() {
		uint8 Value = 0;
		ReadBytes(&Value, sizeof(uint8));
		return Value;
	}

	float ReadFloat() {
		float Value = 0.0f;
		ReadBytes(&Value, sizeof(float));
		return Value;
	}

	FString ReadFixedString(const int32 Length) {
		if (Pos + Length > Size) {
			Pos += Length;
			return FString();
		}

		TArray<ANSICHAR> Chars;
		Chars.SetNumUninitialized(Length + 1);

		FMemory::Memcpy(Chars.GetData(), Buffer + Pos, Length);
		Chars[Length] = 0;
		Pos += Length;

		return FString(ANSI_TO_TCHAR(Chars.GetData()));
	}

	FVector3f ReadVector3f() {
		FVector3f Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		Value.Z = ReadFloat();
		return Value;
	}

	FQuat4f ReadQuat4f() {
		FQuat4f Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		Value.Z = ReadFloat();
		Value.W = ReadFloat();
		return Value;
	}
};

} // namespace

bool ReadPSKXMeshData(const FString& FilePath, FPSKXMeshData& OutData) {
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath)) {
		return false;
	}

	FPSKXReader Reader;
	Reader.Buffer = Bytes.GetData();
	Reader.Size = Bytes.Num();

	/* MRPHINFO gives each morph's name and vertex count; MRPHDATA is one flat
	 * array of deltas, laid out as contiguous per-morph runs in that same order.
	 * Both are accumulated here and sliced together once parsing finishes, since
	 * chunk order in the file isn't guaranteed relative to each other. */
	TArray<TPair<FString, int32>> MorphInfoEntries;
	TArray<FPSKXMorphVertex> FlatMorphDeltas;

	while (Reader.Pos + 32 <= Reader.Size) {
		const FString ChunkId = Reader.ReadFixedString(20);
		Reader.ReadInt32(); /* version / type flag */
		const int32 DataSize = Reader.ReadInt32();
		const int32 DataCount = Reader.ReadInt32();

		const int32 ChunkBytes = DataSize * DataCount;
		if (Reader.Pos + ChunkBytes > Reader.Size) {
			return false;
		}

		if (ChunkId == TEXT("PNTS0000")) {
			OutData.Points.SetNum(DataCount);
			for (int32 PointIndex = 0; PointIndex < DataCount; PointIndex++) {
				FVector3f& Point = OutData.Points[PointIndex];
				Point = Reader.ReadVector3f();
				Point.Y = -Point.Y; /* MIRROR_MESH */
			}
		} else if (ChunkId == TEXT("VTXNORMS")) {
			OutData.Normals.SetNum(DataCount);
			for (int32 NormalIndex = 0; NormalIndex < DataCount; NormalIndex++) {
				FVector3f& Normal = OutData.Normals[NormalIndex];
				Normal = Reader.ReadVector3f();
				Normal.Y = -Normal.Y; /* MIRROR_MESH */
			}
		} else if (ChunkId == TEXT("VTXW0000")) {
			OutData.Wedges.SetNum(DataCount);
			for (int32 WedgeIndex = 0; WedgeIndex < DataCount; WedgeIndex++) {
				FPSKXWedge& Wedge = OutData.Wedges[WedgeIndex];
				Wedge.PointIndex = Reader.ReadUInt32();
				Wedge.UV.X = Reader.ReadFloat();
				Wedge.UV.Y = Reader.ReadFloat();
				Wedge.MatIndex = Reader.ReadUInt8();
				Reader.ReadUInt8(); /* reserved */
				Reader.ReadUInt16(); /* padding */
			}
		} else if (ChunkId == TEXT("FACE0000") || ChunkId == TEXT("FACE3200")) {
			const bool bIs32Bit = ChunkId == TEXT("FACE3200");
			OutData.Faces.SetNum(DataCount);
			for (int32 FaceIndex = 0; FaceIndex < DataCount; FaceIndex++) {
				FPSKXFace& Face = OutData.Faces[FaceIndex];
				for (int32 Corner = 0; Corner < 3; Corner++) {
					/* Written [1], [0], [2] so the winding survives the Y mirror */
					Face.WedgeIndex[Corner] = bIs32Bit ? Reader.ReadUInt32() : Reader.ReadUInt16();
				}
				Face.MatIndex = Reader.ReadUInt8();
				Reader.ReadUInt8(); /* aux material index */
				Face.SmoothingGroups = Reader.ReadUInt32();
			}
		} else if (ChunkId == TEXT("MATT0000")) {
			OutData.MaterialNames.SetNum(DataCount);
			for (int32 MaterialIndex = 0; MaterialIndex < DataCount; MaterialIndex++) {
				OutData.MaterialNames[MaterialIndex] = Reader.ReadFixedString(64);
				Reader.ReadInt32(); /* texture index */
				Reader.ReadUInt32(); /* poly flags */
				Reader.ReadInt32(); /* aux material */
				Reader.ReadUInt32(); /* aux flags */
				Reader.ReadInt32(); /* lod bias */
				Reader.ReadInt32(); /* lod style */
			}
		} else if (ChunkId == TEXT("REFSKELT")) {
			OutData.Bones.SetNum(DataCount);
			for (int32 BoneIndex = 0; BoneIndex < DataCount; BoneIndex++) {
				FPSKXBone& Bone = OutData.Bones[BoneIndex];
				Bone.Name = Reader.ReadFixedString(64);
				Reader.ReadUInt32(); /* flags */
				Reader.ReadInt32(); /* num children */
				Bone.ParentIndex = Reader.ReadInt32();

				/* VJointPosPsk: orientation, position, length, size */
				FQuat4f Orientation = Reader.ReadQuat4f();
				FVector3f Position = Reader.ReadVector3f();
				Reader.ReadFloat(); /* length */
				Reader.ReadVector3f(); /* size */

				/* FModel mirrors the mesh on export, so flip the Y axis back and
				 * undo the root bone quaternion handedness flip (ActorXMesh.cs). */
				Orientation.Y = -Orientation.Y;
				if (BoneIndex == 0) {
					Orientation.W = -Orientation.W;
				}
				Position.Y = -Position.Y;

				Bone.Transform = FTransform3f(Orientation, Position, FVector3f::OneVector);
			}
		} else if (ChunkId == TEXT("RAWWEIGHTS")) {
			OutData.Influences.SetNum(DataCount);
			for (int32 InfluenceIndex = 0; InfluenceIndex < DataCount; InfluenceIndex++) {
				FPSKXInfluence& Influence = OutData.Influences[InfluenceIndex];
				Influence.Weight = Reader.ReadFloat();
				Influence.PointIndex = Reader.ReadUInt32();
				Influence.BoneIndex = Reader.ReadInt32();
			}
		} else if (ChunkId == TEXT("VERTEXCOLOR")) {
			/* Written per source vertex (wedge) after VTXW0000; apply only when
			 * the counts line up so wedges are never invented out of thin air. */
			if (DataCount == OutData.Wedges.Num()) {
				OutData.bHasVertexColors = DataCount > 0;
				for (int32 ColorIndex = 0; ColorIndex < DataCount; ColorIndex++) {
					FColor& Color = OutData.Wedges[ColorIndex].Color;
					Reader.ReadBytes(&Color, sizeof(FColor));
				}
			} else {
				Reader.Pos += ChunkBytes;
			}
		} else if (ChunkId == TEXT("MRPHINFO")) {
			MorphInfoEntries.SetNum(DataCount);
			for (int32 MorphIndex = 0; MorphIndex < DataCount; MorphIndex++) {
				const FString MorphName = Reader.ReadFixedString(64);
				const int32 VertexCount = Reader.ReadInt32();
				MorphInfoEntries[MorphIndex] = TPair<FString, int32>(MorphName, VertexCount);
			}
		} else if (ChunkId == TEXT("MRPHDATA")) {
			FlatMorphDeltas.SetNum(DataCount);
			for (int32 DeltaIndex = 0; DeltaIndex < DataCount; DeltaIndex++) {
				FPSKXMorphVertex& Delta = FlatMorphDeltas[DeltaIndex];
				Delta.PositionDelta = Reader.ReadVector3f();
				Delta.PositionDelta.Y = -Delta.PositionDelta.Y; /* MIRROR_MESH, same as PNTS0000 */
				Delta.TangentZDelta = Reader.ReadVector3f();
				Delta.TangentZDelta.Y = -Delta.TangentZDelta.Y;
				Delta.PointIndex = Reader.ReadUInt32();
			}
		} else if (ChunkId.StartsWith(TEXT("EXTRAUVS"))) {
			const int32 Channel = FCString::Atoi(*ChunkId.RightChop(8));
			if (Channel >= 0 && Channel < 4 && DataCount > 0) {
				TArray<TArray<FVector2f>>& ChannelUVs = OutData.ExtraUVs;
				ChannelUVs.SetNum(FMath::Max(ChannelUVs.Num(), Channel + 1));
				ChannelUVs[Channel].SetNum(DataCount);
				for (int32 UVIndex = 0; UVIndex < DataCount; UVIndex++) {
					ChannelUVs[Channel][UVIndex].X = Reader.ReadFloat();
					ChannelUVs[Channel][UVIndex].Y = Reader.ReadFloat();
				}
			} else {
				/* Unknown/out-of-range channel; skip past its data. */
				Reader.Pos += ChunkBytes;
			}
		} else {
			/* Unknown chunk; skip past its data. */
			Reader.Pos += ChunkBytes;
		}
	}

	if (OutData.Points.IsEmpty() || OutData.Wedges.IsEmpty() || OutData.Faces.IsEmpty()) {
		return false;
	}

	if (MorphInfoEntries.Num() > 0 && FlatMorphDeltas.Num() > 0) {
		OutData.MorphTargets.SetNum(MorphInfoEntries.Num());
		int32 DeltaCursor = 0;
		for (int32 MorphIndex = 0; MorphIndex < MorphInfoEntries.Num(); MorphIndex++) {
			FPSKXMorphTarget& MorphTarget = OutData.MorphTargets[MorphIndex];
			MorphTarget.Name = MorphInfoEntries[MorphIndex].Key;
			const int32 VertexCount = MorphInfoEntries[MorphIndex].Value;

			if (DeltaCursor + VertexCount > FlatMorphDeltas.Num()) {
				/* Malformed chunk; stop rather than reading out of bounds. */
				break;
			}

			MorphTarget.Deltas.Append(&FlatMorphDeltas[DeltaCursor], VertexCount);
			DeltaCursor += VertexCount;
		}
	}

	return true;
}

bool BuildSkeletalMeshFromPSKX(USkeletalMesh* Mesh, const FPSKXMeshData& Data) {
	if (Mesh == nullptr || Data.Bones.IsEmpty() || Data.Influences.IsEmpty()) {
		return false;
	}

	FSkeletalMeshImportData ImportData;

	ImportData.Points = Data.Points;

	ImportData.Wedges.SetNum(Data.Wedges.Num());
	for (int32 WedgeIndex = 0; WedgeIndex < Data.Wedges.Num(); WedgeIndex++) {
		const FPSKXWedge& SourceWedge = Data.Wedges[WedgeIndex];
		SkeletalMeshImportData::FVertex& Wedge = ImportData.Wedges[WedgeIndex];
		Wedge.VertexIndex = SourceWedge.PointIndex;
		Wedge.UVs[0] = SourceWedge.UV;
		Wedge.MatIndex = SourceWedge.MatIndex;
		Wedge.Color = SourceWedge.Color;
	}
	ImportData.bHasVertexColors = Data.bHasVertexColors;

	/* Extra UV channels are written per wedge. */
	ImportData.NumTexCoords = 1;
	for (int32 Channel = 0; Channel < Data.ExtraUVs.Num() && Channel + 1 < MAX_TEXCOORDS; Channel++) {
		const TArray<FVector2f>& ChannelUVs = Data.ExtraUVs[Channel];
		if (ChannelUVs.Num() != Data.Wedges.Num()) {
			continue;
		}
		for (int32 WedgeIndex = 0; WedgeIndex < Data.Wedges.Num(); WedgeIndex++) {
			ImportData.Wedges[WedgeIndex].UVs[Channel + 1] = ChannelUVs[WedgeIndex];
		}
		ImportData.NumTexCoords = Channel + 2;
	}

	/* Faces carry the section material and corner normals (via the wedge to point
	 * mapping, since ActorX stores normals per shared point). */
	const bool bHasNormals = Data.Normals.Num() == Data.Points.Num() && !Data.Normals.IsEmpty();
	ImportData.Faces.SetNum(Data.Faces.Num());
	ImportData.MaxMaterialIndex = 0;
	for (int32 FaceIndex = 0; FaceIndex < Data.Faces.Num(); FaceIndex++) {
		const FPSKXFace& SourceFace = Data.Faces[FaceIndex];
		SkeletalMeshImportData::FTriangle& Face = ImportData.Faces[FaceIndex];

		Face.WedgeIndex[0] = SourceFace.WedgeIndex[0];
		Face.WedgeIndex[1] = SourceFace.WedgeIndex[1];
		Face.WedgeIndex[2] = SourceFace.WedgeIndex[2];
		Face.MatIndex = SourceFace.MatIndex;
		Face.AuxMatIndex = 0;
		Face.SmoothingGroups = SourceFace.SmoothingGroups;

		ImportData.MaxMaterialIndex = FMath::Max<uint32>(ImportData.MaxMaterialIndex, Face.MatIndex);

		if (bHasNormals) {
			for (int32 Corner = 0; Corner < 3; Corner++) {
				if (Data.Wedges.IsValidIndex(SourceFace.WedgeIndex[Corner])) {
					const uint32 PointIndex = Data.Wedges[SourceFace.WedgeIndex[Corner]].PointIndex;
					if (Data.Normals.IsValidIndex(PointIndex)) {
						Face.TangentZ[Corner] = Data.Normals[PointIndex];
					}
				}
			}
		}
	}
	ImportData.bHasNormals = bHasNormals;
	ImportData.bHasTangents = false;

	/* Material slot names come from the MATT0000 chunk (section slot names). */
	ImportData.Materials.SetNum(Data.MaterialNames.Num());
	for (int32 MaterialIndex = 0; MaterialIndex < Data.MaterialNames.Num(); MaterialIndex++) {
		ImportData.Materials[MaterialIndex].MaterialImportName = Data.MaterialNames[MaterialIndex];
	}

	/* Reference skeleton from REFSKELT (local-space, already un-mirrored). */
	ImportData.RefBonesBinary.SetNum(Data.Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Data.Bones.Num(); BoneIndex++) {
		const FPSKXBone& SourceBone = Data.Bones[BoneIndex];
		SkeletalMeshImportData::FBone& Bone = ImportData.RefBonesBinary[BoneIndex];
		Bone.Name = SourceBone.Name;
		Bone.Flags = 0;
		Bone.NumChildren = 0;
		Bone.ParentIndex = SourceBone.ParentIndex;
		Bone.BonePos.Transform = SourceBone.Transform;
		Bone.BonePos.Length = 0.0f;
		Bone.BonePos.XSize = 0.0f;
		Bone.BonePos.YSize = 0.0f;
		Bone.BonePos.ZSize = 0.0f;
	}

	/* Skinning influences are per shared point (RAWWEIGHTS). */
	ImportData.Influences.SetNum(Data.Influences.Num());
	for (int32 InfluenceIndex = 0; InfluenceIndex < Data.Influences.Num(); InfluenceIndex++) {
		const FPSKXInfluence& SourceInfluence = Data.Influences[InfluenceIndex];
		ImportData.Influences[InfluenceIndex].Weight = SourceInfluence.Weight;
		ImportData.Influences[InfluenceIndex].VertexIndex = SourceInfluence.PointIndex;
		ImportData.Influences[InfluenceIndex].BoneIndex = SourceInfluence.BoneIndex;
	}

	/* Morph targets from MRPHINFO / MRPHDATA. */
	for (int32 MorphIndex = 0; MorphIndex < Data.MorphTargets.Num(); MorphIndex++) {
		const FPSKXMorphTarget& SourceMorph = Data.MorphTargets[MorphIndex];
		if (SourceMorph.Deltas.IsEmpty()) {
			continue;
		}

		ImportData.MorphTargetNames.Add(SourceMorph.Name);

		TSet<uint32>& ModifiedPoints = ImportData.MorphTargetModifiedPoints.AddDefaulted_GetRef();
		FSkeletalMeshImportData& MorphImportData = ImportData.MorphTargets.AddDefaulted_GetRef();

		/* Sort deltas by PointIndex so TSet iteration order (hash-based) matches the Points array. */
		TArray<FPSKXMorphVertex> SortedDeltas = SourceMorph.Deltas;
		SortedDeltas.Sort([](const FPSKXMorphVertex& A, const FPSKXMorphVertex& B) {
			return A.PointIndex < B.PointIndex;
		});

		MorphImportData.Points.SetNum(SortedDeltas.Num());
		for (int32 DeltaIndex = 0; DeltaIndex < SortedDeltas.Num(); DeltaIndex++) {
			const FPSKXMorphVertex& SourceDelta = SortedDeltas[DeltaIndex];
			ModifiedPoints.Add(SourceDelta.PointIndex);
			const FVector3f BasePos = Data.Points.IsValidIndex(SourceDelta.PointIndex) ? Data.Points[SourceDelta.PointIndex] : FVector3f::ZeroVector;
			MorphImportData.Points[DeltaIndex] = SourceDelta.PositionDelta + BasePos;
		}
	}

#if ENGINE_UE5
	/* Engine-side setup, mirroring FbxSkeletalMeshImport.cpp so the mesh comes out
	 * exactly like an FBX import. The generic import flow calls
	 * USkeletalMesh::PostEditChange() right after this returns, which runs the
	 * editor's canonical Build() over the source model we just wrote. To prevent
	 * intermediate property changes (materials, ref skeleton, LOD info, etc.) from
	 * triggering premature builds, we scope a FScopedSkeletalMeshPostEditChange with
	 * bCallPostEditChange=false - this increments PostEditChangeStackCounter to
	 * block recursive calls, but the scope's destructor does NOT call PostEditChange.
	 * The single PostEditChange in HandleAssetCreation does the build. */
	FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Mesh, false, false);

	/* Full reset so a re-import replaces everything. */
	for (int32 LODIndex = 0, LODCount = Mesh->GetLODNum(); LODIndex < LODCount; ++LODIndex) {
		Mesh->ClearMeshDescriptionAndBulkData(LODIndex);
	}

	{
		FSkeletalMeshModel* ImportedResource = Mesh->GetImportedModel();
		ImportedResource->EmptyOriginalReductionSourceMeshData();
		ImportedResource->LODModels.Empty();

		Mesh->SetNumSourceModels(0);
		Mesh->GetMaterials().Empty();
		Mesh->GetRefSkeleton().Empty();
		Mesh->SetSkeleton(nullptr);
		Mesh->SetPhysicsAsset(nullptr);
		Mesh->UnregisterAllMorphTarget();
		Mesh->ReleaseResources();
	}

	Mesh->PreEditChange(NULL);
	Mesh->InvalidateDeriveDataCacheGUID();

	FSkeletalMeshModel* ImportedResource = Mesh->GetImportedModel();
	ImportedResource->LODModels.Empty();
	ImportedResource->LODModels.Add(new FSkeletalMeshLODModel());
	const int32 ImportLODModelIndex = 0;
	FSkeletalMeshLODModel& LODModel = ImportedResource->LODModels[ImportLODModelIndex];

	SkeletalMeshImportUtils::ProcessImportMeshMaterials(Mesh->GetMaterials(), ImportData);

	int32 SkeletalDepth = 0;
	if (!SkeletalMeshImportUtils::ProcessImportMeshSkeleton(nullptr, Mesh->GetRefSkeleton(), SkeletalDepth, ImportData)) {
		return false;
	}

	SkeletalMeshImportUtils::ProcessImportMeshInfluences(ImportData, Mesh->GetPathName());

	Mesh->SetNumSourceModels(0);

	FSkeletalMeshLODInfo& NewLODInfo = Mesh->AddLODInfo();
	NewLODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	NewLODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	NewLODInfo.ReductionSettings.MaxDeviationPercentage = 0.0f;
	NewLODInfo.LODHysteresis = 0.02f;

	/* ActorX supplies normals but no tangents; keep the normals and have the build
	 * derive tangents (MikkTSpace), the same as an FBX import of those flags. */
	NewLODInfo.BuildSettings.bRecomputeNormals = false;
	NewLODInfo.BuildSettings.bRecomputeTangents = true;
	NewLODInfo.BuildSettings.bUseMikkTSpace = true;

	/* SaveLODImportedData turns the import data into the source mesh description,
	 * which is what the editor's Build() (and later re-imports) consume. */
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Mesh->SaveLODImportedData(ImportLODModelIndex, ImportData);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	/* Imported bounds from the un-mirrored points. */
	const FBox3f BoundingBox(Data.Points.GetData(), Data.Points.Num());
	Mesh->SetImportedBounds(FBoxSphereBounds((FBox)BoundingBox));

	Mesh->SetHasVertexColors(Data.bHasVertexColors);
	Mesh->SetVertexColorGuid(Data.bHasVertexColors ? FGuid::NewGuid() : FGuid());

	LODModel.NumTexCoords = FMath::Max<uint32>(1, ImportData.NumTexCoords);

	/* The build (render data, inv ref matrices, resource init) all happens in the
	 * PostEditChange() call the generic import flow makes right after this returns. */

	return true;
#else
	return false;
#endif
}
