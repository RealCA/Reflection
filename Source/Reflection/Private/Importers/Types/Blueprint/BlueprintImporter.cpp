/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"
#include "BlueprintBytecodeImporter.h"
#include "Importers/Types/Blueprint/BlueprintStubFactory.h"
#include "UObject/MetaData.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"

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

#include "UObject/UObjectGlobals.h"
#include "UObject/FieldIterator.h"

#if ENGINE_UE5
#include "MVVM/ViewModels/ObjectBindingModel.h"
#endif

#include "Engine/SCS_Node.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimBlueprint.h"
#include "GameFramework/Actor.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Utilities/SehHelpers.h"

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	UClass* Class = GetAssetClass();

	/* A blueprint export WITHOUT a SuperStruct resolves its "parent" to the raw
	 * generated-class type (FindClassByType("BlueprintGeneratedClass") ->
	 * UBlueprintGeneratedClass itself). Parenting a stub onto the raw type
	 * poisons every compile that walks a member typed by it (08.24:
	 * BP_Stockpile's BP_WorldPawn_C-typed variables -> Engine AV at the same
	 * address on every run). Substitute the correct native base by blueprint
	 * kind instead. */
	if (Class == UBlueprintGeneratedClass::StaticClass()
		|| Class == UWidgetBlueprintGeneratedClass::StaticClass()
		|| Class == UAnimBlueprintGeneratedClass::StaticClass())
	{
		const FString ExportType = GetAssetExport()->GetStringField(TEXT("Type"));
		if (ExportType.Contains(TEXT("WidgetBlueprint"))) {
			Class = UUserWidget::StaticClass();
		} else if (ExportType.Contains(TEXT("AnimBlueprint"))) {
			Class = UAnimInstance::StaticClass();
		} else {
			Class = AActor::StaticClass();
		}
		UE_LOG(LogReflection, Warning,
			TEXT("Blueprint export has no SuperStruct - defaulting parent class to %s for \"%s\"."),
			*Class->GetName(), *GetAssetName());
	}

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
	const TArray<TSharedPtr<FJsonValue>>* ChildProperties;
	if (GetAssetExport()->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) {
		/* A re-import into an existing blueprint removes variables the previous import
		 * added that the JSON no longer declares, then rebuilds the ones it does. */
		FBlueprintVariables::ClearStaleVariables(Blueprint, *ChildProperties);
	}

	if (ConstructVariables() > 0) {
		/* Adding a variable only touches the blueprint, the generated class grows the property
		 * when it recompiles, and the default object below is the one that comes out of that */
		CompileBlueprintSafe(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!GeneratedClass) return false;

		ClassDefaultObjectExport->Object = GeneratedClass;
	}

	/* Stub imports (dependency BPs) get variables + blueprint shell but no graph nodes.
	 * This lets the main BP's cast nodes resolve to a real BlueprintGeneratedClass
	 * without importing the dependency's own broken graph content. */
	/* Provenance tags (plan 013): persisted in the package metadata so any later
	 * session can tell plugin-created stubs from real imports from foreign
	 * assets. The stub tag is the ONLY cleanup license - the auto-clean never
	 * touches assets carrying just ReflectionImport or no tag at all. */
	const bool bStub = FBlueprintStubFactory::IsStubImport(GetSourceFile());
	{
		FMetaData& PackageMeta = Blueprint->GetPackage()->GetMetaData();
		if (bStub) {
			PackageMeta.SetValue(Blueprint, TEXT("ReflectionStub"), *GetSourceFile());
		} else {
			PackageMeta.SetValue(Blueprint, TEXT("ReflectionImport"), *GetSourceFile());
		}
	}
	if (!bStub) {
		GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

		/* Experimental (for now) spawning */
		GetObjectSerializer()->bUseExperimentalSpawning = true;

		ConstructScript();
		ConstructWidgetTree();
		ProcessBytecode();
	} else {
		UE_LOG(LogTemp, Log, TEXT("Stub import - creating function stubs for: %s"), *GetSourceFile());
		ConstructStubFunctions();

		/* Compile the stub BP so its GeneratedClass is registered and
		 * cast nodes in other BPs can resolve to it. */
		CompileBlueprintSafe(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		/* Belt-and-suspenders: strip compiler-intermediate graphs the compile
		 * (or an earlier faulted attempt on this same blueprint) may have left
		 * in the live graphs - opening such a stub crashes the editor. */
		SanitizeIntermediateGraphs(Blueprint);
		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);

		/* A stub's variables can reference a class this same batch imports later (the
		 * circular case: the widget stubs declare a GM variable of type GM_Touchy_C,
		 * but GM_Touchy is the root file and imports last). ConstructVariables above
		 * skipped those because the type class didn't exist yet. Re-run construction
		 * once every asset in the batch is populated, then recompile so the variable
		 * actually lands on the class. */
		const TArray<TSharedPtr<FJsonValue>>* StubChildProperties = nullptr;
		if (GetAssetExport()->TryGetArrayField(TEXT("ChildProperties"), StubChildProperties)) {
			UBlueprint* StubBlueprint = Blueprint;
			TArray<TSharedPtr<FJsonValue>> CapturedProps = *StubChildProperties;
			FAssetDependencyRegistry::Get().RequestFinalize([StubBlueprint, CapturedProps]() {
				if (FBlueprintVariables::Construct(StubBlueprint, CapturedProps) > 0) {
					CompileBlueprintSafe(StubBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				}
			});
		}
	}

	return OnAssetCreation(Blueprint);
}

void IBlueprintImporter::ConstructStubFunctions() {
	ConstructBlueprintStubFunctions(Blueprint, GetContainer());
}

void ConstructBlueprintStubFunctions(UBlueprint* InBlueprint, FUObjectExportContainer* Container) {
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(InBlueprint->GeneratedClass);
	if (!GeneratedClass) return;

	if (!Container) return;

	/* Reuse the existing EventGraph created by FKismetEditorUtilities::CreateBlueprint */
	UEdGraph* EventGraph = nullptr;
	for (UEdGraph* Graph : InBlueprint->UbergraphPages) {
		if (Graph && Graph->GetName() == TEXT("EventGraph")) {
			EventGraph = Graph;
			break;
		}
	}
	if (!EventGraph && InBlueprint->UbergraphPages.Num() > 0) {
		EventGraph = InBlueprint->UbergraphPages[0];
	}

    /* A re-import into an existing blueprint has to drop whatever the previous import
     * built first - stub UFunctions on the class and any event nodes in the graph - so
     * the stubs don't stack on top of stale content. Renaming the UFunctions out of the
     * class also stops NewObject from reusing a stale object with the same name. */
    if (GeneratedClass->UberGraphFunction || GeneratedClass->UberGraphFramePointerProperty)
    {
        if (GeneratedClass->UberGraphFunction && GeneratedClass->UberGraphFramePointerProperty)
        {
            GeneratedClass->DestroyPersistentUberGraphFrame(GeneratedClass->GetDefaultObject());
        }
        GeneratedClass->UberGraphFramePointerProperty = nullptr;
        GeneratedClass->UberGraphFunction = nullptr;
#if VALIDATE_UBER_GRAPH_PERSISTENT_FRAME
        GeneratedClass->UberGraphFunctionKey = 0;
#endif
    }

    for (UFunction* Func : TFieldRange<UFunction>(GeneratedClass)) {
		if (Func->GetOuter() == GeneratedClass) {
			GeneratedClass->RemoveFunctionFromFunctionMap(Func);
			Func->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			Func->MarkAsGarbage();
		}
	}

	if (EventGraph) {
		EventGraph->Modify();
		for (UEdGraphNode* Node : EventGraph->Nodes) {
			if (Node) {
				Node->BreakAllNodeLinks();
				Node->ConditionalBeginDestroy();
			}
		}
		EventGraph->Nodes.Empty();
		EventGraph->SubGraphs.Empty();
	}

	/* Also drop any function graphs from a previous stub import so stubs don't
	 * stack on stale content when a blueprint is re-imported. AnimBlueprints
	 * keep their AnimGraph page in FunctionGraphs (FindAnimGraph walks exactly
	 * that array) - garbaging it wiped the whole anim graph and the deferred
	 * compile died on "Expected at least one animation node" (08.25 crash:
	 * AV in the AB finalize lambda). */
	if (!InBlueprint->IsA<UAnimBlueprint>()) {
		for (UEdGraph* Graph : InBlueprint->FunctionGraphs) {
			if (Graph) {
				Graph->MarkAsGarbage();
			}
		}
		InBlueprint->FunctionGraphs.Empty();
	}

	int32 EventX = 0;

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->GetType() != TEXT("Function")) continue;

		const TSharedPtr<FJsonObject>& FuncJson = Export->JsonObject;
		if (!FuncJson.IsValid()) continue;

		FString FuncName;
		if (!FuncJson->TryGetStringField(TEXT("Name"), FuncName)) continue;

		FString FuncFlags;
		if (FuncJson->HasField(TEXT("FunctionFlags"))) {
			FuncFlags = FuncJson->GetStringField(TEXT("FunctionFlags"));
		}

		/* Skip internal functions */
		if (FuncName.StartsWith(TEXT("ExecuteUbergraph_"))) continue;
		if (FuncName == TEXT("UserConstructionScript")) continue;
		/* AnimBP anim-graph entry thunk - the AnimGraph page is not a callable
		 * function; a hollow UFunction with this name only confuses skeleton
		 * regeneration on editor open. */
		if (FuncName == TEXT("AnimGraph")) continue;
		/* Compiler thunks for anim-node input pins - the anim importer binds
		 * those inputs via PropertyBindings, they are not user functions. */
		if (FuncName.StartsWith(TEXT("EvaluateGraphExposedInputs_"))) continue;

		/* Skip native overrides (ReceiveBeginPlay/ReceiveTick on Actor,
		 * Construct/PreConstruct/Tick on UserWidget, ...). The JSON records an
		 * override by pointing SuperStruct at the native UFunction (e.g.
		 * Class'Actor:ReceiveBeginPlay' under /Script/Engine). Scaffolding a stub
		 * UFunction with the same name makes the K2 compiler log "name conflicts
		 * with a native '...' function" on every stub compile, and dependents do
		 * not call it - the full import decompiles its body into the event node
		 * instead. */
		{
			const TSharedPtr<FJsonObject>* SuperStructObj = nullptr;
			if (FuncJson->TryGetObjectField(TEXT("SuperStruct"), SuperStructObj))
			{
				FString SuperObjectPath;
				if ((*SuperStructObj)->TryGetStringField(TEXT("ObjectPath"), SuperObjectPath)
					&& SuperObjectPath.StartsWith(TEXT("/Script/")))
				{
					continue;
				}
			}
		}

		/* Create UFunction on the class */
		UFunction* Func = NewObject<UFunction>(GeneratedClass, FName(*FuncName), RF_Public | RF_Standalone);

		EFunctionFlags FnFlags = FBlueprintBytecodeImporter::ParseFunctionFlags(FuncFlags);
		if (FnFlags == (EFunctionFlags)0) {
			FnFlags = (EFunctionFlags)(FUNC_Event | FUNC_Public | FUNC_BlueprintEvent);
		}
		Func->FunctionFlags = FnFlags;

		/* Walk ChildProperties so the stub carries its real signature (params/return)
		 * and its local variables, mirroring the bytecode importer. Callers resolve
		 * these stubs before the blueprint compiles, so an empty signature leaves
		 * target pins blank and everything downstream of them errors. */
		TArray<TSharedPtr<FJsonObject>> FuncChildProps;
		const TArray<TSharedPtr<FJsonValue>>* FuncChildPropValues = nullptr;
		if (FuncJson->TryGetArrayField(TEXT("ChildProperties"), FuncChildPropValues)) {
			for (const TSharedPtr<FJsonValue>& Value : *FuncChildPropValues) {
				if (TSharedPtr<FJsonObject> Obj = Value->AsObject()) {
					FuncChildProps.Add(Obj);
				}
			}
		}

		FBlueprintBytecodeImporter::PopulateFunctionProperties(Func, FuncChildProps);

		GeneratedClass->AddFunctionToFunctionMap(Func, FName(*FuncName));

		/* Link the scaffold function into the class's Children field chain, the way
		 * the K2 compiler links compiled functions. ResolveMember resolves
		 * function-scoped locals via FindUField<UStruct>(Class, FuncName), which
		 * walks Children - without this link, local-scope variable pins cannot be
		 * created during graph building. */
		Func->Next = GeneratedClass->Children;
		GeneratedClass->Children = Func;

		/* Mirror the scaffold on the skeleton class. When the editor opens this
		 * stub, Kismet regenerates the skeleton class from the graphs; if the
		 * entry node's function reference cannot resolve on the skeleton, the
		 * compiler treats the graph as a brand-new function and auto-adds a
		 * rename-ready duplicate. Registering on the skeleton keeps the reference
		 * valid across the editor-open recompile. */
		if (InBlueprint && InBlueprint->SkeletonGeneratedClass) {
			UFunction* SkeletonFunc = NewObject<UFunction>(InBlueprint->SkeletonGeneratedClass, FName(*FuncName), RF_Public | RF_Standalone);
			SkeletonFunc->FunctionFlags = FnFlags;
			FBlueprintBytecodeImporter::PopulateFunctionProperties(SkeletonFunc, FuncChildProps);
			InBlueprint->SkeletonGeneratedClass->AddFunctionToFunctionMap(SkeletonFunc, FName(*FuncName));
		}

		/* Create event node for real events only. Every Blueprint function carries
		 * FUNC_BlueprintEvent, so that alone is not a discriminator - a callable
		 * function (e.g. LoadPreference on GI_Data) would otherwise be imported as
		 * a CustomEvent node and lose its signature. Real events also carry
		 * FUNC_Event.
		 *
		 * Compiler-generated input/component event thunks carry only
		 * FUNC_BlueprintEvent (no FUNC_Event), so without the name check below
		 * they fall into the function-graph branch and pollute the FUNCTIONS
		 * list. They are hollow shells here - dependents only ever need them as
		 * referenceable UFunctions - but a bare UFunction is dropped on the next
		 * compile, so they get the CustomEvent-node backing like real events
		 * (name prefixes verified in engine source: K2Node_InputKey.cpp,
		 * K2Node_InputAxisEvent.cpp, K2Node_InputTouch.cpp,
		 * K2Node_ActorBoundEvent.cpp). */
		const bool bCompilerGeneratedThunk =
			FuncName.StartsWith(TEXT("InpActEvt_")) ||
			FuncName.StartsWith(TEXT("InpAxisEvt_")) ||
			FuncName.StartsWith(TEXT("InpAxisKeyEvt_")) ||
			FuncName.StartsWith(TEXT("InpTchEvt_")) ||
			FuncName.StartsWith(TEXT("BndEvt__"));

		if ((FuncFlags.Contains(TEXT("FUNC_Event")) || bCompilerGeneratedThunk) && EventGraph) {
			UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
			EventNode->CreateNewGuid();
			EventNode->SetFlags(RF_Transactional);
			EventNode->NodePosX = EventX;
			EventNode->NodePosY = 0;
			EventNode->CustomFunctionName = FName(*FuncName);
			EventGraph->AddNode(EventNode, true, true);
			EventNode->AllocateDefaultPins();
			EventX += 400;
		} else {
			/* No EventGraph to back the function with a CustomEvent node - e.g.
			 * a BlueprintFunctionLibrary, which CreateBlueprint gives no event
			 * graph. A bare UFunction object (added above) is dropped the next
			 * time the blueprint compiles, so back the function with a real
			 * function graph (entry + result nodes) that the compiler turns back
			 * into a UFunction on the GeneratedClass. Without this the function
			 * simply vanishes and callers can't resolve it. */
			UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
				InBlueprint,
				FName(*FuncName),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass()
			);
			if (FunctionGraph) {
				FunctionGraph->bAllowDeletion = false;
				InBlueprint->FunctionGraphs.Add(FunctionGraph);

				/* Entry node - link to the UFunction so pins can be made from
				 * its signature. FunctionReference gets no parent class, and the
				 * pins are created directly from the UFunction, mirroring the
				 * bytecode importer's CreateEntryNode. */
				UK2Node_FunctionEntry* EntryNode = NewObject<UK2Node_FunctionEntry>(FunctionGraph);
				EntryNode->CreateNewGuid();
				EntryNode->SetFlags(RF_Transactional);
				EntryNode->NodePosX = -600;
				EntryNode->NodePosY = 0;
				EntryNode->CustomGeneratedFunctionName = FName(*FuncName);
				EntryNode->FunctionReference.SetExternalMember(Func->GetFName(), nullptr);
				EntryNode->bIsEditable = true;

				EFunctionFlags EntryFlags = FBlueprintBytecodeImporter::ParseFunctionFlags(FuncFlags);
				if (EntryFlags == (EFunctionFlags)0) {
					EntryFlags = (EFunctionFlags)(FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
				}
				EntryNode->AddExtraFlags(EntryFlags);

				FunctionGraph->AddNode(EntryNode, true, true);
				EntryNode->AllocateDefaultPins();
				/* CreateUserDefinedPinsForFunctionEntryExit would suffix every
				 * param pin ("LoadingSource1") via CreateUniquePinName because the
				 * name collides with the UFunction's own property. Create with
				 * bUseUniqueName=false so pins keep the exact param names. */
				{
					const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
					for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
					{
						FProperty* Param = *PropIt;
						const bool bIsFunctionInput = !Param->HasAnyPropertyFlags(CPF_OutParm) || Param->HasAnyPropertyFlags(CPF_ReferenceParm);
						if (bIsFunctionInput)
						{
							FEdGraphPinType PinType;
							K2Schema->ConvertPropertyToPinType(Param, PinType);
							EntryNode->CreateUserDefinedPin(Param->GetFName(), PinType, EGPD_Output, /*bUseUniqueName=*/ false);
						}
					}
				}

				/* Declare the function's local variables on the entry node so they
				 * survive compilation and show up in the editor. Every non-parameter
				 * ChildProperty that isn't an expression slot is a function local. */
				for (const TSharedPtr<FJsonObject>& LocalJson : FuncChildProps) {
					if (!LocalJson.IsValid()) continue;

					const FString LocalName = LocalJson->GetStringField(TEXT("Name"));

					/* Call results and node temps are pure expression slots, not locals */
					if (LocalName.StartsWith(TEXT("CallFunc_")) || LocalName.StartsWith(TEXT("K2Node_"))) continue;

					const FString LocalFlags = LocalJson->HasField(TEXT("PropertyFlags"))
						? LocalJson->GetStringField(TEXT("PropertyFlags")) : TEXT("");
					if (LocalFlags.Contains(TEXT("Parm"))) continue;

					if (FindFProperty<FProperty>(Func, FName(*LocalName))) continue;

					FEdGraphPinType PinType;
					if (!FBlueprintVariables::GetPinType(LocalJson, PinType)) continue;

					FProperty* LocalProp = FBlueprintBytecodeImporter::CreatePropertyFromJson(Func, LocalName, LocalJson);
					if (!LocalProp) continue;
					LocalProp->PropertyFlags |= CPF_BlueprintVisible;
					Func->AddCppProperty(LocalProp);

					FBPVariableDescription Desc;
					Desc.VarName = FName(*LocalName);
					Desc.VarGuid = FGuid::NewGuid();
					Desc.VarType = PinType;
					Desc.FriendlyName = LocalName;
					Desc.Category = FText::GetEmpty();
					Desc.PropertyFlags = CPF_BlueprintVisible;
					EntryNode->LocalVariables.Add(Desc);
				}

				if (FuncChildProps.Num() > 0) {
					Func->Bind();
					Func->StaticLink(true);
				}

				/* Result node so the graph is structurally complete. */
				UK2Node_FunctionResult* ResultNode = NewObject<UK2Node_FunctionResult>(FunctionGraph);
				ResultNode->CreateNewGuid();
				ResultNode->SetFlags(RF_Transactional);
				ResultNode->NodePosX = 600;
				ResultNode->NodePosY = 0;
				ResultNode->FunctionReference.SetExternalMember(Func->GetFName(), nullptr);
				ResultNode->bIsEditable = true;
				FunctionGraph->AddNode(ResultNode, true, true);
				ResultNode->AllocateDefaultPins();
				{
					const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
					for (TFieldIterator<FProperty> PropIt(Func); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
					{
						FProperty* Param = *PropIt;
						const bool bIsFunctionInput = !Param->HasAnyPropertyFlags(CPF_OutParm) || Param->HasAnyPropertyFlags(CPF_ReferenceParm);
						if (!bIsFunctionInput)
						{
							FEdGraphPinType PinType;
							K2Schema->ConvertPropertyToPinType(Param, PinType);
							ResultNode->CreateUserDefinedPin(Param->GetFName(), PinType, EGPD_Input, /*bUseUniqueName=*/ false);
						}
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Stub: created function stubs for %s"), *GeneratedClass->GetName());
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
	FUObjectExportContainer* Container = GetContainer();
	if (!Container)
	{
		UE_LOG(LogTemp, Warning, TEXT("No container found"));
		return;
	}
	RunBlueprintBytecodePass(Blueprint, GetAssetDataAsValue().JsonObject, Container->JsonObjects);
}

void RunBlueprintBytecodePass(UBlueprint* InBlueprint, const TSharedPtr<FJsonObject>& ClassExportJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(InBlueprint->GeneratedClass);
	if (!GeneratedClass || !InBlueprint)
	{
		UE_LOG(LogTemp, Warning, TEXT("No generated class found"));
		return;
	}

	if (JsonObjects.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("No JSON objects found"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("Found %d JSON exports"), JsonObjects.Num());

	// Create bytecode importer
	FBlueprintBytecodeImporter BytecodeImporter(InBlueprint, GeneratedClass);

	// Process dynamic bindings first (from class properties)
	if (ClassExportJson.IsValid() && ClassExportJson->HasField(TEXT("DynamicBindingObjects")))
	{
		BytecodeImporter.ProcessDynamicBindings(ClassExportJson, JsonObjects);
	}

	// Process functions
	BytecodeImporter.ProcessFunctions(JsonObjects);

	// Mark blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(InBlueprint);

	UE_LOG(LogTemp, Log, TEXT("Bytecode processing completed"));
}