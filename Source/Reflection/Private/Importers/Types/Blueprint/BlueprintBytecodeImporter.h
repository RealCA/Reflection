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
    UEdGraphNode* EntryNode = nullptr;
    UEdGraphNode* ResultNode = nullptr;
    TArray<const FBytecodeToken*> Statements;
    TMap<int32, int32> StmtIndexToArrayPos;       // StatementIndex -> index in Statements
    TMap<int32, UEdGraphNode*> StatementAnchors; // StatementIndex -> node with exec input/output for that statement
    TMap<FString, UEdGraphPin*> ProducerPins;    // local var name -> pin producing its value
    UEdGraphPin* LastExecPin = nullptr;          // exec output of last emitted anchor
    TSet<int32> VisitedAnchors;                  // StatementIndex of emitted anchors
};

/* Main bytecode importer class */
class FBlueprintBytecodeImporter {
public:
    FBlueprintBytecodeImporter(UBlueprint* InBlueprint, UBlueprintGeneratedClass* InGeneratedClass);

    /* Main entry point - process all functions and bindings */
    bool ProcessFunctions(const TArray<TSharedPtr<FJsonValue>>& JsonObjects);
    
    /* Process DynamicBindingObjects (input bindings) */
    bool ProcessDynamicBindings(const TSharedPtr<FJsonObject>& ClassProperties, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

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

    /* Classify a function into a kind (built-in vs user function) */
    EFunctionKind ClassifyFunction(const ParsedFunction& Func) const;

    /* Does this function's bytecode match the event-thunk shape? */
    bool IsEventThunk(const ParsedFunction& Func) const;

    /* Is this kind emitted as an event node rather than decompiled? */
    bool IsEventKind(EFunctionKind Kind) const;

    /* Extract the ubergraph entry index passed by a thunk (K2Node_CustomEvent_Index) */
    int32 ExtractEntryIndex(const ParsedFunction& Func) const;

    /* Extract the input action / key name embedded in a generated event name */
    FString ExtractEventName(const FString& FuncName, const FString& K2NodeMarker) const;

    /* Resolve a DynamicBindingObjects entry to its exported binding object */
    const TSharedPtr<FJsonObject> ResolveBindingExport(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects) const;

    /* Create the event node for a thunk function (input action, debug key, native event, custom event) */
    UEdGraphNode* CreateEventNode(const ParsedFunction& Func);

    /* Convert a ChildProperties JSON object to an FEdGraphPinType */
    FEdGraphPinType PinTypeFromJson(const TSharedPtr<FJsonObject>& PropObj) const;

    /* Get or create a function graph (not the event graph) for a user function.
     * A newly created graph gets its entry/result skeleton immediately so it is
     * structurally complete before it is ever compiled. */
    UEdGraph* GetOrCreateFunctionGraph(const ParsedFunction& Func);

    /* Create the event graph if it doesn't exist */
    UEdGraph* GetOrCreateEventGraph();

    /* Parse a single Function JSON object */
    ParsedFunction ParseFunction(const TSharedPtr<FJsonObject>& FunctionJson);

    /* Parse bytecode tokens recursively */
    FBytecodeToken ParseBytecodeToken(const TSharedPtr<FJsonObject>& TokenJson);

    /* Main decompiler entry: for each parsed function, build its graph */
    bool BuildFunctionGraphs();

    /* Build graph for a single function */
    bool BuildFunctionGraph(const ParsedFunction& Func);

    /* Compute CFG successors for a statement */
    TArray<int32> ComputeSuccessors(const FFunctionBuilder& Builder, int32 StmtIdx) const;

    /* Emit a statement into the graph, return the anchor node (exec anchor) */
    UEdGraphNode* EmitStatement(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);

    /* Resolve an expression (JSON token) to a pin value */
    FPinValue ResolveExpression(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson);

    /* Emit a side-effecting expression as an exec node (e.g. EX_Context inside a Let RHS).
     * Returns the emitted node; its return value pin can be retrieved separately. */
    UEdGraphNode* EmitExpressionAsExec(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson, int32 StmtIndex);

    /* Resolve a function call from bytecode Function field */
    UFunction* ResolveFunction(const TSharedPtr<FJsonObject>& FuncJson, const FString& ContextClassName = TEXT(""));

    /* Create a call node from bytecode parameters */
    UK2Node_CallFunction* CreateCallNode(FFunctionBuilder& Builder, UFunction* Func, const TArray<TSharedPtr<FJsonValue>>& ParamsJson, UEdGraphPin* TargetPin = nullptr);

    /* Create a variable get/set node */
    UK2Node_VariableGet* CreateVariableGet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass);
    UK2Node_VariableSet* CreateVariableSet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass);

    /* Emit specific statement types */
    UEdGraphNode* EmitContextCall(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitCallMath(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitLet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
    UEdGraphNode* EmitLetValueOnPersistentFrame(FFunctionBuilder& Builder, const FBytecodeToken& Stmt);
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

    /* Helper to find pin by name */
    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction);

    /* Helper to connect two pins */
    void ConnectPins(UEdGraphPin* PinA, UEdGraphPin* PinB);

    /* Process InputDebugKeyDelegateBinding */
    void ProcessInputDebugKeyBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects);

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