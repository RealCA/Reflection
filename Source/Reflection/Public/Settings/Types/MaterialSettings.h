/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MaterialSettings.generated.h"

/* Which material input a fallback texture parameter should be wired into.
 * Covers the pins that take a texture sample's RGB/A output directly; anything
 * more involved (custom blends, layered overlays) is left for the user to wire
 * by hand in the material editor. */
UENUM()
enum class EReflectionMaterialPin : uint8 {
	BaseColor,
	Metallic,
	Specular,
	Roughness,
	EmissiveColor,
	Opacity,
	OpacityMask,
	Normal,
	AmbientOcclusion
};

/* A user-authored rule teaching the fallback graph builder what a parameter name
 * means, for names the built-in Color/Opacity heuristics don't recognize
 * (e.g. "Specular", "OverLay02", "Alpha Whole"). Checked before the built-in
 * heuristics, in list order, so an earlier rule wins on a name that matches more
 * than one entry. */
USTRUCT()
struct FRMaterialFallbackPinMapping {
	GENERATED_BODY()
public:
	/* Case-insensitive substrings to match against a texture parameter's name.
	 * Any one match is enough. Example: "specular", "spec" */
	UPROPERTY(EditAnywhere, Config, Category = MaterialFallbackPinMapping)
	TArray<FString> NameContains;

	/* Which material input to wire the matching parameter into */
	UPROPERTY(EditAnywhere, Config, Category = MaterialFallbackPinMapping)
	EReflectionMaterialPin Pin = EReflectionMaterialPin::BaseColor;
};

/* Settings for materials */
USTRUCT()
struct FRMaterialSettings {
	GENERATED_BODY()
public:
	/* Creates stub versions of materials that have parameters (for Modding) */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool Stubs = false;

	/* Cooked materials never serialize their editor Expressions graph. When enabled,
	 * graph-less imports get a synthesized fallback graph rebuilt from the compiled
	 * output (UniformExpressionSet) so parameters stay editable and overridable. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool FallbackGraph = true;

	/* Name -> pin overrides consulted before the built-in Color/Opacity name
	 * heuristics when wiring a fallback graph's texture parameters. Seeded with
	 * standard PBR naming conventions so common cases work out of the box; add an
	 * entry for any project-specific naming (e.g. "OverLay02") Reflection can't
	 * already infer. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings, meta = (TitleProperty = "Pin"))
	TArray<FRMaterialFallbackPinMapping> FallbackPinMappings = {
		FRMaterialFallbackPinMapping{ { TEXT("specular") }, EReflectionMaterialPin::Specular },
		FRMaterialFallbackPinMapping{ { TEXT("roughness") }, EReflectionMaterialPin::Roughness },
		FRMaterialFallbackPinMapping{ { TEXT("metallic") }, EReflectionMaterialPin::Metallic },
		FRMaterialFallbackPinMapping{ { TEXT("normal") }, EReflectionMaterialPin::Normal },
		FRMaterialFallbackPinMapping{ { TEXT("emissive") }, EReflectionMaterialPin::EmissiveColor },
		FRMaterialFallbackPinMapping{ { TEXT("ambient occlusion"), TEXT("ambientocclusion") }, EReflectionMaterialPin::AmbientOcclusion }
	};

	/* Optional folder to check for "<MaterialName>.recipe.json" files - full
	 * reconstructed node-formula recipes produced offline by running
	 * material_reconstructor.py against a captured shader bytecode dump for
	 * that material (see Importers/Types/Materials/MaterialFormulaBuilder.h).
	 * When a recipe exists for a material being imported, it takes priority
	 * over FallbackPinMappings/the built-in heuristics for whichever pins it
	 * covers, since it's an actual reconstructed formula rather than a name
	 * guess. Left empty (default), nothing here runs. */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	FString ReconstructionRecipesDirectory;
};