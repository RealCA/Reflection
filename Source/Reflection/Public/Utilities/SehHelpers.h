/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

/* SEH-safe wrappers for UE functions that may trigger ControlRig hardware exceptions.
 * The actual implementations are in SehHelpers.cpp.
 *
 * Note: __try/__except only fails to compile in a function (C2712) when that function
 * has local C++ objects requiring destructor unwinding. Since these wrappers only ever
 * hold pointer locals, it's safe to include the real engine headers (EBlueprintCompileOptions,
 * UBlueprint, UPackage) instead of hand-rolling forward declarations, which was the source
 * of the class/struct and redefinition errors. */

class UBlueprint;
class UPackage;
class UObject;
class UAnimBlueprint;
class UBlueprintGeneratedClass;
class UEdGraph;
struct FUObjectExportContainer;

REFLECTION_API void CompileBlueprintSafe(UBlueprint* Blueprint, EBlueprintCompileOptions Options = EBlueprintCompileOptions::None);
REFLECTION_API UPackage* CreateAssetPackageSafe(const TCHAR* Path, bool bSkipFullyLoad = false);

/* Poison-flag (plan 013): a caught compile access violation leaves the process
 * in an undefined state - the 08.24 crashes showed the editor going down
 * minutes later in unrelated Slate teardown. Once set, the import job aborts
 * instead of continuing to churn damaged state. */
REFLECTION_API bool IsBlueprintCompilePoisoned();
REFLECTION_API void ResetBlueprintCompilePoison();

/* Removes compiler-intermediate graphs and nodes (bIsIntermediateNode) that a
 * FAULTED compile left inside a live blueprint. 08.24: the aborted compile of
 * BP_Stockpile left an intermediate ReceiveBeginPlay graph calling the missing
 * ubergraph function - opening the stub built a widget for that node and took
 * the editor down (the "trojan stub"). Safe to call on any blueprint. */
REFLECTION_API void SanitizeIntermediateGraphs(UBlueprint* Blueprint);

/* Dumps the anim graph node state and the container exports that own those nodes right before
 * compilation, so the log captures every object (and any invalid one) the compile-time reference
 * walk will descend into. Guarded by __try/__except: a faulted dump is logged and swallowed
 * instead of taking the editor down ahead of the compile-time crash it is trying to explain. */
REFLECTION_API void DumpAnimBlueprintPreCompile(UAnimBlueprint* AnimBlueprint, const UBlueprintGeneratedClass* GeneratedClass, UEdGraph* AnimGraph, FUObjectExportContainer* Container);

/* Syncs the Content Browser to Asset inside __try/__except. A freshly imported blueprint's
 * thumbnail instantiates its anim graph, and one that is still uncompiled (compilation is
 * deferred to the batch final phase) hands back dangling node references on the way to an
 * access violation. Never let that take the editor down - the asset was already saved. */
REFLECTION_API void BrowseToAssetSafe(UObject* Asset);