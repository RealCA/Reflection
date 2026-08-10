/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/* .uemodel is a flat, vertex-indexed format (one UV/color/normal per vertex,
 * seams already split by the exporter) as opposed to PSKX's shared-point +
 * wedge layout. Weights and morph deltas are indexed against this same
 * per-vertex space, so they line up with FUEModelMeshData::Vertices the same
 * way FPSKXMeshData's Influences/MorphTargets line up with Points. */

struct FUEModelVertexColorChannel {
	FString Name;
	TArray<FColor> Colors; // one per vertex
};

struct FUEModelMaterialSection {
	FString MaterialName;
	FString MaterialPath;
	int32 FirstIndex = 0; // index into the flattened triangle-index buffer
	int32 NumFaces = 0;
};

struct FUEModelWeight {
	int32 BoneIndex = 0;
	uint32 VertexIndex = 0;
	float Weight = 0.0f;
};

/* Same shape as FPSKXMorphVertex/FPSKXMorphTarget, kept as separate types
 * to avoid coupling the two binary formats; ApplyMorphTargets() in
 * ISkeletalMeshImporter is overloaded for both. */
struct FUEModelMorphVertex {
	FVector3f PositionDelta = FVector3f::ZeroVector;
	FVector3f TangentZDelta = FVector3f::ZeroVector;
	uint32 VertexIndex = 0;
};

struct FUEModelMorphTarget {
	FString Name;
	TArray<FUEModelMorphVertex> Deltas;
};

struct FUEModelLOD {
	FString Name;
	TArray<FVector3f> Vertices;
	TArray<FVector3f> Normals; // one per vertex, already unpacked from WXYZ
	TArray<uint32> Indices; // flattened triangle list, 3 per face
	TArray<TArray<FVector2f>> UVChannels; // one array per channel, one entry per vertex
	TArray<FUEModelVertexColorChannel> ColorChannels;
	TArray<FUEModelMaterialSection> Materials;
	TArray<FUEModelWeight> Weights;
	TArray<FUEModelMorphTarget> MorphTargets;
};

struct FUEModelBone {
	FString Name;
	int32 ParentIndex = -1;
	FVector3f Position = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;
};

struct FUEModelSocket {
	FString Name;
	FString BoneName;
	FVector3f Position = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;
	FVector3f Scale = FVector3f::OneVector;
};

struct FUEModelSkeleton {
	FString SkeletonPath; // metadata only, informational; JSON already carries the authoritative reference
	TArray<FUEModelBone> Bones;
	TArray<FUEModelSocket> Sockets;
};

struct FUEModelMeshData {
	TArray<FUEModelLOD> LODs;
	FUEModelSkeleton Skeleton;
};

/* Parses a UEFormat (.uemodel) mesh export as written by FModel/CUE4Parse.
 * ScaleFactor should be 1.0 when importing straight into UE5 (positions are
 * already in centimeters); it only needs to be 0.01 if you're deliberately
 * matching the Blender addon's cm->m convention. */
bool ReadUEModelMeshData(const FString& FilePath, FUEModelMeshData& OutData, float ScaleFactor = 1.0f);

/* Builds the skeletal mesh (reference skeleton, source models and render
 * data) from the decoded UEModel data for the given LOD index. Returns
 * false on failure. */
bool BuildSkeletalMeshFromUEModel(USkeletalMesh* Mesh, const FUEModelMeshData& Data, int32 LODIndex = 0);
