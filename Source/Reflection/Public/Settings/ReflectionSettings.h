/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

/* Settings Substructures */
#include "Types/AnimationBlueprintSettings.h"
#include "Types/MaterialSettings.h"
#include "Types/TextureSettings.h"
#include "Redirector.h"

#include "ReflectionSettings.generated.h"

extern FName GReflectionSettingsCategoryName;
extern FName GReflectionInternalName;

USTRUCT()
struct FRSettings
{
	GENERATED_BODY()
public:
	/* Constructor to initialize default values */
	FRSettings() {
		Material = FRMaterialSettings();
		Texture = FRTextureSettings();
		AnimationBlueprint = FRAnimationBlueprintSettings();
	}

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRAnimationBlueprintSettings AnimationBlueprint;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRTextureSettings Texture;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRMaterialSettings Material;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FString ProjectName;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	bool SaveAssets = false;
};

USTRUCT()
struct FRVersioningSettings
{
	GENERATED_BODY()
public:
	/* Disable checking for newer updates of Reflection. */
	UPROPERTY(EditAnywhere, Config, Category = VersioningSettings)
	bool Disable = false;
};

/* Reconstruction Toolkit for Unreal Engine */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig)
class REFLECTION_API UReflectionSettings : public UDeveloperSettings {
	GENERATED_BODY()
public:
	UReflectionSettings();

	/* Overriden to stop the Editor spacing the words between Reflection */
	virtual FText GetSectionText() const override;
	
public:
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRVersioningSettings Versioning;
	
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRSettings AssetSettings;

	/* Optional folder to check for "<MaterialName>.recipe.json" files - full
	 * reconstructed node-formula recipes produced offline by running
	 * material_reconstructor.py against a captured shader bytecode dump for
	 * that material (see Importers/Types/Materials/MaterialFormulaBuilder.h).
	 * When a recipe exists for a material being imported, it takes priority
	 * over FallbackPinMappings/the built-in heuristics for whichever pins it
	 * covers, since it's an actual reconstructed formula rather than a name
	 * guess. Left empty (default), nothing here runs. */
	UPROPERTY(EditAnywhere, Config, Category = Materials)
	FString ReconstructionRecipesDirectory;

	UPROPERTY(EditAnywhere, Config, Category = Redirectors, meta = (TitleProperty = "Name"))
	TArray<FRRedirector> Redirectors;

	/* Retrieves assets from an API and imports references directly into your project. */
	UPROPERTY(Config)
	bool EnableCloudServer = true;

	/* Enables experimental/developing features. Features may not work as intended. */
	UPROPERTY(EditAnywhere, Config, DisplayName = "Experiments", Category = Settings, AdvancedDisplay)
	bool EnableExperiments = false;

	/* Dumps the imported ControlRig graph state as bytecode-style JSON to Saved/Logs/BytecodeDump.json after import. */
	UPROPERTY(EditAnywhere, Config, DisplayName = "ControlRig Import Debug Dump", Category = Settings, AdvancedDisplay)
	bool ControlRigImportDebugDump = false;
};