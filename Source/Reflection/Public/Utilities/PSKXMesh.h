/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/* A bone as written into the ActorX (.pskx) mesh binary. ParentIndex refers to
 * this same array; Transform is the local-space reference pose, un-mirrored. */
struct FPSKXBone {
	FString Name;
	int32 ParentIndex = -1;
	FTransform3f Transform;
};

/* A wedge: one corner of a triangle, referencing a shared point. */
struct FPSKXWedge {
	uint32 PointIndex = 0;
	FVector2f UV = FVector2f::ZeroVector;
	uint32 MatIndex = 0;
	FColor Color = FColor::White;
};

/* A triangle. Indices reference the wedge array; the writer already accounts for
 * the Y-axis mirror, so the corner order is taken verbatim. */
struct FPSKXFace {
	uint32 WedgeIndex[3] = { 0, 0, 0 };
	uint32 MatIndex = 0;
	uint32 SmoothingGroups = 0;
};

/* A skinning influence on a shared point. */
struct FPSKXInfluence {
	float Weight = 0.0f;
	uint32 PointIndex = 0;
	int32 BoneIndex = 0;
};

/* A single vertex delta within a morph target (blend shape), as written into
 * the MRPHDATA chunk. PointIndex references the same shared-point space as
 * Points/Influences above. */
struct FPSKXMorphVertex {
	FVector3f PositionDelta = FVector3f::ZeroVector;
	FVector3f TangentZDelta = FVector3f::ZeroVector;
	uint32 PointIndex = 0;
};

/* A named morph target (blend shape), assembled from the MRPHINFO name/count
 * table and its corresponding run of MRPHDATA entries. */
struct FPSKXMorphTarget {
	FString Name;
	TArray<FPSKXMorphVertex> Deltas;
};

/* Decoded contents of an ActorX (.pskx) mesh export. Points, normals and bone
 * transforms are un-mirrored back to Unreal space (the writer flips +Y and the
 * root bone quaternion handedness, both involutions). */
struct FPSKXMeshData {
	TArray<FVector3f> Points;
	TArray<FVector3f> Normals;
	TArray<FPSKXWedge> Wedges;
	TArray<FPSKXFace> Faces;
	TArray<FString> MaterialNames;
	TArray<FPSKXBone> Bones;
	TArray<FPSKXInfluence> Influences;
	/* One array of UVs per extra channel, per wedge. */
	TArray<TArray<FVector2f>> ExtraUVs;
	bool bHasVertexColors = false;

	/* Morph targets (blend shapes) decoded from MRPHINFO/MRPHDATA, if present. */
	TArray<FPSKXMorphTarget> MorphTargets;
};

/* Parses an ActorX (.pskx / .psk) mesh file as produced by FModel/CUE4Parse. */
bool ReadPSKXMeshData(const FString& FilePath, FPSKXMeshData& OutData);

/* Builds the skeletal mesh (reference skeleton, source models and render data)
 * from the decoded ActorX data. Returns false on failure. */
bool BuildSkeletalMeshFromPSKX(USkeletalMesh* Mesh, const FPSKXMeshData& Data);
