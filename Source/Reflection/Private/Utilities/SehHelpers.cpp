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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformStackWalk.h"
#include <Windows.h>

namespace SehHelpersPrivate
{

/* Packages currently inside FullyLoad() */
static TSet<FName> GPackagesBeingFullyLoaded;

/* Set when a guarded compile catches an access violation (plan 013). */
static bool GBlueprintCompilePoisoned = false;

/*---------------------------------------------------------
 * Blueprint compile
 *--------------------------------------------------------*/

/* Runs as the __except filter, BEFORE unwinding: the faulting stack is still
 * intact, so a plain backtrace from here names the exact compiler function
 * that faulted. POD-only frame (C2712). Returns EXCEPTION_EXECUTE_HANDLER. */
static LONG CompileFaultFilter(EXCEPTION_POINTERS* Info)
{
    const DWORD Code = Info->ExceptionRecord ? Info->ExceptionRecord->ExceptionCode : 0;
    const void* Address = Info->ExceptionRecord ? Info->ExceptionRecord->ExceptionAddress : nullptr;

    UE_LOG(LogReflection, Error,
        TEXT("Compile fault captured: code=0x%08X address=%p - faulting stack follows."),
        (uint32)Code, Address);

    uint64 Frames[48] = {};
    const int32 Depth = FPlatformStackWalk::CaptureStackBackTrace(Frames, 48);
    for (int32 i = 0; i < Depth; ++i)
    {
        ANSICHAR Symbol[1024] = {};
        FPlatformStackWalk::ProgramCounterToHumanReadableString(i, Frames[i], Symbol, sizeof(Symbol));
        UE_LOG(LogReflection, Error, TEXT("  [%02d] %hs"), i, Symbol);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static bool TryCompileBlueprintImpl(UBlueprint* Blueprint, EBlueprintCompileOptions Options)
{
    __try
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);
    }
    __except (CompileFaultFilter(GetExceptionInformation()))
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

bool IsBlueprintCompilePoisoned()
{
    return SehHelpersPrivate::GBlueprintCompilePoisoned;
}

void ResetBlueprintCompilePoison()
{
    SehHelpersPrivate::GBlueprintCompilePoisoned = false;
}

/* Normal-function implementation - the SEH wrapper below must stay free of
 * destructor-bearing locals (C2712). */
namespace SehHelpersPrivate
{
    static void SanitizeIntermediateGraphsImpl(UBlueprint* Blueprint)
    {
        if (!Blueprint) return;

        TArray<UEdGraph*> Graphs;
        for (UEdGraph* Graph : Blueprint->UbergraphPages) Graphs.Add(Graph);
        for (UEdGraph* Graph : Blueprint->FunctionGraphs) Graphs.Add(Graph);

        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph) continue;

            bool bHasRealNode = false;
            bool bHasIntermediate = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) continue;
                if (Node->IsIntermediateNode()) bHasIntermediate = true;
                else bHasRealNode = true;
            }
            if (!bHasIntermediate) continue;

            if (!bHasRealNode)
            {
                /* The whole graph is compiler scaffolding a faulted compile left
                 * behind - drop it from the blueprint entirely. */
                UE_LOG(LogReflection, Warning,
                    TEXT("SanitizeIntermediateGraphs: removing compiler-intermediate graph \"%s\" left by a faulted compile."),
                    *Graph->GetName());
                Blueprint->UbergraphPages.Remove(Graph);
                Blueprint->FunctionGraphs.Remove(Graph);
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node)
                    {
                        Node->BreakAllNodeLinks();
                        Node->MarkAsGarbage();
                    }
                }
                Graph->MarkAsGarbage();
            }
            else
            {
                UE_LOG(LogReflection, Warning,
                    TEXT("SanitizeIntermediateGraphs: stripping intermediate node(s) from \"%s\"."),
                    *Graph->GetName());
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (Node && Node->IsIntermediateNode())
                    {
                        Node->BreakAllNodeLinks();
                        Node->MarkAsGarbage();
                    }
                }
                Graph->Nodes.RemoveAll([](const UEdGraphNode* Node) { return Node && Node->IsIntermediateNode(); });
            }
        }
    }

    static bool TrySanitizeIntermediateGraphs(UBlueprint* Blueprint)
    {
        __try
        {
            SanitizeIntermediateGraphsImpl(Blueprint);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void SanitizeIntermediateGraphs(UBlueprint* Blueprint)
{
    /* Runs on state an access violation just damaged - a fault here must not
     * take the editor down on top of the compile fault that caused it. */
    if (!SehHelpersPrivate::TrySanitizeIntermediateGraphs(Blueprint))
    {
        UE_LOG(LogReflection, Error, TEXT("SanitizeIntermediateGraphs faulted - blueprint left as-is (job is poisoned anyway)."));
    }
}

void CompileBlueprintSafe(UBlueprint* Blueprint, EBlueprintCompileOptions Options)
{
    if (!SehHelpersPrivate::TryCompileBlueprintImpl(Blueprint, Options))
    {
        /* The process just survived an access violation: engine state is
         * undefined from here on. Poison the job so the importer aborts
         * instead of churning damaged state into a worse crash. */
        SehHelpersPrivate::GBlueprintCompilePoisoned = true;

        /* Record the package for the next Enqueue's auto-clean (plan 013):
         * only ReflectionStub-tagged assets whose JSON is re-imported in that
         * run will actually be deleted - the record itself is just a hint. */
        const FString PackagePath = Blueprint ? Blueprint->GetPackage()->GetName() : FString();
        const FString RecordPath = FPaths::ProjectSavedDir() / TEXT("CorruptedImports.txt");
        if (!PackagePath.IsEmpty())
        {
            TArray<FString> Existing;
            FFileHelper::LoadFileToStringArray(Existing, *RecordPath);
            Existing.Add(PackagePath);
            FFileHelper::SaveStringArrayToFile(Existing, *RecordPath);
            UE_LOG(LogReflection, Error, TEXT("Recorded corrupted import package: %s"), *PackagePath);

            /* Defuse the trojan (plan 013): a faulted compile leaves compiler-
             * intermediate graphs inside the live blueprint (e.g. a
             * ReceiveBeginPlay graph calling the missing ubergraph function).
             * Opening that stub later built a widget for the intermediate node
             * and crashed Slate. Strip them now, while the job is aborting. */
            SanitizeIntermediateGraphs(Blueprint);
        }

        UE_LOG(
            LogReflection,
            Error,
            TEXT("Access violation while compiling blueprint \"%s\". "
                 "A referenced ControlRig asset may be corrupted. "
                 "Try deleting the existing asset and re-importing. "
                 "Aborting the import job."),
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
