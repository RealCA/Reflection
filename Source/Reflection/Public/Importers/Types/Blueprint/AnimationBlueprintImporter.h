/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class UAnimGraphNode_BlendListBase;
class UAnimGraphNode_BlendListByEnum;
class UAnimGraphNode_Base;
class UAnimSequence;

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the animation blueprint type used to come in from */
#if UE4_25_BELOW
class UAnimBlueprint;
#endif

class IAnimationBlueprintImporter final : public IImporter {
public:
	virtual bool Import() override;

	/* Shell phase (FAssetDependencyRegistry::CreateShells) builds every Internal entry's
	 * UObject before any export is populated, so a circular reference never has to load a
	 * package this batch is still building. Without an override here the base IImporter
	 * implementation returns null for a null argument and no shell is ever created. */
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;

private:
	UAnimBlueprint* CreateAnimBlueprint(UClass* ParentClass);
	
	void ProcessEvaluateGraphExposedInputs(const TSharedPtr<FJsonObject>& AnimNodeProperties) const;

	/* Finds an Animation Graph in an Animation Blueprint */
	static UEdGraph* FindAnimGraph(UAnimBlueprint* AnimBlueprint);

	/* Using [AnimNodeProperties] that is filled with animation nodes, create new nodes, and the Outer being [AnimGraph], then add it to the [Container] */
	void CreateGraph(const TSharedPtr<FJsonObject>& AnimNodeProperties, UEdGraph* AnimGraph, FUObjectExportContainer* Container);

	/* Create Animation Graph Nodes and create a UObjectExportContainer to hold the data */
	static void CreateAnimGraphNodes(UEdGraph* AnimGraph, const TSharedPtr<FJsonObject>& AnimNodeProperties, FUObjectExportContainer& OutContainer);

	/* Add a container full of nodes to a graph */
	static void AddNodesToGraph(UEdGraph* AnimGraph, FUObjectExportContainer* Container);

	/* Deserializes each node in the node container */
	void HandleNodeDeserialization(FUObjectExportContainer* Container);

	/* Links Animation Graph Nodes together using a container */
	void ConnectAnimGraphNodes(FUObjectExportContainer* Container, UEdGraph* AnimGraph);

	void UpdateBlendListByEnumVisibleEntries(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UEdGraph* AnimGraph);

	/* Recovers the enum binding for a BlendListByEnum node. UE5.7 folds EnumToPoseIndex into
	 * the generated class constant data - the node export only carries BlendPose. The
	 * fold is resolved through the class's NodeTypeMap + AnimNodeData: the node's export
	 * position indexes AnimNodeData, whose Entries[NameToIndexMap["EnumToPoseIndex"]] is
	 * the folded flat-property index, naming a constant-data ChildProperty whose value
	 * lives in SerializedSparseClassData. The enum type itself comes from the node's
	 * property access copy record - the source path leaf is the anim instance's enum
	 * property, whose declared enum becomes BoundEnum. */
	void ResolveBlendListByEnumEnum(FUObjectExport* NodeExport, FUObjectExportContainer* Container, UAnimGraphNode_BlendListByEnum* BlendListByEnum);

	/* Re-sizes Node.BlendTime to Node.BlendPose.Num() for any BlendList node. The runtime
	 * struct keeps BlendTime private (EditFixedSize) and the JSON export never carries it
	 * (it's folded into the generated class constant data), so the array stays at the
	 * constructor's single entry while BlendPose grows to the exported pose count — that
	 * makes Update_AnyThread's CurrentBlendTimes[ChildIndex] overrun at runtime
	 * ("Array index out of bounds"). [SourceValues], when supplied, fills the array with
	 * the per-pose blend times decoded from SerializedSparseClassData; otherwise missing
	 * entries default to 0.2s. Uses reflection (FScriptArrayHelper) to bypass the private
	 * member. */
	void ResizeBlendListBlendTime(UAnimGraphNode_BlendListBase* BlendListNode, const TArray<float>* SourceValues = nullptr);

	/* One variable→pin binding recovered from the compiled property access library.
	 * SourcePath is the anim instance variable path; DestPath is the node struct
	 * property path (either <NodeName>.<Pin> or a __AnimBlueprintMutables slot). */
	struct FCompiledPinBinding {
		TArray<FString> SourcePath;
		TArray<FString> DestPath;
	};

	/* Walks SerializedSparseClassData's AnimBlueprintExtension_PropertyAccess library
	 * (CopyRecords → Copies → SrcPaths/DestPaths → PathSegments) and collects the
	 * bindings each node carries, so exports that only ship the compiled form still
	 * get their pins bound to variables. */
	void BuildPropertyAccessBindings();

	/* Turns a destination path into the input pin name it drives. */
	static FName ResolveCompiledBindingPinName(const UAnimGraphNode_Base* Node, const TArray<FString>& DestPath);

	/* Writes the collected bindings for [Node] into its PropertyBindings map. */
	void ApplyCompiledPinBindings(FUObjectExport* NodeExport, UAnimGraphNode_Base* Node);

	/* Finds the first UAnimSequence whose skeleton matches AnimBlueprint->TargetSkeleton.
	 * Called once; result is cached in FallbackAnimSequence. */
	void ResolveFallbackAnimSequence();

	/* Node property name → bindings recovered from the sparse property access library */
	TMap<FString, TArray<FCompiledPinBinding>> CompiledPinBindings;

	/* Maps export key → graph node for every SequencePlayer/BlendList that received a
	 * compiled pin binding during HandleNodeDeserialization.  The deferred finalizer uses
	 * this to re-apply bindings *after* the first compile (which clones the anim graph
	 * into a ConsolidatedEventGraph — the clone's Binding subobjects lose their
	 * PropertyBindings because DuplicateObject doesn't reconstruct the TMap hash). */
	TArray<TPair<FString, UAnimGraphNode_Base*>> DeferredBindingNodes;

	/* Skeleton-compatible UAnimSequence used as a placeholder for SequencePlayer nodes
	 * whose real sequences are provided by runtime property bindings.  Prevents the
	 * "references an unknown Anim X" compilation error AND the runtime crash when the
	 * anim graph evaluator reads Node.Sequence. */
	UPROPERTY()
	TObjectPtr<UAnimSequence> FallbackAnimSequence = nullptr;

protected:
	/* Global Cached data to reuse */
	UAnimBlueprint* AnimBlueprint = nullptr;
	
	TArray<FString> NodesKeys;
	TArray<FString> ReversedNodesKeys;
	
	TArray<TSharedPtr<FJsonValue>> BakedStateMachines;
	
	TSharedPtr<FJsonObject> RootAnimNodeProperties;
	FUObjectExportContainer* RootAnimNodeContainer = new FUObjectExportContainer();

	/* UE5 Copy Record Cache Data */
	TSharedPtr<FJsonObject> SerializedSparseClassData;

	TArray<FString> SyncGroupNames;
};

REGISTER_IMPORTER(IAnimationBlueprintImporter, (TArray<FString>{ 
	TEXT("AnimBlueprintGeneratedClass")
}), TEXT("Blueprints"));