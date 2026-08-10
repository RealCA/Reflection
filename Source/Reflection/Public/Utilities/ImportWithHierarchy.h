/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/MissingDependencies.h"

/* Recursively collects all /Game/ references from a JSON file, following each dependency's
 * own JSON to find transitive dependencies. Returns a topologically sorted list of absolute
 * file paths from root (no deps) to leaf.
 *
 * If bSkipExistingDeps is true, dependencies whose UE asset already exists in the project
 * are skipped (but the file itself is never skipped — it's always included so it gets
 * re-imported).
 *
 * Safeguards:
 *  - OutScanned prevents re-entering the same file (deduplication + cycle detection)
 *  - Depth parameter enforces GMaxDependencyRecursionDepth to prevent stack overflow
 *  - OutOrdered tracks files already added to the result to prevent duplicate entries
 *  - Missing or unreadable files are skipped with a log warning */
inline TArray<FString> CollectHierarchyImportOrder(
	const FString& JsonFilePath,
	TSet<FString>& OutScanned,
	TSet<FString>& OutOrdered,
	bool bSkipExistingDeps,
	int32 Depth = 0
) {
	TArray<FString> Order;

	const FString Normalized = JsonFilePath.Replace(TEXT("\\"), TEXT("/"));

	/* Cycle / duplicate guard: already processed this file */
	if (OutScanned.Contains(Normalized)) return Order;
	OutScanned.Add(Normalized);

	/* Recursion depth guard */
	if (Depth > GMaxDependencyRecursionDepth) {
		UE_LOG(LogReflection, Warning,
			TEXT("ImportWithHierarchy: max recursion depth (%d) reached at \"%s\" — possible circular reference chain."),
			GMaxDependencyRecursionDepth, *Normalized);
		return Order;
	}

	TArray<TSharedPtr<FJsonValue>> DataObjects;
	if (!DeserializeJSON(Normalized, DataObjects)) {
		UE_LOG(LogReflection, Warning,
			TEXT("ImportWithHierarchy: could not read \"%s\" — skipping its dependencies."), *Normalized);
		return Order;
	}

	/* Extract all /Game/ references from this file */
	TSet<FString> Seen;
	TArray<FString> Refs;
	for (const TSharedPtr<FJsonValue>& Obj : DataObjects) {
		Refs.Append(CollectGameReferences(Obj, Seen));
	}

	/* Recurse into each dependency first (depth-first) so parents appear after children */
	for (const FString& Ref : Refs) {
		const FString DepPath = ExportPathForGameRef(Ref, Normalized);
		if (DepPath.IsEmpty()) {
			UE_LOG(LogReflection, Warning,
				TEXT("ImportWithHierarchy: could not resolve export path for \"%s\" referenced by \"%s\"."),
				*Ref, *Normalized);
			continue;
		}

		if (!FPaths::FileExists(DepPath)) {
			/* Missing dependency — already reported by NotifyMissingDependencies, skip silently */
			continue;
		}

		/* If skipping existing, check whether this dependency's UE asset already exists.
		 * Only skip at Depth > 0 — the root file (Depth 0) is always imported. */
		if (bSkipExistingDeps && Depth > 0) {
			const FString DepPackage = GetPackagePathFromJson(DepPath);
			if (!DepPackage.IsEmpty() && AssetExistsInProject(DepPackage)) {
				continue;
			}
		}

		Order.Append(CollectHierarchyImportOrder(DepPath, OutScanned, OutOrdered, bSkipExistingDeps, Depth + 1));
	}

	/* Add this file only if not already in the result (different /Game/ paths could
	 * resolve to the same disk file through redirectors) */
	if (!OutOrdered.Contains(Normalized)) {
		OutOrdered.Add(Normalized);
		Order.Add(Normalized);
	}

	return Order;
}

/* Convenience wrapper: given a root JSON file, returns a topologically sorted import order
 * with no duplicates and cycle protection.
 *
 * If bSkipExistingDeps is true, dependencies that already exist as UE assets in the project
 * are excluded from the result (the root file is always included). */
inline TArray<FString> GetHierarchyImportOrder(const FString& JsonFilePath, bool bSkipExistingDeps = false) {
	TSet<FString> Scanned;
	TSet<FString> Ordered;
	return CollectHierarchyImportOrder(JsonFilePath, Scanned, Ordered, bSkipExistingDeps, 0);
}
