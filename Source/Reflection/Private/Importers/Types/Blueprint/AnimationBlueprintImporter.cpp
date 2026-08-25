/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/AnimationBlueprintImporter.h"

#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendListBase.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Engine/EngineUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimSequence.h"

#include "Importers/Types/Blueprint/Utilities/AnimationBlueprintUtilities.h"
#include "Importers/Types/Blueprint/Utilities/AnimNodeLayoutUtillties.h"
#include "Importers/Types/Blueprint/Utilities/StateMachineUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Blueprint/BlueprintImporter.h"
#include "Importers/Types/Blueprint/BlueprintStubFactory.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "UObject/UnrealTypePrivate.h"
#endif

#include "Utilities/SehHelpers.h"

bool IAnimationBlueprintImporter::Import() {
	/* Imports always target the package path from the JSON export, never whatever
	 * happens to be selected in the content browser. */
	if (GetPackage()) {
		/* Reflecting the same animation blueprint a second time lands on an asset that already exists,
		 * and FKismetEditorUtilities::CreateBlueprint asserts outright when any blueprint of that name
		 * is already in the package. Reuse it in place instead; CreateGraph clears the graph out before
		 * rebuilding it. Same handling as IBlueprintImporter::CreateAsset. */
		/* FindObject mirrors the assert's own lookup. No LoadObject fallback: GetPackage() was already
		 * fully loaded by CreateAssetPackageSafe just before this runs, so anything on disk is already
		 * resident in memory - LoadObject on the same path would re-enter the loader for a package
		 * still mid-load and trigger a recursive partial load. */
		UBlueprint* ExistingBlueprint = FindObject<UBlueprint>(GetPackage(), *GetAssetName());

		if (ExistingBlueprint) {
			AnimBlueprint = Cast<UAnimBlueprint>(ExistingBlueprint);

			/* Something of that name is there but isn't an animation blueprint, so it can neither be reused nor
			 * created over. Bail rather than let the assert take the editor down. */
			if (!AnimBlueprint) {
				AppendNotification(
					FText::FromString("Asset Name Already Taken"),
					FText::FromString(FString::Printf(TEXT("'%s' already exists and is not an Animation Blueprint. Rename or delete it before reflecting."), *GetAssetName())),
					3.0f,
					SNotificationItem::CS_Fail,
					true,
					350.0f
				);

				return false;
			}
		}
	}

	if (!AnimBlueprint) {
		const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
		UClass* ParentClass = LoadClass(SuperStruct);

		AnimBlueprint = CreateAnimBlueprint(ParentClass);
	}

	if (!AnimBlueprint) return false;

	const TSharedPtr<FJsonObject> RootAnimNodeDefaults = GetExportStartingWith("Default__", "Name", GetContainer()->JsonObjects);
	if (!RootAnimNodeDefaults.IsValid()) return false;
	
	RootAnimNodeProperties = RootAnimNodeDefaults->GetObjectField(TEXT("Properties"));
	if (!RootAnimNodeProperties.IsValid()) return false;

	/*
	 * The variables the blueprint declares have to exist before the class default object below can
	 * put anything in them. ChildProperties holds them alongside the anim graph node state, which
	 * FBlueprintVariables filters out.
	 */
	if (const TArray<TSharedPtr<FJsonValue>>* ChildProperties; GetAssetExport()->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) {
		/* A re-import into an existing blueprint removes variables the previous import
		 * added that the JSON no longer declares, then rebuilds the ones it does. */
		FBlueprintVariables::ClearStaleVariables(AnimBlueprint, *ChildProperties);

		if (FBlueprintVariables::Construct(AnimBlueprint, *ChildProperties) > 0) {
			/* The properties only appear on the generated class once it recompiles */
			CompileBlueprintSafe(AnimBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
	}

	/* UClass::GetDefaultObject only became const later on */
#if UE4_24_BELOW
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass);
#else
	const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass);
#endif
	GetObjectSerializer()->Exports = GetContainer()->JsonObjects;
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(RootAnimNodeProperties, {
		"RootComponent"
	}), GeneratedClass->GetDefaultObject());

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		"FuncMap",
		"bCooked",
		"Children",
		"RootAnimNodeIndex",
		"UberGraphFunction",
		"UberGraphFramePointerProperty",
		"SuperStruct"
	}), AnimBlueprint);

	/* TargetSkeleton is a TObjectPtr<USkeleton> that the generic deserializer may not
	 * resolve if the asset wasn't pre-loaded.  Parse the JSON reference directly and
	 * load the skeleton so we can find a skeleton-compatible fallback sequence. */
	if (!AnimBlueprint->TargetSkeleton) {
		/* GetAssetData() returns the CDO export which doesn't carry TargetSkeleton.
		 * Walk all exports in the container to find one whose Properties contain it. */
		TSharedPtr<FJsonObject> SkeletonRef;
		for (const TSharedPtr<FJsonValue>& JsonVal : GetContainer()->JsonObjects) {
			if (!JsonVal.IsValid() || JsonVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject> Obj = JsonVal->AsObject();
			if (!Obj.IsValid() || !Obj->HasField(TEXT("Properties"))) continue;
			const TSharedPtr<FJsonObject> Props = Obj->GetObjectField(TEXT("Properties"));
			if (Props.IsValid() && Props->HasField(TEXT("TargetSkeleton"))) {
				SkeletonRef = Props->GetObjectField(TEXT("TargetSkeleton"));
				break;
			}
		}

		if (SkeletonRef.IsValid() && SkeletonRef->HasField(TEXT("ObjectPath"))) {
			const FString ObjectPath = SkeletonRef->GetStringField(TEXT("ObjectPath"));
			AnimBlueprint->TargetSkeleton = LoadObject<USkeleton>(nullptr, *ObjectPath);
			if (AnimBlueprint->TargetSkeleton) {
				UE_LOG(LogReflection, Log, TEXT("Loaded TargetSkeleton \"%s\" from JSON ObjectPath \"%s\""),
					*AnimBlueprint->TargetSkeleton->GetName(), *ObjectPath);
			}
		} else {
			UE_LOG(LogReflection, Warning, TEXT("Could not find TargetSkeleton in JSON for \"%s\""),
				*AnimBlueprint->GetName());
		}
	}

	ResolveFallbackAnimSequence();

	/* Newer Unreal Engine versions use CopyRecords and SerializedSparseClassData */
	if (RootAnimNodeDefaults->HasField(TEXT("SerializedSparseClassData"))) {
		SerializedSparseClassData = RootAnimNodeDefaults->GetObjectField(TEXT("SerializedSparseClassData"));
	}

	/* Compiled exports carry each node's variable bindings as property access copy records
	 * rather than editor-side EvaluateGraphExposedInputs; recover them up front so every
	 * node deserialized below can have its pins bound. */
	BuildPropertyAccessBindings();

	/* Array of sync group names cached to use at later points of importing */
	if (GetAssetData()->HasField(TEXT("SyncGroupNames"))) {
		for (const auto& SyncGroupNameValue : GetAssetData()->GetArrayField(TEXT("SyncGroupNames"))) {
			SyncGroupNames.Add(SyncGroupNameValue->AsString());
		}
	}

	/* Filter AnimNodeProperties ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	FilterAnimGraphNodeProperties(RootAnimNodeProperties);
	ProcessEvaluateGraphExposedInputs(RootAnimNodeProperties);

	/* Parse LinkIDs to proper Node IDs ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	RootAnimNodeProperties->Values.GetKeys(NodesKeys);
	
	ReversedNodesKeys = NodesKeys;
	Algo::Reverse(ReversedNodesKeys);

	for (const FString& Key : NodesKeys) {
		TSharedPtr<FJsonValue> NodeValue = RootAnimNodeProperties->Values.FindChecked(Key);
		if (!NodeValue.IsValid()) continue;
		
		ReplaceLinkID(NodeValue, NodesKeys);
		RootAnimNodeProperties->Values[Key] = NodeValue;
	}

	/* Sets "State" and "Machine" for each state result */
	if (GetAssetData()->HasField(TEXT("BakedStateMachines"))) {
		BakedStateMachines = GetAssetData()->GetArrayField(TEXT("BakedStateMachines"));
    
		for (const TSharedPtr<FJsonValue>& MachineValue : BakedStateMachines) {
			const TSharedPtr<FJsonObject> MachineObject = MachineValue->AsObject();
			const TArray<TSharedPtr<FJsonValue>> States = MachineObject->GetArrayField(TEXT("States"));
			const FString MachineName = MachineObject->GetStringField(TEXT("MachineName"));
        
			/* Loop through each state */
			for (const TSharedPtr<FJsonValue>& StateValue : States) {
				const TSharedPtr<FJsonObject> StateObject = StateValue->AsObject();
				const int32 StateRootNodeIndex = StateObject->GetIntegerField(TEXT("StateRootNodeIndex"));
            
				if (StateRootNodeIndex == -1 || !ReversedNodesKeys.IsValidIndex(StateRootNodeIndex)) {
					continue;
				}
            
				const FString StartKey = ReversedNodesKeys[StateRootNodeIndex];
				HarvestAndTagConnectedStateMachineNodes(StartKey, StateObject->GetStringField(TEXT("StateName")), MachineName, RootAnimNodeProperties->Values);
			}
		}
	}

	/* Separate main graph nodes (without "State" and "Machine") into RootGraphAnimProperties ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	const TSharedPtr<FJsonObject> RootGraphAnimProperties = MakeShared<FJsonObject>(); {
		for (const FString& Key : NodesKeys) {
			const TSharedPtr<FJsonValue> NodeValue = RootAnimNodeProperties->Values.FindChecked(Key);
		
			if (NodeValue->Type == EJson::Object) {
				const TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
			
				if (!NodeObject->HasField(TEXT("State")) && !NodeObject->HasField(TEXT("Machine"))) {
					RootGraphAnimProperties->SetObjectField(Key, NodeObject);
				}
			}
		}
	}

	UEdGraph* AnimGraph = FindAnimGraph(AnimBlueprint);
	
	if (AnimGraph) {
		AnimGraph->SubGraphs.Empty();
	}

	CreateGraph(RootGraphAnimProperties, AnimGraph, RootAnimNodeContainer);

	/* Some exports carry a leftover Output Pose node in the AnimGraph alongside the wired one, and
	 * the compiler rejects a graph with two roots ("Expected only one root node in graph AnimGraph").
	 * The dead root's Result.LinkID stayed a literal -1 while the wired root's was rewritten to a
	 * node key string by ReplaceLinkID, so a root whose Result.LinkID is not a string never links
	 * into the graph. Drop every such root, keeping the wired one.
	 *
	 * This cannot read Root->Node.Result.LinkID because HandleNodeDeserialization blacklists the
	 * LinkID property, leaving every root at INDEX_NONE. */
	if (AnimGraph) {
		TArray<UAnimGraphNode_Root*> Roots;
		AnimGraph->GetNodesOfClass(Roots);

		if (Roots.Num() > 1) {
			TArray<UAnimGraphNode_Root*> WiredRoots;

			for (const FUObjectExport* Export : RootAnimNodeContainer->Exports) {
				if (Export->Object == nullptr || !Export->IsJsonValid()) continue;

				if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Export->Object)) {
					const TSharedPtr<FJsonObject> Result = Export->JsonObject->GetObjectField(TEXT("Result"));

					if (Result.IsValid() && Result->HasTypedField<EJson::String>(TEXT("LinkID"))) {
						WiredRoots.Add(Root);
					}
				}
			}

			for (UAnimGraphNode_Root* Root : Roots) {
				if (!WiredRoots.Contains(Root)) {
					AnimGraph->RemoveNode(Root);
					Root->ConditionalBeginDestroy();
				}
			}
		}
	}

	/* Function/event reconstruction, on top of the untouched AnimGraph pipeline:
	 *  - stub imports (dependency AnimBPs) get hollow function/event UFunction
	 *    shells so dependent BPs resolve casts and calls;
	 *  - real imports get the event graph + functions decompiled from bytecode
	 *    (ubergraph body, BlueprintInitializeAnimation/UpdateAnimation, user
	 *    functions). The "AnimGraph" entry thunk and the compiler-generated
	 *    EvaluateGraphExposedInputs_* anim-node thunks are skipped inside both
	 *    paths. The deferred finalize compile below covers either addition. */
	if (FBlueprintStubFactory::IsStubImport(GetSourceFile())) {
		FMetaData& PackageMeta = AnimBlueprint->GetPackage()->GetMetaData();
		PackageMeta.SetValue(AnimBlueprint, TEXT("ReflectionStub"), *GetSourceFile());

		ConstructBlueprintStubFunctions(AnimBlueprint, GetContainer());
		UE_LOG(LogTemp, Log, TEXT("AnimBP stub import - creating function stubs for: %s"), *GetSourceFile());
	} else {
		RunBlueprintBytecodePass(AnimBlueprint, GetAssetData(), GetContainer()->JsonObjects);
	}

	/* Capture every graph node and container export (and any invalid object among them) before
	 * the deferred compile, so the log shows the exact state the compile-time reference walk
	 * descends into when it hits the IsValidLowLevel assert. */
	DumpAnimBlueprintPreCompile(AnimBlueprint, GeneratedClass, AnimGraph, RootAnimNodeContainer);

	/* CompileBlueprint triggers ControlRig compilation code that can dereference null on
	 * corrupted or partially loaded ControlRig assets - and a ControlRig this same batch is
	 * still building (a circular reference) is exactly that: a shell whose hierarchy and VM
	 * are not populated yet. Deferred to the registry's final phase (RunFinalPhase), which
	 * runs only after every export in the batch - circular dependencies included - has been
	 * fully populated, so the ControlRig this graph references is complete before its class
	 * is instantiated during compilation. */
	FAssetDependencyRegistry::Get().RequestFinalize([this]() {
		/* Invalidate LayeredBoneBlend PerBoneBlendWeights before the first compile so the
		 * cloned/generated-class nodes also carry stale GUIDs.  At runtime, UpdateCachedBoneData
		 * will see ArePerBoneBlendWeightsValid() == false and rebuild from the skeleton,
		 * preventing SourceIndex OOB in BlendPosesPerBoneFilter. */
		UEdGraph* PreGraph = FindAnimGraph(AnimBlueprint);
		if (PreGraph) {
			for (UEdGraphNode* GN : PreGraph->Nodes) {
				if (UAnimGraphNode_LayeredBoneBlend* LBB = Cast<UAnimGraphNode_LayeredBoneBlend>(GN)) {
					LBB->Node.InvalidatePerBoneBlendWeights();
					UE_LOG(LogReflection, Log, TEXT("[DIAG-FINAL] Invalidated PerBoneBlendWeights on \"%s\""),
						*GN->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
		}

		/* First compile: establishes the generated class skeleton.  Anim graph nodes are
		 * cloned into a ConsolidatedEventGraph by FEdGraphUtilities::CloneGraph; the clone's
		 * Instanced Binding subobjects lose their PropertyBindings TMap entries because
		 * DuplicateObject serialises through FStructuredArchive which doesn't round-trip the
		 * TMap hash table.  AlwaysDynamicProperties (UPROPERTY TSet) survives the clone,
		 * suppressing the "references an unknown Anim X" validation for bound nodes. */
		CompileBlueprintSafe(AnimBlueprint);

		if (DeferredBindingNodes.Num() > 0) {
			for (const auto& Pair : DeferredBindingNodes) {
				UAnimGraphNode_Base* Node = Pair.Value;
				if (!IsValid(Node)) continue;

				const TArray<FCompiledPinBinding>* NodeBindings = CompiledPinBindings.Find(Pair.Key);
				if (!NodeBindings) continue;

				for (const FCompiledPinBinding& Binding : *NodeBindings) {
					const FName PinName = ResolveCompiledBindingPinName(Node, Binding.DestPath);
					if (PinName.IsNone()) continue;

					FAnimGraphNodePropertyBinding PropertyBinding;
					PropertyBinding.PropertyName = PinName;
					PropertyBinding.bIsBound = true;

					for (const FString& Segment : Binding.SourcePath) {
						PropertyBinding.PropertyPath.Add(Segment);
					}

					FString PathAsText;
					for (const FString& Segment : Binding.SourcePath) {
						if (!PathAsText.IsEmpty()) PathAsText += TEXT(".");
						PathAsText += Segment;
					}
					PropertyBinding.PathAsText = FText::FromString(PathAsText);

					if (const UEdGraphPin* DestinationPin = Node->FindPin(PinName, EGPD_Input)) {
						PropertyBinding.PinType = DestinationPin->PinType;
					}

					AddPropertyBinding(Node, PinName, PropertyBinding);
				}

				Node->ReconstructNode();

				for (const FCompiledPinBinding& Binding : *NodeBindings) {
					const FName PinName = ResolveCompiledBindingPinName(Node, Binding.DestPath);
					if (PinName.IsNone()) continue;

					Node->AlwaysDynamicProperties.Add(PinName);

					if (FallbackAnimSequence && PinName == FName("Sequence")) {
						if (UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(Node)) {
							SeqNode->Node.SetSequence(FallbackAnimSequence);
						}
						if (UAnimGraphNode_SequenceEvaluator* EvalNode = Cast<UAnimGraphNode_SequenceEvaluator>(Node)) {
							EvalNode->Node.SetSequence(FallbackAnimSequence);
						}
						if (UEdGraphPin* Pin = Node->FindPin(PinName, EGPD_Input)) {
							Pin->DefaultObject = FallbackAnimSequence;
						}
					}
				}
			}

			/* Apply fallback sequence to ALL SequencePlayer nodes with null sequences.
			 * Nodes using EvaluateGraphExposedInputs skip ApplyCompiledPinBindings entirely
			 * (line 954), so the fallback was never set on them.  At runtime, the evaluator
			 * reads Node.Sequence directly (not Pin->DefaultObject), so both must be set. */
			UE_LOG(LogReflection, Warning, TEXT("[DIAG-FINAL] FallbackAnimSequence=%s AnimBlueprint=%s"),
				FallbackAnimSequence ? *FallbackAnimSequence->GetName() : TEXT("NULL"),
				AnimBlueprint ? *AnimBlueprint->GetName() : TEXT("NULL"));

			/* Apply fallback sequence to ALL SequencePlayer nodes with null sequences.
			 * Nodes using EvaluateGraphExposedInputs skip ApplyCompiledPinBindings entirely
			 * (line 954), so the fallback was never set on them.  At runtime, the evaluator
			 * reads Node.Sequence directly (not Pin->DefaultObject), so both must be set. */
			if (FallbackAnimSequence) {
				UEdGraph* Graph = FindAnimGraph(AnimBlueprint);
				if (Graph) {
					int32 FixedCount = 0;
					for (UEdGraphNode* GraphNode : Graph->Nodes) {
						bool bFixed = false;

						if (UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(GraphNode)) {
							if (!SeqNode->Node.GetSequence()) {
								SeqNode->Node.SetSequence(FallbackAnimSequence);
								bFixed = true;
							}
						}
						if (UAnimGraphNode_SequenceEvaluator* EvalNode = Cast<UAnimGraphNode_SequenceEvaluator>(GraphNode)) {
							if (!EvalNode->Node.GetSequence()) {
								EvalNode->Node.SetSequence(FallbackAnimSequence);
								bFixed = true;
							}
						}

						if (bFixed) {
							if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(GraphNode)) {
								AnimNode->AlwaysDynamicProperties.Add(FName("Sequence"));
							}
							if (UEdGraphPin* Pin = GraphNode->FindPin(FName("Sequence"), EGPD_Input)) {
								Pin->DefaultObject = FallbackAnimSequence;
							}
							FixedCount++;
						}
					}
					if (FixedCount > 0) {
						UE_LOG(LogReflection, Warning, TEXT("[DIAG-FINAL] Fixed %d null SequencePlayer/Evaluator nodes"), FixedCount);
					}
				}
			}

			CompileBlueprintSafe(AnimBlueprint);
		}
	});
	
	if (AnimBlueprint == nullptr) {
		UE_LOG(LogReflection, Error, TEXT("AnimationBlueprintImporter: \"%s\" produced no blueprint, skipping asset creation."), *GetAssetName());
		return false;
	}

	return OnAssetCreation(AnimBlueprint);
}

UAnimBlueprint* IAnimationBlueprintImporter::CreateAnimBlueprint(UClass* ParentClass) {
	const EBlueprintType BlueprintType = GetBlueprintType(ParentClass);

	if (UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetPackage(), FName(*GetAssetName()), BlueprintType, UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass())) {
		return Cast<UAnimBlueprint>(CreateAsset(Blueprint));
	}

	return nullptr;
}

UObject* IAnimationBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	if (CreatedAsset == nullptr) {
		/* Shell phase: same creation path as Import(), so the shell is exactly the blueprint
		 * the populate phase picks back up via GetSelectedAsset/FindObject. Returns the
		 * generated class - the object class-style references across the batch resolve to. */
		if (AnimBlueprint == nullptr) {
			const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
			AnimBlueprint = CreateAnimBlueprint(LoadShellParentClass(SuperStruct, UAnimInstance::StaticClass()));
		}

		if (AnimBlueprint != nullptr) {
			return IImporter::CreateAsset(AnimBlueprint->GeneratedClass);
		}

		return nullptr;
	}

	return IImporter::CreateAsset(CreatedAsset);
}

void IAnimationBlueprintImporter::CreateGraph(const TSharedPtr<FJsonObject>& AnimNodeProperties, UEdGraph* AnimGraph, FUObjectExportContainer* Container) {
	/* Remove all pre-existing nodes */
	if (AnimGraph) {
		for (UEdGraphNode* Node : AnimGraph->Nodes) {
			if (Node) {
				Node->BreakAllNodeLinks();
				Node->ConditionalBeginDestroy();
			}
		}
        
		AnimGraph->Nodes.Empty();
		AnimGraph->SubGraphs.Empty();
	}
	
	CreateAnimGraphNodes(AnimGraph, AnimNodeProperties, *Container);
	AddNodesToGraph(AnimGraph, Container);

	HandleNodeDeserialization(Container);
	ConnectAnimGraphNodes(Container, AnimGraph);
	AutoLayoutAnimGraphNodes(Container->Exports);

	for (const FUObjectExport* ExportNode : Container->Exports) {
		const TSharedPtr<FJsonObject> ExportJsonObject = ExportNode->JsonObject;
		
		if (UAnimGraphNode_StateMachine* StateMachine = Cast<UAnimGraphNode_StateMachine>(ExportNode->Object)) {
			UAnimationStateMachineGraph* EditorStateMachineGraph = CastChecked<UAnimationStateMachineGraph>(FBlueprintEditorUtils::CreateNewGraph(StateMachine, NAME_None, UAnimationStateMachineGraph::StaticClass(), UAnimationStateMachineSchema::StaticClass()));
			EditorStateMachineGraph->OwnerAnimGraphNode = StateMachine;

			const TSharedPtr<FJsonObject> StateMachineObject = BakedStateMachines[ExportJsonObject->GetIntegerField(TEXT("StateMachineIndexInClass"))]->AsObject();
					
			FString MachineName = StateMachineObject->GetStringField(TEXT("MachineName"));
			EditorStateMachineGraph->Rename(*MachineName);

			const UEdGraphSchema* Schema = EditorStateMachineGraph->GetSchema();
			Schema->CreateDefaultNodesForGraph(*EditorStateMachineGraph);

			UEdGraph* ParentGraph = StateMachine->GetGraph();
	
			if(ParentGraph->SubGraphs.Find(EditorStateMachineGraph) == INDEX_NONE) {
				ParentGraph->Modify();
				ParentGraph->SubGraphs.Add(EditorStateMachineGraph);
			}

			StateMachine->EditorStateMachineGraph = EditorStateMachineGraph;
			CreateStateMachineGraph(EditorStateMachineGraph, StateMachineObject, GetObjectSerializer(), RootAnimNodeContainer, ReversedNodesKeys, this, AnimBlueprint);

			/* Add nodes to graph */
			if (!StateMachineObject->HasField(TEXT("States"))) continue;

			TArray<TSharedPtr<FJsonValue>> States = StateMachineObject->GetArrayField(TEXT("States"));

			for (const TSharedPtr<FJsonValue>& StateValue : States) {
				const TSharedPtr<FJsonObject> StateObject = StateValue->AsObject();
				FString StateName = StateObject->GetStringField(TEXT("StateName"));

				UAnimationStateGraph* Graph = nullptr;

				for (UEdGraph* SubGraph : EditorStateMachineGraph->SubGraphs) {
					if (SubGraph->GetName() == StateName) {
						Graph = Cast<UAnimationStateGraph>(SubGraph);
					}
				}

				TSharedPtr<FJsonObject> StateMachineAnimNodeProperties = MakeShared<FJsonObject>();

				for (const auto& Pair : RootAnimNodeProperties->Values) {
					const  FString Key = Pair.Key;
					const TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();

					if (!Value.IsValid()) continue;

					if (Value->HasField(TEXT("State")) && Value->HasField(TEXT("Machine"))) {
						const FString NodeStateName = Value->GetStringField(TEXT("State"));
						const FString NodeMachineName = Value->GetStringField(TEXT("Machine"));

						if (StateName == NodeStateName && NodeMachineName == MachineName) {
							StateMachineAnimNodeProperties->SetObjectField(Key, Value);
						}
					}
				}

				if (Graph) {
					FUObjectExportContainer* StateMachineContainer = new FUObjectExportContainer();
					CreateGraph(StateMachineAnimNodeProperties, Graph, StateMachineContainer);

					if (Graph->MyResultNode) {
						Graph->MyResultNode->BreakAllNodeLinks();
						Graph->RemoveNode(Graph->MyResultNode);
						Graph->MyResultNode->ConditionalBeginDestroy();
						Graph->MyResultNode = nullptr;
					}

					for (const FUObjectExport* StateMachineExport : StateMachineContainer->Exports) {
						if (UAnimGraphNode_StateResult* StateResult = Cast<UAnimGraphNode_StateResult>(StateMachineExport->Object)) {
							Graph->MyResultNode = StateResult;
						}
					}
				}
			}
		}
	}
}

void inline LinkPoseInputPin(const FString& PinName, UAnimGraphNode_Base* Node, UAnimGraphNode_Base* TargetNode, UEdGraph* AnimGraph) {
	UEdGraphPin* InputPin = Node->FindPin(PinName, EGPD_Input);
	UEdGraphPin* OutputPin = GetFirstOutputPin(TargetNode);
	
	if (InputPin && OutputPin) {
		InputPin->MakeLinkTo(OutputPin);
		InputPin->DefaultValue.Reset();
		
		Node->Modify();
		TargetNode->Modify();
		AnimGraph->Modify();
	}
}

namespace {
/* Resolves a folded node property (EnumToPoseIndex / BlendTime) down to its flat TArray value
 * in SerializedSparseClassData. Walks the same chain as the fold resolution inside
 * ResolveBlendListByEnumEnum: NodeTypeMap[NodeTypeKey] → NameToIndexMap[FoldKey] is the fold
 * index, AnimNodeData[NodeIndex].Entries[foldIndex] is the flat constant-data property index,
 * and that ChildProperty's name keys the sparse array. Returns false on any missing hop. */
bool ResolveFoldedSparseArray(
	const TSharedPtr<FJsonObject>& ClassProperties,
	const TSharedPtr<FJsonObject>& ConstantData,
	const TSharedPtr<FJsonObject>& SerializedSparseClassData,
	int32 NodeIndex,
	const FString& NodeTypeKey,
	const FString& FoldPropertyKey,
	TArray<TSharedPtr<FJsonValue>>& OutValues) {
	OutValues.Reset();

	if (!ClassProperties.IsValid() || !ConstantData.IsValid() || !SerializedSparseClassData.IsValid()) {
		return false;
	}

	int32 FoldedPropertyIndex = INDEX_NONE;

	if (ClassProperties->HasTypedField<EJson::Array>(TEXT("NodeTypeMap"))) {
		for (const TSharedPtr<FJsonValue>& NodeTypeValue : ClassProperties->GetArrayField(TEXT("NodeTypeMap"))) {
			const TSharedPtr<FJsonObject> NodeTypeObject = NodeTypeValue->AsObject();
			if (!NodeTypeObject.IsValid() || NodeTypeObject->GetStringField(TEXT("Key")) != NodeTypeKey) continue;

			const TSharedPtr<FJsonObject> NameToIndexObject = NodeTypeObject->GetObjectField(TEXT("Value"));
			if (NameToIndexObject.IsValid()) {
				for (const TSharedPtr<FJsonValue>& NameIndexValue : NameToIndexObject->GetArrayField(TEXT("NameToIndexMap"))) {
					const TSharedPtr<FJsonObject> NameIndexObject = NameIndexValue->AsObject();
					if (NameIndexObject.IsValid() && NameIndexObject->GetStringField(TEXT("Key")) == FoldPropertyKey) {
						FoldedPropertyIndex = NameIndexObject->GetIntegerField(TEXT("Value"));
						break;
					}
				}
			}
			break;
		}
	}

	if (FoldedPropertyIndex == INDEX_NONE || !ClassProperties->HasTypedField<EJson::Array>(TEXT("AnimNodeData"))) {
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>> AnimNodeData = ClassProperties->GetArrayField(TEXT("AnimNodeData"));
	if (!AnimNodeData.IsValidIndex(NodeIndex)) return false;

	const TSharedPtr<FJsonObject> NodeData = AnimNodeData[NodeIndex]->AsObject();
	if (!NodeData.IsValid()) return false;

	const TArray<TSharedPtr<FJsonValue>> Entries = NodeData->GetArrayField(TEXT("Entries"));
	if (!Entries.IsValidIndex(FoldedPropertyIndex) || Entries[FoldedPropertyIndex]->Type != EJson::Number) {
		return false;
	}

	const int32 FlatPropertyIndex = static_cast<int32>(Entries[FoldedPropertyIndex]->AsNumber());
	const TArray<TSharedPtr<FJsonValue>> ConstantChildProperties = ConstantData->GetArrayField(TEXT("ChildProperties"));
	if (!ConstantChildProperties.IsValidIndex(FlatPropertyIndex)) return false;

	const TSharedPtr<FJsonObject> FlatProperty = ConstantChildProperties[FlatPropertyIndex]->AsObject();
	if (!FlatProperty.IsValid()) return false;

	const FString FlatPropertyName = FlatProperty->GetStringField(TEXT("Name"));
	if (FlatPropertyName.IsEmpty() || !SerializedSparseClassData->HasTypedField<EJson::Array>(FlatPropertyName)) {
		return false;
	}

	OutValues = SerializedSparseClassData->GetArrayField(FlatPropertyName);
	return OutValues.Num() > 0;
}
}

void IAnimationBlueprintImporter::UpdateBlendListByEnumVisibleEntries(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UEdGraph* AnimGraph) {
	UAnimGraphNode_BlendListByEnum* BlendListByEnum = Cast<UAnimGraphNode_BlendListByEnum>(NodeExport->Object);
	if (!BlendListByEnum || !NodeExport->JsonObject) {
		return;
	}

	/* Recover BoundEnum + EnumToPoseIndex from the generated class data so the node compiles
	 * with its enum mapping (see ResolveBlendListByEnumEnum). */
	ResolveBlendListByEnumEnum(NodeExport, Container, BlendListByEnum);

	/* The runtime BlendPose array was deserialized from the JSON already, so pin BlendPose_i
	 * corresponds one-to-one with BlendPoseArray[i] - BlendPose_0 is the default pose. */
	const TArray<TSharedPtr<FJsonValue>> BlendPoseArray = NodeExport->JsonObject->GetArrayField(TEXT("BlendPose"));

	for (int32 PoseIndex = 0; PoseIndex < BlendPoseArray.Num(); ++PoseIndex) {
		const TSharedPtr<FJsonObject> PoseObject = BlendPoseArray[PoseIndex]->AsObject();
		if (!PoseObject.IsValid() || !PoseObject->HasTypedField<EJson::String>(TEXT("LinkID"))) continue;

		FUObjectExport* TargetNodeExport = Container->Find(PoseObject->GetStringField(TEXT("LinkID")));
		UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(TargetNodeExport->Object);

		LinkPoseInputPin(FString::Printf(TEXT("BlendPose_%d"), PoseIndex), BlendListByEnum, TargetNode, AnimGraph);
	}
}

void IAnimationBlueprintImporter::ResolveBlendListByEnumEnum(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UAnimGraphNode_BlendListByEnum* BlendListByEnum) {
	if (!BlendListByEnum || !SerializedSparseClassData.IsValid()) return;

	const FString NodeName = NodeExport->GetName().ToString();

	/* --- Node index + node type key from the class ChildProperties ------------------------
	 * The generated class lists one StructProperty per anim graph node, contiguously and in
	 * node order. The offset past the first AnimGraphNode_* entry is the node's index into
	 * the class's AnimNodeData; the struct descriptor builds the NodeTypeMap lookup key. */
	const TSharedPtr<FJsonObject> ClassExport = GetAssetExport();
	const TArray<TSharedPtr<FJsonValue>>* ClassChildProperties = nullptr;
	int32 NodeIndex = INDEX_NONE;
	FString NodeTypeKey;

	if (ClassExport.IsValid() && ClassExport->TryGetArrayField(TEXT("ChildProperties"), ClassChildProperties)) {
		int32 FirstNodePropertyIndex = INDEX_NONE;
		for (int32 i = 0; i < ClassChildProperties->Num(); ++i) {
			const TSharedPtr<FJsonObject> Property = (*ClassChildProperties)[i]->AsObject();
			if (Property.IsValid() && Property->GetStringField(TEXT("Name")).StartsWith(TEXT("AnimGraphNode"))) {
				FirstNodePropertyIndex = i;
				break;
			}
		}

		if (FirstNodePropertyIndex != INDEX_NONE) {
			for (int32 i = FirstNodePropertyIndex; i < ClassChildProperties->Num(); ++i) {
				const TSharedPtr<FJsonObject> Property = (*ClassChildProperties)[i]->AsObject();
				if (!Property.IsValid() || Property->GetStringField(TEXT("Name")) != NodeName) continue;

				NodeIndex = i - FirstNodePropertyIndex;

				const TSharedPtr<FJsonObject> Struct = Property->GetObjectField(TEXT("Struct"));
				if (Struct.IsValid()) {
					FString StructName = Struct->GetStringField(TEXT("ObjectName"));
					StructName.RemoveFromStart(TEXT("Class'"));
					StructName.RemoveFromEnd(TEXT("'"));

					NodeTypeKey = FString::Printf(TEXT("Class'%s.%s'"), *Struct->GetStringField(TEXT("ObjectPath")), *StructName);
				}

				break;
			}
		}
	}

	if (NodeIndex == INDEX_NONE || NodeTypeKey.IsEmpty()) return;

	/* --- BoundEnum from the compiled property access copy ---------------------------------
	 * Each node's sparse CopyRecords name the copy that feeds it. The leaf of that copy's
	 * source path is the anim instance property holding the enum the node switches on. */
	UEnum* BoundEnum = nullptr;
	FString SourceLeaf;
	{
		const TSharedPtr<FJsonObject> PropertyAccess = SerializedSparseClassData->GetObjectField(TEXT("AnimBlueprintExtension_PropertyAccess"));
		const TSharedPtr<FJsonObject> Library = PropertyAccess.IsValid() ? PropertyAccess->GetObjectField(TEXT("Library")) : nullptr;

		if (Library.IsValid()) {
			/* PathSegments are a flat list of property path segment names */
			TArray<FString> PathSegments;
			for (const TSharedPtr<FJsonValue>& SegmentValue : Library->GetArrayField(TEXT("PathSegments"))) {
				const TSharedPtr<FJsonObject> SegmentObject = SegmentValue->AsObject();
				if (SegmentObject.IsValid() && SegmentObject->HasTypedField<EJson::String>(TEXT("Name"))) {
					PathSegments.Add(SegmentObject->GetStringField(TEXT("Name")));
				}
			}

			const TArray<TSharedPtr<FJsonValue>> SrcPaths = Library->GetArrayField(TEXT("SrcPaths"));
			const TArray<TSharedPtr<FJsonValue>> CopyBatches = Library->GetArrayField(TEXT("CopyBatchArray"));

			/* Copies are compiled into CopyBatchArray; a node's CopyRecords CopyIndex indexes
			 * the first batch's Copies array (the WorkerThread_Unbatched call site). */
			TArray<TSharedPtr<FJsonObject>> Copies;
			if (CopyBatches.IsValidIndex(0)) {
				const TSharedPtr<FJsonObject> BatchObject = CopyBatches[0]->AsObject();
				if (BatchObject.IsValid()) {
					for (const TSharedPtr<FJsonValue>& CopyValue : BatchObject->GetArrayField(TEXT("Copies"))) {
						const TSharedPtr<FJsonObject> CopyObject = CopyValue->AsObject();
						if (CopyObject.IsValid()) Copies.Add(CopyObject);
					}
				}
			}

			const TSharedPtr<FJsonObject> SparseNode = SerializedSparseClassData->GetObjectField(NodeName);
			if (SparseNode.IsValid() && SparseNode->HasTypedField<EJson::Array>(TEXT("CopyRecords"))) {
				for (const TSharedPtr<FJsonValue>& RecordValue : SparseNode->GetArrayField(TEXT("CopyRecords"))) {
					const TSharedPtr<FJsonObject> RecordObject = RecordValue->AsObject();
					if (!RecordObject.IsValid() || !RecordObject->HasTypedField<EJson::Number>(TEXT("CopyIndex"))) continue;

					const int32 CopyIndex = RecordObject->GetIntegerField(TEXT("CopyIndex"));
					if (!Copies.IsValidIndex(CopyIndex)) continue;

					const TSharedPtr<FJsonObject> CopyObject = Copies[CopyIndex];
					if (!CopyObject.IsValid() || !CopyObject->HasTypedField<EJson::Number>(TEXT("AccessIndex"))) continue;

					const int32 AccessIndex = CopyObject->GetIntegerField(TEXT("AccessIndex"));
					if (!SrcPaths.IsValidIndex(AccessIndex)) continue;

					const TSharedPtr<FJsonObject> SrcPath = SrcPaths[AccessIndex]->AsObject();
					if (!SrcPath.IsValid()) continue;

					FString SourcePath;
					const int32 StartIndex = SrcPath->GetIntegerField(TEXT("PathSegmentStartIndex"));
					const int32 Count = SrcPath->GetIntegerField(TEXT("PathSegmentCount"));
					for (int32 i = StartIndex; i < StartIndex + Count; ++i) {
						if (!PathSegments.IsValidIndex(i)) break;
						if (!SourcePath.IsEmpty()) SourcePath += TEXT(">");
						SourcePath += PathSegments[i];
					}

					/* A single-segment source has no separator; the leaf only narrows when one exists */
					SourceLeaf = SourcePath;
					SourcePath.Split(TEXT(">"), nullptr, &SourceLeaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					break;
				}
			}
		}
	}

	if (!SourceLeaf.IsEmpty()) {
		/* The enum variable's descriptor on the generated class carries the enum reference */
		if (ClassChildProperties) {
			for (const TSharedPtr<FJsonValue>& Value : *ClassChildProperties) {
				const TSharedPtr<FJsonObject> Property = Value->AsObject();
				if (!Property.IsValid() || Property->GetStringField(TEXT("Name")) != SourceLeaf) continue;

				const TSharedPtr<FJsonObject> EnumObject = Property->GetObjectField(TEXT("Enum"));
				if (!EnumObject.IsValid()) break;

				const FName EnumName = GetExportNameOfSubobject(EnumObject->GetStringField(TEXT("ObjectName")));
				FString EnumPath = EnumObject->GetStringField(TEXT("ObjectPath"));

				int32 Dot;
				if (EnumPath.FindLastChar(TEXT('.'), Dot)) {
					EnumPath.LeftInline(Dot);
				}

				if (UEnum* LoadedEnum = LoadObjectByPath<UEnum>(EnumPath + TEXT(".") + EnumName.ToString())) {
					BoundEnum = LoadedEnum;
				}
#if UE5_1_BEYOND
				else if (UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumName.ToString())) {
#else
				else if (UEnum* FoundEnum = FindObject<UEnum>(ANY_PACKAGE, *EnumName.ToString())) {
#endif
					BoundEnum = FoundEnum;
				}

				break;
			}
		}

		/* Fall back to the compiled anim instance property - the enum reference lives on it */
		if (!BoundEnum && AnimBlueprint && AnimBlueprint->GeneratedClass) {
			if (FProperty* Prop = AnimBlueprint->GeneratedClass->FindPropertyByName(*SourceLeaf)) {
				if (FByteProperty* ByteProperty = CastField<FByteProperty>(Prop)) {
					BoundEnum = ByteProperty->Enum;
				}
#if ENGINE_UE5
				else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Prop)) {
					BoundEnum = EnumProperty->GetEnum();
				}
#endif
			}
		}

		if (BoundEnum) {
			BlendListByEnum->ReloadEnum(BoundEnum);
		}
	}

	/* --- EnumToPoseIndex from the folded constant data -----------------------------------
	 * The class's NodeTypeMap maps the node struct type to its property name/index table; the
	 * EnumToPoseIndex entry is the fold index into the node's AnimNodeData row. That row's
	 * value is the flat index into the constant data's ChildProperties, whose name is the key
	 * of the pose index array in the sparse class data. */
	TArray<int32> EnumToPoseIndex;
	{
		int32 FoldedPropertyIndex = INDEX_NONE;

		const TSharedPtr<FJsonObject> ClassProperties = GetAssetData();
		if (ClassProperties.IsValid() && ClassProperties->HasTypedField<EJson::Array>(TEXT("NodeTypeMap"))) {
			for (const TSharedPtr<FJsonValue>& NodeTypeValue : ClassProperties->GetArrayField(TEXT("NodeTypeMap"))) {
				const TSharedPtr<FJsonObject> NodeTypeObject = NodeTypeValue->AsObject();
				if (!NodeTypeObject.IsValid() || NodeTypeObject->GetStringField(TEXT("Key")) != NodeTypeKey) continue;

				const TSharedPtr<FJsonObject> NameToIndexObject = NodeTypeObject->GetObjectField(TEXT("Value"));
				if (!NameToIndexObject.IsValid()) break;

				for (const TSharedPtr<FJsonValue>& NameIndexValue : NameToIndexObject->GetArrayField(TEXT("NameToIndexMap"))) {
					const TSharedPtr<FJsonObject> NameIndexObject = NameIndexValue->AsObject();
					if (NameIndexObject.IsValid() && NameIndexObject->GetStringField(TEXT("Key")) == TEXT("EnumToPoseIndex")) {
						FoldedPropertyIndex = NameIndexObject->GetIntegerField(TEXT("Value"));
						break;
					}
				}
				break;
			}
		}

		if (FoldedPropertyIndex != INDEX_NONE && ClassProperties.IsValid() && ClassProperties->HasTypedField<EJson::Array>(TEXT("AnimNodeData"))) {
			const TArray<TSharedPtr<FJsonValue>> AnimNodeData = ClassProperties->GetArrayField(TEXT("AnimNodeData"));
			if (AnimNodeData.IsValidIndex(NodeIndex)) {
				const TSharedPtr<FJsonObject> NodeData = AnimNodeData[NodeIndex]->AsObject();
				const TArray<TSharedPtr<FJsonValue>> Entries = NodeData->GetArrayField(TEXT("Entries"));

				if (Entries.IsValidIndex(FoldedPropertyIndex) && Entries[FoldedPropertyIndex]->Type == EJson::Number) {
					const int32 FlatPropertyIndex = static_cast<int32>(Entries[FoldedPropertyIndex]->AsNumber());

					const TSharedPtr<FJsonObject> ConstantData = GetExportStartingWith(TEXT("AnimBlueprintGeneratedConstantData"), TEXT("Name"), GetContainer()->JsonObjects);
					if (ConstantData.IsValid()) {
						const TArray<TSharedPtr<FJsonValue>> ConstantChildProperties = ConstantData->GetArrayField(TEXT("ChildProperties"));
						if (ConstantChildProperties.IsValidIndex(FlatPropertyIndex)) {
							const TSharedPtr<FJsonObject> FlatProperty = ConstantChildProperties[FlatPropertyIndex]->AsObject();
							if (FlatProperty.IsValid()) {
								const FString FlatPropertyName = FlatProperty->GetStringField(TEXT("Name"));

								if (SerializedSparseClassData->HasTypedField<EJson::Array>(FlatPropertyName)) {
									for (const TSharedPtr<FJsonValue>& Value : SerializedSparseClassData->GetArrayField(FlatPropertyName)) {
										EnumToPoseIndex.Add(static_cast<int32>(Value->AsNumber()));
									}
								}
							}
						}
					}
				}
			}
		}
	}

	/* --- BlendTime from the folded constant data -----------------------------------------
	 * Same resolution as EnumToPoseIndex, using the BlendTime fold. The node's constructor
	 * seeds BlendTime with a single 0.1s entry, and the JSON export carries no BlendTime at
	 * all, so the array keeps its size-1 while BlendPose grows to the exported pose count.
	 * Update_AnyThread then reads CurrentBlendTimes[ChildIndex] out of bounds ("Array index
	 * out of bounds: 1 into an array of size 1"). Resize it to match BlendPose, filling the
	 * per-pose values straight from the sparse pool so the fold bakes real blend times. */
	{
		const TSharedPtr<FJsonObject> ClassProperties = GetAssetData();
		const TSharedPtr<FJsonObject> ConstantData = GetExportStartingWith(TEXT("AnimBlueprintGeneratedConstantData"), TEXT("Name"), GetContainer()->JsonObjects);

		TArray<TSharedPtr<FJsonValue>> SparseBlendTimes;
		if (ResolveFoldedSparseArray(ClassProperties, ConstantData, SerializedSparseClassData, NodeIndex, NodeTypeKey, TEXT("BlendTime"), SparseBlendTimes)) {
			TArray<float> BlendTimeValues;
			BlendTimeValues.Reserve(SparseBlendTimes.Num());
			for (const TSharedPtr<FJsonValue>& Value : SparseBlendTimes) {
				BlendTimeValues.Add(static_cast<float>(Value->AsNumber()));
			}

			ResizeBlendListBlendTime(BlendListByEnum, &BlendTimeValues);
		} else {
			/* The fold didn't resolve - the size is what keeps Update_AnyThread in bounds, so
			 * still resync the length with the constructor's default blend time. */
			ResizeBlendListBlendTime(BlendListByEnum, nullptr);
		}
	}

	/* --- VisibleEnumEntries, index-aligned with the pose pins ------------------------------
	 * EnumToPoseIndex[e] is the pose index (1-based pin order; 0 = default/unexposed). The pin
	 * at position (pose - 1) is named after enum entry e, which is exactly what the engine's
	 * BakeDataDuringCompilation expects to rebuild the mapping. */
	if (BoundEnum && EnumToPoseIndex.Num() > 0) {
		TArray<FName> VisibleEnumEntries;

		for (int32 EnumIndex = 0; EnumIndex < EnumToPoseIndex.Num() && EnumIndex < BoundEnum->NumEnums(); ++EnumIndex) {
			const int32 PoseIndex = EnumToPoseIndex[EnumIndex];
			if (PoseIndex <= 0) continue;

			if (VisibleEnumEntries.Num() < PoseIndex) {
				VisibleEnumEntries.SetNum(PoseIndex);
			}

			VisibleEnumEntries[PoseIndex - 1] = BoundEnum->GetNameByIndex(EnumIndex);
		}

		if (VisibleEnumEntries.Num() > 0) {
			if (const FArrayProperty* VisEnumArrayProp = FindFProperty<FArrayProperty>(BlendListByEnum->GetClass(), TEXT("VisibleEnumEntries"))) {
				const void* ArrayPtr = VisEnumArrayProp->ContainerPtrToValuePtr<void>(BlendListByEnum);
				FScriptArrayHelper ArrayHelper(VisEnumArrayProp, ArrayPtr);

				ArrayHelper.Resize(0);

				const FNameProperty* NameProp = CastField<FNameProperty>(VisEnumArrayProp->Inner);
				if (NameProp) {
					for (const FName& Entry : VisibleEnumEntries) {
						const int32 NewIdx = ArrayHelper.AddValue();
						NameProp->SetPropertyValue(ArrayHelper.GetRawPtr(NewIdx), Entry);
					}

					BlendListByEnum->ReconstructNode();
				}
			}
		}
	}
}

void IAnimationBlueprintImporter::ResizeBlendListBlendTime(UAnimGraphNode_BlendListBase* BlendListNode, const TArray<float>* SourceValues) {
	if (!BlendListNode) return;

	/* BlendPose is protected and BlendTime is private on FAnimNode_BlendListBase, so the
	 * arrays are reached through reflection on the node's runtime struct. */
	const FStructProperty* NodeProp = GetNodeStructProperty(BlendListNode);
	if (!NodeProp) return;

	/* FindFProperty's iterator walks the struct's super chain, so BlendPose/BlendTime on the
	 * base FAnimNode_BlendListBase are found from the ByEnum/ByBool/ByInt struct. Base is
	 * laid out first, so base offsets apply straight to the derived struct instance. */
	const FArrayProperty* BlendPoseProp = FindFProperty<FArrayProperty>(NodeProp->Struct, TEXT("BlendPose"));
	const FArrayProperty* BlendTimeProp = FindFProperty<FArrayProperty>(NodeProp->Struct, TEXT("BlendTime"));
	if (!BlendPoseProp || !BlendTimeProp) return;

	const FFloatProperty* BlendTimeInner = CastField<FFloatProperty>(BlendTimeProp->Inner);
	if (!BlendTimeInner) return;

	/* Node's struct properties are relative to the FAnimNode_BlendListBase instance inside the
	 * graph node, so first reach the Node struct through the class-owned StructProperty, then
	 * offset from that (void* form - the UObject overload asserts on struct-owned properties). */
	void* NodeStructPtr = NodeProp->ContainerPtrToValuePtr<void>(BlendListNode);

	void* BlendPosePtr = BlendPoseProp->ContainerPtrToValuePtr<void>(NodeStructPtr);
	void* BlendTimePtr = BlendTimeProp->ContainerPtrToValuePtr<void>(NodeStructPtr);

	FScriptArrayHelper PoseHelper(BlendPoseProp, BlendPosePtr);
	FScriptArrayHelper TimeHelper(BlendTimeProp, BlendTimePtr);

	const int32 DesiredCount = PoseHelper.Num();
	TimeHelper.Resize(DesiredCount);

	for (int32 Index = 0; Index < DesiredCount; ++Index) {
		float Value = 0.2f;
		if (SourceValues && SourceValues->IsValidIndex(Index)) {
			Value = (*SourceValues)[Index];
		}
		BlendTimeInner->SetPropertyValue(TimeHelper.GetRawPtr(Index), Value);
	}

	BlendListNode->Modify();
}

void IAnimationBlueprintImporter::BuildPropertyAccessBindings() {
	if (!SerializedSparseClassData.IsValid()) return;

	const TSharedPtr<FJsonObject> PropertyAccess = SerializedSparseClassData->GetObjectField(TEXT("AnimBlueprintExtension_PropertyAccess"));
	const TSharedPtr<FJsonObject> Library = PropertyAccess.IsValid() ? PropertyAccess->GetObjectField(TEXT("Library")) : nullptr;
	if (!Library.IsValid()) return;

	/* PathSegments are a flat list of property path segment names */
	TArray<FString> PathSegments;
	for (const TSharedPtr<FJsonValue>& SegmentValue : Library->GetArrayField(TEXT("PathSegments"))) {
		const TSharedPtr<FJsonObject> SegmentObject = SegmentValue->AsObject();
		if (SegmentObject.IsValid() && SegmentObject->HasTypedField<EJson::String>(TEXT("Name"))) {
			PathSegments.Add(SegmentObject->GetStringField(TEXT("Name")));
		}
	}

	const TArray<TSharedPtr<FJsonValue>> SrcPaths = Library->GetArrayField(TEXT("SrcPaths"));
	const TArray<TSharedPtr<FJsonValue>> DestPaths = Library->GetArrayField(TEXT("DestPaths"));

	/* Copies are compiled into CopyBatchArray; a node's CopyRecords CopyIndex indexes
	 * the first batch's Copies array (the WorkerThread_Unbatched call site). */
	TArray<TSharedPtr<FJsonObject>> Copies;
	const TArray<TSharedPtr<FJsonValue>> CopyBatches = Library->GetArrayField(TEXT("CopyBatchArray"));
	if (CopyBatches.IsValidIndex(0)) {
		const TSharedPtr<FJsonObject> BatchObject = CopyBatches[0]->AsObject();
		if (BatchObject.IsValid()) {
			for (const TSharedPtr<FJsonValue>& CopyValue : BatchObject->GetArrayField(TEXT("Copies"))) {
				const TSharedPtr<FJsonObject> CopyObject = CopyValue->AsObject();
				if (CopyObject.IsValid()) Copies.Add(CopyObject);
			}
		}
	}

	/* Resolve a path index into the segment names it spans */
	auto ResolvePath = [&PathSegments](const TSharedPtr<FJsonValue>& PathValue) {
		TArray<FString> Segments;
		const TSharedPtr<FJsonObject> PathObject = PathValue->AsObject();
		if (!PathObject.IsValid()) return Segments;

		const int32 StartIndex = PathObject->GetIntegerField(TEXT("PathSegmentStartIndex"));
		const int32 Count = PathObject->GetIntegerField(TEXT("PathSegmentCount"));
		for (int32 i = StartIndex; i < StartIndex + Count; ++i) {
			if (!PathSegments.IsValidIndex(i)) break;
			Segments.Add(PathSegments[i]);
		}

		return Segments;
	};

	for (const auto& SparsePair : SerializedSparseClassData->Values) {
		if (!SparsePair.Key.StartsWith(TEXT("AnimGraphNode"))) continue;

		const TSharedPtr<FJsonObject> SparseNode = SparsePair.Value->AsObject();
		if (!SparseNode.IsValid() || !SparseNode->HasTypedField<EJson::Array>(TEXT("CopyRecords"))) continue;

		TArray<FCompiledPinBinding>& Bindings = CompiledPinBindings.FindOrAdd(SparsePair.Key);

		for (const TSharedPtr<FJsonValue>& RecordValue : SparseNode->GetArrayField(TEXT("CopyRecords"))) {
			const TSharedPtr<FJsonObject> RecordObject = RecordValue->AsObject();
			if (!RecordObject.IsValid() || !RecordObject->HasTypedField<EJson::Number>(TEXT("CopyIndex"))) continue;

			const int32 CopyIndex = RecordObject->GetIntegerField(TEXT("CopyIndex"));
			if (!Copies.IsValidIndex(CopyIndex)) continue;

			const TSharedPtr<FJsonObject> CopyObject = Copies[CopyIndex];
			if (!CopyObject.IsValid()) continue;

			FCompiledPinBinding Binding;

			if (CopyObject->HasTypedField<EJson::Number>(TEXT("AccessIndex"))) {
				const int32 AccessIndex = CopyObject->GetIntegerField(TEXT("AccessIndex"));
				if (SrcPaths.IsValidIndex(AccessIndex)) {
					Binding.SourcePath = ResolvePath(SrcPaths[AccessIndex]);
				}
			}

			if (CopyObject->HasTypedField<EJson::Number>(TEXT("DestAccessStartIndex"))) {
				const int32 DestStartIndex = CopyObject->GetIntegerField(TEXT("DestAccessStartIndex"));
				if (DestPaths.IsValidIndex(DestStartIndex)) {
					Binding.DestPath = ResolvePath(DestPaths[DestStartIndex]);
				}
			}

			if (Binding.SourcePath.Num() > 0 && Binding.DestPath.Num() > 0) {
				Bindings.Add(MoveTemp(Binding));
			}
		}

		if (Bindings.Num() == 0) {
			CompiledPinBindings.Remove(SparsePair.Key);
		}
	}
}

FName IAnimationBlueprintImporter::ResolveCompiledBindingPinName(const UAnimGraphNode_Base* Node, const TArray<FString>& DestPath) {
	if (DestPath.Num() == 0) return NAME_None;

	const FString& First = DestPath[0];

	/* Blend list nodes evaluate through staging slots in __AnimBlueprintMutables rather than
	 * straight onto the node property, so the pin has to come from the node type itself. */
	if (First == TEXT("__AnimBlueprintMutables")) {
		if (Cast<UAnimGraphNode_BlendListByEnum>(Node)) return TEXT("ActiveEnumValue");
		if (Cast<UAnimGraphNode_BlendListByBool>(Node)) return TEXT("Bool");
		if (Cast<UAnimGraphNode_BlendListByInt>(Node)) return TEXT("BlendIndex");
		return NAME_None;
	}

	/* Control rig custom controls compile into a __CustomProperty_<Name>_<GUID> slot */
	if (First.StartsWith(TEXT("__CustomProperty_"))) {
		FString CustomProperty = First;
		CustomProperty.RightChopInline(FCString::Strlen(TEXT("__CustomProperty_")));

		int32 GuidSeparator;
		if (CustomProperty.FindLastChar(TEXT('_'), GuidSeparator)) {
			CustomProperty.LeftInline(GuidSeparator);
		}

		return FName(*CustomProperty);
	}

	/* A directly bound pin's destination is <NodeName>.<PinName> */
	return FName(*DestPath.Last());
}

void IAnimationBlueprintImporter::ResolveFallbackAnimSequence() {
	if (FallbackAnimSequence || !AnimBlueprint) return;

	USkeleton* TargetSkeleton = AnimBlueprint->TargetSkeleton;
	if (!TargetSkeleton) {
		UE_LOG(LogReflection, Warning, TEXT("ResolveFallbackAnimSequence: AnimBlueprint \"%s\" has no TargetSkeleton"), *AnimBlueprint->GetName());
		return;
	}

	TArray<FAssetData> Assets;
	FAssetRegistryModule& ARModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	ARModule.Get().GetAssetsByClass(
		FTopLevelAssetPath(TEXT("/Script/Engine.AnimSequence")), Assets, true);

	for (const FAssetData& Asset : Assets) {
		UAnimSequence* Seq = Cast<UAnimSequence>(Asset.GetAsset());
		if (!Seq) continue;
		if (Seq->GetSkeleton() == TargetSkeleton) {
			FallbackAnimSequence = Seq;
			UE_LOG(LogReflection, Log, TEXT("ResolveFallbackAnimSequence: using \"%s\" (skeleton \"%s\") for SequencePlayer placeholders"),
				*Seq->GetName(), *TargetSkeleton->GetName());
			return;
		}
	}

	UE_LOG(LogReflection, Warning, TEXT("ResolveFallbackAnimSequence: no UAnimSequence found matching skeleton \"%s\" for AnimBlueprint \"%s\""),
		*TargetSkeleton->GetName(), *AnimBlueprint->GetName());
}

void IAnimationBlueprintImporter::ApplyCompiledPinBindings(FUObjectExport* NodeExport, UAnimGraphNode_Base* Node) {
	if (!NodeExport || !Node || CompiledPinBindings.Num() == 0) return;

	/* Nodes that shipped the editor-side form are already handled by HandlePropertyBinding */
	if (NodeExport->JsonObject.IsValid() && NodeExport->JsonObject->HasField(TEXT("EvaluateGraphExposedInputs"))) return;

	const TArray<FCompiledPinBinding>* NodeBindings = CompiledPinBindings.Find(NodeExport->GetName().ToString());
	if (!NodeBindings) return;

#if !UE4_25_BELOW
	for (const FCompiledPinBinding& Binding : *NodeBindings) {
		const FName PinName = ResolveCompiledBindingPinName(Node, Binding.DestPath);
		if (PinName.IsNone()) continue;

		FAnimGraphNodePropertyBinding PropertyBinding;
		PropertyBinding.PropertyName = PinName;
		PropertyBinding.bIsBound = true;

		for (const FString& Segment : Binding.SourcePath) {
			PropertyBinding.PropertyPath.Add(Segment);
		}

		FString PathAsText;
		for (const FString& Segment : Binding.SourcePath) {
			if (!PathAsText.IsEmpty()) PathAsText += TEXT(".");
			PathAsText += Segment;
		}
		PropertyBinding.PathAsText = FText::FromString(PathAsText);

		if (const UEdGraphPin* DestinationPin = Node->FindPin(PinName, EGPD_Input)) {
			PropertyBinding.PinType = DestinationPin->PinType;
		}

		if (!AddPropertyBinding(Node, PinName, PropertyBinding)) {
			UE_LOG(LogReflection, Warning, TEXT("Binding dropped: %s.%s <- %s"), *Node->GetClass()->GetName(), *PinName.ToString(), *PathAsText);
		}

		/* DuplicateObject (used by CloneGraph during compilation) loses the TMap entries
		 * on the Binding subobject, so HasBinding() returns false on the cloned node.
		 * The validation at AnimGraphNode_AssetPlayerBase.cpp:501-548 fires when:
		 *   1. Node.Sequence (inner struct property) is null
		 *   2. Pin->DefaultObject is null
		 *   3. AlwaysDynamicProperties does NOT contain the property name
		 *   4. HasBinding() returns false (TMap lost during clone)
		 *   5. Pin has no wire connections
		 *
		 * Adding "Sequence" to AlwaysDynamicProperties suppresses the compile error.
		 * Setting Node.Sequence + Pin->DefaultObject to a skeleton-compatible asset
		 * also prevents the runtime crash when SequencePlayer::Evaluate() reads the
		 * inner struct property directly (it does NOT fall back to Pin->DefaultObject). */
		Node->AlwaysDynamicProperties.Add(PinName);

		if (FallbackAnimSequence && PinName == FName("Sequence")) {
			if (UAnimGraphNode_SequencePlayer* SeqNode = Cast<UAnimGraphNode_SequencePlayer>(Node)) {
				SeqNode->Node.SetSequence(FallbackAnimSequence);
			}
			if (UAnimGraphNode_SequenceEvaluator* EvalNode = Cast<UAnimGraphNode_SequenceEvaluator>(Node)) {
				EvalNode->Node.SetSequence(FallbackAnimSequence);
			}
			if (UEdGraphPin* Pin = Node->FindPin(PinName, EGPD_Input)) {
				Pin->DefaultObject = FallbackAnimSequence;
			}
		}
	}
#endif
}

void IAnimationBlueprintImporter::CreateAnimGraphNodes(UEdGraph* AnimGraph, const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer& OutContainer) {
	for (const auto& Pair : AnimNodeProperties->Values) {
		FString Key = Pair.Key;

		TSharedPtr<FJsonObject> Value = Pair.Value->AsObject();

		/* Find the NodeType and GUID from the key */
		FString NodeType, NodeStringGUID; {
			Key.Split(TEXT("_"), &NodeType, &NodeStringGUID, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

			/* Handle case for format: "AnimGraphNode[0]" */
			if (Key.Contains("[")) {
				FString CleanKey = Key.Left(Key.Find("["));
				
				TArray<FString> Parts; {
					CleanKey.ParseIntoArray(Parts, TEXT("_"));
				}
				
				NodeType = Parts.Num() >= 2 ? Parts[0] + TEXT("_") + Parts[1] : CleanKey;
				NodeStringGUID.Empty();
			}
		}

		if (NodeType == "AnimGraphNode") {
			NodeType = Key;
		}

		/* Redirections */
		if (NodeType == "AnimGraphNode_SubInput") {
			NodeType = "AnimGraphNode_LinkedInputPose";
		}

		/* Only add json object data, transition result is handled different */
		if (NodeType == "AnimGraphNode_TransitionResult") {
			OutContainer.Exports.Add(
				new FUObjectExport(
					FName(*Key),
					FName(*NodeType),
					StringToName(AnimGraph->GetName()),
					Value,
					nullptr,
					nullptr
				)
			);

			continue;
		}

		/* Parse the NodeGuid, if not parsed properly, generate a new one.
		 *
		 * UE4 named node properties after the node's guid ("AnimGraphNode_ApplyAdditive_10AB22C6...") so it
		 * parses straight back out. UE5 numbers them instead ("AnimGraphNode_ModifyBone_3"), so the parse fails
		 * for every node - and FGuid() is all zeroes, not a fresh guid, so they would all end up sharing one.
		 * FAnimBlueprintCompilerContext does NodeGuidToIndexMap.Add(Node->NodeGuid, Index) per node, and TMap::Add
		 * overwrites on a duplicate key, so a whole graph of zeroes collapses to a single entry pointing at
		 * whichever node compiled last. Everything that resolves a node through that map afterwards - state
		 * machines, asset players, blend space graphs - then reads the wrong index. */
		FGuid NodeGuid; {
			FGuid::Parse(NodeStringGUID, NodeGuid);

			if (!NodeGuid.IsValid()) NodeGuid = FGuid::NewGuid();
		}

		const UClass* Class = FindClassByType(NodeType);
		
		if (!Class) continue;

		UAnimGraphNode_Base* Node = NewObject<UAnimGraphNode_Base>(AnimGraph, ToNewObjectClass(Class), NAME_None, RF_Transactional);
		Node->NodeGuid = NodeGuid;

		/* Add new node */
		OutContainer.Exports.Add(
			new FUObjectExport(
				FName(*Key),
				FName(*NodeType),
				StringToName(AnimGraph->GetName()),
				Value,
				Node,
				AnimGraph
			)
		);
	}
}

void IAnimationBlueprintImporter::AddNodesToGraph(UEdGraph* AnimGraph, FUObjectExportContainer* Container) {
    for (const FUObjectExport* Export : Container->Exports) {
        if (!IsValid(Export->Object) || !Export->JsonObject.IsValid()) {
            continue;
        }

        UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);

        Node->Rename(nullptr, AnimGraph);
        AnimGraph->Nodes.Add(Node);
        Node->Modify();
    }
}

void IAnimationBlueprintImporter::HandleNodeDeserialization(FUObjectExportContainer* Container) {
	GetObjectSerializer()->GetPropertySerializer()->BlacklistedPropertyNames.Add(TEXT("LinkID"));

	for (FUObjectExport* NodeExport : Container->Exports) {
		if (NodeExport->Object == nullptr) continue;

		UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(NodeExport->Object);
		TSharedPtr<FJsonObject> NodeProperties = NodeExport->JsonObject;

		/* Post-processing modifications ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		if (NodeProperties->HasField(TEXT("GroupRole")) && NodeProperties->HasField(TEXT("GroupIndex"))) {
			const int GroupIndexInteger = NodeProperties->GetIntegerField(TEXT("GroupIndex"));

			/* -1 is no group role */
			if (GroupIndexInteger != -1) {
				TSharedPtr<FJsonObject> SyncGroup = MakeShared<FJsonObject>();
				FString SyncGroupName = SyncGroupNames[GroupIndexInteger];
			
				SyncGroup->SetStringField(TEXT("GroupName"), SyncGroupName);
				SyncGroup->SetStringField(TEXT("GroupRole"), NodeProperties->GetStringField(TEXT("GroupRole")));

				NodeProperties->SetObjectField(TEXT("SyncGroup"), SyncGroup);
			}
		}

#if ENGINE_UE4
		/* UE5+ games use PhysicsBodyDefinitions for AnimGraphNode_AnimDynamics */
		if (NodeProperties->HasField(TEXT("PhysicsBodyDefinitions"))) {
			TSharedPtr<FJsonObject> PhysicsBodyDefinition = NodeProperties->GetArrayField(TEXT("PhysicsBodyDefinitions"))[0]->AsObject();
			if (PhysicsBodyDefinition.IsValid()) {
				for (const auto& Pair : PhysicsBodyDefinition->Values) {
					NodeExport->JsonObject->SetField(Pair.Key, Pair.Value);
				}
			}
		}
#else
		/* UE5+ games use PhysicsBodyDefinitions for AnimGraphNode_AnimDynamics */
		if (!NodeExport->HasProperty("PhysicsBodyDefinitions")) {
			TSharedPtr<FJsonObject> PhysicsBodyDefinition = MakeShared<FJsonObject>();
			auto& RootValues = NodeExport->JsonObject->Values;

			auto MoveField = [&](const FString& Key) {
				if (RootValues.Contains(Key)) {
					if (Key == TEXT("LocalJointOffset")) {
						TSharedPtr<FJsonObject> Original = RootValues[Key]->AsObject();

						if (Original.IsValid()) {
							TSharedPtr<FJsonObject> VecObj = MakeShared<FJsonObject>(*Original);

							for (const auto& Pair : Original->Values) {
								if (Pair.Value->Type == EJson::Number && Pair.Value->AsNumber() != 0.0) {
									VecObj->SetNumberField(Pair.Key, -Pair.Value->AsNumber());
								}
								else {
									VecObj->SetField(Pair.Key, Pair.Value);
								}
							}

							PhysicsBodyDefinition->SetObjectField(Key, VecObj);
							return;
						}
					}

					PhysicsBodyDefinition->SetField(Key, RootValues[Key]);
				}
			};

			/* Move all PhysicsBodyDefinition related fields */
			MoveField(TEXT("BoundBone"));
			MoveField(TEXT("BoxExtents"));
			MoveField(TEXT("LocalJointOffset"));
			MoveField(TEXT("ConstraintSetup"));
			MoveField(TEXT("CollisionType"));
			MoveField(TEXT("SphereCollisionRadius"));

			/* Create array and assign */
			TArray<TSharedPtr<FJsonValue>> PhysicsBodyDefinitionsArray;
			PhysicsBodyDefinitionsArray.Add(MakeShared<FJsonValueObject>(PhysicsBodyDefinition));

			NodeExport->JsonObject->SetArrayField(TEXT("PhysicsBodyDefinitions"), PhysicsBodyDefinitionsArray);
		}
#endif

#if ENGINE_UE4
		/* Looks like UE5 flipped axes on LocalJointOffset */
		if (GReflectionRuntime.IsUE5()) {
			if (NodeProperties->HasField(TEXT("LocalJointOffset"))) {
				auto LocalJointOffset = NodeProperties->GetObjectField(TEXT("LocalJointOffset"));
				LocalJointOffset->SetNumberField("X", -LocalJointOffset->GetNumberField(TEXT("X")));
				LocalJointOffset->SetNumberField("Y", -LocalJointOffset->GetNumberField(TEXT("Y")));
				LocalJointOffset->SetNumberField("Z", -LocalJointOffset->GetNumberField(TEXT("Z")));
			}
		}
#endif
		
		GetObjectSerializer()->DeserializeObjectProperties(NodeProperties, Node);

		/* Diagnostic: log key array sizes for nodes that can crash at runtime ~~~~~~~~~~~~~~ */
		if (UAnimGraphNode_LayeredBoneBlend* LBB = Cast<UAnimGraphNode_LayeredBoneBlend>(Node)) {
			UE_LOG(LogReflection, Warning, TEXT("[DIAG] LayeredBoneBlend \"%s\": BlendPoses=%d BlendWeights=%d BlendMode=%d LayerSetup=%d BlendMasks=%d"),
				*NodeExport->GetName().ToString(),
				LBB->Node.BlendPoses.Num(),
				LBB->Node.BlendWeights.Num(),
				(int32)LBB->Node.BlendMode,
				LBB->Node.LayerSetup.Num(),
				LBB->Node.BlendMasks.Num());
		}
		if (UAnimGraphNode_BlendListByBool* BLB = Cast<UAnimGraphNode_BlendListByBool>(Node)) {
			/* BlendPose is protected on FAnimNode_BlendListBase, so count through reflection.
			 * Node struct properties need the Node instance as the container (void* form - the
			 * UObject overload asserts on struct-owned properties). */
			int32 PoseCount = -1;
			if (const FStructProperty* NodeProp = GetNodeStructProperty(BLB)) {
				if (const FArrayProperty* BlendPoseProp = FindFProperty<FArrayProperty>(NodeProp->Struct, TEXT("BlendPose"))) {
					void* NodeStructPtr = NodeProp->ContainerPtrToValuePtr<void>(BLB);
					PoseCount = FScriptArrayHelper(BlendPoseProp, BlendPoseProp->ContainerPtrToValuePtr<void>(NodeStructPtr)).Num();
				}
			}

			UE_LOG(LogReflection, Warning, TEXT("[DIAG] BlendListByBool \"%s\": BlendPose=%d"),
				*NodeExport->GetName().ToString(),
				PoseCount);
		}
		if (UAnimGraphNode_SequencePlayer* SP = Cast<UAnimGraphNode_SequencePlayer>(Node)) {
			UE_LOG(LogReflection, Warning, TEXT("[DIAG] SequencePlayer \"%s\": Sequence=%s"),
				*NodeExport->GetName().ToString(),
				SP->Node.GetSequence() ? *SP->Node.GetSequence()->GetName() : TEXT("NULL"));
		}

		/* Specific needs for certain nodes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		if (UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(Node)) {
			SaveCachedPose->CacheName = NodeProperties->GetStringField(TEXT("CachePoseName"));
		}

		if (UAnimGraphNode_UseCachedPose* UseCachedPose = Cast<UAnimGraphNode_UseCachedPose>(Node)) {
			if (NodeProperties->HasField(TEXT("LinkToCachingNode"))) {
				const TSharedPtr<FJsonObject> LinkToCachingNode = NodeProperties->GetObjectField(TEXT("LinkToCachingNode"));
				
				if (LinkToCachingNode->HasField(TEXT("LinkID"))) {
					const FString LinkID = LinkToCachingNode->GetStringField(TEXT("LinkID"));

					/* Specifically use RootAnimNodeContainer, because cached poses won't move with state machines */
					FUObjectExport* SaveCachedPoseExport = RootAnimNodeContainer->Find(LinkID);
					if (!SaveCachedPoseExport->IsJsonAndObjectValid()) continue;

					UAnimGraphNode_SaveCachedPose* SaveCachedPose = Cast<UAnimGraphNode_SaveCachedPose>(SaveCachedPoseExport->Object);
					if (!SaveCachedPose) continue;
					
					UseCachedPose->SaveCachedPoseNode = SaveCachedPose;
					UseCachedPose->Modify();
					SaveCachedPose->Modify();
				}
			}
		}

		HandlePropertyBinding(NodeExport, GetContainer()->JsonObjects, Node, this, AnimBlueprint);

		const UReflectionSettings* Settings = GetSettings();
		if (Settings->AssetSettings.AnimationBlueprint.NodeIDComments) {
			Node->NodeComment = NodeExport->GetName().ToString();
			Node->bCommentBubbleVisible = true;
		}
		
		Node->AllocateDefaultPins();
		Node->Modify();
		Node->PostPlacedNewNode();

		/* Remember every node that received a binding so the deferred finalizer can
		 * re-apply them after the first compile (which clones the graph — the clone's
		 * Binding subobjects lose their PropertyBindings). */
		const FString ExportKey = NodeExport->GetName().ToString();
		if (CompiledPinBindings.Contains(ExportKey)) {
			DeferredBindingNodes.Add(MakeTuple(ExportKey, Node));
			Node->ReconstructNode();
		}

		/* Compiled exports have no EvaluateGraphExposedInputs to drive HandlePropertyBinding;
		 * restore the variable bindings recovered from the property access library here, after
		 * pins have been allocated so the binding's PinType can be read off the actual pin.
		 * Must run after ReconstructNode because ReconstructNode recreates pins. */
		ApplyCompiledPinBindings(NodeExport, Node);
	}
}

void IAnimationBlueprintImporter::ConnectAnimGraphNodes(FUObjectExportContainer* Container, UEdGraph* AnimGraph) {
    for (FUObjectExport* Export : Container->Exports) {
        UAnimGraphNode_Base* Node = Cast<UAnimGraphNode_Base>(Export->Object);
        const TSharedPtr<FJsonObject> Json = Export->JsonObject;

        if (Cast<UAnimGraphNode_BlendListByEnum>(Node)) {
            UpdateBlendListByEnumVisibleEntries(Export, Container, AnimGraph);
        	continue;
        }

        if (UAnimGraphNode_BlendListBase* BlendListNode = Cast<UAnimGraphNode_BlendListBase>(Node)) {
            /* ByBool's constructor AddPose()s twice (matching the exported poses) but ByInt
             * AddPose()s three times while the export can carry two - a stale BlendTime length
             * is a latent OOB. Resync to BlendPose; existing constructor values are kept. */
            ResizeBlendListBlendTime(BlendListNode, nullptr);
        }
    	
        for (const auto& Pair : Json->Values) {
            const FString& Key = Pair.Key;
            const TSharedPtr<FJsonValue>& Value = Pair.Value;
            
            if (Value->Type == EJson::Array) {
                const TArray<TSharedPtr<FJsonValue>>& JsonArray = Value->AsArray();
                
                for (int32 Index = 0; Index < JsonArray.Num(); ++Index) {
                    const TSharedPtr<FJsonValue>& Elem = JsonArray[Index];
                    
                    if (!Elem.IsValid() || !Elem->AsObject().IsValid()) {
                        continue;
                    }
                    
                    const TSharedPtr<FJsonObject>& Obj = Elem->AsObject();
                    if (!Obj->HasField(TEXT("LinkID"))) {
                        continue;
                    }
                    
                    const FString LinkID = Obj->GetStringField(TEXT("LinkID"));
                    UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(Container->Find(LinkID)->Object);
                    
                    if (!TargetNode) {
                        continue;
                    }
                    
                    const FStructProperty* NodeProp = GetNodeStructProperty(Node);
                    if (!NodeProp) {
                        continue;
                    }
                    
                    for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It) {
                        FProperty* Property = *It;
                        
                        if (Property->GetName() != Pair.Key) {
                            continue;
                        }
                        
                        if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property)) {
                            const FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
                            
                            if (!InnerStruct || !InnerStruct->Struct->IsChildOf(FPoseLinkBase::StaticStruct())) {
                                continue;
                            }
                            
                            const FString IndexedPinName = FString::Printf(TEXT("%s_%d"), *Pair.Key, Index);
                            LinkPoseInputPin(IndexedPinName, Node, TargetNode, AnimGraph);
                        }
                    }
                }
            }
            
            if (Value->Type == EJson::Object && Value->AsObject()->HasTypedField<EJson::String>(TEXT("LinkID"))) {
                const FString LinkID = Value->AsObject()->GetStringField(TEXT("LinkID"));
                UAnimGraphNode_Base* TargetNode = Cast<UAnimGraphNode_Base>(Container->Find(LinkID)->Object);
                
                if (!TargetNode) {
                    continue;
                }
                
                const FStructProperty* NodeProp = GetNodeStructProperty(Node);

                if (!NodeProp) {
                    continue;
                }
                
                for (TFieldIterator<FProperty> It(NodeProp->Struct); It; ++It) {
                    const FProperty* Property = *It;
                    
                    if (Property->GetName() != Pair.Key) {
                        continue;
                    }
                    
                    LinkPoseInputPin(Key, Node, TargetNode, AnimGraph);
                }
            }
        }
    }
}

/* In newer versions of Unreal Engine, EvaluateGraphExposedInputs was moved to the main AnimBlueprintGeneratedClass class */
/* Here, we move them into the node data to use more easily */
void IAnimationBlueprintImporter::ProcessEvaluateGraphExposedInputs(const TSharedPtr<FJsonObject>& AnimNodeProperties) const {
	if (!GetAssetData()->HasField(TEXT("EvaluateGraphExposedInputs"))) return;
	TArray<TSharedPtr<FJsonValue>> EvaluateInputs = GetAssetData()->GetArrayField(TEXT("EvaluateGraphExposedInputs"));
	
	for (const auto& Value : EvaluateInputs) {
		TSharedPtr<FJsonObject> InputObj = Value->AsObject();
		
		FString NodeName = InputObj->GetObjectField(TEXT("ValueHandlerNodeProperty"))->GetStringField(TEXT("ObjectName")); {
			NodeName.Split(":", nullptr, &NodeName);
			NodeName = NodeName.Replace(TEXT("'"), TEXT(""));	
		}
		
		AnimNodeProperties->GetObjectField(NodeName)->SetObjectField(TEXT("EvaluateGraphExposedInputs"), InputObj);
	}
}

UEdGraph* IAnimationBlueprintImporter::FindAnimGraph(UAnimBlueprint* AnimBlueprint) {
	for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs) {
		if (Graph && Graph->GetName() == TEXT("AnimGraph")) {
			return Graph;
		}
	}
	
	return nullptr;
}
