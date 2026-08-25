/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class UBlueprint;
struct FUObjectExportContainer;

/* Shared with IAnimationBlueprintImporter: hollow function/event UFunction shells
 * for stub imports of any blueprint kind (BP/Widget/Anim). Dependents resolve
 * casts and calls against these; no graph body content is created. */
void ConstructBlueprintStubFunctions(UBlueprint* InBlueprint, FUObjectExportContainer* Container);

/* Shared bytecode pass: event graph + function reconstruction from the container's
 * Function exports. Used by IBlueprintImporter::ProcessBytecode and by the anim
 * importer for the event graph + functions its AnimGraph-only path historically
 * skipped. ClassExportJson is the root generated-class export (read for
 * DynamicBindingObjects). */
void RunBlueprintBytecodePass(UBlueprint* InBlueprint, const TSharedPtr<FJsonObject>& ClassExportJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

class IBlueprintImporter final : public IImporter {
protected:
	UBlueprint* Blueprint = nullptr;
	
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	
	virtual bool Import() override;
	
protected:
	/* Recreates the variables the blueprint declares, returns how many were added.
	 * Not const, reading the export off the container isn't. */
	int32 ConstructVariables();

	/* Handles SimpleConstructionScript, the component layout for Actor blueprints */
	void ConstructScript() const;

	/* Handles WidgetTree, the UI layout for Widget blueprints */
	void ConstructWidgetTree();

	/* Handles bytecode processing for event graphs and functions */
	void ProcessBytecode() const;

	/* For stub imports: creates UFunction stubs on the class and event nodes
	 * in the event graph. No graph body content is created. */
	void ConstructStubFunctions();
};

REGISTER_IMPORTER(IBlueprintImporter, (TArray<FString>{ 
	TEXT("BlueprintGeneratedClass"),
	TEXT("WidgetBlueprintGeneratedClass")
}), TEXT("Blueprints"));