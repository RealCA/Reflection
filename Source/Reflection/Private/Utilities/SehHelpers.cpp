/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/SehHelpers.h"
#include "Engine/Log.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Utilities/MissingDependencies.h"
#include "Utilities/ContentBrowser.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Containers/ExportContainer.h"
#include <Windows.h>

namespace SehHelpersPrivate
{

/* Packages currently inside FullyLoad() */
static TSet<FName> GPackagesBeingFullyLoaded;

/*---------------------------------------------------------
 * Blueprint compile
 *--------------------------------------------------------*/

static bool TryCompileBlueprintImpl(UBlueprint* Blueprint, EBlueprintCompileOptions Options)
{
    __try
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return true;
}

/*---------------------------------------------------------
 * Package creation
 *--------------------------------------------------------*/

/* IMPORTANT:
 * This function MUST NOT create any UE objects (FString, FName,
 * TArray, TMap, etc.) otherwise MSVC emits C2712.
 */
static UPackage* TryCreatePackageImpl(
    const TCHAR* Path,
    const FName* PathName,
    bool bSkipFullyLoad,
    bool& bAccessViolation)
{
    bAccessViolation = false;

    UPackage* Result = nullptr;

    __try
    {
        Result = CreatePackage(Path);

        if (Result && !bSkipFullyLoad)
        {
            GPackagesBeingFullyLoaded.Add(*PathName);
            Result->FullyLoad();
            GPackagesBeingFullyLoaded.Remove(*PathName);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bAccessViolation = true;

        if (!bSkipFullyLoad)
        {
            GPackagesBeingFullyLoaded.Remove(*PathName);
        }

        return nullptr;
    }

    return Result;
}

static UPackage* TryCreatePackage(const TCHAR* Path, bool bSkipFullyLoadRequested, bool& bAccessViolation)
{
    bAccessViolation = false;

    // Safe to create UE objects here because there is NO __try.
    const FName PathName(Path);
    const FString PathString(Path);

    const bool bKnownCircular = IsKnownCircularPackage(PathString);
    const bool bAlreadyLoading = GPackagesBeingFullyLoaded.Contains(PathName);
    const bool bSkipFullyLoad = bSkipFullyLoadRequested || bKnownCircular || bAlreadyLoading;

    return TryCreatePackageImpl(
        Path,
        &PathName,
        bSkipFullyLoad,
        bAccessViolation);
}

} // namespace SehHelpersPrivate

/*---------------------------------------------------------
 * Public API
 *--------------------------------------------------------*/

void CompileBlueprintSafe(UBlueprint* Blueprint, EBlueprintCompileOptions Options)
{
    if (!SehHelpersPrivate::TryCompileBlueprintImpl(Blueprint, Options))
    {
        UE_LOG(
            LogReflection,
            Error,
            TEXT("Access violation while compiling blueprint \"%s\". "
                 "A referenced ControlRig asset may be corrupted. "
                 "Try deleting the existing asset and re-importing."),
            *Blueprint->GetName());
    }
}

namespace SehHelpersPrivate
{

/* Lives outside the __try so the guarded region stays C2712-clean (no destructor-bearing
 * locals, no FString temporaries in this frame at all). */
static void BrowseToAssetImpl(UObject* Asset)
{
    BrowseToAsset(Asset);
}

/* The only function that contains __try: parameters are POD, the guarded region is a single
 * call, and the body builds nothing that would require C++ unwinding. */
static bool TryBrowseToAsset(UObject* Asset)
{
    __try
    {
        BrowseToAssetImpl(Asset);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace SehHelpersPrivate

void BrowseToAssetSafe(UObject* Asset)
{
    if (Asset == nullptr)
    {
        return;
    }

    if (!SehHelpersPrivate::TryBrowseToAsset(Asset))
    {
        UE_LOG(
            LogReflection,
            Error,
            TEXT("Access violation while syncing the Content Browser to \"%s\". "
                 "The asset may still be uncompiled; it has been saved regardless."),
            *Asset->GetName());
    }
}

/*---------------------------------------------------------
 * Pre-compile diagnostic dump
 *--------------------------------------------------------*/

namespace SehHelpersPrivate
{

static void DumpObjectHealth(const FString& Label, UObject* Obj)
{
    if (Obj == nullptr)
    {
        UE_LOG(LogReflection, Log, TEXT("[pre-compile] %s: NULL"), *Label);
        return;
    }

    if (!Obj->IsValidLowLevelFast())
    {
        UE_LOG(LogReflection, Warning, TEXT("[pre-compile] %s: *** INVALID POINTER *** (%p)"),
               *Label, static_cast<void*>(Obj));
        return;
    }

    UE_LOG(
        LogReflection,
        Log,
        TEXT("[pre-compile] %s: %s (%s) %s InternalIndex=%d"),
        *Label,
        *Obj->GetPathName(),
        *Obj->GetClass()->GetName(),
        Obj->HasAnyFlags(RF_NeedLoad) ? TEXT("NEEDS LOAD") : TEXT("loaded"),
        Obj->GetUniqueID());
}

/* Lives outside the __try so the guarded region stays C2712-clean (no destructor-bearing
 * locals, no FString temporaries in this frame at all). */
static void DumpAnimBlueprintPreCompileImpl(
    UAnimBlueprint* AnimBlueprint,
    const UBlueprintGeneratedClass* GeneratedClass,
    UEdGraph* AnimGraph,
    FUObjectExportContainer* Container)
{
    UE_LOG(LogReflection, Log, TEXT("[pre-compile] === state before deferred compilation of \"%s\" ==="),
           AnimBlueprint ? *AnimBlueprint->GetName() : TEXT("<null>"));

    DumpObjectHealth(TEXT("AnimBlueprint"), AnimBlueprint);
    DumpObjectHealth(TEXT("GeneratedClass"), const_cast<UBlueprintGeneratedClass*>(GeneratedClass));
    DumpObjectHealth(TEXT("AnimGraph"), AnimGraph);

    if (Container != nullptr)
    {
        UE_LOG(LogReflection, Log, TEXT("[pre-compile] RootAnimNodeContainer: %d exports"), Container->Exports.Num());

        int32 InvalidCount = 0;

        for (int32 i = 0; i < Container->Exports.Num(); ++i)
        {
            const FUObjectExport* Export = Container->Exports[i];

            if (Export == nullptr)
            {
                UE_LOG(LogReflection, Warning, TEXT("[pre-compile]   export[%d]: NULL export"), i);
                ++InvalidCount;
                continue;
            }

            const FString ExpName = Export->GetName().ToString();
            const FString ExpType = Export->GetType().ToString();

            if (Export->Object == nullptr)
            {
                continue; /* not yet spawned is fine */
            }

            if (!Export->Object->IsValidLowLevelFast())
            {
                UE_LOG(LogReflection, Warning, TEXT("[pre-compile]   export[%d] %s (%s) @%d: *** INVALID OBJECT *** (%p)"),
                       i, *ExpName, *ExpType, Export->Position, static_cast<void*>(Export->Object));
                ++InvalidCount;
                continue;
            }

            UE_LOG(LogReflection, Log, TEXT("[pre-compile]   export[%d] %s (%s) @%d -> %s (%s) %s"),
                   i, *ExpName, *ExpType, Export->Position,
                   *Export->Object->GetName(), *Export->Object->GetClass()->GetName(),
                   Export->Object->HasAnyFlags(RF_NeedLoad) ? TEXT("NEEDS LOAD") : TEXT("loaded"));
        }

        UE_LOG(LogReflection, Log, TEXT("[pre-compile]   %d invalid object(s) in container"), InvalidCount);
    }

    if (AnimGraph != nullptr)
    {
        UE_LOG(LogReflection, Log, TEXT("[pre-compile] AnimGraph \"%s\" has %d nodes"),
               *AnimGraph->GetName(), AnimGraph->Nodes.Num());

        for (UEdGraphNode* Node : AnimGraph->Nodes)
        {
            if (Node == nullptr)
            {
                UE_LOG(LogReflection, Warning, TEXT("[pre-compile]   node: NULL"));
                continue;
            }

            if (!Node->IsValidLowLevelFast())
            {
                UE_LOG(LogReflection, Warning, TEXT("[pre-compile]   node: *** INVALID *** (%p)"),
                       static_cast<void*>(Node));
                continue;
            }

            UE_LOG(LogReflection, Log, TEXT("[pre-compile]   node %s (\"%s\") pins=%d"),
                   *Node->GetClass()->GetName(),
                   *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
                   Node->Pins.Num());

            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin == nullptr || Pin->LinkedTo.Num() == 0)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (LinkedPin == nullptr)
                    {
                        UE_LOG(LogReflection, Warning, TEXT("[pre-compile]     pin %s links to a NULL pin"),
                               *Pin->GetName());
                        continue;
                    }

                    UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();

                    if (LinkedNode != nullptr && !LinkedNode->IsValidLowLevelFast())
                    {
                        UE_LOG(LogReflection, Warning, TEXT("[pre-compile]     pin %s links to a node owned by INVALID object (%p)"),
                               *Pin->GetName(), static_cast<void*>(LinkedNode));
                    }
                }
            }
        }
    }

    UE_LOG(LogReflection, Log, TEXT("[pre-compile] === end dump for \"%s\" ==="),
           AnimBlueprint ? *AnimBlueprint->GetName() : TEXT("<null>"));
}

} // namespace SehHelpersPrivate

static bool TryDumpAnimBlueprintPreCompile(
    UAnimBlueprint* AnimBlueprint,
    const UBlueprintGeneratedClass* GeneratedClass,
    UEdGraph* AnimGraph,
    FUObjectExportContainer* Container)
{
    __try
    {
        SehHelpersPrivate::DumpAnimBlueprintPreCompileImpl(AnimBlueprint, GeneratedClass, AnimGraph, Container);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void DumpAnimBlueprintPreCompile(
    UAnimBlueprint* AnimBlueprint,
    const UBlueprintGeneratedClass* GeneratedClass,
    UEdGraph* AnimGraph,
    FUObjectExportContainer* Container)
{
    if (!TryDumpAnimBlueprintPreCompile(AnimBlueprint, GeneratedClass, AnimGraph, Container))
    {
        UE_LOG(LogReflection, Error,
               TEXT("[pre-compile] Access violation while dumping animation blueprint state before compilation."));
    }
}

UPackage* CreateAssetPackageSafe(const TCHAR* Path, bool bSkipFullyLoad)
{
    bool bAccessViolation = false;

    UPackage* Result =
        SehHelpersPrivate::TryCreatePackage(Path, bSkipFullyLoad, bAccessViolation);

    if (bAccessViolation)
    {
        UE_LOG(
            LogReflection,
            Error,
            TEXT("Access violation while creating/loading package \"%s\". "
                 "A referenced ControlRig asset may be corrupted. "
                 "Try deleting the existing asset and re-importing."),
            Path);

        return nullptr;
    }

    return Result;
}