/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Settings/ReflectionSettings.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"
#include "Importers/Constructor/DependencyRegistry.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the blueprint function library type used to come in from */
#if UE4_25_BELOW
#include "Kismet/BlueprintFunctionLibrary.h"
#endif

inline TSubclassOf<UObject> LoadClassFromPath(const FString& ObjectName, const FString& ObjectPath) {
	const FString FullPath = ObjectPath + TEXT(".") + ObjectName;

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject)) {
			return LoadedClass;
		}
	}

	return nullptr;
}

inline TSubclassOf<UObject> LoadBlueprintClass(FString& ObjectPath) {
	const UReflectionSettings* Settings = GetSettings();
	
	if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Content"), TEXT("/Game"));
	}
	
	FString FullPath = ObjectPath; 
	if (FullPath.EndsWith(TEXT(".1"))) {
		FullPath = FullPath.LeftChop(2);
	}

	/* A parent Blueprint reachable through the registry is either mid-import in this same batch
	 * or was already shelled ahead of it - either way, LoadObjectByPath on that same package
	 * would re-enter the loader for something this batch is already responsible for, which is
	 * the exact shape of the recursive-load bug this registry exists to avoid. */
	if (FAssetEntry* PlannedEntry = FAssetDependencyRegistry::Get().FindByPackagePath(FullPath)) {
		if (UClass* PlannedClass = Cast<UClass>(PlannedEntry->CreatedObject)) {
			return PlannedClass;
		}
	}

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		const UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject);
		
		if (LoadedBlueprint && LoadedBlueprint->GeneratedClass) {
			return LoadedBlueprint->GeneratedClass;
		}
	}

	return nullptr;
}

inline UClass* LoadClass(const TSharedPtr<FJsonObject>& SuperStruct) {
	const FString ObjectName = SuperStruct->GetStringField(TEXT("ObjectName")).Replace(TEXT("Class'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
	FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));

	/* It's a C++ class if it has Script in it */
	if (ObjectPath.Contains("/Script/")) {
		return LoadClassFromPath(ObjectName, ObjectPath);
	}
	
	ObjectPath.Split(".", &ObjectPath, nullptr);

	return LoadBlueprintClass(ObjectPath);
}

/* Shell-phase parent resolution. A blueprint shell must never load anything from disk - the
 * batch is still building packages, and a /Game/ parent pulled in here would re-enter the
 * loader mid-creation (the recursive-flush crash). Native /Script/ parents are always safe;
 * a /Game/ parent is used only when this batch has already shelled it (LoadClass then resolves
 * against memory). Anything else falls back to the caller's native default - the shell's only
 * job is to exist at the right object path, and the populate phase re-creates the blueprint
 * with its real parent once that parent has been shelled. */
inline UClass* LoadShellParentClass(const TSharedPtr<FJsonObject>& SuperStruct, UClass* NativeFallback) {
	if (!SuperStruct.IsValid() || !SuperStruct->HasField(TEXT("ObjectPath"))) {
		return NativeFallback;
	}

	const FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));
	if (!ObjectPath.Contains(TEXT("/Game/"))) {
		return LoadClass(SuperStruct);
	}

	FString ParentPackage = ObjectPath;
	ParentPackage.Split(TEXT("."), &ParentPackage, nullptr);

	const FAssetEntry* ParentEntry = FAssetDependencyRegistry::Get().FindByPackagePath(ParentPackage);
	if (ParentEntry != nullptr && ParentEntry->bShellCreated) {
		return LoadClass(SuperStruct);
	}

	return NativeFallback;
}

inline TSharedPtr<FJsonObject> GetSuperStructJsonObject(const TSharedPtr<FJsonObject>& JsonObject) {
	if (JsonObject->HasField(TEXT("Next"))) {
		return JsonObject->GetObjectField(TEXT("Next"));
	}
	
	return JsonObject->GetObjectField(TEXT("SuperStruct"));
}

inline EBlueprintType GetBlueprintType(const UClass* Class) {
	EBlueprintType BlueprintType = BPTYPE_Normal;

	if (Class->HasAnyClassFlags(CLASS_Const)) {
		BlueprintType = BPTYPE_Const;
	}
	
	if (Class == UBlueprintFunctionLibrary::StaticClass()) {
		BlueprintType = BPTYPE_FunctionLibrary;
	}
	
	if (Class == UInterface::StaticClass()) {
		BlueprintType = BPTYPE_Interface;
	}
	
	return BlueprintType;
}

inline FUObjectExport* GetClassDefaultObject(FUObjectExportContainer* AssetContainer, const FUObjectJsonValueExport& JsonObject) {
	FUObjectExport* Export = AssetContainer->GetExportByObjectPath(JsonObject.GetObject("ClassDefaultObject"));
	if (!Export->IsJsonValid()) {
		Export = AssetContainer->GetExportStartingWith("Name", "Default__");
	}

	return Export;
}