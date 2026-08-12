/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#if ENGINE_UE5
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/SettingsAccess.h"
#include "Settings/Types/MaterialSettings.h"

/* Consumes the JSON recipe emitted by material_reconstructor.py's --json
 * option (one AST per pin, reconstructed from decompiled base-pass shader
 * bytecode - see that script's module docstring for how it gets built and
 * what its limits are) and builds a real, wired UMaterialExpression chain
 * for a pin instead of the single-texture heuristic in MaterialFallback.h.
 *
 * This is opt-in and additive: MaterialFallback's existing heuristics and
 * FallbackPinMappings config are untouched and still run for every pin this
 * doesn't cover. A recipe is only used when a sidecar
 * "<MaterialName>.recipe.json" file sits next to the material's own JSON
 * export - nothing here runs automatically without that file being present,
 * since it has to be produced by manually running the Python tool against
 * a shader dump for this specific material first. */
namespace Reflection::MaterialFormula {

/* Every op this understands. Anything else (var/call/select/cmp/and/or -
 * i.e. an unresolved SSA reference, an opaque helper-function call, or
 * engine-level conditional logic we deliberately never try to turn into a
 * material If node) makes the whole pin fail validation rather than build a
 * half-correct graph from a formula we don't actually understand. */
inline bool IsUnresolvableLeaf(const FString& Op) {
	return Op == TEXT("var") || Op == TEXT("call") || Op == TEXT("select")
		|| Op == TEXT("cmp") || Op == TEXT("and") || Op == TEXT("or");
}

/* Dry-run check: does every leaf in this tree resolve to something buildable
 * (a known texture, a parameter, or a literal)? Run before creating any UE
 * objects so a formula this can't fully understand never leaves a half-built
 * orphan node chain sitting in the material - it's all or nothing per pin. */
inline bool ValidateRecipeNode(const TSharedPtr<FJsonObject>& Node, const TMap<FString, UMaterialExpressionTextureSampleParameter2D*>& TexturesByName) {
	if (!Node.IsValid()) return false;
	FString Op;
	if (!Node->TryGetStringField(TEXT("op"), Op)) return false;

	if (Op == TEXT("const") || Op == TEXT("param")) return true;

	if (Op == TEXT("tex")) {
		FString Name;
		return Node->TryGetStringField(TEXT("name"), Name) && TexturesByName.Contains(Name);
	}

	if (Op == TEXT("member")) {
		const TSharedPtr<FJsonObject>* Target;
		return Node->TryGetObjectField(TEXT("target"), Target) && ValidateRecipeNode(*Target, TexturesByName);
	}

	if (IsUnresolvableLeaf(Op)) return false;

	// Every other op (lerp/oneminus/saturate/mul/add/sub/div/neg) is an
	// n-ary math node - valid if every argument is.
	const TArray<TSharedPtr<FJsonValue>>* Args;
	if (!Node->TryGetArrayField(TEXT("args"), Args)) return false;
	for (const TSharedPtr<FJsonValue>& Arg : *Args) {
		if (!ValidateRecipeNode(Arg->AsObject(), TexturesByName)) return false;
	}
	return true;
}

struct FBuildResult {
	UMaterialExpression* Expression = nullptr;
	int32 OutputIndex = 0;
};

/* Deterministic string for a JSON AST node, used as a dedup key. The exact
 * same sub-formula shows up under more than one pin fairly often here - both
 * Specular and Roughness reference the same "Wetness texture * a scalar
 * param" term in this codebase's own validated example - and since each
 * pin's tree is built independently, without this every repeated subtree
 * would silently become its own duplicate node chain instead of one shared,
 * reused node. */
inline FString CanonicalKey(const TSharedPtr<FJsonObject>& Node) {
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Node.ToSharedRef(), Writer);
	return Out;
}

/* Texture sample outputs: 0=RGBA(all), 1=R, 2=G, 3=B, 4=A - matches
 * UMaterialExpressionTextureSampleParameter2D's fixed output list. */
inline int32 ChannelToOutputIndex(const FString& Channel) {
	if (Channel == TEXT("x") || Channel == TEXT("r")) return 1;
	if (Channel == TEXT("y") || Channel == TEXT("g")) return 2;
	if (Channel == TEXT("z") || Channel == TEXT("b")) return 3;
	if (Channel == TEXT("w") || Channel == TEXT("a")) return 4;
	return 0;
}

inline void AddNode(UMaterialEditorOnlyData* EditorOnlyData, UMaterialExpression* Expr, int32& X, int32& Y) {
	Expr->MaterialExpressionEditorX = X;
	Expr->MaterialExpressionEditorY = Y;
	EditorOnlyData->ExpressionCollection.Expressions.Add(Expr);
	Expr->UpdateMaterialExpressionGuid(true, false);
	X += 180;
	Y += 40;
}

inline void SetInput(FExpressionInput& Input, const FBuildResult& Value) {
	Input.Expression = Value.Expression;
	Input.OutputIndex = Value.OutputIndex;
}

/* Only called after ValidateRecipeNode has already confirmed the whole tree
 * resolves - every branch here can assume the fields it needs are present.
 * Cache is shared across an entire ApplyRecipe call (all pins), not just one
 * pin's tree, so a term repeated across pins (see CanonicalKey above) is
 * built once and reused rather than duplicated per pin. */
inline FBuildResult BuildRecipeNode(UMaterial* Material, UMaterialEditorOnlyData* EditorOnlyData, const TSharedPtr<FJsonObject>& Node,
	const TMap<FString, UMaterialExpressionTextureSampleParameter2D*>& TexturesByName, TMap<FString, FBuildResult>& Cache, int32& X, int32& Y) {

	// "tex" leaves already always resolve to the same pre-existing texture
	// node via TexturesByName, so they don't need (or benefit from) caching -
	// skip the cache for them and let every other op type share one.
	const FString Op = Node->GetStringField(TEXT("op"));
	FString Key;
	if (Op != TEXT("tex")) {
		Key = CanonicalKey(Node);
		if (const FBuildResult* Existing = Cache.Find(Key)) {
			return *Existing;
		}
	}

	auto Cached = [&](const FBuildResult& Result) {
		if (!Key.IsEmpty()) Cache.Add(Key, Result);
		return Result;
	};

	if (Op == TEXT("const")) {
		UMaterialExpressionConstant* C = NewObject<UMaterialExpressionConstant>(Material);
		C->R = static_cast<float>(Node->GetNumberField(TEXT("value")));
		AddNode(EditorOnlyData, C, X, Y);
		return Cached({ C, 0 });
	}

	if (Op == TEXT("param")) {
		// Preshader constant with no recoverable name (see module docstring
		// in material_reconstructor.py) - created as a real, editable, per-
		// instance-overridable ScalarParameter rather than a baked constant,
		// since that's what it was in the source material; the name is a
		// placeholder the user should rename to the real parameter once
		// known, not a guess presented as fact.
		const int32 Index = Node->GetIntegerField(TEXT("index"));
		const FString Channel = Node->GetStringField(TEXT("channel"));
		const FString Name = FString::Printf(TEXT("ReconstructedParam_%d_%s"), Index, *Channel);
		UMaterialExpressionScalarParameter* P = NewObject<UMaterialExpressionScalarParameter>(Material);
		P->ParameterName = FName(*Name);
		P->DefaultValue = 0.0f; // real value unknown - see comment above
		AddNode(EditorOnlyData, P, X, Y);
		Material->AddExpressionParameter(P, Material->EditorParameters);
		return Cached({ P, 0 });
	}

	if (Op == TEXT("tex")) {
		UMaterialExpressionTextureSampleParameter2D* const* Found = TexturesByName.Find(Node->GetStringField(TEXT("name")));
		check(Found); // ValidateRecipeNode already guaranteed this exists
		return { *Found, 0 }; // bare Tex(name) with no .channel means "RGBA" (output 0)
	}

	if (Op == TEXT("member")) {
		const FBuildResult Target = BuildRecipeNode(Material, EditorOnlyData, Node->GetObjectField(TEXT("target")), TexturesByName, Cache, X, Y);
		// A member access is only meaningful directly on a texture sample
		// (Tex(Name).channel) - ValidateRecipeNode only accepts "member"
		// wrapping "tex", so Target.Expression is always the texture node here.
		return Cached({ Target.Expression, ChannelToOutputIndex(Node->GetStringField(TEXT("channel"))) });
	}

	const TArray<TSharedPtr<FJsonValue>>& Args = Node->GetArrayField(TEXT("args"));
	auto BuildArg = [&](int32 i) { return BuildRecipeNode(Material, EditorOnlyData, Args[i]->AsObject(), TexturesByName, Cache, X, Y); };

	if (Op == TEXT("lerp")) {
		UMaterialExpressionLinearInterpolate* L = NewObject<UMaterialExpressionLinearInterpolate>(Material);
		SetInput(L->A, BuildArg(0));
		SetInput(L->B, BuildArg(1));
		SetInput(L->Alpha, BuildArg(2));
		AddNode(EditorOnlyData, L, X, Y);
		return Cached({ L, 0 });
	}
	if (Op == TEXT("oneminus")) {
		UMaterialExpressionOneMinus* N = NewObject<UMaterialExpressionOneMinus>(Material);
		SetInput(N->Input, BuildArg(0));
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("saturate")) {
		// UMaterialExpressionSaturate exists in some engine versions but isn't
		// guaranteed - UMaterialExpressionClamp(0,1) is the same operation via
		// a node that's been stable since UE4, so it's used here instead.
		UMaterialExpressionClamp* N = NewObject<UMaterialExpressionClamp>(Material);
		SetInput(N->Input, BuildArg(0));
		N->MinDefault = 0.0f;
		N->MaxDefault = 1.0f;
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("mul")) {
		UMaterialExpressionMultiply* N = NewObject<UMaterialExpressionMultiply>(Material);
		SetInput(N->A, BuildArg(0));
		SetInput(N->B, BuildArg(1));
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("add")) {
		UMaterialExpressionAdd* N = NewObject<UMaterialExpressionAdd>(Material);
		SetInput(N->A, BuildArg(0));
		SetInput(N->B, BuildArg(1));
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("sub")) {
		UMaterialExpressionSubtract* N = NewObject<UMaterialExpressionSubtract>(Material);
		SetInput(N->A, BuildArg(0));
		SetInput(N->B, BuildArg(1));
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("div")) {
		UMaterialExpressionDivide* N = NewObject<UMaterialExpressionDivide>(Material);
		SetInput(N->A, BuildArg(0));
		SetInput(N->B, BuildArg(1));
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}
	if (Op == TEXT("neg")) {
		// No dedicated Negate node - Multiply by a -1 constant does the same
		// thing and keeps this from needing a special-cased subtract-from-zero.
		UMaterialExpressionConstant* NegOne = NewObject<UMaterialExpressionConstant>(Material);
		NegOne->R = -1.0f;
		AddNode(EditorOnlyData, NegOne, X, Y);
		UMaterialExpressionMultiply* N = NewObject<UMaterialExpressionMultiply>(Material);
		SetInput(N->A, BuildArg(0));
		N->B.Expression = NegOne;
		AddNode(EditorOnlyData, N, X, Y);
		return Cached({ N, 0 });
	}

	checkNoEntry(); // ValidateRecipeNode should have rejected anything reaching here
	return {};
}

/* Loads "<RecipesDirectory>/<MaterialName>.recipe.json", if the directory is
 * configured and the file exists. Returns an invalid pointer (not an error)
 * when there's no recipe for this material - the caller should just skip
 * straight to the existing fallback heuristics in that case. Keyed by
 * material name rather than derived from the source JSON's own path: the
 * fallback-graph code only ever receives the already-parsed FJsonObject,
 * not the file path it came from, so a configured directory (matching the
 * pattern FallbackPinMappings already uses) is what's actually wireable
 * here without adding new plumbing through the importer call chain. */
inline TSharedPtr<FJsonObject> TryLoadRecipe(const FString& MaterialName) {
	const FString Directory = GetSettings()->AssetSettings.Material.ReconstructionRecipesDirectory;
	if (Directory.IsEmpty()) return nullptr;

	const FString RecipePath = FPaths::Combine(Directory, MaterialName + TEXT(".recipe.json"));
	if (!FPaths::FileExists(RecipePath)) return nullptr;

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *RecipePath)) return nullptr;

	TSharedPtr<FJsonObject> Recipe;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Recipe) || !Recipe.IsValid()) {
		UE_LOG(LogReflection, Warning, TEXT("Found a recipe file for \"%s\" but it failed to parse as JSON: %s"), *MaterialName, *RecipePath);
		return nullptr;
	}
	return Recipe;
}

inline void WireBuiltExpressionIntoPin(UMaterialEditorOnlyData* EditorOnlyData, EReflectionMaterialPin Pin, const FBuildResult& Result) {
	switch (Pin) {
	case EReflectionMaterialPin::BaseColor: SetInput(EditorOnlyData->BaseColor, Result); break;
	case EReflectionMaterialPin::Metallic: SetInput(EditorOnlyData->Metallic, Result); break;
	case EReflectionMaterialPin::Specular: SetInput(EditorOnlyData->Specular, Result); break;
	case EReflectionMaterialPin::Roughness: SetInput(EditorOnlyData->Roughness, Result); break;
	case EReflectionMaterialPin::EmissiveColor: SetInput(EditorOnlyData->EmissiveColor, Result); break;
	case EReflectionMaterialPin::Opacity: SetInput(EditorOnlyData->Opacity, Result); break;
	case EReflectionMaterialPin::OpacityMask: SetInput(EditorOnlyData->OpacityMask, Result); break;
	case EReflectionMaterialPin::Normal: SetInput(EditorOnlyData->Normal, Result); break;
	case EReflectionMaterialPin::AmbientOcclusion: SetInput(EditorOnlyData->AmbientOcclusion, Result); break;
	}
}

/* Applies every pin in Recipe that validates cleanly, wiring the built chain
 * into EditorOnlyData exactly like WireParamIntoPin does for the single-
 * texture heuristic. Returns the set of pins it successfully wired, so the
 * caller (CreateFallbackGraph) can skip its own heuristics for those and
 * seed WiredPins correctly. Channels of a multi-channel pin (BaseColor,
 * Normal) that don't all validate are skipped individually rather than
 * failing the whole pin - a material with e.g. only .x and .y recoverable
 * still gets those two wired instead of nothing. */
inline TSet<EReflectionMaterialPin> ApplyRecipe(UMaterial* Material, UMaterialEditorOnlyData* EditorOnlyData, const TSharedPtr<FJsonObject>& Recipe,
	const TMap<FString, UMaterialExpressionTextureSampleParameter2D*>& TexturesByName, int32 StartX, int32 StartY) {

	TSet<EReflectionMaterialPin> Wired;
	int32 X = StartX, Y = StartY;
	// Shared across every pin below, not reset per-pin, so a term repeated
	// across pins (e.g. this codebase's validated case: both Specular and
	// Roughness reference the same "Wetness texture * a scalar param" node)
	// is built once and the same node object is reused everywhere it's
	// referenced, instead of silently duplicating it once per pin.
	TMap<FString, FBuildResult> Cache;

	auto TryPin = [&](const TCHAR* PinName, EReflectionMaterialPin Pin) {
		const TSharedPtr<FJsonObject>* PinObj;
		if (!Recipe->TryGetObjectField(PinName, PinObj)) return;

		// Single-channel pins (Metallic/Specular/Roughness/AO) key their one
		// entry as "x" regardless of which GBuffer channel it actually came
		// from - multi-channel pins (BaseColor/Normal) use x/y/z per-component.
		static const TArray<FString> Channels = { TEXT("x"), TEXT("y"), TEXT("z") };
		TArray<FBuildResult> ComponentResults;
		bool bAnyChannel = false;
		for (const FString& Channel : Channels) {
			const TSharedPtr<FJsonObject>* ChannelNode;
			if (!(*PinObj)->TryGetObjectField(Channel, ChannelNode)) continue;
			if (!ValidateRecipeNode(*ChannelNode, TexturesByName)) {
				UE_LOG(LogReflection, Warning, TEXT("Recipe for \"%s\" channel \"%s\" references a texture or op this importer can't build - skipping that channel."), PinName, *Channel);
				continue;
			}
			bAnyChannel = true;
			ComponentResults.Add(BuildRecipeNode(Material, EditorOnlyData, *ChannelNode, TexturesByName, Cache, X, Y));
		}
		if (!bAnyChannel) return;

		if (ComponentResults.Num() == 1) {
			WireBuiltExpressionIntoPin(EditorOnlyData, Pin, ComponentResults[0]);
		} else {
			// Each channel was reconstructed as its own separate scalar chain
			// (that's how the dataflow slicing this recipe came from
			// necessarily works - one GBuffer channel at a time), so
			// recombining them into one RGB/vector value for the pin needs
			// an explicit Append. AppendVector only takes two inputs, so
			// three channels chain through two Append nodes: (x,y) first,
			// then (that, z). Each component was already independently
			// validated above, so this is just wiring, not another guess.
			UMaterialExpressionAppendVector* First = NewObject<UMaterialExpressionAppendVector>(Material);
			SetInput(First->A, ComponentResults[0]);
			SetInput(First->B, ComponentResults[1]);
			AddNode(EditorOnlyData, First, X, Y);
			FBuildResult Combined{ First, 0 };
			if (ComponentResults.Num() >= 3) {
				UMaterialExpressionAppendVector* Second = NewObject<UMaterialExpressionAppendVector>(Material);
				SetInput(Second->A, Combined);
				SetInput(Second->B, ComponentResults[2]);
				AddNode(EditorOnlyData, Second, X, Y);
				Combined = { Second, 0 };
			}
			WireBuiltExpressionIntoPin(EditorOnlyData, Pin, Combined);
		}
		Wired.Add(Pin);
	};

	TryPin(TEXT("BaseColor"), EReflectionMaterialPin::BaseColor);
	TryPin(TEXT("Metallic"), EReflectionMaterialPin::Metallic);
	TryPin(TEXT("Specular"), EReflectionMaterialPin::Specular);
	TryPin(TEXT("Roughness"), EReflectionMaterialPin::Roughness);
	TryPin(TEXT("EmissiveColor"), EReflectionMaterialPin::EmissiveColor);
	TryPin(TEXT("Opacity"), EReflectionMaterialPin::Opacity);
	TryPin(TEXT("OpacityMask"), EReflectionMaterialPin::OpacityMask);
	TryPin(TEXT("Normal"), EReflectionMaterialPin::Normal);
	TryPin(TEXT("AmbientOcclusion"), EReflectionMaterialPin::AmbientOcclusion);

	return Wired;
}

}
#endif