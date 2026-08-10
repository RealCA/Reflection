/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class USkeleton;
class UAnimSequenceBase;

/* A single bone as written into the ActorX (.psa) animation binary. */
struct FPSABone {
	FString Name;
	int32 ParentIndex = -1;
};

/* Decoded contents of an ActorX (.psa) animation export. */
struct FPSAAnimationData {
	FString Name;
	int32 NumFrames = 0;
	float FramesPerSecond = 30.0f;
	TArray<FPSABone> Bones;
	/* [bone][frame] local-space bone transforms, un-mirrored back to Unreal space. */
	TArray<TArray<FTransform>> Tracks;
};

/* Parses an ActorX (.psa) file as produced by FModel/CUE4Parse. */
bool ReadPSAAnimationData(const FString& FilePath, FPSAAnimationData& OutData);

/* Feeds the decoded tracks into an animation sequence through its controller. */
bool ApplyPSAAnimationData(UAnimSequenceBase* Sequence, USkeleton* Skeleton, const FPSAAnimationData& Data);
