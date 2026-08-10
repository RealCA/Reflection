/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/PSAAnimation.h"
#include "Engine/Compatibility.h"
#include "Engine/AssetCompatibility.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Misc/FileHelper.h"

#if UE5_2_BEYOND
#include "Animation/AnimData/IAnimationDataController.h"
#endif

/* The ActorX binary layout as written by FModel's ActorXAnim.cs. All integers are
 * little-endian, every chunk starts with a fixed 20 byte chunk id followed by an
 * int32 version/type flag, an int32 data size and an int32 data count. */
namespace {

struct FPSAReader {
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

	FVector ReadVector() {
		FVector Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		Value.Z = ReadFloat();
		return Value;
	}

	FQuat ReadQuat() {
		FQuat Value;
		Value.X = ReadFloat();
		Value.Y = ReadFloat();
		Value.Z = ReadFloat();
		Value.W = ReadFloat();
		return Value;
	}
};

} // namespace

bool ReadPSAAnimationData(const FString& FilePath, FPSAAnimationData& OutData) {
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath)) {
		return false;
	}

	FPSAReader Reader;
	Reader.Buffer = Bytes.GetData();
	Reader.Size = Bytes.Num();

	int32 NumBones = 0;
	bool bGotAnimInfo = false;
	TArray<FTransform> FlatKeys;

	while (Reader.Pos + 32 <= Reader.Size) {
		const FString ChunkId = Reader.ReadFixedString(20);
		Reader.ReadInt32(); /* version / type flag */
		const int32 DataSize = Reader.ReadInt32();
		const int32 DataCount = Reader.ReadInt32();

		const int32 ChunkBytes = DataSize * DataCount;
		if (Reader.Pos + ChunkBytes > Reader.Size) {
			return false;
		}

		if (ChunkId == TEXT("BONENAMES")) {
			NumBones = DataCount;
			OutData.Bones.Reserve(NumBones);

			for (int32 BoneIndex = 0; BoneIndex < NumBones; BoneIndex++) {
				FPSABone Bone;
				Bone.Name = Reader.ReadFixedString(64);
				Reader.ReadInt32(); /* flags */
				Reader.ReadInt32(); /* num children */
				Bone.ParentIndex = Reader.ReadInt32();

				/* VJointPosPsk: orientation, position, length, size */
				Reader.ReadQuat();
				Reader.ReadVector();
				Reader.ReadFloat();
				Reader.ReadVector();

				OutData.Bones.Add(MoveTemp(Bone));
			}
		} else if (ChunkId == TEXT("ANIMINFO")) {
			Reader.ReadFixedString(64); /* name */
			Reader.ReadFixedString(64); /* group */
			Reader.ReadInt32(); /* total bones */
			Reader.ReadInt32(); /* root include */
			Reader.ReadInt32(); /* key compression style */
			Reader.ReadInt32(); /* key quotum */
			Reader.ReadFloat(); /* key reduction */
			Reader.ReadFloat(); /* track time */
			OutData.FramesPerSecond = Reader.ReadFloat();
			Reader.ReadInt32(); /* start bone */
			Reader.ReadInt32(); /* first raw frame */
			OutData.NumFrames = Reader.ReadInt32();

			bGotAnimInfo = true;
		} else if (ChunkId == TEXT("ANIMKEYS")) {
			if (!bGotAnimInfo || NumBones <= 0 || DataCount <= 0) {
				Reader.Pos += ChunkBytes;
				continue;
			}

			FlatKeys.SetNum(DataCount);
			for (int32 KeyIndex = 0; KeyIndex < DataCount; KeyIndex++) {
				/* VQuatAnimKey: position, orientation, time */
				FVector Position = Reader.ReadVector();
				FQuat Orientation = Reader.ReadQuat();
				Reader.ReadFloat(); /* time */

				/* FModel mirrors the mesh on export (ActorXAnim.cs), so flip the Y axis
				 * back to recover the original Unreal-space transforms. Root bone 0 also
				 * gets its quaternion handedness flipped. Both are involutions. */
				Position.Y = -Position.Y;
				Orientation.Y = -Orientation.Y;
				if (KeyIndex % NumBones == 0) {
					Orientation.W = -Orientation.W;
				}

				FlatKeys[KeyIndex] = FTransform(Orientation, Position, FVector::OneVector);
			}
		} else if (ChunkId == TEXT("SCALEKEYS")) {
			for (int32 KeyIndex = 0; KeyIndex < DataCount; KeyIndex++) {
				/* VScaleAnimKey: scale, time */
				const FVector Scale = Reader.ReadVector();
				Reader.ReadFloat(); /* time */

				if (FlatKeys.IsValidIndex(KeyIndex)) {
					FlatKeys[KeyIndex].SetScale3D(Scale);
				}
			}
		} else {
			/* Unknown chunk; skip past its data. */
			Reader.Pos += ChunkBytes;
		}
	}

	if (!bGotAnimInfo || OutData.NumFrames <= 0 || NumBones <= 0) {
		return false;
	}

	/* Keys arrive frame-major (frame 0: every bone, frame 1: every bone, ...), so re-index
	 * them into one contiguous local-space track per bone. */
	const int32 ExpectedKeys = OutData.NumFrames * NumBones;
	if (FlatKeys.Num() != ExpectedKeys) {
		return false;
	}

	OutData.Tracks.SetNum(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; BoneIndex++) {
		OutData.Tracks[BoneIndex].SetNum(OutData.NumFrames);
		for (int32 Frame = 0; Frame < OutData.NumFrames; Frame++) {
			OutData.Tracks[BoneIndex][Frame] = FlatKeys[Frame * NumBones + BoneIndex];
		}
	}

	return true;
}

bool ApplyPSAAnimationData(UAnimSequenceBase* Sequence, USkeleton* Skeleton, const FPSAAnimationData& Data) {
#if UE5_2_BEYOND
	if (Sequence == nullptr || Skeleton == nullptr) {
		return false;
	}

	if (Data.NumFrames <= 0 || Data.Bones.Num() == 0 || Data.Tracks.Num() != Data.Bones.Num()) {
		return false;
	}

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	/* Only bones that exist on this skeleton can become tracks. */
	TArray<int32> ValidBones;
	ValidBones.Reserve(Data.Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Data.Bones.Num(); BoneIndex++) {
		if (RefSkeleton.FindBoneIndex(FName(*Data.Bones[BoneIndex].Name)) != INDEX_NONE) {
			ValidBones.Add(BoneIndex);
		}
	}

	if (ValidBones.Num() == 0) {
		return false;
	}

	const float FPS = Data.FramesPerSecond > 0.0f ? Data.FramesPerSecond : 30.0f;
	const int32 FramesPerSecond = FMath::Max(1, FMath::RoundToInt(FPS));

	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("PSA Import")));

	/* On UE5.2+ the data model is an interface; since UE 5.5 the AnimSequence defaults to the
	 * Sequencer-backed model (UAnimationSequencerDataModel) whose MovieScene and Control Rig
	 * track are only created here. On the legacy UAnimDataController this call is a no-op, so
	 * it is safe to invoke regardless of which model implementation the sequence uses. */
	Controller.InitializeModel();

	Controller.SetFrameRate(FFrameRate(FramesPerSecond, 1), false);
	Controller.SetNumberOfFrames(Data.NumFrames, false);

	TArray<FVector> Positions;
	TArray<FQuat> Rotations;
	TArray<FVector> Scales;

	for (const int32 BoneIndex : ValidBones) {
		const FName BoneName(*Data.Bones[BoneIndex].Name);
		Controller.AddBoneTrack(BoneName, false);

		const TArray<FTransform>& Track = Data.Tracks[BoneIndex];
		const int32 NumKeys = Track.Num();

		Positions.SetNum(NumKeys);
		Rotations.SetNum(NumKeys);
		Scales.SetNum(NumKeys);

		for (int32 Frame = 0; Frame < NumKeys; Frame++) {
			Positions[Frame] = Track[Frame].GetLocation();
			Rotations[Frame] = Track[Frame].GetRotation();
			Scales[Frame] = Track[Frame].GetScale3D();
		}

		Controller.SetBoneTrackKeys(BoneName, Positions, Rotations, Scales, false);
	}

	Controller.NotifyPopulated();

	Controller.CloseBracket(false);

	/* Also set the legacy SequenceLength so the Animation Editor timeline reads a non-zero length. */
	if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Sequence)) {
		const float PlayLength = FramesPerSecond > 0 ? Data.NumFrames / static_cast<float>(FramesPerSecond) : 0.0f;
		if (PlayLength > 0.0f) {
			SetAnimSequenceLength(AnimSequence, PlayLength);
		}
	}

	return true;
#else
	return false;
#endif
}
