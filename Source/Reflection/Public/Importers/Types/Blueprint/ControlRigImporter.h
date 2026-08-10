/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

#if ENGINE_UE5
#include "ControlRigDeveloper/Public/ControlRigBlueprintLegacy.h"
#include "ControlRig/Public/ControlRigBlueprintGeneratedClass.h"
#endif

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the animation blueprint type used to come in from */
#if UE4_25_BELOW
class UControlRigBlueprint;
#endif

class IControlRigImporter final : public IImporter {
public:
	virtual bool Import() override;

	/* Shell phase (FAssetDependencyRegistry::CreateShells) builds every Internal entry's
	 * UObject before any export is populated, so a circular reference never has to load a
	 * package this batch is still building. Without an override here the base IImporter
	 * implementation returns null for a null argument and no shell is ever created. */
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;

private:
	/* Using [AssetData] that is filled with control rig data, create new asset, and the Outer being [Package], then add it to the [Container] */
	UControlRigBlueprint* CreateControlRigBlueprint(UClass* ParentClass);

	/* Deserialize Hierarchy and VM from exports */
	void DeserializeHierarchyAndVM(UControlRigBlueprint* InControlRigBlueprint);

	/* Deserialize the RigHierarchy from the Elements array in the DynamicHierarchy export */
	void DeserializeHierarchy(UControlRigBlueprint* InControlRigBlueprint);

	/* Deserialize the URigVM from the VM export into the CDO's existing VM */
	void DeserializeVM(UControlRigBlueprint* InControlRigBlueprint);

	/* Create graph model with entry nodes from bytecode entries so functions appear in editor */
	void DeserializeGraph(UControlRigBlueprint* InControlRigBlueprint);

	/* Read LiteralMemory PropertyValues from JSON to extract constant values for pin defaults */
	void DeserializeLiteralMemory(UControlRigBlueprint* InControlRigBlueprint, TMap<int32, FString>& OutRegisterToValue);

	/* Read DefaultWorkMemory PropertyValues from JSON to extract initial values for Work register pins */
	void DeserializeWorkMemory(UControlRigBlueprint* InControlRigBlueprint, TMap<int32, FString>& OutRegisterToValue);

	/* Recreates the variables the control rig declares, returns how many were added */
	int32 ConstructVariables();

protected:
	UControlRigBlueprint* ControlRigBlueprint = nullptr;
};

REGISTER_IMPORTER(IControlRigImporter, (TArray<FString>{ 
	TEXT("RigVMBlueprintGeneratedClass"),
	TEXT("ControlRigBlueprintGeneratedClass")
}), TEXT("ControlRig"));