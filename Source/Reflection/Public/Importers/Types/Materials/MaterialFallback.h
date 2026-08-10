/* Copyright Reflection Contributors 2024-2026
 *
 * Fallback expression-graph synthesis for graph-less cooked UMaterial imports.
 *
 * Cooked UMaterial packages never serialize their editor Expressions node graph,
 * so without this fallback Reflection would import them as blank shells. This
 * mirrors Tools/batch_material_graph_fix.py: parameters are recovered from
 * LoadedMaterialResources[].Content.MaterialCompilationOutput.UniformExpressionSet
 * and rebuilt as real parameter nodes, so the material keeps its editable
 * parameters even when the preprocessor was not run on the export batch. */

#pragma once

#if ENGINE_UE5
#include "Importers/Types/Materials/MaterialImporter.h"

#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/SettingsAccess.h"

namespace Reflection::MaterialFallback {

enum class EKind {
	Scalar,
	Vector,
	Unknown
};

/* Color / opacity name heuristics, kept in sync with the Python tool */
inline bool IsColorish(const FString& Name) {
	const FString Lower = Name.ToLower();
	static const TCHAR* Tokens[] = { TEXT("color"), TEXT("albedo"), TEXT("diffuse"), TEXT("base"), TEXT("diff") };
	for (const TCHAR* Token : Tokens) {
		if (Lower.Contains(Token)) return true;
	}
	return false;
}

inline bool IsOpacity(const FString& Name) {
	const FString Lower = Name.ToLower();
	static const TCHAR* Tokens[] = { TEXT("alpha"), TEXT("mask"), TEXT("opacity"), TEXT("opac"), TEXT("ao") };
	for (const TCHAR* Token : Tokens) {
		if (Lower.Contains(Token)) return true;
	}
	return false;
}

/* Sampler type for a created texture node. Prefer the engine's own classification
 * from the loaded texture's compression settings; fall back to name heuristics so
 * a not-yet-loaded placeholder doesn't pick SAMPLERTYPE_Color for a normal map
 * (which makes the material fail to compile with a "Default Material will be used"
 * warning and spams "Sampler type is X, should be Y" errors). */
inline EMaterialSamplerType SamplerTypeForTexture(const UTexture* Texture, const FString& Name) {
	if (Texture != nullptr) {
		return UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture);
	}

	const FString Lower = Name.ToLower();
	if (Lower.Contains(TEXT("normal")) || Lower.EndsWith(TEXT("_n"))) return SAMPLERTYPE_Normal;
	if (Lower.Contains(TEXT("linear")) || Lower.Contains(TEXT("rough")) || Lower.Contains(TEXT("metallic"))
		|| Lower.Contains(TEXT("occlusion")) || Lower.Contains(TEXT("height")) || Lower.Contains(TEXT("mask"))
		|| Lower.Contains(TEXT("opacity")) || Lower.Contains(TEXT("alpha"))) return SAMPLERTYPE_LinearColor;
	return SAMPLERTYPE_Color;
}

/* Pull the parameter name out of the several entry shapes FModel uses */
inline FString ParameterNameFromEntry(const TSharedPtr<FJsonObject>& Entry) {
	if (!Entry.IsValid()) return FString();

	FString Name;
	if (Entry->TryGetStringField(TEXT("Name"), Name)) return Name;
	if (Entry->TryGetStringField(TEXT("ParameterName"), Name)) return Name;

	const TSharedPtr<FJsonObject>* Info;
	if (Entry->TryGetObjectField(TEXT("ParameterInfo"), Info)) {
		if ((*Info)->TryGetStringField(TEXT("Name"), Name)) return Name;
	}
	return FString();
}

/* 'Scalar'/'Vector' from the loose UniformNumericParameters Type/ParameterType tags */
inline EKind NumericKind(const TSharedPtr<FJsonObject>& Entry) {
	if (!Entry.IsValid()) return EKind::Unknown;
	FString Type;
	if (!Entry->TryGetStringField(TEXT("Type"), Type)) {
		if (!Entry->TryGetStringField(TEXT("ParameterType"), Type)) return EKind::Unknown;
	}

	const FString Lower = Type.ToLower();
	if (Lower.Contains(TEXT("scalar")) || Lower.Contains(TEXT("float"))) return EKind::Scalar;
	if (Lower.Contains(TEXT("vector")) || Lower.Contains(TEXT("linear")) || Lower.Contains(TEXT("float4"))) return EKind::Vector;
	return EKind::Unknown;
}

/* First non-empty array among the candidate field names */
inline TArray<TSharedPtr<FJsonValue>> FirstArray(const TSharedPtr<FJsonObject>& Uniform, std::initializer_list<const TCHAR*> Keys) {
	for (const TCHAR* Key : Keys) {
		if (Uniform->HasTypedField<EJson::Array>(Key)) {
			return Uniform->GetArrayField(Key);
		}
	}
	return TArray<TSharedPtr<FJsonValue>>();
}

/* Parallel *ParameterValues array entry, falling back to inline Value fields */
inline TSharedPtr<FJsonValue> ParallelValue(const TSharedPtr<FJsonObject>& Entry, const TArray<TSharedPtr<FJsonValue>>& Values, int32 Index) {
	if (Values.IsValidIndex(Index) && Values[Index].IsValid() && !Values[Index]->IsNull()) {
		return Values[Index];
	}
	for (const TCHAR* Key : { TEXT("Value"), TEXT("DefaultValue"), TEXT("ParameterValue") }) {
		if (Entry->HasField(Key)) {
			return Entry->TryGetField(Key);
		}
	}
	return nullptr;
}

/* Texture path out of a TextureParameterValues entry */
inline FString TexturePathFromValue(const TSharedPtr<FJsonValue>& Value) {
	if (!Value.IsValid() || !Value->AsObject().IsValid()) return FString();

	const TSharedPtr<FJsonObject> Obj = Value->AsObject();
	FString Path;
	if (Obj->TryGetStringField(TEXT("ObjectPath"), Path) && !Path.IsEmpty()) return Path;
	if (Obj->TryGetStringField(TEXT("AssetPathName"), Path) && !Path.IsEmpty()) return Path;
	return FString();
}

/* FModel ObjectPaths look like "/Game/Path/Asset.0". Convert them to the loadable
 * "/Game/Path/Asset.Asset" form (mirrors Export.cpp / Importer.h conventions). */
inline FString MakeLoadablePath(const FString& RawPath) {
	FString Path = RawPath;
	FString Left, Right;
	if (Path.Split(TEXT("."), &Left, &Right, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
		bool bNumericIndex = !Right.IsEmpty();
		for (const TCHAR C : Right) {
			if (!FChar::IsDigit(C)) { bNumericIndex = false; break; }
		}
		if (bNumericIndex) Path = Left;
	}

	FString LastSegment;
	Path.Split(TEXT("/"), &Left, &LastSegment, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	FString NamePart = LastSegment;
	NamePart.Split(TEXT("."), &NamePart, nullptr);
	if (NamePart.IsEmpty()) return Path;

	FString CurrentSuffix;
	Path.Split(TEXT("."), &Left, &CurrentSuffix, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (CurrentSuffix != NamePart) {
		Path = Path + TEXT(".") + NamePart;
	}
	return Path;
}

/* The import root is the whole export object (AssetExport->JsonObject); its
 * "Properties" field holds the UObject props. Legacy shapes merged the compiled
 * data into Properties itself, so fall back to it. */
inline TSharedPtr<FJsonObject> ExportedProperties(const TSharedPtr<FJsonObject>& Root) {
	const TSharedPtr<FJsonObject>* Properties;
	if (Root->TryGetObjectField(TEXT("Properties"), Properties)) return *Properties;
	return Root;
}

/* First LoadedMaterialResources entry's Content object. Real FModel dumps keep
 * LoadedMaterialResources at the export top level, each entry wrapping its compiled
 * data in LoadedShaderMap.Content; older shapes nest it under Properties with a
 * direct Content. */
inline TSharedPtr<FJsonObject> FindLoadedContent(const TSharedPtr<FJsonObject>& Root) {
	const TArray<TSharedPtr<FJsonValue>>* LoadedResources = nullptr;
	if (Root->HasTypedField<EJson::Array>(TEXT("LoadedMaterialResources"))) {
		Root->TryGetArrayField(TEXT("LoadedMaterialResources"), LoadedResources);
	}
	else {
		const TSharedPtr<FJsonObject> Properties = ExportedProperties(Root);
		if (Properties.IsValid() && Properties->HasTypedField<EJson::Array>(TEXT("LoadedMaterialResources"))) {
			Properties->TryGetArrayField(TEXT("LoadedMaterialResources"), LoadedResources);
		}
	}
	if (LoadedResources == nullptr || LoadedResources->Num() == 0) return nullptr;

	const TSharedPtr<FJsonObject> Resource = (*LoadedResources)[0]->AsObject();
	if (!Resource.IsValid()) return nullptr;

	const TSharedPtr<FJsonObject>* Content;
	if (Resource->TryGetObjectField(TEXT("Content"), Content)) return *Content;

	const TSharedPtr<FJsonObject>* ShaderMap;
	if (Resource->TryGetObjectField(TEXT("LoadedShaderMap"), ShaderMap)) {
		if ((*ShaderMap)->TryGetObjectField(TEXT("Content"), Content)) return *Content;
	}
	return nullptr;
}

/* Locate the compiled UniformExpressionSet inside the material export */
inline TSharedPtr<FJsonObject> FindUniformExpressionSet(const TSharedPtr<FJsonObject>& Root) {
	const TSharedPtr<FJsonObject> Content = FindLoadedContent(Root);
	if (!Content.IsValid()) return nullptr;

	const TSharedPtr<FJsonObject>* Compilation;
	if (!Content->TryGetObjectField(TEXT("MaterialCompilationOutput"), Compilation)) return nullptr;

	const TSharedPtr<FJsonObject>* Uniform;
	if (!(*Compilation)->TryGetObjectField(TEXT("UniformExpressionSet"), Uniform)) return nullptr;
	return *Uniform;
}

/* Parent MaterialCompilationOutput object: FunctionInfos/PropertyConnectedMask live
 * there in real FModel dumps (and inside UniformExpressionSet in older ones) */
inline TSharedPtr<FJsonObject> FindCompilationOutput(const TSharedPtr<FJsonObject>& Root) {
	const TSharedPtr<FJsonObject> Content = FindLoadedContent(Root);
	if (!Content.IsValid()) return nullptr;

	const TSharedPtr<FJsonObject>* Compilation;
	if (!Content->TryGetObjectField(TEXT("MaterialCompilationOutput"), Compilation)) return nullptr;
	return *Compilation;
}

/* Top-level CachedExpressionData: FunctionInfos/PropertyConnectedMask and a
 * parallel ReferencedTextures list live here in real 5.x dumps. */
inline TSharedPtr<FJsonObject> FindCachedExpressionData(const TSharedPtr<FJsonObject>& Root) {
	const TSharedPtr<FJsonObject>* Cached;
	if (Root->TryGetObjectField(TEXT("CachedExpressionData"), Cached)) return *Cached;
	return nullptr;
}

/* ReferencedTextures: real exports index texture parameters into this list. Found
 * at the export top level, under CachedExpressionData, or in the first
 * LoadedMaterialResources entry's Content. */
inline TArray<TSharedPtr<FJsonValue>> FindReferencedTextures(const TSharedPtr<FJsonObject>& Root) {
	if (Root->HasTypedField<EJson::Array>(TEXT("ReferencedTextures"))) {
		return Root->GetArrayField(TEXT("ReferencedTextures"));
	}

	const TSharedPtr<FJsonObject> Cached = FindCachedExpressionData(Root);
	if (Cached.IsValid() && Cached->HasTypedField<EJson::Array>(TEXT("ReferencedTextures"))) {
		return Cached->GetArrayField(TEXT("ReferencedTextures"));
	}

	const TSharedPtr<FJsonObject> Content = FindLoadedContent(Root);
	if (Content.IsValid() && Content->HasTypedField<EJson::Array>(TEXT("ReferencedTextures"))) {
		return Content->GetArrayField(TEXT("ReferencedTextures"));
	}
	return TArray<TSharedPtr<FJsonValue>>();
}

inline UMaterialExpressionScalarParameter* CreateScalarParameter(UMaterial* Material, UMaterialEditorOnlyData* EditorOnlyData, const FString& Name, const TSharedPtr<FJsonValue>& Default, int32& X, int32& Y, bool& bCreated) {
	UMaterialExpressionScalarParameter* Param = NewObject<UMaterialExpressionScalarParameter>(Material);
	Param->ParameterName = FName(*Name);
	Param->MaterialExpressionEditorX = X;
	Param->MaterialExpressionEditorY = Y;

	double Scalar = 0.0;
	if (Default.IsValid() && Default->TryGetNumber(Scalar)) {
		Param->DefaultValue = static_cast<float>(Scalar);
	}

	EditorOnlyData->ExpressionCollection.Expressions.Add(Param);
	Param->UpdateMaterialExpressionGuid(true, false);
	Material->AddExpressionParameter(Param, Material->EditorParameters);
	X += 150;
	bCreated = true;
	return Param;
}

inline UMaterialExpressionVectorParameter* CreateVectorParameter(UMaterial* Material, UMaterialEditorOnlyData* EditorOnlyData, const FString& Name, const TSharedPtr<FJsonValue>& Default, int32& X, int32& Y, bool& bCreated) {
	UMaterialExpressionVectorParameter* Param = NewObject<UMaterialExpressionVectorParameter>(Material);
	Param->ParameterName = FName(*Name);
	Param->MaterialExpressionEditorX = X;
	Param->MaterialExpressionEditorY = Y;

	const TSharedPtr<FJsonObject> Color = Default.IsValid() ? Default->AsObject() : nullptr;
	if (Color.IsValid()) {
		float R = 0.0f, G = 0.0f, B = 0.0f, A = 1.0f;
		Color->TryGetNumberField(TEXT("R"), R);
		Color->TryGetNumberField(TEXT("G"), G);
		Color->TryGetNumberField(TEXT("B"), B);
		Color->TryGetNumberField(TEXT("A"), A);
		Param->DefaultValue = FLinearColor(R, G, B, A);
	}

	EditorOnlyData->ExpressionCollection.Expressions.Add(Param);
	Param->UpdateMaterialExpressionGuid(true, false);
	Material->AddExpressionParameter(Param, Material->EditorParameters);
	X += 150;
	bCreated = true;
	return Param;
}

inline UMaterialExpressionTextureSampleParameter2D* CreateTextureParameter(UMaterial* Material, UMaterialEditorOnlyData* EditorOnlyData, const FString& Name, const FString& TexturePath, int32& X, int32& Y, bool& bCreated) {
	UMaterialExpressionTextureSampleParameter2D* Param = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
	Param->ParameterName = FName(*Name);
	Param->MaterialExpressionEditorX = X;
	Param->MaterialExpressionEditorY = Y;

	UTexture* LoadedTexture = nullptr;
	if (!TexturePath.IsEmpty()) {
		LoadedTexture = TSoftObjectPtr<UTexture>(FSoftObjectPath(TexturePath)).LoadSynchronous();
		Param->Texture = LoadedTexture;
	}
	Param->SamplerType = SamplerTypeForTexture(LoadedTexture, Name);

	EditorOnlyData->ExpressionCollection.Expressions.Add(Param);
	Param->UpdateMaterialExpressionGuid(true, false);
	Material->AddExpressionParameter(Param, Material->EditorParameters);
	X += 150;
	bCreated = true;
	return Param;
}

/* Points Pin's FExpressionInput at Param's RGBA output. Every pin covered here is
 * a plain FExpressionInput-derived struct on UMaterialEditorOnlyData, so setting
 * .Expression/.OutputIndex is all wiring a texture sample into it takes. */
inline void WireParamIntoPin(UMaterialEditorOnlyData* EditorOnlyData, EReflectionMaterialPin Pin, UMaterialExpressionTextureSampleParameter2D* Param) {
	switch (Pin) {
	case EReflectionMaterialPin::BaseColor: EditorOnlyData->BaseColor.Expression = Param; EditorOnlyData->BaseColor.OutputIndex = 0; break;
	case EReflectionMaterialPin::Metallic: EditorOnlyData->Metallic.Expression = Param; EditorOnlyData->Metallic.OutputIndex = 0; break;
	case EReflectionMaterialPin::Specular: EditorOnlyData->Specular.Expression = Param; EditorOnlyData->Specular.OutputIndex = 0; break;
	case EReflectionMaterialPin::Roughness: EditorOnlyData->Roughness.Expression = Param; EditorOnlyData->Roughness.OutputIndex = 0; break;
	case EReflectionMaterialPin::EmissiveColor: EditorOnlyData->EmissiveColor.Expression = Param; EditorOnlyData->EmissiveColor.OutputIndex = 0; break;
	case EReflectionMaterialPin::Opacity: EditorOnlyData->Opacity.Expression = Param; EditorOnlyData->Opacity.OutputIndex = 0; break;
	case EReflectionMaterialPin::OpacityMask: EditorOnlyData->OpacityMask.Expression = Param; EditorOnlyData->OpacityMask.OutputIndex = 0; break;
	case EReflectionMaterialPin::Normal: EditorOnlyData->Normal.Expression = Param; EditorOnlyData->Normal.OutputIndex = 0; break;
	case EReflectionMaterialPin::AmbientOcclusion: EditorOnlyData->AmbientOcclusion.Expression = Param; EditorOnlyData->AmbientOcclusion.OutputIndex = 0; break;
	}
}

/* First user-configured mapping whose NameContains matches Name, checked before
 * the built-in Color/Opacity heuristics run. Returns false when nothing matches,
 * so the caller can fall through to those heuristics unchanged. */
inline bool FindUserPinMapping(const FString& Name, EReflectionMaterialPin& OutPin) {
	const FString Lower = Name.ToLower();

	for (const FRMaterialFallbackPinMapping& Mapping : GetSettings()->AssetSettings.Material.FallbackPinMappings) {
		for (const FString& Token : Mapping.NameContains) {
			if (!Token.IsEmpty() && Lower.Contains(Token.ToLower())) {
				OutPin = Mapping.Pin;
				return true;
			}
		}
	}

	return false;
}

/* Bit position of each pin within FMaterialCachedExpressionData::PropertyConnectedMask,
 * matching EMaterialProperty's enum order (MaterialShared.h) as of UE 5.7. The mask is
 * compiler-authoritative: it records whether the *source* material actually had an
 * expression feeding that pin, independent of anything this file can infer from names -
 * so it's what should be checked before trusting (or silently accepting a gap in) the
 * name-based wiring below. Pins this file doesn't wire (Anisotropy, Tangent, etc.) are
 * left out since there's nothing useful to do with that information yet. */
inline bool GetMaterialPropertyBit(EReflectionMaterialPin Pin, int32& OutBit) {
	switch (Pin) {
	case EReflectionMaterialPin::EmissiveColor: OutBit = 0; return true;
	case EReflectionMaterialPin::Opacity: OutBit = 1; return true;
	case EReflectionMaterialPin::OpacityMask: OutBit = 2; return true;
	case EReflectionMaterialPin::BaseColor: OutBit = 5; return true;
	case EReflectionMaterialPin::Metallic: OutBit = 6; return true;
	case EReflectionMaterialPin::Specular: OutBit = 7; return true;
	case EReflectionMaterialPin::Roughness: OutBit = 8; return true;
	case EReflectionMaterialPin::Normal: OutBit = 10; return true;
	case EReflectionMaterialPin::AmbientOcclusion: OutBit = 18; return true;
	}
	return false;
}

inline const TCHAR* MaterialPinToString(EReflectionMaterialPin Pin) {
	switch (Pin) {
	case EReflectionMaterialPin::BaseColor: return TEXT("Base Color");
	case EReflectionMaterialPin::Metallic: return TEXT("Metallic");
	case EReflectionMaterialPin::Specular: return TEXT("Specular");
	case EReflectionMaterialPin::Roughness: return TEXT("Roughness");
	case EReflectionMaterialPin::EmissiveColor: return TEXT("Emissive Color");
	case EReflectionMaterialPin::Opacity: return TEXT("Opacity");
	case EReflectionMaterialPin::OpacityMask: return TEXT("Opacity Mask");
	case EReflectionMaterialPin::Normal: return TEXT("Normal");
	case EReflectionMaterialPin::AmbientOcclusion: return TEXT("Ambient Occlusion");
	}
	return TEXT("Unknown");
}

/* Pins the source material actually had wired to an expression, per
 * CachedExpressionData.PropertyConnectedMask. Empty if the field isn't present
 * (older dumps) rather than assumed empty-on-purpose. */
inline TSet<EReflectionMaterialPin> GetSourceConnectedPins(const TSharedPtr<FJsonObject>& Root) {
	TSet<EReflectionMaterialPin> Result;

	const TSharedPtr<FJsonObject> Cached = FindCachedExpressionData(Root);
	if (!Cached.IsValid()) return Result;

	int64 Mask = 0;
	if (!Cached->TryGetNumberField(TEXT("PropertyConnectedMask"), Mask)) return Result;

	for (int32 PinValue = 0; PinValue <= static_cast<int32>(EReflectionMaterialPin::AmbientOcclusion); ++PinValue) {
		const EReflectionMaterialPin Pin = static_cast<EReflectionMaterialPin>(PinValue);
		int32 Bit;
		if (GetMaterialPropertyBit(Pin, Bit) && (Mask & (1ll << Bit)) != 0) {
			Result.Add(Pin);
		}
	}

	return Result;
}

inline bool CreateFallbackGraph(IMaterialImporter* MaterialImporter, const TSharedPtr<FJsonObject>& Root) {
	UMaterial* Material = MaterialImporter->GetTypedAsset<UMaterial>();
	if (!Material) return false;

	UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData();

	const TSharedPtr<FJsonObject> Uniform = FindUniformExpressionSet(Root);
	if (!Uniform.IsValid()) return false;

	const TSharedPtr<FJsonObject> Compilation = FindCompilationOutput(Root);

	bool bCreated = false;
	int32 X = 0;
	int32 Y = 0;

	const TArray<TSharedPtr<FJsonValue>> ScalarValues = FirstArray(Uniform, { TEXT("ScalarParameterValues") });
	const TArray<TSharedPtr<FJsonValue>> VectorValues = FirstArray(Uniform, { TEXT("VectorParameterValues") });
	const TArray<TSharedPtr<FJsonValue>> TextureValues = FirstArray(Uniform, { TEXT("TextureParameterValues") });

	/* Numeric parameters: prefer the loose UniformNumericParameters form, fall back
	 * to the real UniformScalarExpressions / UniformVectorExpressions fields */
	const TArray<TSharedPtr<FJsonValue>> NumericEntries = FirstArray(Uniform, { TEXT("UniformNumericParameters") });
	int32 ScalarIndex = 0;
	int32 VectorIndex = 0;

	if (NumericEntries.Num() > 0) {
		for (const TSharedPtr<FJsonValue>& Value : NumericEntries) {
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const FString Name = ParameterNameFromEntry(Entry);
			if (!Name.IsEmpty()) {
				if (NumericKind(Entry) == EKind::Vector) {
					CreateVectorParameter(Material, EditorOnlyData, Name, ParallelValue(Entry, VectorValues, VectorIndex), X, Y, bCreated);
					VectorIndex++;
				}
				else {
					CreateScalarParameter(Material, EditorOnlyData, Name, ParallelValue(Entry, ScalarValues, ScalarIndex), X, Y, bCreated);
					ScalarIndex++;
				}
			}
		}
	}
	else {
		for (const TSharedPtr<FJsonValue>& Value : Uniform->GetArrayField(TEXT("UniformScalarExpressions"))) {
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const FString Name = ParameterNameFromEntry(Entry);
			if (!Name.IsEmpty() && NumericKind(Entry) != EKind::Vector) {
				CreateScalarParameter(Material, EditorOnlyData, Name, ParallelValue(Entry, ScalarValues, ScalarIndex), X, Y, bCreated);
			}
			ScalarIndex++;
		}
		for (const TSharedPtr<FJsonValue>& Value : Uniform->GetArrayField(TEXT("UniformVectorExpressions"))) {
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			const FString Name = ParameterNameFromEntry(Entry);
			if (!Name.IsEmpty() && NumericKind(Entry) != EKind::Scalar) {
				CreateVectorParameter(Material, EditorOnlyData, Name, ParallelValue(Entry, VectorValues, VectorIndex), X, Y, bCreated);
			}
			VectorIndex++;
		}
	}

	/* Texture parameters. UniformTextureParameters is a per-sampler-type array of
	 * arrays, so flatten before iterating. */
	TArray<UMaterialExpressionTextureSampleParameter2D*> TextureParams;
	const TArray<TSharedPtr<FJsonValue>> TextureBuckets = FirstArray(Uniform, { TEXT("Uniform2DTextureExpressions"), TEXT("UniformTextureExpressions"), TEXT("UniformTextureParameters") });
	TArray<TSharedPtr<FJsonValue>> TextureEntries;
	for (const TSharedPtr<FJsonValue>& Value : TextureBuckets) {
		if (Value->Type == EJson::Array) {
			TextureEntries.Append(Value->AsArray());
		}
		else {
			TextureEntries.Add(Value);
		}
	}
	const TArray<TSharedPtr<FJsonValue>> ReferencedTextures = FindReferencedTextures(Root);
	int32 TextureIndex = 0;
	for (const TSharedPtr<FJsonValue>& Value : TextureEntries) {
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		const FString Name = ParameterNameFromEntry(Entry);
		if (!Name.IsEmpty()) {
			FString TexturePath = TexturePathFromValue(ParallelValue(Entry, TextureValues, TextureIndex));
			if (TexturePath.IsEmpty() && Entry.IsValid()) {
				int32 IndexInList = INDEX_NONE;
				if (Entry->TryGetNumberField(TEXT("TextureIndex"), IndexInList) && ReferencedTextures.IsValidIndex(IndexInList)) {
					TexturePath = TexturePathFromValue(ReferencedTextures[IndexInList]);
				}
			}
			UMaterialExpressionTextureSampleParameter2D* Param = CreateTextureParameter(
				Material, EditorOnlyData, Name, MakeLoadablePath(TexturePath), X, Y, bCreated);
			if (Param) TextureParams.Add(Param);
		}
		TextureIndex++;
	}

	/* Function calls: MF_PhongToMetalRoughness -> ToMetalRoughness drives
	 * Metallic (output 0) and Roughness (output 1). FunctionInfos sits on
	 * CachedExpressionData in real dumps; fall back to MaterialCompilationOutput
	 * and finally UniformExpressionSet. */
	const TSharedPtr<FJsonObject> Cached = FindCachedExpressionData(Root);
	TSharedPtr<FJsonObject> FunctionInfosOwner;
	if (Cached.IsValid() && Cached->HasTypedField<EJson::Array>(TEXT("FunctionInfos"))) {
		FunctionInfosOwner = Cached;
	}
	else if (Compilation.IsValid() && Compilation->HasTypedField<EJson::Array>(TEXT("FunctionInfos"))) {
		FunctionInfosOwner = Compilation;
	}
	else {
		FunctionInfosOwner = Uniform;
	}
	for (const TSharedPtr<FJsonValue>& Value : FunctionInfosOwner->GetArrayField(TEXT("FunctionInfos"))) {
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!Entry.IsValid()) continue;

		const TSharedPtr<FJsonObject>* Function;
		if (!Entry->TryGetObjectField(TEXT("Function"), Function)) continue;

		FString FunctionPath;
		if (!(*Function)->TryGetStringField(TEXT("ObjectPath"), FunctionPath)) continue;
		if (!FunctionPath.Contains(TEXT("MF_PhongToMetalRoughness"))) continue;

		UMaterialExpressionMaterialFunctionCall* Call = NewObject<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->MaterialExpressionEditorX = X;
		Call->MaterialExpressionEditorY = Y + 100;

		if (UMaterialFunctionInterface* Loaded = TSoftObjectPtr<UMaterialFunctionInterface>(FSoftObjectPath(MakeLoadablePath(FunctionPath))).LoadSynchronous()) {
			Call->MaterialFunction = Loaded;
		}

		EditorOnlyData->ExpressionCollection.Expressions.Add(Call);
		Call->UpdateMaterialExpressionGuid(true, false);
		Material->AddExpressionParameter(Call, Material->EditorParameters);
		bCreated = true;

		EditorOnlyData->Metallic.Expression = Call;
		EditorOnlyData->Metallic.OutputIndex = 0;
		EditorOnlyData->Roughness.Expression = Call;
		EditorOnlyData->Roughness.OutputIndex = 1;
		break;
	}

	/* Wire texture params into the material outputs using the v1 heuristic.
	 * BlendMode may serialize as a number (1 = BLEND_Masked), a full enum string
	 * ("EBlendMode::BLEND_Masked") or live under BasePropertyOverrides. */
	const TSharedPtr<FJsonObject> MatProperties = ExportedProperties(Root);
	bool bMasked = false;
	int32 BlendNumber;
	if (MatProperties->TryGetNumberField(TEXT("BlendMode"), BlendNumber)) {
		bMasked = (BlendNumber == 1);
	}
	FString BlendString;
	if (MatProperties->TryGetStringField(TEXT("BlendMode"), BlendString) && BlendString.Contains(TEXT("Masked"))) {
		bMasked = true;
	}
	const TSharedPtr<FJsonObject>* Overrides;
	if (MatProperties->TryGetObjectField(TEXT("BasePropertyOverrides"), Overrides)) {
		FString BlendMode;
		if ((*Overrides)->TryGetStringField(TEXT("BlendMode"), BlendMode) && BlendMode == TEXT("BLEND_Masked")) {
			bMasked = true;
		}
	}

	/* Two tiers: user-configured name -> pin rules run first, since they're exact
	 * intent for this project's naming conventions. Anything left over falls
	 * through to the built-in Color/Opacity heuristics so unconfigured projects
	 * keep working exactly as before. Either tier can only claim a given pin once,
	 * and Metallic/Roughness are pre-claimed here if the function-call block above
	 * already wired them, so neither tier steals the pin back off that call. */
	TSet<EReflectionMaterialPin> WiredPins;
	if (EditorOnlyData->Metallic.Expression != nullptr) WiredPins.Add(EReflectionMaterialPin::Metallic);
	if (EditorOnlyData->Roughness.Expression != nullptr) WiredPins.Add(EReflectionMaterialPin::Roughness);

	for (UMaterialExpressionTextureSampleParameter2D* Param : TextureParams) {
		if (Param->ParameterName.IsNone()) continue;
		/* A texture parameter whose default texture could not be resolved must not
		 * be wired into the graph: compiling it would fail with "Found NULL,
		 * requires Texture2D" and downgrade the whole material to the default one. */
		if (Param->Texture == nullptr) continue;

		const FString Name = Param->ParameterName.ToString();

		EReflectionMaterialPin UserPin;
		if (FindUserPinMapping(Name, UserPin)) {
			if (!WiredPins.Contains(UserPin)) {
				WireParamIntoPin(EditorOnlyData, UserPin, Param);
				WiredPins.Add(UserPin);
			}
			continue;
		}

		if (!WiredPins.Contains(EReflectionMaterialPin::BaseColor) && IsColorish(Name)) {
			WireParamIntoPin(EditorOnlyData, EReflectionMaterialPin::BaseColor, Param);
			WiredPins.Add(EReflectionMaterialPin::BaseColor);
		}
		else if (bMasked && !WiredPins.Contains(EReflectionMaterialPin::OpacityMask) && IsOpacity(Name)) {
			WireParamIntoPin(EditorOnlyData, EReflectionMaterialPin::OpacityMask, Param);
			WiredPins.Add(EReflectionMaterialPin::OpacityMask);
		}
	}

	/* PropertyConnectedMask tells us which pins the source material actually had
	 * wired, independent of whether we found a matching texture parameter for them.
	 * Anything connected there but not wired here is a real gap, not an assumption -
	 * call it out by name instead of leaving it silently sitting on a default value. */
	for (const EReflectionMaterialPin Pin : GetSourceConnectedPins(Root)) {
		if (!WiredPins.Contains(Pin)) {
			UE_LOG(LogReflection, Warning, TEXT("Fallback graph for \"%s\": \"%s\" was connected to an expression in the source material, but no matching texture parameter was found to wire into it. Add a FallbackPinMappings entry under Project Settings > Reflection > Materials naming the parameter, or wire it manually."),
				*Root->GetStringField(TEXT("Name")), MaterialPinToString(Pin));
		}
	}

	return bCreated;
}

}
#endif