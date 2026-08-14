/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"
#include "BlueprintBytecodeImporter.h"

#include "KismetCompilerModule.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if ENGINE_UE5
#include "MVVM/ViewModels/ObjectBindingModel.h"
#endif

#include "Engine/SCS_Node.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Utilities/SehHelpers.h"

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	UClass* Class = GetAssetClass();
	
	if (!Class) {
		AppendNotification(
			FText::FromString("Failed to Resolve Parent Class"),
			FText::FromString("The Blueprint's parent class could not be found or loaded. Verify that the class is defined and available when reflecting."),
			2.0f,
			SNotificationItem::CS_Fail,
			true,
			350.0f
		);
		
		return nullptr;
	}
	
	/* Find the blueprint class and generated class */
	UClass* BlueprintClass = nullptr, *GeneratedClass = nullptr;
	
	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>
		("KismetCompiler")
			.GetBlueprintTypesForClass(
				Class,
				BlueprintClass,
				GeneratedClass
			);

	/* Propagate blueprint defaults if it already exists. GetPackage() was already fully loaded by
	 * CreateAssetPackageSafe just before this runs, so anything on disk is already resident in memory -
	 * FindObject (not LoadObject) avoids re-entering the loader for a package still mid-load, which
	 * would trigger a recursive partial load. */
	if (const UBlueprint* ExistingBlueprint = FindObject<UBlueprint>(GetPackage(), *GetAssetName())) {
		UBlueprintGeneratedClass* BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ExistingBlueprint->GeneratedClass);
		FBlueprintEditorUtils::PropagateParentBlueprintDefaults(BlueprintGeneratedClass);

		/* Return GeneratedClass instead of UBlueprint* */
		return IImporter::CreateAsset(BlueprintGeneratedClass);
	}

	const UBlueprint* CreatedBlueprint = FKismetEditorUtilities::CreateBlueprint(
		Class,
		GetPackage(),
		FName(*GetAssetName()),
		GetBlueprintType(Class),
		BlueprintClass,
		GeneratedClass
	);

	if (!CreatedBlueprint) return nullptr;

	/* Return GeneratedClass instead of UBlueprint* */
	return IImporter::CreateAsset(CreatedBlueprint->GeneratedClass);
}

bool IBlueprintImporter::Import() {
	const UBlueprintGeneratedClass* BlueprintGeneratedClass = Create<UBlueprintGeneratedClass>();
	if (!BlueprintGeneratedClass) return false;

	/* Update Blueprint Reference for sub functions */
	Blueprint = UBlueprint::GetBlueprintFromClass(BlueprintGeneratedClass);
	if (!Blueprint) return false;

	/* Deserialize Generated Class (blueprint defaults) */
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	/* A blueprint with no class default object export has nothing to deserialize defaults from,
	 * and writing to what the lookup handed back would land on the shared empty export */
	if (ClassDefaultObjectExport->IsJsonInvalid()) return false;

	ClassDefaultObjectExport->Object = GeneratedClass;

	/* The variables have to exist before their defaults can land anywhere. A recreated blueprint
	 * only has what its parent class gave it, so any property the blueprint declared itself is
	 * missing, and deserializing the class default object over it would drop those values on the
	 * floor without complaining. */
	if (ConstructVariables() > 0) {
		/* Adding a variable only touches the blueprint, the generated class grows the property
		 * when it recompiles, and the default object below is the one that comes out of that */
		CompileBlueprintSafe(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!GeneratedClass) return false;

		ClassDefaultObjectExport->Object = GeneratedClass;
	}

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

	/* Experimental (for now) spawning */
	GetObjectSerializer()->bUseExperimentalSpawning = true;

	ConstructScript();
	ConstructWidgetTree();
	ProcessBytecode();

	return OnAssetCreation(Blueprint);
}

int32 IBlueprintImporter::ConstructVariables() {
	const TArray<TSharedPtr<FJsonValue>>* ChildProperties;

	/* A blueprint that declared nothing of its own has no ChildProperties at all */
	if (!GetAssetExport()->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) {
		return 0;
	}

	return FBlueprintVariables::Construct(Blueprint, *ChildProperties);
}

void IBlueprintImporter::ConstructScript() const {
	if (!GetAssetDataAsValue().Has("SimpleConstructionScript")) return;
	
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);

	/* Destroy Construction Script */
	if (USimpleConstructionScript* PreviousSimpleConstructionScript = GeneratedClass->SimpleConstructionScript; PreviousSimpleConstructionScript != nullptr) {
		for (USCS_Node* Node : PreviousSimpleConstructionScript->GetAllNodes()) {
			MoveToTransientPackageAndRename(Node->ComponentTemplate);
		}
		
		MoveToTransientPackagesAndRename({
			PreviousSimpleConstructionScript,
			Blueprint->SimpleConstructionScript
		});
	}

	FUObjectExport* Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("SimpleConstructionScript"));

	/* Spawn the new Construction Script */
	USimpleConstructionScript* SimpleConstructionScript =
		Cast<USimpleConstructionScript>(
			GetObjectSerializer()->SpawnExport(Export)
		);

	/* Update SimpleConstructionScript on the Blueprint */
	Blueprint->SimpleConstructionScript = SimpleConstructionScript;
	GeneratedClass->SimpleConstructionScript = SimpleConstructionScript;

	/* Engine Ensures */
	SimpleConstructionScript->FixupRootNodeParentReferences();
	SimpleConstructionScript->ValidateSceneRootNodes();
}

class UWidgetTreeAccessor final : public UWidgetTree {
public:

#if ENGINE_UE5
	TArray<TObjectPtr<UWidget>> GetWidgets() {
#else
	TArray<UWidget*> GetWidgets() {
#endif
		return AllWidgets;
	}
};

void IBlueprintImporter::ConstructWidgetTree() {
	if (!GetAssetDataAsValue().Has("WidgetTree")) return;

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
	
	for (UWidget* Widget : Cast<UWidgetTreeAccessor>(WidgetBlueprint->WidgetTree)->GetWidgets()) {
		MoveToTransientPackageAndRename(Widget);
	}

	WidgetBlueprint->WidgetTree->PostLoad();

	for (UWidgetAnimation* WidgetAnimation : WidgetBlueprint->Animations) {
		MoveToTransientPackageAndRename(WidgetAnimation);
	}

	WidgetBlueprint->Animations.Empty();
	
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	/* Same as above: the empty export is shared, so a miss here must not be written to */
	if (ClassDefaultObjectExport->IsJsonValid()) {
		ClassDefaultObjectExport->Object = WidgetBlueprint;
	}

	SetAsset(WidgetBlueprint);

	MoveToTransientPackageAndRename(WidgetBlueprint->WidgetTree->RootWidget);
	WidgetBlueprint->WidgetTree->RootWidget = nullptr;

	FUObjectExport* Export;

	if (GetAssetDataAsValue().Has("TemplateAsset")) {
		FUObjectExport* TemplateAsset = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("TemplateAsset"));
		Export = GetContainer()->GetExportByObjectPath(TemplateAsset->GetPropertiesAsValue().GetObject("WidgetTree"));
	} else {
		Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("WidgetTree"));
	}
	
	Export->Object = WidgetBlueprint->WidgetTree;
	GetObjectSerializer()->SpawnExport(Export, true);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	GetContainer()->ExportsLoop(GetAssetDataAsValue().GetArray("Animations"), [this, WidgetBlueprint](FUObjectExport* DirectExport) {
		if (UObject* Object = GetObjectSerializer()->SpawnExport(DirectExport)) {
			UWidgetAnimation* WidgetAnimation = Cast<UWidgetAnimation>(Object);
		
			WidgetBlueprint->Animations.Add(WidgetAnimation);

			for (int32 Index = 0; Index < WidgetAnimation->MovieScene->GetPossessableCount(); ++Index) {
				FMovieScenePossessable& Possessable = WidgetAnimation->MovieScene->GetPossessable(Index);

				TArray<UWidget*> Widgets;
				WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);

				for (UWidget* Widget : Widgets) {
					if (Widget->GetName() == Possessable.GetName()) {
#if ENGINE_UE5
						Possessable.SetPossessedObjectClass(Widget->GetClass());
#endif
					}
				}
			}
			
			for (const FMovieSceneBinding& Binding : WidgetAnimation->MovieScene->GetBindings()) {
				for (UMovieSceneTrack* Track : Binding.GetTracks()) {
					Track->Modify();
					Track->MarkAsChanged();

					if (UMovieSceneWidgetMaterialTrack* MaterialTrack = Cast<UMovieSceneWidgetMaterialTrack>(Track)) {
						MaterialTrack->SetDisplayName(FText::FromString(MaterialTrack->GetBrushPropertyNamePath()[0].ToString()));
					}
				}
			}
		}
	});
}

void IBlueprintImporter::ProcessBytecode() const {
	UE_LOG(LogTemp, Log, TEXT("Processing bytecode..."));

	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("No generated class found"));
		return;
	}

	// Get all exports from the container
	FUObjectExportContainer* Container = GetContainer();
	if (!Container)
	{
		UE_LOG(LogTemp, Warning, TEXT("No container found"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>& JsonObjects = Container->JsonObjects;
	if (JsonObjects.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No JSON objects found"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("Found %d JSON exports"), JsonObjects.Num());

	// Create bytecode importer
	FBlueprintBytecodeImporter BytecodeImporter(Blueprint, GeneratedClass);

	// Process dynamic bindings first (from class properties)
	FUObjectJsonValueExport AssetData = GetAssetDataAsValue();
	if (AssetData.JsonObject.IsValid() && AssetData.JsonObject->HasField(TEXT("Properties")))
	{
		const TSharedPtr<FJsonObject>& Properties = AssetData.JsonObject->GetObjectField(TEXT("Properties"));
		BytecodeImporter.ProcessDynamicBindings(Properties, JsonObjects);
	}

	// Process functions
	BytecodeImporter.ProcessFunctions(JsonObjects);

	// Mark blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	UE_LOG(LogTemp, Log, TEXT("Bytecode processing completed"));
}