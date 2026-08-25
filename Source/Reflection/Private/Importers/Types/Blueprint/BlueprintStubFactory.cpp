/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintStubFactory.h"

#include "Misc/FileHelper.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Utilities/MissingDependencies.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintStub, Log, All);

TSet<FString> FBlueprintStubFactory::StubFiles;

bool FBlueprintStubFactory::IsStubImport(const FString& FilePath) {
	FString Normalized = FilePath.Replace(TEXT("\\"), TEXT("/"));
	return StubFiles.Contains(Normalized);
}

void FBlueprintStubFactory::UnregisterStubImport(const FString& FilePath) {
	FString Normalized = FilePath.Replace(TEXT("\\"), TEXT("/"));
	if (StubFiles.Remove(Normalized) > 0) {
		UE_LOG(LogBlueprintStub, Log, TEXT("Unregistered stub import: %s"), *Normalized);
	}
}

void FBlueprintStubFactory::ClearStubImports() {
	StubFiles.Empty();
}

static FString GetContentRoot(const FString& FilePath) {
	FString P = FilePath.Replace(TEXT("\\"), TEXT("/"));
	int32 Idx = P.Find(TEXT("/Content/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (Idx != INDEX_NONE) return P.Left(Idx + 9);
	return FPaths::GetPath(P);
}

static FString UePathToDisk(const FString& UePath, const FString& ContentRoot) {
	FString Clean = UePath;
	Clean.RemoveFromStart(TEXT("/Game/"));
	int32 Dot;
	if (Clean.FindLastChar(TEXT('.'), Dot)) Clean = Clean.Left(Dot);
	return ContentRoot + Clean + TEXT(".json");
}

static void CollectGameRefs(const TSharedPtr<FJsonValue>& Val, TSet<FString>& Out, TSet<FString>* OutDataRefs = nullptr) {
	if (!Val.IsValid() || Val->IsNull()) return;

	if (Val->Type == EJson::Object) {
		auto Obj = Val->AsObject();
		if (!Obj.IsValid()) return;

		FString ObjName, ObjPath;
		if (Obj->TryGetStringField(TEXT("ObjectName"), ObjName)
			&& Obj->TryGetStringField(TEXT("ObjectPath"), ObjPath)
			&& ObjPath.StartsWith(TEXT("/Game/")))
		{
			FString Clean = ObjPath;
			int32 Dot;
			if (Clean.FindLastChar(TEXT('.'), Dot)) Clean = Clean.Left(Dot);

			/* Blueprint classes become stubs; UserDefinedStruct/Enum refs are
			 * DATA dependencies - they import real and must exist before any
			 * function signature or variable that types against them
			 * (08.24: BP_Stockpile compiled with null-struct locals because
			 * S_ClothesStats was outside the closure). */
			if (OutDataRefs
				&& (ObjName.Contains(TEXT("UserDefinedStruct'"))
					|| ObjName.Contains(TEXT("UserDefinedEnum'"))))
			{
				OutDataRefs->Add(Clean);
			}
			else if (ObjName.Contains(TEXT("BlueprintGeneratedClass"))
				|| ObjName.Contains(TEXT("WidgetBlueprintGeneratedClass"))
				|| ObjName.Contains(TEXT("AnimBlueprintGeneratedClass"))
				|| ObjName.Contains(TEXT("'Default__")))
			{
				Out.Add(Clean);
			}
		}

		for (const auto& Pair : Obj->Values) {
			CollectGameRefs(Pair.Value, Out, OutDataRefs);
		}
	} else if (Val->Type == EJson::Array) {
		for (const auto& Elem : Val->AsArray()) {
			CollectGameRefs(Elem, Out, OutDataRefs);
		}
	}
}

/* Extracts the class export's Super reference (package path form, empty when
 * absent/native). Used by the parent-chain walk below. */
static FString ExtractSuperRef(const FString& JsonFilePath) {
	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *JsonFilePath)) return FString();

	TArray<TSharedPtr<FJsonValue>> Arr;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(R, Arr) || Arr.Num() == 0) return FString();

	for (const auto& Export : Arr) {
		auto Root = Export->AsObject();
		if (!Root.IsValid()) continue;

		FString Type;
		if (!Root->TryGetStringField(TEXT("Type"), Type)) continue;
		if (!(Type.Contains(TEXT("BlueprintGeneratedClass"))
			|| Type.Contains(TEXT("WidgetBlueprintGeneratedClass"))
			|| Type.Contains(TEXT("AnimBlueprintGeneratedClass")))) {
			continue;
		}

		const TSharedPtr<FJsonObject>* SuperPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("Super"), SuperPtr) && SuperPtr && SuperPtr->IsValid()) {
			FString ObjName, ObjPath;
			if ((*SuperPtr)->TryGetStringField(TEXT("ObjectName"), ObjName)
				&& (*SuperPtr)->TryGetStringField(TEXT("ObjectPath"), ObjPath)
				&& ObjPath.StartsWith(TEXT("/Game/"))
				&& (ObjName.Contains(TEXT("BlueprintGeneratedClass"))
					|| ObjName.Contains(TEXT("WidgetBlueprintGeneratedClass"))
					|| ObjName.Contains(TEXT("AnimBlueprintGeneratedClass")))) {
				FString Clean = ObjPath;
				int32 Dot;
				if (Clean.FindLastChar(TEXT('.'), Dot)) Clean = Clean.Left(Dot);
				return Clean;
			}
		}
		return FString();
	}
	return FString();
}

TArray<FString> FBlueprintStubFactory::ResolveDependencies(const FString& JsonFilePath) {
	TArray<FString> Result;

	FString Normalized = JsonFilePath.Replace(TEXT("\\"), TEXT("/"));
	FString ContentRoot = GetContentRoot(Normalized);
	UE_LOG(LogBlueprintStub, Log, TEXT("ResolveDependencies: %s (root: %s)"),
		*FPaths::GetCleanFilename(Normalized), *ContentRoot);

	FString JsonContent;
	if (!FFileHelper::LoadFileToString(JsonContent, *Normalized)) {
		UE_LOG(LogBlueprintStub, Warning, TEXT("Failed to load: %s"), *Normalized);
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonContent);
	if (!FJsonSerializer::Deserialize(R, Arr) || Arr.Num() == 0) {
		return Result;
	}

	bool bBP = false;
	for (const auto& Export : Arr) {
		auto Root = Export->AsObject();
		if (!Root.IsValid()) continue;

		FString Type;
		if (Root->TryGetStringField(TEXT("Type"), Type)
			&& (Type.Contains(TEXT("BlueprintGeneratedClass"))
				|| Type.Contains(TEXT("WidgetBlueprintGeneratedClass"))
				|| Type.Contains(TEXT("AnimBlueprintGeneratedClass")))) {
			bBP = true;
			break;
		}
	}
	if (!bBP) return Result;

	/* Own package path to skip self */
	FString OwnPkgPath;
	{
		int32 ContentIdx = Normalized.Find(TEXT("/Content/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (ContentIdx != INDEX_NONE) {
			OwnPkgPath = TEXT("/Game/") + Normalized.Mid(ContentIdx + 9);
			int32 Dot;
			if (OwnPkgPath.FindLastChar(TEXT('.'), Dot)) OwnPkgPath = OwnPkgPath.Left(Dot);
		}
	}

	TSet<FString> AllRefs;
	TSet<FString> DataRefs;
	for (const auto& Export : Arr) {
		auto Root = Export->AsObject();
		if (Root.IsValid()) {
			CollectGameRefs(MakeShared<FJsonValueObject>(Root), AllRefs, &DataRefs);
		}
	}

	UE_LOG(LogBlueprintStub, Log, TEXT("Found %d BP refs, %d struct/enum data refs"), AllRefs.Num(), DataRefs.Num());

	/* A dependency whose blueprint already exists - on disk or already loaded in memory -
	 * is not a stub. References resolve to the real asset, so there is nothing to build.
	 * AssetExistsInProject is deliberately NOT used here: a failed LoadPackage attempt
	 * (e.g. resolving a stub's BP-typed property) leaves a hollow in-memory package that
	 * FindObject finds but that contains no class, so it would wrongly suppress the stub
	 * and leave every cast/reference to the dependency unresolvable. Require either the
	 * .uasset on disk or a fully-imported (rooted) in-memory asset instead. */
	const auto IsRealAsset = [](const FString& Ref) {
		return AssetExistsOnDisk(Ref) || IsAssetFullyImported(Ref);
	};

	/* Only the DIRECT dependencies of the importing file become stubs. Transitive deps are
	 * intentionally not followed - walking a stub's own references pulls in the whole
	 * project (widgets, anim BPs, characters) and caused a 143-file import plus a crash.
	 * A stub is a hollow class; its function signatures only need the class object to
	 * exist, and direct deps are registered before the importing file is processed. */
	for (auto& Ref : AllRefs) {
		if (Ref == OwnPkgPath) continue;

		if (IsRealAsset(Ref)) {
			UE_LOG(LogBlueprintStub, Log, TEXT("  dep already exists, not stubbing: %s"), *Ref);
			continue;
		}

		FString DiskPath = UePathToDisk(Ref, ContentRoot);
		FString DiskNorm = DiskPath.Replace(TEXT("\\"), TEXT("/"));

		if (FPaths::FileExists(DiskNorm)) {
			Result.Add(DiskNorm);
			StubFiles.Add(DiskNorm);
			UE_LOG(LogBlueprintStub, Log, TEXT("  registered as stub: %s"), *FPaths::GetCleanFilename(DiskNorm));
		} else {
			UE_LOG(LogBlueprintStub, Warning, TEXT("  dep not found: %s"), *DiskNorm);
		}
	}

	/* The class-parent chain is NOT optional (plan 013): a child blueprint
	 * cannot compile without its parent class object. 08.24: BP_Stockpile
	 * compiled with parent BP_ManagerialGame_C absent - never imported, never
	 * stubbed, because only DIRECT refs are walked - and both of its compile
	 * attempts faulted inside the Kismet compiler. Follow Super transitively
	 * and stub every missing ancestor - for EVERY registered stub, not just
	 * the picked file (BP_ManagerialGame is BP_Stockpile's parent, discovered
	 * only through the stub, not through the root's own refs). Parent chains
	 * are short and bounded, unlike the full transitive ref walk that was
	 * explicitly rejected. */
	TSet<FString> VisitedParents;

	auto WalkParentChain = [&](const FString& RefPkgPath) {
		FString ParentRef = ExtractSuperRef(UePathToDisk(RefPkgPath, ContentRoot));
		int32 ChainGuard = 0;
		while (!ParentRef.IsEmpty() && ParentRef != OwnPkgPath && ChainGuard < 16 && !VisitedParents.Contains(ParentRef)) {
			VisitedParents.Add(ParentRef);
			ChainGuard++;

			if (IsRealAsset(ParentRef)) {
				UE_LOG(LogBlueprintStub, Log, TEXT("  parent already exists, not stubbing: %s"), *ParentRef);
				return;
			}

			FString ParentDisk = UePathToDisk(ParentRef, ContentRoot);
			FString ParentNorm = ParentDisk.Replace(TEXT("\\"), TEXT("/"));
			if (!FPaths::FileExists(ParentNorm)) {
				UE_LOG(LogBlueprintStub, Warning, TEXT("  parent json not found: %s"), *ParentNorm);
				return;
			}

			/* Ancestors must be processed BEFORE the descendant that needs them:
			 * prepend so the queue order is deepest-parent first. 08.24: the
			 * walk appended BP_ManagerialGame AFTER BP_Stockpile - the job
			 * aborted on BP_Stockpile before the parent stub was ever created,
			 * so the compile still ran with a missing parent. */
			Result.Insert(ParentNorm, 0);
			StubFiles.Add(ParentNorm);
			UE_LOG(LogBlueprintStub, Log, TEXT("  registered parent stub (Super chain): %s"), *FPaths::GetCleanFilename(ParentNorm));

			/* Walk up to the parent's own parent. */
			ParentRef = ExtractSuperRef(ParentNorm);
		}
	};

	for (auto& Ref : AllRefs) {
		if (Ref == OwnPkgPath) continue;

		if (IsRealAsset(Ref)) {
			UE_LOG(LogBlueprintStub, Log, TEXT("  dep already exists, not stubbing: %s"), *Ref);
			continue;
		}

		FString DiskPath = UePathToDisk(Ref, ContentRoot);
		FString DiskNorm = DiskPath.Replace(TEXT("\\"), TEXT("/"));

		if (FPaths::FileExists(DiskNorm)) {
			Result.Add(DiskNorm);
			StubFiles.Add(DiskNorm);
			UE_LOG(LogBlueprintStub, Log, TEXT("  registered as stub: %s"), *FPaths::GetCleanFilename(DiskNorm));
			WalkParentChain(Ref);
		} else {
			UE_LOG(LogBlueprintStub, Warning, TEXT("  dep not found: %s"), *DiskNorm);
		}
	}

	/* The picked file imports real, but its own compile needs its parent too. */
	WalkParentChain(OwnPkgPath);

	/* Data-asset closure (plan 013): UserDefinedStruct/Enum refs import REAL
	 * (no stub concept) and are walked recursively - a struct's members can
	 * reference further structs/enums, and skipping one level just moves the
	 * null-type bomb into the struct import. Every discovered json is
	 * prepended, so each dependency lands before its dependent. BP refs found
	 * inside data jsons are stubbed like any other (with their parent chains). */
	{
		TSet<FString> VisitedData;
		TArray<FString> DataStack;

		auto TryPushData = [&](const FString& DataRef) {
			if (DataRef.IsEmpty() || DataRef == OwnPkgPath || VisitedData.Contains(DataRef)) return;
			if (IsRealAsset(DataRef)) return;
			FString Disk = UePathToDisk(DataRef, ContentRoot).Replace(TEXT("\\"), TEXT("/"));
			if (!FPaths::FileExists(Disk)) {
				UE_LOG(LogBlueprintStub, Warning, TEXT("  data dep json not found: %s"), *Disk);
				return;
			}
			VisitedData.Add(DataRef);
			DataStack.Push(Disk);
		};

		for (const FString& DataRef : DataRefs) {
			TryPushData(DataRef);
		}

		while (DataStack.Num() > 0) {
			FString DataJson = DataStack.Pop();

			/* Real import, before every BP that references it. */
			Result.Insert(DataJson, 0);
			UE_LOG(LogBlueprintStub, Log, TEXT("  registered data import (struct/enum): %s"), *FPaths::GetCleanFilename(DataJson));

			/* Walk the struct/enum json's own refs: data refs recurse, BP refs
			 * stub with their parent chains. */
			FString InnerContent;
			if (!FFileHelper::LoadFileToString(InnerContent, *DataJson)) continue;
			TArray<TSharedPtr<FJsonValue>> InnerArr;
			TSharedRef<TJsonReader<>> InnerReader = TJsonReaderFactory<>::Create(InnerContent);
			if (!FJsonSerializer::Deserialize(InnerReader, InnerArr) || InnerArr.Num() == 0) continue;

			TSet<FString> InnerBPRefs;
			TSet<FString> InnerDataRefs;
			for (const auto& Export : InnerArr) {
				auto Root = Export->AsObject();
				if (Root.IsValid()) {
					CollectGameRefs(MakeShared<FJsonValueObject>(Root), InnerBPRefs, &InnerDataRefs);
				}
			}

			for (const FString& Inner : InnerDataRefs) {
				TryPushData(Inner);
			}
			for (const FString& Inner : InnerBPRefs) {
				if (Inner == OwnPkgPath || VisitedParents.Contains(Inner)) continue;
				if (IsRealAsset(Inner)) continue;
				FString InnerDisk = UePathToDisk(Inner, ContentRoot).Replace(TEXT("\\"), TEXT("/"));
				if (!FPaths::FileExists(InnerDisk)) continue;
				Result.Add(InnerDisk);
				StubFiles.Add(InnerDisk);
				VisitedParents.Add(Inner);
				UE_LOG(LogBlueprintStub, Log, TEXT("  registered as stub (data closure): %s"), *FPaths::GetCleanFilename(InnerDisk));
				WalkParentChain(Inner);
			}
		}
	}

	return Result;
}
