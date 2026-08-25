/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class FBlueprintStubFactory {
public:
	static TArray<FString> ResolveDependencies(const FString& JsonFilePath);
	static bool IsStubImport(const FString& FilePath);
	static void UnregisterStubImport(const FString& FilePath);
	static void ClearStubImports();

private:
	static TSet<FString> StubFiles;
};
