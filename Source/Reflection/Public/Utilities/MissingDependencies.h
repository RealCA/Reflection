/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "Engine/Notifications.h"
#include "Utilities/JsonHelpers.h"

/* Maximum recursion depth for dependency scanning. Prevents stack overflow on circular
 * or extremely deep reference chains. */
constexpr int32 GMaxDependencyRecursionDepth = 64;

/* Reads the "Package" field from the first export in a JSON file (e.g. "/Game/TouchyGame/BP/MyBP").
 * Returns empty string if the file can't be read or has no Package field. */
inline FString GetPackagePathFromJson(const FString& JsonFilePath) {
	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(JsonFilePath, DataObjects) || DataObjects.Num() == 0) return FString();

	const TSharedPtr<FJsonObject> RootObj = DataObjects[0]->AsObject();
	if (!RootObj.IsValid() || !RootObj->HasField(TEXT("Package"))) return FString();

	return RootObj->GetStringField(TEXT("Package"));
}

/* Checks whether an asset already exists in the current project at the given /Game/ path.
 * Uses FindObject for a fast non-loading check. */
inline bool AssetExistsInProject(const FString& GamePackagePath) {
	if (!GamePackagePath.StartsWith(TEXT("/Game/"))) return false;

	/* FindObject with nullptr outer searches the entire default object pool */
	return FindObject<UObject>(nullptr, *GamePackagePath) != nullptr;
}

/* Checks whether the .uasset/.umap for a /Game/ package path exists on disk.
 * Complements AssetExistsInProject (memory) without ever touching the loader. */
inline bool AssetExistsOnDisk(const FString& GamePackagePath) {
	if (!GamePackagePath.StartsWith(TEXT("/Game/"))) return false;

	const FString AssetFile = FPackageName::LongPackageNameToFilename(GamePackagePath, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(AssetFile)) return true;

	const FString MapFile = FPackageName::LongPackageNameToFilename(GamePackagePath, FPackageName::GetMapPackageExtension());
	return FPaths::FileExists(MapFile);
}

/* True when the asset at a /Game/ path is already resident in memory AND rooted - i.e. fully
 * imported this session (HandleAssetCreation calls AddToRoot), not merely a shell this batch
 * built (shells are never rooted). A StaticLoadObject on a rooted asset resolves against
 * memory and never re-enters the loader. */
inline bool IsAssetFullyImported(const FString& GamePackagePath) {
	if (!GamePackagePath.StartsWith(TEXT("/Game/"))) return false;

	if (UObject* Existing = FindObject<UObject>(nullptr, *GamePackagePath)) {
		return Existing->IsRooted();
	}

	return false;
}

/* Extracts all /Game/ asset references from a JSON value tree. OutSeen deduplicates across
 * the entire scan so the same package path is only ever collected once. */
inline TArray<FString> CollectGameReferences(const TSharedPtr<FJsonValue>& Value, TSet<FString>& OutSeen) {
	TArray<FString> Results;

	if (!Value.IsValid()) return Results;

	if (Value->Type == EJson::Object) {
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		if (!Obj.IsValid()) return Results;

		FString ObjectPath;

		if (Obj->TryGetStringField(TEXT("ObjectPath"), ObjectPath) && ObjectPath.StartsWith(TEXT("/Game/"))) {
			/* Strip trailing export index (.0, .1, etc.) */
			FString PackagePath = ObjectPath;
			const int32 DotIndex = PackagePath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (DotIndex != INDEX_NONE) {
				PackagePath = PackagePath.Left(DotIndex);
			}

			if (!OutSeen.Contains(PackagePath)) {
				OutSeen.Add(PackagePath);
				Results.Add(PackagePath);
			}
		}

		/* Recurse into all fields */
		for (const auto& Pair : Obj->Values) {
			Results.Append(CollectGameReferences(Pair.Value, OutSeen));
		}
	} else if (Value->Type == EJson::Array) {
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Value->TryGetArray(Arr)) {
			for (const TSharedPtr<FJsonValue>& Elem : *Arr) {
				Results.Append(CollectGameReferences(Elem, OutSeen));
			}
		}
	}

	return Results;
}

/* Given a /Game/ package path (e.g. "/Game/TouchyGame/BP/MyEnum"), returns the expected
 * absolute file path of its JSON export inside the export directory. The export directory
 * is derived by walking up from SourceFilePath until a "Content" directory is found. */
inline FString ExportPathForGameRef(const FString& GamePath, const FString& SourceFilePath) {
	FString RelPath = GamePath;
	RelPath.RemoveFromStart(TEXT("/Game/"));
	RelPath.RemoveFromStart(TEXT("Game/"));

	/* Walk up from the source file until we find the Content/ directory */
	FString ContentRoot = FPaths::GetPath(SourceFilePath);
	int32 Depth = 0;
	while (!ContentRoot.IsEmpty() && !FPaths::GetCleanFilename(ContentRoot).Equals(TEXT("Content"), ESearchCase::IgnoreCase)) {
		FString Parent = FPaths::GetPath(ContentRoot);
		if (Parent == ContentRoot) break; /* reached drive root */
		ContentRoot = Parent;
		if (++Depth > 20) break; /* safety: don't walk above the project */
	}

	if (FPaths::GetCleanFilename(ContentRoot).Equals(TEXT("Content"), ESearchCase::IgnoreCase)) {
		return FPaths::Combine(ContentRoot, RelPath + TEXT(".json"));
	}

	/* Could not locate Content/ — return empty so callers can detect the failure */
	return FString();
}

/* Validates that the source file sits inside a Content/ directory whose subfolder structure
 * matches its /Game/ package path. Returns true if the hierarchy is correct. */
inline bool ValidateExportHierarchy(const FString& JsonFilePath, FString& OutError) {
	/* Extract the /Game/ path from the JSON's own Package field */
	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(JsonFilePath, DataObjects) || DataObjects.Num() == 0) {
		OutError = TEXT("Could not read JSON file");
		return false;
	}

	const TSharedPtr<FJsonObject> RootObj = DataObjects[0]->AsObject();
	if (!RootObj.IsValid() || !RootObj->HasField(TEXT("Package"))) {
		/* Not all exports have a Package field (e.g. sub-objects); skip validation */
		return true;
	}

	const FString Package = RootObj->GetStringField(TEXT("Package"));
	if (!Package.StartsWith(TEXT("/Game/"))) return true; /* engine or /Script/ path, skip */

	FString RelPath = Package;
	RelPath.RemoveFromStart(TEXT("/Game/"));

	/* The file should live at <ContentRoot>/<RelPath>.json */
	const FString ExpectedFile = ExportPathForGameRef(Package, JsonFilePath);
	if (ExpectedFile.IsEmpty()) {
		OutError = TEXT("Cannot locate Content/ directory relative to: ") + JsonFilePath;
		return false;
	}

	/* Normalize both paths for comparison */
	const FString NormalizedActual = FPaths::ConvertRelativePathToFull(JsonFilePath).Replace(TEXT("\\"), TEXT("/"));
	const FString NormalizedExpected = ExpectedFile.Replace(TEXT("\\"), TEXT("/"));

	if (!NormalizedActual.Equals(NormalizedExpected, ESearchCase::IgnoreCase)) {
		OutError = FString::Printf(
			TEXT("Wrong export hierarchy.\nExpected: %s\nActual: %s\nKeep the file hierarchy the same as in the .pak."),
			*NormalizedExpected,
			*NormalizedActual
		);
		return false;
	}

	return true;
}

/* Scans a JSON export file for all /Game/ references and returns the list of package paths
 * whose export files do not exist on disk. */
inline TArray<FString> FindMissingDependencies(const FString& JsonFilePath) {
	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(JsonFilePath, DataObjects)) return {};

	TSet<FString> Seen;
	TArray<FString> AllRefs;

	for (const TSharedPtr<FJsonValue>& Obj : DataObjects) {
		AllRefs.Append(CollectGameReferences(Obj, Seen));
	}

	TArray<FString> Missing;
	for (const FString& Ref : AllRefs) {
		const FString ExpectedPath = ExportPathForGameRef(Ref, JsonFilePath);
		if (ExpectedPath.IsEmpty() || !FPaths::FileExists(ExpectedPath)) {
			Missing.Add(Ref);
		}
	}

	return Missing;
}

/* Follows /Game/ references out from JsonFilePath to their expected on-disk export JSON
 * (via ExportPathForGameRef) and recurses, purely reading files and strings - no UPackage or
 * UObject is ever touched. If the walk loops back to a package already on the current path,
 * every package on that loop is a genuine circular reference: importing one eventually needs
 * the other, which is exactly the shape that makes UE's loader fall back to a partially-loaded
 * package (see "Flushing package X recursively from another package X ... partially loaded to
 * avoid a deadlock" in the log) if something forces a synchronous load mid-cycle. Detecting
 * this ahead of time from the JSON means we never have to find out about it that way. */
inline TSet<FString> FindCircularPackageReferences(const FString& RootJsonFilePath) {
	TSet<FString> InCycle;
	TArray<FString> Stack;
	TSet<FString> OnStack;
	TSet<FString> Visited;

	TFunction<void(const FString&, int32)> Walk = [&](const FString& CurrentFile, int32 Depth) {
		if (Depth > GMaxDependencyRecursionDepth || CurrentFile.IsEmpty()) return;

		const FString PackagePath = GetPackagePathFromJson(CurrentFile);
		if (PackagePath.IsEmpty() || Visited.Contains(PackagePath)) return;

		Visited.Add(PackagePath);
		Stack.Add(PackagePath);
		OnStack.Add(PackagePath);

		TArray<TSharedPtr<FJsonValue>> DataObjects;
		if (DeserializeJSON(CurrentFile, DataObjects)) {
			TSet<FString> Seen;
			TArray<FString> References;
			for (const TSharedPtr<FJsonValue>& Obj : DataObjects) {
				References.Append(CollectGameReferences(Obj, Seen));
			}

			for (const FString& Ref : References) {
				if (Ref.Equals(PackagePath, ESearchCase::IgnoreCase)) continue;

				if (OnStack.Contains(Ref)) {
					/* Back-edge: everything from Ref's position to the top of the stack is the cycle */
					const int32 StartIndex = Stack.IndexOfByKey(Ref);
					if (StartIndex != INDEX_NONE) {
						for (int32 i = StartIndex; i < Stack.Num(); ++i) {
							InCycle.Add(Stack[i]);
						}
					}
					continue;
				}

				const FString RefFile = ExportPathForGameRef(Ref, CurrentFile);
				if (!RefFile.IsEmpty() && FPaths::FileExists(RefFile)) {
					Walk(RefFile, Depth + 1);
				}
			}
		}

		Stack.Pop();
		OnStack.Remove(PackagePath);
	};

	Walk(RootJsonFilePath, 0);
	return InCycle;
}

/* Shared across every TU that includes this header - a function-local static in an inline
 * function has one definition program-wide, so this is a single registry regardless of how
 * many .cpp files call into it. Populated by NotifyMissingDependencies below, consulted by
 * CreateAssetPackageSafe to decide whether it's safe to FullyLoad a package. */
inline TSet<FString>& GetKnownCircularPackages() {
	static TSet<FString> Packages;
	return Packages;
}

inline bool IsKnownCircularPackage(const FString& PackagePath) {
	return GetKnownCircularPackages().Contains(PackagePath);
}

/* Shows a notification listing missing and/or circular dependencies for the given JSON file.
 * Returns true if either was found (and a notification was shown). */
inline bool NotifyMissingDependencies(const FString& JsonFilePath) {
	/* First validate the file's own hierarchy */
	FString HierarchyError;
	if (!ValidateExportHierarchy(JsonFilePath, HierarchyError)) {
		AppendNotification(
			FText::FromString("Wrong Export Hierarchy"),
			FText::FromString(HierarchyError),
			10.0f,
			SNotificationItem::CS_Fail,
			true,
			500.0f
		);
		return true;
	}

	const TArray<FString> Missing = FindMissingDependencies(JsonFilePath);

	const TSet<FString> Circular = FindCircularPackageReferences(JsonFilePath);
	if (Circular.Num() > 0) {
		GetKnownCircularPackages().Append(Circular);

		FString CircularNames;
		int32 CircularShown = 0;
		for (const FString& Pkg : Circular) {
			if (CircularShown >= 10) break;
			FString Leaf = FPaths::GetBaseFilename(Pkg);
			if (CircularShown > 0) CircularNames += TEXT(", ");
			CircularNames += Leaf;
			++CircularShown;
		}

		AppendNotification(
			FText::FromString(TEXT("Circular Reference Detected")),
			FText::FromString(FString::Printf(
				TEXT("%s reference each other. They'll import without automatic reuse-detection to avoid an editor crash - worth checking them after import finishes."),
				*CircularNames
			)),
			8.0f,
			SNotificationItem::CS_Fail,
			true,
			450.0f
		);
	}

	if (Missing.Num() == 0) return Circular.Num() > 0;

	FString Names;
	for (int32 i = 0; i < Missing.Num() && i < 10; ++i) {
		FString Leaf = FPaths::GetBaseFilename(Missing[i]);
		Leaf.RemoveFromStart(TEXT("/Game/"));
		if (i > 0) Names += TEXT(", ");
		Names += Leaf;
	}
	if (Missing.Num() > 10) {
		Names += FString::Printf(TEXT(" ...and %d more"), Missing.Num() - 10);
	}

	AppendNotification(
		FText::FromString(FString::Printf(TEXT("Missing %d Dependencies"), Missing.Num())),
		FText::FromString(Names),
		8.0f,
		SNotificationItem::CS_Fail,
		true,
		450.0f
	);

	return true;
}