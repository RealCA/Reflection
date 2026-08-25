/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Dom/JsonObject.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_Switch.h"
#include "K2Node_Select.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_StructMemberGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Self.h"
#include "K2Node_Literal.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_SetVariableOnPersistentFrame.h"
#include "K2Node_EnhancedInputAction.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "UObject/UObjectGlobals.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;
class UClass;
class FProperty;
struct FEdGraphPinType;

/* How a blueprint function came to be. Detected from name patterns, the shape
 * of its bytecode and its FunctionFlags. Functions that are built-in are never
 * decompiled as standalone graphs - they become event nodes instead, or are
 * referenced directly on the blueprint. */
enum class EFunctionKind : uint8 {
	Ubergraph,             /* ExecuteUbergraph_* - the event graph body */
	ConstructionScript,    /* UserConstructionScript - the construction graph body */
	EnhancedInputAction,   /* InpActEvt_<Action>_K2Node_EnhancedInputActionEvent_<N> */
	InputDebugKey,         /* InpActEvt_<Key>_K2Node_InputDebugKeyEvent_<N> */
	LegacyInputAction,     /* InpActEvt_<Action>_K2Node_InputActionEvent_<N> */
	InputAxis,             /* InpAxisEvt_<Axis>_K2Node_InputAxisEvent_<N> */
	InputKey,              /* InpActEvt_<[Mods+]Key>_K2Node_InputKeyEvent_<N> */
	NativeEvent,           /* Receive* - events declared by a native class */
	CustomEvent,           /* User created event node (thunk into the ubergraph) */
	UserFunction,          /* A real user function graph with a body */
	Unknown
};

/* Runtime binding info read from DynamicBindingObjects. Lets the importer pick
 * the correct trigger pin (Started/Completed/Pressed...) for input event nodes. */
struct FInputBindingInfo {
	FString FunctionName;
	FString InputActionName;   /* IA_LeftClick */
	FString InputActionPath;   /* /Game/TouchyGame/Input/IA_LeftClick */
	FString TriggerEvent;      /* ETriggerEvent::Started / Completed / ... */
	FString KeyName;           /* T */
	FString InputKeyEvent;     /* EInputEvent::IE_Pressed */
	FString NodeType;          /* K2Node_EnhancedInputActionEvent / K2Node_InputDebugKeyEvent ... */
	int32 BindingOrder = INDEX_NONE;  /* Position inside the DynamicBindingObjects array */
	bool bShift = false;
	bool bCtrl = false;
	bool bAlt = false;
	bool bCmd = false;
	bool bExecuteWhenPaused = false;
	bool bConsumeInput = false;
	bool bOverrideParentBinding = false;
};

/* Represents a parsed bytecode token from JSON */
struct FBytecodeToken {
    FString Token;
    int32 StatementIndex = -1;
    TSharedPtr<FJsonObject> JsonData;
    TArray<FBytecodeToken> Children;
};

/* Represents a parsed function from JSON */
struct ParsedFunction {
    FString Name;
    FString Flags;
    EFunctionKind Kind = EFunctionKind::Unknown;
    int32 EntryIndex = INDEX_NONE; /* ubergraph entry index (from thunk's K2Node_CustomEvent_Index) */
    TArray<TSharedPtr<FJsonObject>> ChildProperties;
    TArray<FBytecodeToken> BytecodeTokens;
    TSharedPtr<FJsonObject> SuperStruct;
};

/* Expanded /Engine/StandardMacros loop kinds recognizable in bytecode (docs/plans/006) */
enum class EMacroLoopType : uint8
{
    None,
    WhileLoop,
    ForLoop,
    ForLoopWithBreak,
    ForEachLoop,
    ForEachLoopWithBreak,
    ReverseForEachLoop
};

/* One reconstructed loop cluster. All indices are bytecode statement indices
 * (si) except ExitAddr/IncrementAddr which are bytecode addresses (jump
 * targets). Detection is structural - see plan 006 signature table. */
struct FDetectedLoop {
    EMacroLoopType Type = EMacroLoopType::None;
    int32 PushSi = INDEX_NONE;         // outermost PushExecutionFlow (pushes ExitAddr)
    int32 InnerPushSi = INDEX_NONE;    // push resuming at IncrementAddr (-1 when none)
    int32 ExitAddr = INDEX_NONE;       // Completed-chain head address
    int32 BodyFirst = INDEX_NONE;      // body head (back-edge latch target)
    int32 BodyLast = INDEX_NONE;       // plain pop ending the body
    int32 IncrementAddr = INDEX_NONE;  // resume address of InnerPushSi (counter++)
    int32 CondLast = INDEX_NONE;       // PopExecutionFlowIfNot exiting the loop
    int32 LatchSi = INDEX_NONE;        // backward jump statement
    TArray<int32> BreakSites;          // si of LetBool(breakFlag = true)
    TSharedPtr<FJsonObject> ArrayExpr; // iterated array expression (for macro Array pin)
    FString CounterLocal;              // loop counter temp (invisible outside macro)
    FString IndexLocal;                // exposed Array Index output temp
    FString ElementLocal;              // exposed Element output temp
    FString BreakFlagLocal;            // break flag temp (invisible outside macro)
};

/* Result of resolving an expression: either a producer pin or a constant value */
struct FPinValue {
    UEdGraphPin* Pin = nullptr;
    bool bConstant = false;
    FString ConstString;
    UObject* ConstObject = nullptr;
};

/* State for building a single function's graph */
struct FFunctionBuilder {
    UEdGraph* Graph = nullptr;
    const ParsedFunction* Func = nullptr;
    /* Function whose BytecodeTokens the linear Statements were drawn from.
     * Event thunks and trampolined user functions decompile the ubergraph
     * case body while Func stays the 3-token thunk - loop detection must
     * scan the ubergraph tokens, so this points at it when they differ. */
    const ParsedFunction* TokenSource = nullptr;
    UEdGraphNode* EntryNode = nullptr;
    UEdGraphNode* ResultNode = nullptr;
    TArray<const FBytecodeToken*> Statements;
    TMap<int32, int32> StmtIndexToArrayPos;       // StatementIndex -> index in Statements
    TMap<int32, UEdGraphNode*> StatementAnchors; // StatementIndex -> node with exec input/output for that statement
    TMap<FString, UEdGraphPin*> ProducerPins;    // local var name -> pin producing its value
    TMap<FString, FString> TempConstants;        // temp var name -> constant value (value-guard temps: Select options, literal args)
    /* EX_StructConst assignments to frame temps: literal field data recorded by
     * EmitLet instead of node creation; consumed as split-subpin defaults on
     * calls that take the temp as a struct-by-ref argument (plan 010 item 3). */
    TMap<FString, TMap<FString, FString>> TempStructFields;
    /* Deferred value connections (plan 011 item 2): consumers whose temp
     * producer was unavailable at resolve time. Resolved after the full walk
     * (+ macro splice) so emission order never matters. */
    TArray<TPair<UEdGraphPin*, FString>> PendingDataWires;
    UEdGraphPin* LastExecPin = nullptr;          // exec output of last emitted anchor
    UEdGraphPin* LastBoolPin = nullptr;          // most recently produced bool value pin (for PopExecutionFlowIfNot)
    TSet<int32> VisitedAnchors;                  // StatementIndex of emitted anchors
    TArray<TPair<UEdGraphPin*, int32>> PendingExecWires; // deferred else wires (FromPin, target stmt idx) for forward jump targets
    TSet<int32> RegionStartIndices;              // statement indices that begin a jump-target region (switch case bodies)
    int32 NextNodeX = 0;                         // Next X position for new nodes
    int32 NextNodeY = 0;                         // Next Y position for new nodes
    TMap<FString, UK2Node_MakeStruct*> MakeStructNodes;   // struct type + run segment -> MakeStruct node
    int32 MakeStructRunId = 0;                           // per-construction-run counter (separate nodes per branch)
    TMap<FString, UK2Node_BreakStruct*> BreakStructNodes; // source struct temp var -> BreakStruct node
    /* Pure-read reuse (plan 013): one VariableGet node per variable per function
     * graph. Per-member struct reads each resolved the source variable into a
     * fresh GET, stranding 13 unwired "Appearance Temp" nodes in Change Race. */
    TMap<FString, UK2Node_VariableGet*> VariableGetNodes;
    TSet<FString> FunctionLocalNames;                      // declared locals of this function (SetLocalMember scope)
    TMap<FString, FGuid> FunctionLocalGuids;               // declared local name -> VarGuid (FunctionEntry LocalVariables)

    /* Reconstructed StandardMacro loops (plan 006). Filled by DetectMacroLoops
     * before the emission walk; LoopSuppressedSis lists scaffold statement
     * indices the walk must skip (pushes/pops/cond math/latch). */
    TArray<FDetectedLoop> DetectedLoops;
    TSet<int32> LoopSuppressedSis;
    /* First non-suppressed si of each loop body: the walk lets the incoming
     * exec chain flow across the suppressed init-run into these statements so
     * the emitter's splice can capture that wire onto Macro Exec. */
    TSet<int32> LoopChainBridgeSis;
};

/* Structured per-import diagnostics (plan 013): the same findings that go to
 * the log as warnings, collected so the Dump Blueprint Debug Data tool can
 * write them into a per-asset report instead of forcing a log grep. */
struct FBlueprintImportDiagnostics {
    FString AssetName;
    TArray<FString> Lines;                              // "[Category] text"
    TMap<FString, TMap<int32, FString>> StatementNodes; // graph -> bytecode si -> emitted node title

    void Reset(const FString& InAssetName)
    {
        AssetName = InAssetName;
        Lines.Reset();
        StatementNodes.Reset();
    }

    void Add(const FString& Category, const FString& Text)
    {
        Lines.Add(FString::Printf(TEXT("[%s] %s"), *Category, *Text));
    }
};

/* Main bytecode importer class */
class FBlueprintBytecodeImporter {
public:
    FBlueprintBytecodeImporter(UBlueprint* InBlueprint, UBlueprintGeneratedClass* InGeneratedClass);

    /* Diagnostics of the most recent import (shared across instances - the
     * dump tool runs long after the import itself). */
    static FBlueprintImportDiagnostics LastImportDiagnostics;

    /* Appends to LastImportDiagnostics; safe to call anywhere. */
    void AddDiagnostic(const FString& Category, const FString& Text) const;

    /* Main entry point - process all functions and bindings */
    bool ProcessFunctions(const TArray<TSharedPtr<FJsonValue>>& JsonObjects);
    
    /* Process DynamicBindingObjects (input bindings) */
    bool ProcessDynamicBindings(const TSharedPtr<FJsonObject>& ClassProperties, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

    /* Create a typed FProperty (without flags) from a ChildProperties JSON object.
     * Returns nullptr for unsupported types; the caller owns the property.
     * Owner is FFieldVariant so FArrayProperty can recurse with itself as the
     * inner element's owner (fields are not UObjects in UE5). */
    static FProperty* CreatePropertyFromJson(FFieldVariant Owner, const FString& PropName,
        const TSharedPtr<FJsonObject>& PropJson);

    /* Parse a FUNC_* flag string from the JSON export into EFunctionFlags bits. */
    static EFunctionFlags ParseFunctionFlags(const FString& FlagsStr);

    /* Populate a UFunction with its parameter/return properties from ChildProperties,
     * then Bind + StaticLink so entry/result nodes get proper typed pins. Used by the
     * full importer (BuildScaffoldFunction) and by the stub path so stub functions
     * carry their real signatures. */
    static void PopulateFunctionProperties(UFunction* FuncObj, const TArray<TSharedPtr<FJsonObject>>& ChildProperties);

private:
    UBlueprint* Blueprint;
    UBlueprintGeneratedClass* GeneratedClass;
    UEdGraph* EventGraph;

    /* Cache of parsed functions */
    TMap<FString, ParsedFunction> ParsedFunctions;

    /* Event node created for each thunk function (keyed by function name) */
    TMap<FString, UEdGraphNode*> EventNodes;

    /* Input bindings read from DynamicBindingObjects (keyed by FunctionNameToBind) */
    TMap<FString, FInputBindingInfo> InputBindings;

    /* Y position tracker for event nodes (created before builders exist) */
    int32 EventNodeY = 0;

    /* Ubergraph territory guards (plan 009): raw EntryIndex of every event-kind
     * function marks where each event's section begins in the shared ubergraph
     * bytecode; a linear walk stops when it reaches a foreign section instead of
     * absorbing it. Claims record which function collected each statement index
     * for cross-event overlap diagnostics. */
    TSet<int32> UbergraphEventEntrySis;
    TMap<int32, FString> ClaimedSiOwners;

    /* Variable Registry (plan 011 item 0): classification + canonical pin type
     * straight from ChildProperties flags - never inferred from names. */
    enum class EVarKind : uint8 { FrameTemp, FunctionParm, GraphVariable };
    struct FVarInfo { FEdGraphPinType PinType; EVarKind Kind; };
    TMap<FString, FVarInfo> UbergraphFrameLocals;   // ubergraph frame entries
    TMap<FString, FVarInfo> ClassMemberVars;        // blueprint graph variables

    void BuildVariableRegistry(const TArray<TSharedPtr<FJsonValue>>& Properties, EVarKind DefaultKind);
    const FVarInfo* FindVariableInfo(const FString& Name, EVarKind& OutKind) const;
    void RegisterProducer(FFunctionBuilder& Builder, const FString& Name, UEdGraphPin* Pin) const;
    UEdGraphPin* FindProducerExact(FFunctionBuilder& Builder, const FString& Name);
    void ResolvePendingDataWires(FFunctionBuilder& Builder);
    void LogWiringAudit(FFunctionBuilder& Builder, const FString& GraphLabel);

    /* Exact parameter->frame-slot bindings parsed from each function's thunk
     * (EX_LetValueOnPersistentFrame assignments) - plan 011 follow-up B. */
    TMap<FString, TArray<TPair<FString, FString>>> ThunkParamBindings;
    /* Every frame slot any Let-family statement ever writes - reads of slots
     * outside this set are compiler bookkeeping/inputs and must never become
     * VariableGet/SET nodes. */
    TSet<FString> WrittenUbergraphSlots;

    /* Pop-statement targets reconstructed from BuildLinearPath's FlowStack
     * (plan 010 follow-up): pop opcodes carry no address in the export JSON -
     * the destination exists only on the compiler's push/pop stack. Keyed by
     * pop statement index -> popped target statement index. */
    TMap<int32, int32> UbergraphPopTargets;

    /* Name of the function whose linear path is currently being collected
     * (BuildLinearPath attributes claims to it). */
    FString ActiveUbergraphOwnerName;

    /* Classify a function into a kind (built-in vs user function) */
    EFunctionKind ClassifyFunction(const ParsedFunction& Func) const;

    /* Is this kind emitted as an event node rather than decompiled? */
    bool IsEventKind(EFunctionKind Kind) const;

    /* Extract the ubergraph entry index passed by a thunk (K2Node_CustomEvent_Index) */
    int32 ExtractEntryIndex(const ParsedFunction& Func) const;

    /* Extract the input action / key name embedded in a generated event name */
    FString ExtractEventName(const FString& FuncName, const FString& K2NodeMarker) const;

    /* Resolve a DynamicBindingObjects entry to its exported binding object */
    const TSharedPtr<FJsonObject> ResolveBindingExport(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects) const;

    /* Create the event node for a thunk function (input action, debug key, native event, custom event) */
    UEdGraphNode* CreateEventNode(const ParsedFunction& Func, int32 OffsetX = 0);

    /* Convert a ChildProperties JSON object to an FEdGraphPinType */
    FEdGraphPinType PinTypeFromJson(const TSharedPtr<FJsonObject>& PropObj) const;

    /* Get or create a function graph (not the event graph) for a user function.
     * A newly created graph gets its entry/result skeleton immediately so it is
     * structurally complete before it is ever compiled. */
    UEdGraph* GetOrCreateFunctionGraph(const ParsedFunction& Func);

    /* Create the event graph if it doesn't exist */
    UEdGraph* GetOrCreateEventGraph();

    /* Position a node using the builder's NextNodeX/NextNodeY, then advance Y.
     * Call this after AddNode + AllocateDefaultPins. */
    void PositionNode(UEdGraphNode* Node, FFunctionBuilder& Builder);

    /* Parse a single Function JSON object */
    ParsedFunction ParseFunction(const TSharedPtr<FJsonObject>& FunctionJson);

    /* Parse bytecode tokens recursively */
    FBytecodeToken ParseBytecodeToken(const TSharedPtr<FJsonObject>& TokenJson);

    /* Main decompiler entry: for each parsed function, build its graph */
    bool BuildFunctionGraphs();

    /* Create UFunction objects with properties from ChildProperties so entry/result
     * nodes get proper typed pins. Skips internal temps (CallFunc_*, K2Node_*, Temp_*). */
    void CreateFunctionProperties();

    /* Declare the frame-local properties a function body references (owned by the
     * ubergraph function) as real function-local variables: an FProperty on the
     * function's UFunction, an FBPVariableDescription on the entry node, and the
     * names on the builder so variable get/set nodes use SetLocalMember. */
    void CreateFunctionLocalVariables(FFunctionBuilder& Builder, const ParsedFunction& Ubergraph,
        const TArray<const FBytecodeToken*>& Stmts);

    /* Find the parsed ubergraph function (ExecuteUbergraph_*), or nullptr */
    const ParsedFunction* FindUbergraph() const;

    /* Clear content from a previous import so a re-import rebuilds from scratch:
     * removes importer-created UFunction objects from the generated class and every
     * node from the ubergraph pages and function graphs. Safe on first import too. */
    void ClearExistingGraphContent();

    /* Build graph for a single function, offset X by FuncOffsetX */
    bool BuildFunctionGraph(const ParsedFunction& Func, int32 FuncOffsetX = 0);

    /* Compute CFG successors for a statement */
    TArray<int32> ComputeSuccessors(const FFunctionBuilder& Builder, int32 StmtIdx) const;

    /* Follow the linear control-flow path (exec chain) starting at StartIdx,
     * appending statements in execution order. Tracks the execution-flow stack
     * (PushExecutionFlow/PopExecutionFlow) so body-terminating pops correctly
     * exit instead of bleeding into the next region. Stops at loop-back edges
     * (a target already in the path), Return/EndOfScript and ComputedJump. */
    bool BuildLinearPath(const ParsedFunction& Ubergraph, int32 StartIdx,
        TArray<const FBytecodeToken*>& OutPath, TSet<int32>& OutIndices);

    /* Statement index of the token immediately after the one at StmtIdx, or -1 */
    int32 NextSequentialStatementIndex(const ParsedFunction& Ubergraph, int32 StmtIdx) const;

    /* Emit a statement into the graph, return the anchor node (exec anchor) */
    UEdGraphNode* EmitStatement(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);

    /* Resolve an expression (JSON token) to a pin value */
    FPinValue ResolveExpression(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson);
    FPinValue ResolveSwitchValue(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson);

    /* Emit a side-effecting expression as an exec node (e.g. EX_Context inside a Let RHS).
     * Returns the emitted node; its return value pin can be retrieved separately. */
    UEdGraphNode* EmitExpressionAsExec(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson, int32 StmtIndex);

    /* Resolve a function call from bytecode Function field */
    UFunction* ResolveFunction(const TSharedPtr<FJsonObject>& FuncJson, const FString& ContextClassName = TEXT(""));

    /* Create a call node from bytecode parameters */
    UK2Node_CallFunction* CreateCallNode(FFunctionBuilder& Builder, UFunction* Func, const TArray<TSharedPtr<FJsonValue>>& ParamsJson, UEdGraphPin* TargetPin = nullptr);

    /* Create a variable get/set node */
    UK2Node_VariableGet* CreateVariableGet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass, const TSharedPtr<FJsonObject>& PropObj = nullptr);
    UK2Node_VariableSet* CreateVariableSet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass);

    /* Emit specific statement types */
    UEdGraphNode* EmitContextCall(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitCallMath(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitLet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitLetValueOnPersistentFrame(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitMakeStructFieldSet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitStructFieldRefWrite(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitJump(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitJumpIfNot(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitPushExecutionFlow(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitPopExecutionFlow(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitPopExecutionFlowIfNot(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitComputedJump(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitReturn(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitSwitchValue(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);

    /* Wire exec from source to destination anchor */
    void WireExec(FFunctionBuilder& Builder, UEdGraphNode* FromNode, int32 ToStmtIdx);
    void WireExecFromPin(FFunctionBuilder& Builder, UEdGraphPin* FromPin, int32 ToStmtIdx);

    /* Resolve deferred exec wires (forward jump targets like switch case bodies)
     * once all statements have been emitted and their anchors exist. */
    void ResolvePendingExecWires(FFunctionBuilder& Builder);

    /* --- StandardMacro loop reconstruction (plan 006) --- */

    /* Structural detection of expanded /Engine/StandardMacros loops over
     * Builder.Func bytecode. Fills DetectedLoops and LoopSuppressedSis.
     * Conservative: clusters below the signature threshold are ignored and
     * keep the flat reconstruction. */
    void DetectMacroLoops(FFunctionBuilder& Builder);

    /* After the flat walk: create K2Node_MacroInstance nodes for detected
     * loops, splice them into the emitted chain (Exec/LoopBody/Completed/
     * Break) and repoint consumers of macro-internal temps (Element /
     * Array Index) onto the instance output pins. */
    void EmitMacroLoopNodes(FFunctionBuilder& Builder);

    /* Extract the short math function name ("Add_IntInt") from an
     * EX_CallMath token's Function.ObjectName, or empty when not math. */
    static FString CallMathFunctionName(const FBytecodeToken& Tok);

    /* Strip the compiler-generated suffix ("_<Index>_<32hex>") from a user-defined
     * struct member name so fields from different structs can be matched (e.g. a
     * MakeStruct pin "Race_2_..." wired from a BreakStruct pin "Race_67_..."). */
    FString StripStructMemberSuffix(const FString& FullName) const;

    /* Helper to find pin by name */
    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);

    /* Helper to connect two pins */
    void ConnectPins(UEdGraphPin* PinA, UEdGraphPin* PinB);

    /* Process InputDebugKeyDelegateBinding */
    void ProcessInputDebugKeyBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

    /* Process InputKeyDelegateBinding (K2Node_InputKey events: InpActEvt_<Key>_K2Node_InputKeyEvent_<N>) */
    void ProcessInputKeyBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

    /* Process EnhancedInputActionDelegateBinding */
    void ProcessEnhancedInputBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

    /* Find function by name in parsed functions */
    const ParsedFunction* FindFunction(const FString& Name) const;

    /* Create a stub function node for missing functions */
    UK2Node_CallFunction* CreateStubCallNode(const FString& FunctionName, UEdGraph* Graph);

    /* Create entry node for a function type */
    UEdGraphNode* CreateEntryNode(const ParsedFunction& Func, UEdGraph* Graph);

    /* Create result node for a function type */
    UEdGraphNode* CreateResultNode(const ParsedFunction& Func, UEdGraph* Graph);
};