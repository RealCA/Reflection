/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/UEModelMesh.h"

#include "Engine/Compatibility.h"
#include "Engine/Log.h"
#include "Misc/FileHelper.h"
#include "Misc/Compression.h"

#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshRenderData.h"

#if ENGINE_UE5
#include "ImportUtils/SkeletalMeshImportUtils.h"
#include "SkeletalMeshTypes.h"
#endif

/* Mirrors io_scene_ueformat's FArchiveReader/classes.py. All integers are
 * little-endian. Sections at both the top level and inside a LOD/Skeleton
 * chunk share the same triplet header: FString name, int32 array_size,
 * int32 byte_size - byte_size is always trusted to reposition the cursor,
 * so an unrecognized or partially-handled section never desyncs the reader. */
namespace {

constexpr int32 UEFORMAT_VERSION_LOD_RESTRUCTURE = 4;
constexpr int32 UEFORMAT_VERSION_SERIALIZE_MATERIAL_PATH = 6;
constexpr int32 UEFORMAT_VERSION_PRESERVE_ORIGINAL_TRANSFORMS = 8;

struct FUEModelReader {
	const uint8* Buffer = nullptr;
	int32 Size = 0;
	int32 Pos = 0;
	int32 FileVersion = 0;

	bool Eof() const { return Pos >= Size; }

	bool ReadBytes(void* Out, const int32 Count) {
		if (Pos + Count > Size) {
			return false;
		}
		FMemory::Memcpy(Out, Buffer + Pos, Count);
		Pos += Count;
		return true;
	}

	bool ReadBool() {
		uint8 Value = 0;
		ReadBytes(&Value, 1);
		return Value != 0;
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

	int16 ReadInt16() {
		int16 Value = 0;
		ReadBytes(&Value, sizeof(int16));
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

	/* Fixed-length string used only by the top-level 8 byte "UEFORMAT" magic. */
	FString ReadFixedString(const int32 Length) {
		TArray<ANSICHAR> Chars;
		Chars.SetNumUninitialized(Length + 1);
		if (!ReadBytes(Chars.GetData(), Length)) {
			return FString();
		}
		Chars[Length] = 0;
		return FString(ANSI_TO_TCHAR(Chars.GetData()));
	}

	/* UE-style length-prefixed string: int32 byte count followed by the bytes
	 * (not null-terminated in the buffer, matching read_fstring in reader.py). */
	FString ReadFString() {
		const int32 Length = ReadInt32();
		if (Length <= 0) {
			return FString();
		}
		TArray<ANSICHAR> Chars;
		Chars.SetNumUninitialized(Length + 1);
		if (!ReadBytes(Chars.GetData(), Length)) {
			return FString();
		}
		Chars[Length] = 0;
		return FString(ANSI_TO_TCHAR(Chars.GetData()));
	}

	FVector3f ReadVec3() {
		FVector3f Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		Value.Z = ReadFloat();
		return Value;
	}

	FVector2f ReadVec2() {
		FVector2f Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		return Value;
	}

	/* WXYZ order on disk; returns XYZW to match FQuat4f's constructor order. */
	FQuat4f ReadQuatFromWXYZ() {
		const float W = ReadFloat();
		const float X = ReadFloat();
		const float Y = ReadFloat();
		const float Z = ReadFloat();
		return FQuat4f(X, Y, Z, W);
	}

	/* XYZW order on disk (bones/sockets/morph rotations use this order, only
	 * split-normal "NORMALS" chunks use WXYZ above). */
	FQuat4f ReadQuatFromXYZW() {
		const float X = ReadFloat();
		const float Y = ReadFloat();
		const float Z = ReadFloat();
		const float W = ReadFloat();
		return FQuat4f(X, Y, Z, W);
	}

	void Skip(const int32 Count) {
		Pos = FMath::Min(Pos + Count, Size);
	}
};

struct FSectionHeader {
	FString Name;
	int32 ArraySize = 0;
	int32 ByteSize = 0;
	int32 BodyStart = 0;
};

FSectionHeader ReadSectionHeader(FUEModelReader& Reader) {
	FSectionHeader Header;
	Header.Name = Reader.ReadFString();
	Header.ArraySize = Reader.ReadInt32();
	Header.ByteSize = Reader.ReadInt32();
	Header.BodyStart = Reader.Pos;
	return Header;
}

/* Always call after handling (or ignoring) a section, known or not - this is
 * what lets an unrecognized/future section be skipped safely without the
 * caller needing to know its internal layout. */
void SeekPastSection(FUEModelReader& Reader, const FSectionHeader& Header) {
	Reader.Pos = Header.BodyStart + Header.ByteSize;
}

void ParseLOD(FUEModelReader& Reader, FUEModelLOD& LOD, const float ScaleFactor) {
	LOD.Name = Reader.ReadFString();
	const int32 LODByteSize = Reader.ReadInt32();
	const int32 LODEnd = Reader.Pos + LODByteSize;

	while (Reader.Pos < LODEnd) {
		FSectionHeader Header = ReadSectionHeader(Reader);

		if (Header.Name == TEXT("VERTICES")) {
			LOD.Vertices.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				LOD.Vertices[Index] = Reader.ReadVec3() * ScaleFactor;
			}
			/* Intentionally not applying the (1,-1,1) mirror the Blender addon
			 * applies here for file_version >= PreserveOriginalTransforms - that
			 * flip converts *into* Blender's coordinate space. Verify against a
			 * known-good exported asset before trusting this on your data. */
		} else if (Header.Name == TEXT("INDICES")) {
			LOD.Indices.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				LOD.Indices[Index] = Reader.ReadUInt32();
			}
		} else if (Header.Name == TEXT("NORMALS")) {
			/* Stored as WXYZ per vertex; W (binormal sign) is dropped here since
			 * we recompute tangents at build time the same way PSKX import does. */
			LOD.Normals.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				const FQuat4f Packed = Reader.ReadQuatFromWXYZ();
				LOD.Normals[Index] = FVector3f(Packed.X, Packed.Y, Packed.Z);
			}
		} else if (Header.Name == TEXT("TANGENTS")) {
			/* Not consumed; UE5's build step (bRecomputeTangents) regenerates
			 * these via MikkTSpace, same as the PSKX import path. */
		} else if (Header.Name == TEXT("VERTEXCOLORS")) {
			LOD.ColorChannels.SetNum(Header.ArraySize);
			for (int32 ChannelIndex = 0; ChannelIndex < Header.ArraySize; ChannelIndex++) {
				FUEModelVertexColorChannel& Channel = LOD.ColorChannels[ChannelIndex];
				Channel.Name = Reader.ReadFString();
				const int32 Count = Reader.ReadInt32();
				Channel.Colors.SetNum(Count);
				for (int32 ColorIndex = 0; ColorIndex < Count; ColorIndex++) {
					const uint8 R = Reader.ReadUInt8();
					const uint8 G = Reader.ReadUInt8();
					const uint8 B = Reader.ReadUInt8();
					const uint8 A = Reader.ReadUInt8();
					Channel.Colors[ColorIndex] = FColor(R, G, B, A);
				}
			}
		} else if (Header.Name == TEXT("TEXCOORDS")) {
			LOD.UVChannels.SetNum(Header.ArraySize);
			for (int32 ChannelIndex = 0; ChannelIndex < Header.ArraySize; ChannelIndex++) {
				const int32 Count = Reader.ReadInt32();
				TArray<FVector2f>& UVs = LOD.UVChannels[ChannelIndex];
				UVs.SetNum(Count);
				for (int32 UVIndex = 0; UVIndex < Count; UVIndex++) {
					UVs[UVIndex] = Reader.ReadVec2();
				}
				/* Not applying the Blender addon's V-flip (uv * (1,-1) + (0,1));
				 * that's a Blender-convention fix-up, not a data correction. */
			}
		} else if (Header.Name == TEXT("MATERIALS")) {
			LOD.Materials.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				FUEModelMaterialSection& Section = LOD.Materials[Index];
				Section.MaterialName = Reader.ReadFString();
				if (Reader.FileVersion >= UEFORMAT_VERSION_SERIALIZE_MATERIAL_PATH) {
					Section.MaterialPath = Reader.ReadFString();
				}
				Section.FirstIndex = Reader.ReadInt32();
				Section.NumFaces = Reader.ReadInt32();
			}
		} else if (Header.Name == TEXT("WEIGHTS")) {
			LOD.Weights.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				FUEModelWeight& Weight = LOD.Weights[Index];
				Weight.BoneIndex = Reader.ReadInt16();
				Weight.VertexIndex = Reader.ReadUInt32();
				Weight.Weight = Reader.ReadFloat();
			}
		} else if (Header.Name == TEXT("MORPHTARGETS")) {
			LOD.MorphTargets.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				FUEModelMorphTarget& Morph = LOD.MorphTargets[Index];
				Morph.Name = Reader.ReadFString();

				const int32 DeltaCount = Reader.ReadInt32(); // serialized array prefix
				Morph.Deltas.SetNum(DeltaCount);
				for (int32 DeltaIndex = 0; DeltaIndex < DeltaCount; DeltaIndex++) {
					FUEModelMorphVertex& Delta = Morph.Deltas[DeltaIndex];
					Delta.PositionDelta = Reader.ReadVec3() * ScaleFactor;
					Delta.TangentZDelta = Reader.ReadVec3();
					Delta.VertexIndex = Reader.ReadUInt32();
				}
			}
		}
		/* Unknown section names fall through and are skipped below. */

		SeekPastSection(Reader, Header);
	}
}

void ParseSkeleton(FUEModelReader& Reader, FUEModelSkeleton& Skeleton, const int32 ByteSize, const float ScaleFactor) {
	const int32 SkeletonEnd = Reader.Pos + ByteSize;

	while (Reader.Pos < SkeletonEnd) {
		FSectionHeader Header = ReadSectionHeader(Reader);

		if (Header.Name == TEXT("METADATA")) {
			Skeleton.SkeletonPath = Reader.ReadFString();
		} else if (Header.Name == TEXT("BONES")) {
			Skeleton.Bones.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				FUEModelBone& Bone = Skeleton.Bones[Index];
				Bone.Name = Reader.ReadFString();
				Bone.ParentIndex = Reader.ReadInt32();
				Bone.Position = Reader.ReadVec3() * ScaleFactor;
				Bone.Rotation = Reader.ReadQuatFromXYZW();
			}
		} else if (Header.Name == TEXT("SOCKETS")) {
			Skeleton.Sockets.SetNum(Header.ArraySize);
			for (int32 Index = 0; Index < Header.ArraySize; Index++) {
				FUEModelSocket& Socket = Skeleton.Sockets[Index];
				Socket.Name = Reader.ReadFString();
				Socket.BoneName = Reader.ReadFString();
				Socket.Position = Reader.ReadVec3() * ScaleFactor;
				Socket.Rotation = Reader.ReadQuatFromXYZW();
				Socket.Scale = Reader.ReadVec3();
			}
		}
		/* VIRTUALBONES intentionally not parsed - not needed to build the mesh;
		 * add here if you need retargeting metadata later. */

		SeekPastSection(Reader, Header);
	}
}

} // namespace

bool ReadUEModelMeshData(const FString& FilePath, FUEModelMeshData& OutData, const float ScaleFactor) {
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath)) {
		return false;
	}

	FUEModelReader Header;
	Header.Buffer = FileBytes.GetData();
	Header.Size = FileBytes.Num();

	const FString Magic = Header.ReadFixedString(8);
	if (Magic != TEXT("UEFORMAT")) {
		return false;
	}

	const FString Identifier = Header.ReadFString();
	if (Identifier != TEXT("UEMODEL")) {
		return false;
	}

	Header.FileVersion = Header.ReadUInt8();
	const FString ObjectName = Header.ReadFString();
	(void)ObjectName; // JSON already carries the authoritative asset name

	if (Header.FileVersion < UEFORMAT_VERSION_LOD_RESTRUCTURE) {
		/* The legacy (pre-restructure) single-LOD layout used a different, flatter
		 * chunk arrangement (see UEModel.from_archive_legacy in classes.py) and
		 * isn't handled here. Modern FModel exports use the current format. */
		return false;
	}

	const bool bIsCompressed = Header.ReadBool();

	TArray<uint8> DecompressedBytes;
	const uint8* BodyBuffer = Header.Buffer + Header.Pos;
	int32 BodySize = Header.Size - Header.Pos;

	if (bIsCompressed) {
		const FString CompressionType = Header.ReadFString();
		const int32 UncompressedSize = Header.ReadInt32();
		const int32 CompressedSize = Header.ReadInt32();
		(void)CompressedSize;

		DecompressedBytes.SetNumUninitialized(UncompressedSize);

		const uint8* CompressedStart = Header.Buffer + Header.Pos;
		const int32 CompressedBytesAvailable = Header.Size - Header.Pos;

		bool bDecompressOk = false;
		if (CompressionType == TEXT("GZIP")) {
			bDecompressOk = FCompression::UncompressMemory(NAME_Gzip, DecompressedBytes.GetData(), UncompressedSize, CompressedStart, CompressedBytesAvailable);
		} else if (CompressionType == TEXT("ZSTD")) {
			/* FCompression has no built-in Zstd codec (NAME_Zstd doesn't exist in
			 * stock UE5) - it's only available if you add a plugin that registers
			 * one, or link the engine's bundled ThirdParty/zstd yourself and call
			 * ZSTD_decompress() directly here instead of going through
			 * FCompression::UncompressMemory. Left unimplemented rather than
			 * silently failing to a wrong codec - if you hit this, check whether
			 * FModel is even writing your exports compressed first. */
			UE_LOG(LogReflection, Error, TEXT("ZSTD-compressed .uemodel is not supported by this build - see comment in UEModelMesh.cpp"));
			bDecompressOk = false;
		}

		if (!bDecompressOk) {
			return false;
		}

		BodyBuffer = DecompressedBytes.GetData();
		BodySize = DecompressedBytes.Num();
	}

	FUEModelReader Reader;
	Reader.Buffer = BodyBuffer;
	Reader.Size = BodySize;
	Reader.FileVersion = Header.FileVersion;

	while (!Reader.Eof()) {
		FSectionHeader Section = ReadSectionHeader(Reader);

		if (Section.Name == TEXT("LODS")) {
			OutData.LODs.SetNum(Section.ArraySize);
			for (int32 Index = 0; Index < Section.ArraySize; Index++) {
				ParseLOD(Reader, OutData.LODs[Index], ScaleFactor);
			}
		} else if (Section.Name == TEXT("SKELETON")) {
			ParseSkeleton(Reader, OutData.Skeleton, Section.ByteSize, ScaleFactor);
		}
		/* COLLISION intentionally not parsed here - not required to build the
		 * skeletal mesh; add a case here if you need convex collision too. */

		SeekPastSection(Reader, Section);
	}

	return !OutData.LODs.IsEmpty();
}

bool BuildSkeletalMeshFromUEModel(USkeletalMesh* Mesh, const FUEModelMeshData& Data, const int32 LODIndex) {
	if (Mesh == nullptr || !Data.LODs.IsValidIndex(LODIndex) || Data.Skeleton.Bones.IsEmpty()) {
		return false;
	}

	const FUEModelLOD& LOD = Data.LODs[LODIndex];
	if (LOD.Vertices.IsEmpty() || LOD.Indices.IsEmpty()) {
		return false;
	}

	FSkeletalMeshImportData ImportData;

	/* .uemodel vertices already occupy the same role PSKX's Points do - one
	 * position per unique attribute-vertex, seams pre-split by the exporter. */
	ImportData.Points = LOD.Vertices;

	const int32 FaceCount = LOD.Indices.Num() / 3;
	const bool bHasNormals = LOD.Normals.Num() == LOD.Vertices.Num() && !LOD.Normals.IsEmpty();

	/* One wedge per face corner, built directly from the flat vertex-indexed
	 * buffers - there's no separate wedge table to reuse the way PSKX has one. */
	ImportData.Wedges.SetNum(FaceCount * 3);
	ImportData.Faces.SetNum(FaceCount);
	ImportData.MaxMaterialIndex = 0;

	const int32 NumUVChannels = FMath::Max(1, LOD.UVChannels.Num());
	ImportData.NumTexCoords = FMath::Min<int32>(NumUVChannels, static_cast<int32>(MAX_TEXCOORDS));

	for (int32 FaceIndex = 0; FaceIndex < FaceCount; FaceIndex++) {
		SkeletalMeshImportData::FTriangle& Face = ImportData.Faces[FaceIndex];
		Face.AuxMatIndex = 0;
		Face.SmoothingGroups = 0;

		for (int32 Corner = 0; Corner < 3; Corner++) {
			const int32 WedgeIndex = FaceIndex * 3 + Corner;
			const uint32 VertexIndex = LOD.Indices[WedgeIndex];

			SkeletalMeshImportData::FVertex& Wedge = ImportData.Wedges[WedgeIndex];
			Wedge.VertexIndex = VertexIndex;

			for (int32 Channel = 0; Channel < static_cast<int32>(ImportData.NumTexCoords) && Channel < LOD.UVChannels.Num(); Channel++) {
				if (LOD.UVChannels[Channel].IsValidIndex(VertexIndex)) {
					Wedge.UVs[Channel] = LOD.UVChannels[Channel][VertexIndex];
				}
			}

			if (!LOD.ColorChannels.IsEmpty() && LOD.ColorChannels[0].Colors.IsValidIndex(VertexIndex)) {
				Wedge.Color = LOD.ColorChannels[0].Colors[VertexIndex];
			}

			Face.WedgeIndex[Corner] = WedgeIndex;
			if (bHasNormals) {
				Face.TangentZ[Corner] = LOD.Normals[VertexIndex];
			}
		}
	}

	ImportData.bHasVertexColors = !LOD.ColorChannels.IsEmpty();
	ImportData.bHasNormals = bHasNormals;
	ImportData.bHasTangents = false;

	/* Material sections index into the flat triangle buffer the same way
	 * PSKX's do, just addressed as vertex-index-space here. */
	ImportData.Materials.SetNum(LOD.Materials.Num());
	for (int32 MaterialIndex = 0; MaterialIndex < LOD.Materials.Num(); MaterialIndex++) {
		ImportData.Materials[MaterialIndex].MaterialImportName = LOD.Materials[MaterialIndex].MaterialName;

		const int32 StartFace = LOD.Materials[MaterialIndex].FirstIndex / 3;
		const int32 EndFace = FMath::Min(StartFace + LOD.Materials[MaterialIndex].NumFaces, FaceCount);
		for (int32 FaceIndex = StartFace; FaceIndex < EndFace; FaceIndex++) {
			if (ImportData.Faces.IsValidIndex(FaceIndex)) {
				ImportData.Faces[FaceIndex].MatIndex = MaterialIndex;
				ImportData.MaxMaterialIndex = FMath::Max<uint32>(ImportData.MaxMaterialIndex, MaterialIndex);
			}
		}
	}

	/* Reference skeleton. No PSKX-style un-mirroring here - see the header note
	 * on FUEModelBone about PreserveOriginalTransforms already being native. */
	ImportData.RefBonesBinary.SetNum(Data.Skeleton.Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Data.Skeleton.Bones.Num(); BoneIndex++) {
		const FUEModelBone& SourceBone = Data.Skeleton.Bones[BoneIndex];
		SkeletalMeshImportData::FBone& Bone = ImportData.RefBonesBinary[BoneIndex];
		Bone.Name = SourceBone.Name;
		Bone.Flags = 0;
		Bone.NumChildren = 0;
		Bone.ParentIndex = SourceBone.ParentIndex;
		Bone.BonePos.Transform = FTransform3f(SourceBone.Rotation, SourceBone.Position, FVector3f::OneVector);
		Bone.BonePos.Length = 0.0f;
		Bone.BonePos.XSize = 0.0f;
		Bone.BonePos.YSize = 0.0f;
		Bone.BonePos.ZSize = 0.0f;
	}

	/* Skinning influences - vertex-index-space matches ImportData.Points 1:1. */
	ImportData.Influences.SetNum(LOD.Weights.Num());
	for (int32 Index = 0; Index < LOD.Weights.Num(); Index++) {
		ImportData.Influences[Index].Weight = LOD.Weights[Index].Weight;
		ImportData.Influences[Index].VertexIndex = LOD.Weights[Index].VertexIndex;
		ImportData.Influences[Index].BoneIndex = LOD.Weights[Index].BoneIndex;
	}

	/* Morph targets - same vertex-index-space too, so ApplyMorphTargets'
	 * MeshToImportVertexMap reverse-lookup works unchanged. */
	for (int32 MorphIndex = 0; MorphIndex < LOD.MorphTargets.Num(); MorphIndex++) {
		const FUEModelMorphTarget& SourceMorph = LOD.MorphTargets[MorphIndex];
		if (SourceMorph.Deltas.IsEmpty()) {
			continue;
		}

		ImportData.MorphTargetNames.Add(SourceMorph.Name);

		TSet<uint32>& ModifiedPoints = ImportData.MorphTargetModifiedPoints.AddDefaulted_GetRef();
		FSkeletalMeshImportData& MorphImportData = ImportData.MorphTargets.AddDefaulted_GetRef();

		/* Sort deltas by VertexIndex so TSet iteration order (hash-based) matches the Points array. */
		TArray<FUEModelMorphVertex> SortedDeltas = SourceMorph.Deltas;
		SortedDeltas.Sort([](const FUEModelMorphVertex& A, const FUEModelMorphVertex& B) {
			return A.VertexIndex < B.VertexIndex;
		});

		MorphImportData.Points.SetNum(SortedDeltas.Num());
		for (int32 DeltaIndex = 0; DeltaIndex < SortedDeltas.Num(); DeltaIndex++) {
			const FUEModelMorphVertex& SourceDelta = SortedDeltas[DeltaIndex];
			ModifiedPoints.Add(SourceDelta.VertexIndex);
			/* FModel stores absolute target positions; UE expects deltas (target - base). */
			const FVector3f BasePos = LOD.Vertices.IsValidIndex(SourceDelta.VertexIndex) ? LOD.Vertices[SourceDelta.VertexIndex] : FVector3f::ZeroVector;
			MorphImportData.Points[DeltaIndex] = SourceDelta.PositionDelta + BasePos;
		}
	}

#if ENGINE_UE5
	/* From here down this mirrors BuildSkeletalMeshFromPSKX exactly - same
	 * engine-side build sequence as the FBX importer. */
	FScopedSkeletalMeshPostEditChange ScopedPostEditChange(Mesh, false, false);

	for (int32 ExistingLODIndex = 0, LODCount = Mesh->GetLODNum(); ExistingLODIndex < LODCount; ++ExistingLODIndex) {
		Mesh->ClearMeshDescriptionAndBulkData(ExistingLODIndex);
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

	NewLODInfo.BuildSettings.bRecomputeNormals = false;
	NewLODInfo.BuildSettings.bRecomputeTangents = true;
	NewLODInfo.BuildSettings.bUseMikkTSpace = true;

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Mesh->SaveLODImportedData(ImportLODModelIndex, ImportData);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	const FBox3f BoundingBox(LOD.Vertices.GetData(), LOD.Vertices.Num());
	Mesh->SetImportedBounds(FBoxSphereBounds((FBox)BoundingBox));

	Mesh->SetHasVertexColors(ImportData.bHasVertexColors);
	Mesh->SetVertexColorGuid(ImportData.bHasVertexColors ? FGuid::NewGuid() : FGuid());

	LODModel.NumTexCoords = FMath::Max<uint32>(1, ImportData.NumTexCoords);

	return true;
#else
	return false;
#endif
}