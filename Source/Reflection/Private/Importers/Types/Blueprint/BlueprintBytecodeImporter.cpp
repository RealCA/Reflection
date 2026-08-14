/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintBytecodeImporter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_Select.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_StructMemberGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Self.h"
#include "K2Node_Literal.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "InputAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBytecodeImporter, Log, All);

// ============================================================================
// Constructor and top-level entry
// ============================================================================

FBlueprintBytecodeImporter::FBlueprintBytecodeImporter(UBlueprint* InBlueprint, UBlueprintGeneratedClass* InGeneratedClass)
    : Blueprint(InBlueprint)
    , GeneratedClass(InGeneratedClass)
    , EventGraph(nullptr)
{
}

bool FBlueprintBytecodeImporter::ProcessFunctions(const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!Blueprint || !GeneratedClass)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Error, TEXT("Invalid blueprint or generated class"));
        return false;
    }

    // Step 1: Parse all function JSON objects
    for (const TSharedPtr<FJsonValue>& JsonValue : JsonObjects)
    {
        const TSharedPtr<FJsonObject>& JsonObject = JsonValue->AsObject();
        if (!JsonObject) continue;

        if (!JsonObject->HasField(TEXT("Type"))) continue;
        const FString& TypeName = JsonObject->GetStringField(TEXT("Type"));
        if (TypeName != TEXT("Function")) continue;

        ParsedFunction Func = ParseFunction(JsonObject);
        if (!Func.Name.IsEmpty())
        {
            Func.Kind = ClassifyFunction(Func);
            Func.EntryIndex = ExtractEntryIndex(Func);
            ParsedFunctions.Add(Func.Name, MoveTemp(Func));
        }
    }

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Parsed %d functions"), ParsedFunctions.Num());

    // Step 2: Create the event graph
    EventGraph = GetOrCreateEventGraph();
    if (!EventGraph)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Error, TEXT("Failed to create event graph"));
        return false;
    }

    // Step 3: Build function graphs with linking
    BuildFunctionGraphs();

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Successfully processed blueprint functions"));
    return true;
}

// ============================================================================
// Parsing
// ============================================================================

UEdGraph* FBlueprintBytecodeImporter::GetOrCreateEventGraph()
{
    if (EventGraph)
    {
        return EventGraph;
    }

    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph && Graph->GetName() == TEXT("EventGraph"))
        {
            EventGraph = Graph;
            return EventGraph;
        }
    }

    EventGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        TEXT("EventGraph"),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass()
    );

    if (EventGraph)
    {
        Blueprint->UbergraphPages.Add(EventGraph);
    }

    return EventGraph;
}

UEdGraph* FBlueprintBytecodeImporter::GetOrCreateFunctionGraph(const ParsedFunction& Func)
{
    const FString& FuncName = Func.Name;

    // Reuse an existing function graph with the same name
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName() == FuncName)
        {
            return Graph;
        }
    }

    UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*FuncName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass()
    );

    if (FunctionGraph)
    {
        FunctionGraph->bAllowDeletion = false;
        Blueprint->FunctionGraphs.Add(FunctionGraph);

        // Build the graph skeleton (entry + result nodes, pins allocated) at
        // creation time so the graph is structurally complete before it is ever
        // compiled. There is deliberately no MarkBlueprintAsStructurallyModified
        // here: that synchronously compiles the whole blueprint mid-import, which
        // crashed on the half-built graphs. The importer compiles once at the end
        // (BlueprintImporter::ProcessBytecode) after every graph is fully built.
        CreateEntryNode(Func, FunctionGraph);
        CreateResultNode(Func, FunctionGraph);
    }

    return FunctionGraph;
}

ParsedFunction FBlueprintBytecodeImporter::ParseFunction(const TSharedPtr<FJsonObject>& FunctionJson)
{
    ParsedFunction Func;

    Func.Name = FunctionJson->GetStringField(TEXT("Name"));

    if (FunctionJson->HasField(TEXT("FunctionFlags")))
    {
        Func.Flags = FunctionJson->GetStringField(TEXT("FunctionFlags"));
    }

    if (FunctionJson->HasField(TEXT("SuperStruct")))
    {
        Func.SuperStruct = FunctionJson->GetObjectField(TEXT("SuperStruct"));
    }

    if (FunctionJson->HasField(TEXT("ChildProperties")))
    {
        const TArray<TSharedPtr<FJsonValue>>& ChildProps = FunctionJson->GetArrayField(TEXT("ChildProperties"));
        for (const TSharedPtr<FJsonValue>& PropValue : ChildProps)
        {
            const TSharedPtr<FJsonObject>& PropObj = PropValue->AsObject();
            if (PropObj)
            {
                Func.ChildProperties.Add(PropObj);
            }
        }
    }

    if (FunctionJson->HasField(TEXT("ScriptBytecode")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Bytecode = FunctionJson->GetArrayField(TEXT("ScriptBytecode"));
        for (const TSharedPtr<FJsonValue>& TokenValue : Bytecode)
        {
            const TSharedPtr<FJsonObject>& TokenJson = TokenValue->AsObject();
            if (TokenJson)
            {
                FBytecodeToken Token = ParseBytecodeToken(TokenJson);
                Func.BytecodeTokens.Add(MoveTemp(Token));
            }
        }
    }

    return Func;
}

FBytecodeToken FBlueprintBytecodeImporter::ParseBytecodeToken(const TSharedPtr<FJsonObject>& TokenJson)
{
    FBytecodeToken Token;
    Token.JsonData = TokenJson;
    Token.Token = TokenJson->GetStringField(TEXT("Token"));

    if (TokenJson->HasField(TEXT("StatementIndex")))
    {
        Token.StatementIndex = TokenJson->GetIntegerField(TEXT("StatementIndex"));
    }

    return Token;
}

// ============================================================================
// Function classification (detector)
// ============================================================================

EFunctionKind FBlueprintBytecodeImporter::ClassifyFunction(const ParsedFunction& Func) const
{
    const FString& Name = Func.Name;

    if (Name.StartsWith(TEXT("ExecuteUbergraph_")))
    {
        return EFunctionKind::Ubergraph;
    }

    if (Name == TEXT("UserConstructionScript"))
    {
        return EFunctionKind::ConstructionScript;
    }

    if (Name.StartsWith(TEXT("InpActEvt_")) || Name.StartsWith(TEXT("InpAxisEvt_")))
    {
        if (Name.Contains(TEXT("K2Node_EnhancedInputActionEvent")))
        {
            return EFunctionKind::EnhancedInputAction;
        }
        if (Name.Contains(TEXT("K2Node_InputDebugKeyEvent")))
        {
            return EFunctionKind::InputDebugKey;
        }
        if (Name.Contains(TEXT("K2Node_InputActionEvent")))
        {
            return EFunctionKind::LegacyInputAction;
        }
        if (Name.Contains(TEXT("K2Node_InputAxisEvent")))
        {
            return EFunctionKind::InputAxis;
        }
        return EFunctionKind::CustomEvent;
    }

    if (Name.StartsWith(TEXT("Receive")))
    {
        return EFunctionKind::NativeEvent;
    }

    if (IsEventThunk(Func))
    {
        return EFunctionKind::CustomEvent;
    }

    return EFunctionKind::UserFunction;
}

bool FBlueprintBytecodeImporter::IsEventKind(EFunctionKind Kind) const
{
    return Kind == EFunctionKind::EnhancedInputAction
        || Kind == EFunctionKind::InputDebugKey
        || Kind == EFunctionKind::LegacyInputAction
        || Kind == EFunctionKind::InputAxis
        || Kind == EFunctionKind::NativeEvent
        || Kind == EFunctionKind::CustomEvent;
}

bool FBlueprintBytecodeImporter::IsEventThunk(const ParsedFunction& Func) const
{
    /* A thunk's top-level bytecode is:
     *   EX_LetValueOnPersistentFrame <K2Node_*_Index> = <entry index>
     *   EX_LocalFinalFunction ExecuteUbergraph_*()
     *   EX_Return
     * Real function graphs never call the ubergraph. */
    for (const FBytecodeToken& Token : Func.BytecodeTokens)
    {
        const FString& T = Token.Token;
        if (T == TEXT("EX_LocalFinalFunction") || T == TEXT("EX_FinalFunction") ||
            T == TEXT("EX_LocalVirtualFunction") || T == TEXT("EX_VirtualFunction"))
        {
            if (!Token.JsonData || !Token.JsonData->HasField(TEXT("Function")))
            {
                continue;
            }

            const TSharedPtr<FJsonObject>& FuncObj = Token.JsonData->GetObjectField(TEXT("Function"));
            if (!FuncObj)
            {
                continue;
            }

            const FString ObjectName = FuncObj->GetStringField(TEXT("ObjectName"));
            if (ObjectName.Contains(TEXT("ExecuteUbergraph")))
            {
                return true;
            }
        }
    }
    return false;
}

int32 FBlueprintBytecodeImporter::ExtractEntryIndex(const ParsedFunction& Func) const
{
    /* The thunk calls ExecuteUbergraph with the entry index as its only parameter:
     *   Parameters: [ { "Token": "EX_IntConst", "Value": 9107 } ] */
    for (const FBytecodeToken& Token : Func.BytecodeTokens)
    {
        const FString& T = Token.Token;
        if (T == TEXT("EX_LocalFinalFunction") || T == TEXT("EX_FinalFunction") ||
            T == TEXT("EX_LocalVirtualFunction") || T == TEXT("EX_VirtualFunction"))
        {
            if (!Token.JsonData || !Token.JsonData->HasField(TEXT("Parameters")))
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>& Params = Token.JsonData->GetArrayField(TEXT("Parameters"));
            if (Params.Num() > 0)
            {
                const TSharedPtr<FJsonObject>& ParamObj = Params[0]->AsObject();
                if (ParamObj && ParamObj->GetStringField(TEXT("Token")) == TEXT("EX_IntConst"))
                {
                    return ParamObj->GetIntegerField(TEXT("Value"));
                }
            }
        }
    }
    return INDEX_NONE;
}

FString FBlueprintBytecodeImporter::ExtractEventName(const FString& FuncName, const FString& K2NodeMarker) const
{
    /* FuncName = "InpActEvt_<Action>_K2Node_EnhancedInputActionEvent_0" */
    const int32 MarkerIdx = FuncName.Find(K2NodeMarker);
    if (MarkerIdx == INDEX_NONE)
    {
        return FString();
    }

    const FString Prefix = TEXT("InpActEvt_");
    if (!FuncName.StartsWith(Prefix))
    {
        return FString();
    }

    FString Inner = FuncName.Mid(Prefix.Len(), MarkerIdx - Prefix.Len());
    return Inner.Replace(TEXT("_"), TEXT(" "));
}

// ============================================================================
// Function graph building
// ============================================================================

bool FBlueprintBytecodeImporter::BuildFunctionGraphs()
{
    // First pass: create event nodes for event-like kinds so they exist on the
    // event graph before we decompile the ubergraph body.
    for (const auto& Pair : ParsedFunctions)
    {
        const ParsedFunction& Func = Pair.Value;
        if (IsEventKind(Func.Kind))
        {
            UEdGraphNode* EventNode = CreateEventNode(Func);
            if (EventNode)
            {
                EventNodes.Add(Func.Name, EventNode);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Created event node for: %s"), *Func.Name);
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Failed to create event node for: %s"), *Func.Name);
            }
        }
    }

    // Second pass: decompile function bodies.
    for (const auto& Pair : ParsedFunctions)
    {
        const ParsedFunction& Func = Pair.Value;
        BuildFunctionGraph(Func);
    }
    return true;
}

bool FBlueprintBytecodeImporter::BuildFunctionGraph(const ParsedFunction& Func)
{
    // Event-like kinds have their body in the ubergraph at EntryIndex.
    // Decompile the body and wire it to the event node's exec output pin.
    if (IsEventKind(Func.Kind))
    {
        UEdGraphNode** FoundNode = EventNodes.Find(Func.Name);
        if (!FoundNode || !*FoundNode)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No event node for: %s"), *Func.Name);
            return false;
        }

        // Find the ubergraph function
        const ParsedFunction* Ubergraph = ParsedFunctions.Find(TEXT("ExecuteUbergraph_BP_CharCreation"));
        if (!Ubergraph)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No ubergraph found for event: %s"), *Func.Name);
            return false;
        }

        // Collect all event offsets to determine body boundaries
        TArray<int32> EventOffsets;
        for (const auto& Pair : ParsedFunctions)
        {
            if (IsEventKind(Pair.Value.Kind) && Pair.Value.EntryIndex >= 0)
            {
                EventOffsets.Add(Pair.Value.EntryIndex);
            }
        }
        EventOffsets.Sort();

        // Find next event offset (end boundary for this event's body)
        int32 EndOffset = MAX_int32;
        for (int32 Off : EventOffsets)
        {
            if (Off > Func.EntryIndex)
            {
                EndOffset = Off;
                break;
            }
        }

        // Extract statements from ubergraph within [EntryIndex, EndOffset)
        TArray<const FBytecodeToken*> EventStmts;
        bool bCollecting = false;
        for (const FBytecodeToken& Token : Ubergraph->BytecodeTokens)
        {
            if (Token.StatementIndex == Func.EntryIndex)
            {
                bCollecting = true;
            }

            if (bCollecting)
            {
                if (Token.StatementIndex >= EndOffset)
                {
                    break;
                }
                EventStmts.Add(&Token);
            }
        }

        if (EventStmts.Num() == 0)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No body statements for event: %s at offset %d"), *Func.Name, Func.EntryIndex);
            return false;
        }

        // Build the body using the event node as entry
        FFunctionBuilder Builder;
        Builder.Graph = EventGraph;
        Builder.Func = &Func;
        Builder.EntryNode = *FoundNode;
        Builder.ResultNode = nullptr;

        // Build statement index map
        for (int32 Idx = 0; Idx < EventStmts.Num(); ++Idx)
        {
            const FBytecodeToken* Stmt = EventStmts[Idx];
            Builder.Statements.Add(Stmt);
            Builder.StmtIndexToArrayPos.Add(Stmt->StatementIndex, Builder.Statements.Num() - 1);
        }

        // Determine the correct exec output pin name
        UEdGraphPin* EventExecPin = nullptr;
        if (Func.Kind == EFunctionKind::EnhancedInputAction)
        {
            const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
            if (Binding && !Binding->TriggerEvent.IsEmpty())
            {
                EventExecPin = FindPin(Builder.EntryNode, *Binding->TriggerEvent, EGPD_Output);
            }
        }
        if (!EventExecPin)
        {
            EventExecPin = FindPin(Builder.EntryNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
        }
        if (!EventExecPin)
        {
            EventExecPin = FindPin(Builder.EntryNode, TEXT("then"), EGPD_Output);
        }
        Builder.LastExecPin = EventExecPin;

        // Process each statement
        for (int32 Idx = 0; Idx < Builder.Statements.Num(); ++Idx)
        {
            const FBytecodeToken* Stmt = Builder.Statements[Idx];

            if (Stmt->Token == TEXT("EX_EndOfScript") || Stmt->Token == TEXT("EX_Nothing"))
            {
                continue;
            }

            UEdGraphNode* Anchor = EmitStatement(Builder, *Stmt);
            if (Anchor)
            {
                Builder.StatementAnchors.Add(Stmt->StatementIndex, Anchor);
                Builder.VisitedAnchors.Add(Stmt->StatementIndex);
            }
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Decompiled event body: %s (%d statements, pin=%s)"),
            *Func.Name, EventStmts.Num(),
            EventExecPin ? *EventExecPin->PinName.ToString() : TEXT("NONE"));
        return true;
    }

    UEdGraph* TargetGraph = EventGraph;
    if (Func.Kind == EFunctionKind::UserFunction)
    {
        TargetGraph = GetOrCreateFunctionGraph(Func);
        if (!TargetGraph)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Error, TEXT("Failed to get or create function graph for: %s"), *Func.Name);
            return false;
        }
    }

    FFunctionBuilder Builder;
    Builder.Graph = TargetGraph;
    Builder.Func = &Func;

    // User function graphs already carry their entry/result skeleton (created
    // together with the graph in GetOrCreateFunctionGraph). Locate those; event
    // graph bodies create theirs here.
    TArray<UK2Node_FunctionEntry*> EntryNodes;
    TargetGraph->GetNodesOfClass(EntryNodes);
    Builder.EntryNode = EntryNodes.Num() > 0 ? EntryNodes[0] : CreateEntryNode(Func, TargetGraph);

    TArray<UK2Node_FunctionResult*> ResultNodes;
    TargetGraph->GetNodesOfClass(ResultNodes);
    Builder.ResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : CreateResultNode(Func, TargetGraph);

    if (!Builder.EntryNode)
    {
        return false;
    }

    // Build statement list
    Builder.Statements.Reserve(Func.BytecodeTokens.Num());
    for (const FBytecodeToken& Token : Func.BytecodeTokens)
    {
        Builder.Statements.Add(&Token);
        Builder.StmtIndexToArrayPos.Add(Token.StatementIndex, Builder.Statements.Num() - 1);
    }

    if (Builder.Statements.Num() == 0)
    {
        return false;
    }

    // Wire entry exec to first statement
    UEdGraphPin* EntryExecPin = FindPin(Builder.EntryNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (!EntryExecPin)
    {
        EntryExecPin = FindPin(Builder.EntryNode, TEXT("then"), EGPD_Output);
    }
    Builder.LastExecPin = EntryExecPin;

    // Process each statement in order
    for (int32 Idx = 0; Idx < Builder.Statements.Num(); ++Idx)
    {
        const FBytecodeToken* Stmt = Builder.Statements[Idx];

        // Skip EndOfScript and Nothing
        if (Stmt->Token == TEXT("EX_EndOfScript") || Stmt->Token == TEXT("EX_Nothing"))
        {
            continue;
        }

        UEdGraphNode* Anchor = EmitStatement(Builder, *Stmt);
        if (Anchor)
        {
            Builder.StatementAnchors.Add(Stmt->StatementIndex, Anchor);
            Builder.VisitedAnchors.Add(Stmt->StatementIndex);
        }
    }

    // Wire result node exec if present
    if (Builder.ResultNode && Builder.LastExecPin)
    {
        UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ResultExecPin && Builder.LastExecPin != ResultExecPin)
        {
            ConnectPins(Builder.LastExecPin, ResultExecPin);
        }
    }

    return true;
}

// ============================================================================
// Entry / Result node creation
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::CreateEntryNode(const ParsedFunction& Func, UEdGraph* Graph)
{
    UK2Node_FunctionEntry* EntryNode = NewObject<UK2Node_FunctionEntry>(Graph);
    EntryNode->CreateNewGuid();
    EntryNode->SetFlags(RF_Transactional);
    EntryNode->NodePosX = -600;
    EntryNode->NodePosY = 0;
    EntryNode->CustomGeneratedFunctionName = FName(*Func.Name);
    Graph->AddNode(EntryNode, true, true);
    EntryNode->AllocateDefaultPins();
    return EntryNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::CreateResultNode(const ParsedFunction& Func, UEdGraph* Graph)
{
    // Events and ubergraph don't need explicit result nodes
    if (Func.Kind == EFunctionKind::Ubergraph || Func.Kind == EFunctionKind::ConstructionScript)
    {
        return nullptr;
    }

    UK2Node_FunctionResult* ResultNode = NewObject<UK2Node_FunctionResult>(Graph);
    ResultNode->CreateNewGuid();
    ResultNode->SetFlags(RF_Transactional);
    ResultNode->NodePosX = 600;
    ResultNode->NodePosY = 0;
    Graph->AddNode(ResultNode, true, true);
    ResultNode->AllocateDefaultPins();
    return ResultNode;
}

// ============================================================================
// Event node creation
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::CreateEventNode(const ParsedFunction& Func)
{
    UEdGraph* Graph = GetOrCreateEventGraph();
    if (!Graph)
    {
        return nullptr;
    }

    switch (Func.Kind)
    {
    case EFunctionKind::EnhancedInputAction:
    {
        /* UK2Node_EnhancedInputAction: node for an input action. It creates an
         * exec output pin per trigger event (Triggered/Started/Completed/...)
         * and the binding tells us which one this thunk corresponds to. */
        const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
        UK2Node_EnhancedInputAction* ActionNode = NewObject<UK2Node_EnhancedInputAction>(Graph);
        ActionNode->CreateNewGuid();
        ActionNode->SetFlags(RF_Transactional);
        ActionNode->NodePosX = -800;
        ActionNode->NodePosY = 0;

        if (Binding)
        {
            // InputActionPath is already stripped of the ".0" export suffix in the binding.
            if (UInputAction* InputAction = LoadObject<UInputAction>(nullptr, *Binding->InputActionPath))
            {
                ActionNode->InputAction = InputAction;
            }
        }

        Graph->AddNode(ActionNode, true, true);
        ActionNode->AllocateDefaultPins();
        return ActionNode;
    }

    case EFunctionKind::InputDebugKey:
    {
        /* The dedicated UK2Node_InputDebugKey lives in a private engine header,
         * so fall back to a generic event node bound to the generated function.
         * The binding supplies the key and event, which is logged for reference. */
        const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
        if (Binding)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("InputDebugKey event: %s -> Key=%s Event=%s"), *Func.Name, *Binding->KeyName, *Binding->InputKeyEvent);
        }

        UK2Node_Event* DebugKeyNode = NewObject<UK2Node_Event>(Graph);
        DebugKeyNode->CreateNewGuid();
        DebugKeyNode->SetFlags(RF_Transactional);
        DebugKeyNode->NodePosX = -800;
        DebugKeyNode->NodePosY = 0;
        DebugKeyNode->CustomFunctionName = FName(*Func.Name);
        DebugKeyNode->EventReference.SetExternalMember(FName(*Func.Name), GeneratedClass);
        Graph->AddNode(DebugKeyNode, true, true);
        DebugKeyNode->AllocateDefaultPins();
        return DebugKeyNode;
    }

    case EFunctionKind::LegacyInputAction:
    {
        /* UK2Node_InputAction: legacy ActionBindings, no trigger sub-pins. */
        UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
        InputActionNode->CreateNewGuid();
        InputActionNode->SetFlags(RF_Transactional);
        InputActionNode->NodePosX = -800;
        InputActionNode->NodePosY = 0;

        const FString ActionName = ExtractEventName(Func.Name, TEXT("K2Node_InputActionEvent"));
        if (!ActionName.IsEmpty())
        {
            InputActionNode->InputActionName = FName(*ActionName);
        }

        Graph->AddNode(InputActionNode, true, true);
        InputActionNode->AllocateDefaultPins();
        return InputActionNode;
    }

    case EFunctionKind::InputAxis:
    {
        UK2Node_InputAxisEvent* InputAxisNode = NewObject<UK2Node_InputAxisEvent>(Graph);
        InputAxisNode->CreateNewGuid();
        InputAxisNode->SetFlags(RF_Transactional);
        InputAxisNode->NodePosX = -800;
        InputAxisNode->NodePosY = 0;

        const FString AxisName = ExtractEventName(Func.Name, TEXT("K2Node_InputAxisEvent"));
        if (!AxisName.IsEmpty())
        {
            InputAxisNode->InputAxisName = FName(*AxisName);
        }

        Graph->AddNode(InputAxisNode, true, true);
        InputAxisNode->AllocateDefaultPins();
        return InputAxisNode;
    }

    case EFunctionKind::NativeEvent:
    {
        /* Receive* events are declared by the native super class; reference it
         * directly on the blueprint instead of decompiling the thunk. */
        UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
        EventNode->CreateNewGuid();
        EventNode->SetFlags(RF_Transactional);
        EventNode->NodePosX = -800;
        EventNode->NodePosY = 0;

        if (UClass* SuperClass = GeneratedClass ? GeneratedClass->GetSuperClass() : nullptr)
        {
            if (UFunction* NativeFunc = SuperClass->FindFunctionByName(FName(*Func.Name)))
            {
                EventNode->EventReference.SetFromField<UFunction>(NativeFunc, true);
            }
            else
            {
                EventNode->CustomFunctionName = FName(*Func.Name);
                EventNode->EventReference.SetExternalMember(FName(*Func.Name), SuperClass);
            }
        }
        else
        {
            EventNode->CustomFunctionName = FName(*Func.Name);
        }

        Graph->AddNode(EventNode, true, true);
        EventNode->AllocateDefaultPins();
        return EventNode;
    }

    case EFunctionKind::CustomEvent:
    default:
    {
        /* User created event: a custom event node that was thunked into the
         * ubergraph. Create the node; downstream wiring to the ubergraph case
         * happens by EntryIndex when the jump table is available. */
        UK2Node_CustomEvent* CustomEventNode = NewObject<UK2Node_CustomEvent>(Graph);
        CustomEventNode->CreateNewGuid();
        CustomEventNode->SetFlags(RF_Transactional);
        CustomEventNode->NodePosX = -800;
        CustomEventNode->NodePosY = 0;
        CustomEventNode->CustomFunctionName = FName(*Func.Name);
        Graph->AddNode(CustomEventNode, true, true);
        CustomEventNode->AllocateDefaultPins();
        return CustomEventNode;
    }
    }
}

FEdGraphPinType FBlueprintBytecodeImporter::PinTypeFromJson(const TSharedPtr<FJsonObject>& PropObj) const
{
    FEdGraphPinType PinType;
    if (!PropObj)
    {
        return PinType;
    }

    const FString TypeName = PropObj->GetStringField(TEXT("Type"));
    const FString& Name = PropObj->GetStringField(TEXT("Name"));

    if (TypeName == TEXT("IntProperty") || TypeName == TEXT("Int8Property") || TypeName == TEXT("Int16Property") || TypeName == TEXT("Int64Property") || TypeName == TEXT("ByteProperty") || TypeName == TEXT("UInt16Property") || TypeName == TEXT("UInt32Property") || TypeName == TEXT("UInt64Property"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    else if (TypeName == TEXT("BoolProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    }
    else if (TypeName == TEXT("FloatProperty") || TypeName == TEXT("DoubleProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (TypeName == TEXT("NameProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    }
    else if (TypeName == TEXT("StrProperty") || TypeName == TEXT("TextProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (TypeName == TEXT("ObjectProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        if (PropObj->HasField(TEXT("PropertyClass")))
        {
            const TSharedPtr<FJsonObject>& PropClass = PropObj->GetObjectField(TEXT("PropertyClass"));
            if (PropClass)
            {
                PinType.PinSubCategoryObject = FindObject<UClass>(nullptr, *PropClass->GetStringField(TEXT("Name")));
            }
        }
    }
    else
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
    }

    return PinType;
}

// ============================================================================
// Statement emission
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::EmitStatement(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const FString& Token = Stmt.Token;

    if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent"))
    {
        return EmitContextCall(Builder, Stmt);
    }
    else if (Token == TEXT("EX_CallMath"))
    {
        return EmitCallMath(Builder, Stmt);
    }
    else if (Token == TEXT("EX_LocalFinalFunction") || Token == TEXT("EX_FinalFunction") || Token == TEXT("EX_LocalVirtualFunction") || Token == TEXT("EX_VirtualFunction"))
    {
        return EmitContextCall(Builder, Stmt);
    }
    else if (Token == TEXT("EX_Let") || Token == TEXT("EX_LetObj") || Token == TEXT("EX_LetBool"))
    {
        return EmitLet(Builder, Stmt);
    }
    else if (Token == TEXT("EX_LetValueOnPersistentFrame"))
    {
        return EmitLetValueOnPersistentFrame(Builder, Stmt);
    }
    else if (Token == TEXT("EX_Jump"))
    {
        return EmitJump(Builder, Stmt);
    }
    else if (Token == TEXT("EX_JumpIfNot"))
    {
        return EmitJumpIfNot(Builder, Stmt);
    }
    else if (Token == TEXT("EX_PushExecutionFlow"))
    {
        return EmitPushExecutionFlow(Builder, Stmt);
    }
    else if (Token == TEXT("EX_PopExecutionFlow"))
    {
        return EmitPopExecutionFlow(Builder, Stmt);
    }
    else if (Token == TEXT("EX_PopExecutionFlowIfNot"))
    {
        return EmitPopExecutionFlowIfNot(Builder, Stmt);
    }
    else if (Token == TEXT("EX_ComputedJump"))
    {
        return EmitComputedJump(Builder, Stmt);
    }
    else if (Token == TEXT("EX_Return"))
    {
        return EmitReturn(Builder, Stmt);
    }
    else if (Token == TEXT("EX_SwitchValue"))
    {
        // SwitchValue as a top-level statement is rare; treat as pure value
        // Usually appears as part of a Let RHS
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EX_SwitchValue as statement at si=%d - treating as no-op"), Stmt.StatementIndex);
        return nullptr;
    }
    else
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Unhandled token: %s at si=%d"), *Token, Stmt.StatementIndex);
        return nullptr;
    }
}

// ============================================================================
// Expression resolution
// ============================================================================

FPinValue FBlueprintBytecodeImporter::ResolveExpression(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson)
{
    if (!ExprJson.IsValid())
    {
        return FPinValue();
    }

    const FString Token = ExprJson->GetStringField(TEXT("Token"));

    if (Token == TEXT("EX_LocalVariable") || Token == TEXT("EX_InstanceVariable"))
    {
        const TSharedPtr<FJsonObject>& VarObj = ExprJson->GetObjectField(TEXT("Variable"));
        if (!VarObj.IsValid()) return FPinValue();

        const TSharedPtr<FJsonObject>& PropObj = VarObj->GetObjectField(TEXT("Property"));
        if (!PropObj.IsValid()) return FPinValue();

        FString VarName = PropObj->GetStringField(TEXT("Name"));

        // Check if we have a producer pin for this variable
        if (UEdGraphPin** FoundPin = Builder.ProducerPins.Find(VarName))
        {
            return FPinValue{ *FoundPin, false, TEXT(""), nullptr };
        }

        // Determine owner class
        UClass* OwnerClass = GeneratedClass;
        if (VarObj->HasField(TEXT("Owner")))
        {
            const TSharedPtr<FJsonObject>& OwnerObj = VarObj->GetObjectField(TEXT("Owner"));
            if (OwnerObj.IsValid())
            {
                FString OwnerObjectName = OwnerObj->GetStringField(TEXT("ObjectName"));
                // "BlueprintGeneratedClass'BP_CharCreation_C'" -> "BP_CharCreation_C"
                OwnerObjectName.RemoveFromStart(TEXT("BlueprintGeneratedClass'"));
                OwnerObjectName.RemoveFromEnd(TEXT("'"));
                OwnerObjectName.RemoveFromStart(TEXT("Function'"));
                OwnerObjectName.RemoveFromEnd(TEXT("'"));
                // Extract class name before ':'
                int32 ColonIdx;
                if (OwnerObjectName.FindChar(TEXT(':'), ColonIdx))
                {
                    OwnerObjectName = OwnerObjectName.Left(ColonIdx);
                }

                if (UClass* FoundClass = FindObject<UClass>(nullptr, *OwnerObjectName))
                {
                    OwnerClass = FoundClass;
                }
            }
        }

        UK2Node_VariableGet* GetNode = CreateVariableGet(Builder, VarName, OwnerClass);
        if (GetNode && GetNode->Pins.Num() > 0)
        {
            UEdGraphPin* ValuePin = FindPin(GetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
            if (ValuePin)
            {
                return FPinValue{ ValuePin, false, TEXT(""), nullptr };
            }
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_Self"))
    {
        UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Builder.Graph);
        SelfNode->CreateNewGuid();
        SelfNode->SetFlags(RF_Transactional);
        SelfNode->NodePosX = -200;
        SelfNode->NodePosY = 0;
        Builder.Graph->AddNode(SelfNode, true, true);
        SelfNode->AllocateDefaultPins();

        UEdGraphPin* SelfPin = FindPin(SelfNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        if (SelfPin)
        {
            return FPinValue{ SelfPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_ObjectConst"))
    {
        const TSharedPtr<FJsonObject>& ValueObj = ExprJson->GetObjectField(TEXT("Value"));
        if (!ValueObj.IsValid()) return FPinValue();

        FString ObjectName = ValueObj->GetStringField(TEXT("ObjectName"));
        FString ObjectPath = ValueObj->GetStringField(TEXT("ObjectPath"));

        // Try to resolve the object
        UObject* ResolvedObject = nullptr;

        // Handle CDO references like "BPL_DollSystem_C'Default__BPL_DollSystem_C'"
        FString CleanName = ObjectName;
        CleanName.RemoveFromStart(TEXT("BlueprintGeneratedClass'"));
        CleanName.RemoveFromStart(TEXT("Function'"));
        CleanName.RemoveFromStart(TEXT("Class'"));
        CleanName.RemoveFromEnd(TEXT("'"));

        // Find by ObjectPath if available
        if (!ObjectPath.IsEmpty())
        {
            // Strip [index] suffix
            FString CleanPath = ObjectPath;
            int32 BracketIdx;
            if (CleanPath.FindChar(TEXT('['), BracketIdx))
            {
                CleanPath = CleanPath.Left(BracketIdx);
            }

            ResolvedObject = FindObject<UObject>(nullptr, *CleanPath);
        }

        if (!ResolvedObject)
        {
            // Try to find the CDO by name
            int32 SingleQuoteIdx;
            if (ObjectName.FindChar(TEXT('\''), SingleQuoteIdx))
            {
                FString ClassName = ObjectName.Mid(1, SingleQuoteIdx - 1);
                ClassName.RemoveFromStart(TEXT("Class'"));
                int32 ColonIdx;
                if (ClassName.FindChar(TEXT(':'), ColonIdx))
                {
                    ClassName = ClassName.Left(ColonIdx);
                }

                UClass* ObjClass = FindObject<UClass>(nullptr, *ClassName);
                if (ObjClass)
                {
                    ResolvedObject = ObjClass->GetDefaultObject();
                }
            }
        }

        if (ResolvedObject)
        {
            UK2Node_Literal* LiteralNode = NewObject<UK2Node_Literal>(Builder.Graph);
            LiteralNode->CreateNewGuid();
            LiteralNode->SetFlags(RF_Transactional);
            LiteralNode->NodePosX = -200;
            LiteralNode->NodePosY = 0;
            Builder.Graph->AddNode(LiteralNode, true, true);
            LiteralNode->SetObjectRef(ResolvedObject);
            LiteralNode->AllocateDefaultPins();

            UEdGraphPin* ValuePin = LiteralNode->GetValuePin();
            if (ValuePin)
            {
                return FPinValue{ ValuePin, false, TEXT(""), nullptr };
            }
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_NoObject"))
    {
        // Null object literal
        return FPinValue{ nullptr, true, TEXT("None"), nullptr };
    }
    else if (Token == TEXT("EX_IntConst"))
    {
        int32 Value = ExprJson->GetIntegerField(TEXT("Value"));
        return FPinValue{ nullptr, true, FString::FromInt(Value), nullptr };
    }
    else if (Token == TEXT("EX_FloatConst") || Token == TEXT("EX_DoubleConst"))
    {
        double Value = ExprJson->GetNumberField(TEXT("Value"));
        return FPinValue{ nullptr, true, FString::SanitizeFloat(Value), nullptr };
    }
    else if (Token == TEXT("EX_ByteConst"))
    {
        int32 Value = ExprJson->GetIntegerField(TEXT("Value"));
        return FPinValue{ nullptr, true, FString::FromInt(Value), nullptr };
    }
    else if (Token == TEXT("EX_NameConst"))
    {
        FString Value = ExprJson->GetStringField(TEXT("Value"));
        return FPinValue{ nullptr, true, Value, nullptr };
    }
    else if (Token == TEXT("EX_StringConst"))
    {
        FString Value = ExprJson->GetStringField(TEXT("Value"));
        return FPinValue{ nullptr, true, FString::Printf(TEXT("\"%s\""), *Value), nullptr };
    }
    else if (Token == TEXT("EX_True"))
    {
        return FPinValue{ nullptr, true, TEXT("true"), nullptr };
    }
    else if (Token == TEXT("EX_False"))
    {
        return FPinValue{ nullptr, true, TEXT("false"), nullptr };
    }
    else if (Token == TEXT("EX_VectorConst"))
    {
        double X = ExprJson->GetNumberField(TEXT("X"));
        double Y = ExprJson->GetNumberField(TEXT("Y"));
        double Z = ExprJson->GetNumberField(TEXT("Z"));
        FString Value = FString::Printf(TEXT("(X=%.6f,Y=%.6f,Z=%.6f)"), X, Y, Z);
        return FPinValue{ nullptr, true, Value, nullptr };
    }
    else if (Token == TEXT("EX_RotationConst"))
    {
        double Pitch = ExprJson->GetNumberField(TEXT("Pitch"));
        double Yaw = ExprJson->GetNumberField(TEXT("Yaw"));
        double Roll = ExprJson->GetNumberField(TEXT("Roll"));
        FString Value = FString::Printf(TEXT("(Pitch=%.6f,Yaw=%.6f,Roll=%.6f)"), Pitch, Yaw, Roll);
        return FPinValue{ nullptr, true, Value, nullptr };
    }
    else if (Token == TEXT("EX_BitFieldConst"))
    {
        bool Value = ExprJson->GetBoolField(TEXT("Value"));
        return FPinValue{ nullptr, true, Value ? TEXT("true") : TEXT("false"), nullptr };
    }
    else if (Token == TEXT("EX_SwitchValue"))
    {
        // Value switch expression - resolve to Select node
        return FPinValue(); // TODO: full Select node implementation
    }
    else if (Token == TEXT("EX_StructConst"))
    {
        // Make struct
        const TSharedPtr<FJsonObject>& StructObj = ExprJson->GetObjectField(TEXT("Struct"));
        if (!StructObj.IsValid()) return FPinValue();

        FString StructObjectName = StructObj->GetStringField(TEXT("ObjectName"));
        StructObjectName.RemoveFromStart(TEXT("Class'"));
        StructObjectName.RemoveFromEnd(TEXT("'"));

        // Find the struct
        UScriptStruct* StructType = nullptr;
        FString StructPath = StructObj->GetStringField(TEXT("ObjectPath"));
        if (!StructPath.IsEmpty())
        {
            StructType = FindObject<UScriptStruct>(nullptr, *StructObjectName);
        }

        if (!StructType)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Could not resolve struct: %s"), *StructObjectName);
            return FPinValue();
        }

        UK2Node_MakeStruct* MakeStructNode = NewObject<UK2Node_MakeStruct>(Builder.Graph);
        MakeStructNode->CreateNewGuid();
        MakeStructNode->SetFlags(RF_Transactional);
        MakeStructNode->StructType = StructType;
        MakeStructNode->NodePosX = -200;
        MakeStructNode->NodePosY = 0;
        Builder.Graph->AddNode(MakeStructNode, true, true);
        MakeStructNode->AllocateDefaultPins();

        // Wire struct member pins
        const TArray<TSharedPtr<FJsonValue>>& Properties = ExprJson->GetArrayField(TEXT("Properties"));
        TArray<UEdGraphPin*> OutputPins;
        for (UEdGraphPin* Pin : MakeStructNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && Pin->PinName != UEdGraphSchema_K2::PN_ReturnValue)
            {
                OutputPins.Add(Pin);
            }
        }

        for (int32 i = 0; i < Properties.Num() && i < OutputPins.Num(); ++i)
        {
            FPinValue MemberValue = ResolveExpression(Builder, Properties[i]->AsObject());
            if (MemberValue.Pin)
            {
                ConnectPins(MemberValue.Pin, OutputPins[i]);
            }
            else if (MemberValue.bConstant)
            {
                OutputPins[i]->DefaultValue = MemberValue.ConstString;
            }
        }

        UEdGraphPin* ReturnValuePin = FindPin(MakeStructNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        if (ReturnValuePin)
        {
            return FPinValue{ ReturnValuePin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_StructMemberContext"))
    {
        // Struct member read
        const TSharedPtr<FJsonObject>& StructObj = ExprJson->GetObjectField(TEXT("Struct"));
        const TSharedPtr<FJsonObject>& MemberObj = ExprJson->GetObjectField(TEXT("Property"));

        if (!StructObj.IsValid() || !MemberObj.IsValid()) return FPinValue();

        // For now, treat as a variable read (simplified)
        FString MemberName = MemberObj->GetStringField(TEXT("Name"));
        FPinValue ParentValue = ResolveExpression(Builder, StructObj);
        if (ParentValue.Pin)
        {
            // In a real implementation, we'd use UK2Node_StructMemberGet
            // For now, return the parent pin as a placeholder
            return ParentValue;
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_ArrayGetByRef"))
    {
        // Array get by index
        const TSharedPtr<FJsonObject>& ArrayExpr = ExprJson->GetObjectField(TEXT("ArrayExpression"));
        const TSharedPtr<FJsonObject>& IndexExpr = ExprJson->GetObjectField(TEXT("IndexTerm"));

        if (!ArrayExpr.IsValid()) return FPinValue();

        FPinValue ArrayValue = ResolveExpression(Builder, ArrayExpr);
        FPinValue IndexValue = IndexExpr.IsValid() ? ResolveExpression(Builder, IndexExpr) : FPinValue();

        UK2Node_CallArrayFunction* ArrayGetNode = NewObject<UK2Node_CallArrayFunction>(Builder.Graph);
        ArrayGetNode->CreateNewGuid();
        ArrayGetNode->SetFlags(RF_Transactional);
        ArrayGetNode->NodePosX = -200;
        ArrayGetNode->NodePosY = 0;
        Builder.Graph->AddNode(ArrayGetNode, true, true);
        ArrayGetNode->AllocateDefaultPins();

        if (ArrayValue.Pin)
        {
            UEdGraphPin* ArrayPin = FindPin(ArrayGetNode, TEXT("Array"), EGPD_Input);
            if (ArrayPin) ConnectPins(ArrayValue.Pin, ArrayPin);
        }
        if (IndexValue.Pin)
        {
            UEdGraphPin* IndexPin = FindPin(ArrayGetNode, TEXT("Dimension 1"), EGPD_Input);
            if (IndexPin) ConnectPins(IndexValue.Pin, IndexPin);
        }
        else if (IndexValue.bConstant)
        {
            UEdGraphPin* IndexPin = FindPin(ArrayGetNode, TEXT("Dimension 1"), EGPD_Input);
            if (IndexPin) IndexPin->DefaultValue = IndexValue.ConstString;
        }

        UEdGraphPin* OutputPin = FindPin(ArrayGetNode, TEXT("Output"), EGPD_Output);
        if (OutputPin)
        {
            return FPinValue{ OutputPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_Cast") || Token == TEXT("EX_DynamicCast"))
    {
        // Dynamic cast
        const TSharedPtr<FJsonObject>& ClassObj = ExprJson->GetObjectField(TEXT("Class"));
        const TSharedPtr<FJsonObject>& ObjectExpr = ExprJson->GetObjectField(TEXT("ObjectExpression"));

        if (!ObjectExpr.IsValid()) return FPinValue();

        FPinValue ObjectValue = ResolveExpression(Builder, ObjectExpr);

        UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Builder.Graph);
        CastNode->CreateNewGuid();
        CastNode->SetFlags(RF_Transactional);
        CastNode->NodePosX = -200;
        CastNode->NodePosY = 0;

        if (ClassObj.IsValid())
        {
            FString ClassName = ClassObj->GetStringField(TEXT("ObjectName"));
            ClassName.RemoveFromStart(TEXT("Class'"));
            ClassName.RemoveFromEnd(TEXT("'"));

            UClass* CastClass = FindObject<UClass>(nullptr, *ClassName);
            if (CastClass)
            {
                CastNode->TargetType = CastClass;
            }
        }

        Builder.Graph->AddNode(CastNode, true, true);
        CastNode->AllocateDefaultPins();

        if (ObjectValue.Pin)
        {
            UEdGraphPin* SourcePin = CastNode->GetCastSourcePin();
            if (SourcePin) ConnectPins(ObjectValue.Pin, SourcePin);
        }

        UEdGraphPin* ResultPin = CastNode->GetCastResultPin();
        if (ResultPin)
        {
            return FPinValue{ ResultPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_CallMath"))
    {
        // Pure math/function call
        const TSharedPtr<FJsonObject>& FuncObj = ExprJson->GetObjectField(TEXT("Function"));
        if (!FuncObj.IsValid()) return FPinValue();

        FString FuncObjectName = FuncObj->GetStringField(TEXT("ObjectName"));
        FString FuncObjectPath = FuncObj->GetStringField(TEXT("ObjectPath"));

        UFunction* Func = ResolveFunction(ExprJson, TEXT(""));
        if (!Func) return FPinValue();

        // Create a call node (pure, no exec)
        UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Builder.Graph);
        CallNode->CreateNewGuid();
        CallNode->SetFlags(RF_Transactional);
        CallNode->NodePosX = -200;
        CallNode->NodePosY = 0;

        // Resolve the class
        FString ClassName;
        FString FuncName;
        int32 ColonIdx;
        if (FuncObjectName.FindChar(TEXT(':'), ColonIdx))
        {
            ClassName = FuncObjectName.Left(ColonIdx);
            FuncName = FuncObjectName.Mid(ColonIdx + 1);
        }
        ClassName.RemoveFromStart(TEXT("Class'"));
        ClassName.RemoveFromEnd(TEXT("'"));
        FuncName.RemoveFromEnd(TEXT("'"));

        UClass* TargetClass = FindObject<UClass>(nullptr, *ClassName);
        if (!TargetClass) TargetClass = GeneratedClass;

        CallNode->FunctionReference.SetExternalMember(FName(*FuncName), TargetClass);
        Builder.Graph->AddNode(CallNode, true, true);
        CallNode->AllocateDefaultPins();

        // Wire parameters
        const TArray<TSharedPtr<FJsonValue>>& Params = ExprJson->GetArrayField(TEXT("Parameters"));
        int32 ParamIdx = 0;
        for (TFieldIterator<FProperty> It(Func); It; ++It)
        {
            FProperty* Prop = *It;
            if (Prop->PropertyFlags & CPF_ReturnParm) continue;
            if (ParamIdx >= Params.Num()) break;

            UEdGraphPin* ParamPin = FindPin(CallNode, *Prop->GetName(), EGPD_Input);
            if (ParamPin)
            {
                FPinValue ParamValue = ResolveExpression(Builder, Params[ParamIdx]->AsObject());
                if (ParamValue.Pin)
                {
                    ConnectPins(ParamValue.Pin, ParamPin);
                }
                else if (ParamValue.bConstant)
                {
                    ParamPin->DefaultValue = ParamValue.ConstString;
                }
            }
            ++ParamIdx;
        }

        UEdGraphPin* ReturnValuePin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        if (ReturnValuePin)
        {
            return FPinValue{ ReturnValuePin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }

    UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Unhandled expression token: %s"), *Token);
    return FPinValue();
}

// ============================================================================
// Expression-as-exec emission (for side-effecting expressions inside Let RHS)
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::EmitExpressionAsExec(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson, int32 StmtIndex)
{
    if (!ExprJson.IsValid())
    {
        return nullptr;
    }

    const FString Token = ExprJson->GetStringField(TEXT("Token"));

    // Only emit side-effecting expressions; pure expressions should use ResolveExpression
    if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent"))
    {
        // Synthesize a temporary FBytecodeToken and dispatch to EmitContextCall
        FBytecodeToken TempStmt;
        TempStmt.Token = Token;
        TempStmt.StatementIndex = StmtIndex;
        TempStmt.JsonData = ExprJson;
        return EmitContextCall(Builder, TempStmt);
    }
    else if (Token == TEXT("EX_LocalFinalFunction") || Token == TEXT("EX_FinalFunction") ||
             Token == TEXT("EX_LocalVirtualFunction") || Token == TEXT("EX_VirtualFunction"))
    {
        FBytecodeToken TempStmt;
        TempStmt.Token = Token;
        TempStmt.StatementIndex = StmtIndex;
        TempStmt.JsonData = ExprJson;
        return EmitContextCall(Builder, TempStmt);
    }
    else if (Token == TEXT("EX_CallMath"))
    {
        FBytecodeToken TempStmt;
        TempStmt.Token = Token;
        TempStmt.StatementIndex = StmtIndex;
        TempStmt.JsonData = ExprJson;
        return EmitCallMath(Builder, TempStmt);
    }

    return nullptr;
}

// ============================================================================
// Function resolution
// ============================================================================

UFunction* FBlueprintBytecodeImporter::ResolveFunction(const TSharedPtr<FJsonObject>& FuncJson, const FString& ContextClassName)
{
    // Try to get function from Function field
    const TSharedPtr<FJsonObject>& FuncObj = FuncJson->GetObjectField(TEXT("Function"));
    if (!FuncObj.IsValid()) return nullptr;

    FString FuncObjectName = FuncObj->GetStringField(TEXT("ObjectName"));
    FString FuncObjectPath = FuncObj->GetStringField(TEXT("ObjectPath"));

    // Parse "Class'SceneComponent:SetVisibility'" or "Function'BP_CharCreation_C:Build Eye'"
    FString ClassName;
    FString FuncName;
    int32 ColonIdx;
    if (FuncObjectName.FindChar(TEXT(':'), ColonIdx))
    {
        ClassName = FuncObjectName.Left(ColonIdx);
        FuncName = FuncObjectName.Mid(ColonIdx + 1);
    }
    ClassName.RemoveFromStart(TEXT("Class'"));
    ClassName.RemoveFromStart(TEXT("Function'"));
    ClassName.RemoveFromEnd(TEXT("'"));
    FuncName.RemoveFromEnd(TEXT("'"));

    // Find the class
    UClass* TargetClass = nullptr;
    if (!ClassName.IsEmpty())
    {
        TargetClass = FindObject<UClass>(nullptr, *ClassName);
    }
    if (!TargetClass && !ContextClassName.IsEmpty())
    {
        TargetClass = FindObject<UClass>(nullptr, *ContextClassName);
    }
    if (!TargetClass)
    {
        TargetClass = GeneratedClass;
    }

    if (TargetClass)
    {
        return TargetClass->FindFunctionByName(FName(*FuncName));
    }

    return nullptr;
}

// ============================================================================
// Call node creation
// ============================================================================

UK2Node_CallFunction* FBlueprintBytecodeImporter::CreateCallNode(FFunctionBuilder& Builder, UFunction* Func, const TArray<TSharedPtr<FJsonValue>>& ParamsJson, UEdGraphPin* TargetPin)
{
    if (!Func)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreateCallNode: Func is null, returning nullptr"));
        return nullptr;
    }

    UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Builder.Graph);
    CallNode->CreateNewGuid();
    CallNode->SetFlags(RF_Transactional);
    CallNode->NodePosX = -200;
    CallNode->NodePosY = 0;

    // Get the function's owner class
    UClass* OwnerClass = Func->GetOwnerClass();
    if (!OwnerClass) OwnerClass = GeneratedClass;

    CallNode->FunctionReference.SetExternalMember(Func->GetFName(), OwnerClass);
    Builder.Graph->AddNode(CallNode, true, true);
    CallNode->AllocateDefaultPins();

    // Wire target pin (self/context)
    if (TargetPin)
    {
        UEdGraphPin* SelfPin = FindPin(CallNode, TEXT("self"), EGPD_Input);
        if (!SelfPin)
        {
            SelfPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input);
        }
        if (SelfPin)
        {
            ConnectPins(TargetPin, SelfPin);
        }
    }

    // Wire parameters (skip self parameter if present)
    int32 ParamIdx = 0;
    for (TFieldIterator<FProperty> It(Func); It; ++It)
    {
        FProperty* Prop = *It;
        if (Prop->PropertyFlags & CPF_ReturnParm) continue;
        if (Prop->GetFName() == TEXT("self")) continue; // Skip implicit self
        if (ParamIdx >= ParamsJson.Num()) break;

        UEdGraphPin* ParamPin = FindPin(CallNode, *Prop->GetName(), EGPD_Input);
        if (ParamPin)
        {
            FPinValue ParamValue = ResolveExpression(Builder, ParamsJson[ParamIdx]->AsObject());
            if (ParamValue.Pin)
            {
                ConnectPins(ParamValue.Pin, ParamPin);
            }
            else if (ParamValue.bConstant)
            {
                ParamPin->DefaultValue = ParamValue.ConstString;
            }
        }
        ++ParamIdx;
    }

    return CallNode;
}

// ============================================================================
// Variable node creation
// ============================================================================

UK2Node_VariableGet* FBlueprintBytecodeImporter::CreateVariableGet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass)
{
    UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Builder.Graph);
    GetNode->CreateNewGuid();
    GetNode->SetFlags(RF_Transactional);
    GetNode->NodePosX = -400;
    GetNode->NodePosY = 0;
    GetNode->VariableReference.SetExternalMember(FName(*VarName), OwnerClass ? OwnerClass : GeneratedClass);
    Builder.Graph->AddNode(GetNode, true, true);
    GetNode->AllocateDefaultPins();
    return GetNode;
}

UK2Node_VariableSet* FBlueprintBytecodeImporter::CreateVariableSet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass)
{
    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Builder.Graph);
    SetNode->CreateNewGuid();
    SetNode->SetFlags(RF_Transactional);
    SetNode->NodePosX = -200;
    SetNode->NodePosY = 0;
    SetNode->VariableReference.SetExternalMember(FName(*VarName), OwnerClass ? OwnerClass : GeneratedClass);
    Builder.Graph->AddNode(SetNode, true, true);
    SetNode->AllocateDefaultPins();
    return SetNode;
}

// ============================================================================
// Specific statement emitters
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::EmitContextCall(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Resolve the target object (context)
    UEdGraphPin* TargetPin = nullptr;
    if (Json->HasField(TEXT("ObjectExpression")))
    {
        const TSharedPtr<FJsonObject>& ObjExpr = Json->GetObjectField(TEXT("ObjectExpression"));
        if (ObjExpr.IsValid())
        {
            FPinValue TargetValue = ResolveExpression(Builder, ObjExpr);
            if (TargetValue.Pin)
            {
                TargetPin = TargetValue.Pin;
            }
        }
    }

    // Resolve the function call
    const TSharedPtr<FJsonObject>& ContextExpr = Json->GetObjectField(TEXT("ContextExpression"));
    if (!ContextExpr.IsValid()) return nullptr;

    const FString CallToken = ContextExpr->GetStringField(TEXT("Token"));

    UFunction* Func = nullptr;
    TArray<TSharedPtr<FJsonValue>> ParamsJson;

    if (CallToken == TEXT("EX_FinalFunction") || CallToken == TEXT("EX_LocalFinalFunction"))
    {
        Func = ResolveFunction(ContextExpr, TEXT(""));
        const TSharedPtr<FJsonObject>& FuncObj = ContextExpr->GetObjectField(TEXT("Function"));
        if (FuncObj.IsValid())
        {
            FString FuncObjectName = FuncObj->GetStringField(TEXT("ObjectName"));
            FString FuncName;
            int32 ColonIdx;
            if (FuncObjectName.FindChar(TEXT(':'), ColonIdx))
            {
                FuncName = FuncObjectName.Mid(ColonIdx + 1);
            }
            FuncName.RemoveFromEnd(TEXT("'"));
        }
    }
    else if (CallToken == TEXT("EX_VirtualFunction") || CallToken == TEXT("EX_LocalVirtualFunction"))
    {
        // Virtual functions - use the function name
        FString FuncName = ContextExpr->GetStringField(TEXT("Function"));
        // Try to find in GeneratedClass first
        Func = GeneratedClass->FindFunctionByName(FName(*FuncName));
        // If not found, try resolving from the ObjectExpression's class type
        if (!Func && Json->HasField(TEXT("ObjectExpression")))
        {
            const TSharedPtr<FJsonObject>& ObjExpr = Json->GetObjectField(TEXT("ObjectExpression"));
            if (ObjExpr.IsValid() && ObjExpr->HasField(TEXT("Variable")))
            {
                const TSharedPtr<FJsonObject>& InnerVarObj = ObjExpr->GetObjectField(TEXT("Variable"));
                const TSharedPtr<FJsonObject>& InnerInner = InnerVarObj->HasField(TEXT("Variable")) ? InnerVarObj->GetObjectField(TEXT("Variable")) : InnerVarObj;
                if (InnerInner->HasField(TEXT("Property")))
                {
                    const TSharedPtr<FJsonObject>& PropObj = InnerInner->GetObjectField(TEXT("Property"));
                    if (PropObj->HasField(TEXT("PropertyClass")))
                    {
                        const TSharedPtr<FJsonObject>& PropClass = PropObj->GetObjectField(TEXT("PropertyClass"));
                        FString ClassName = PropClass->GetStringField(TEXT("ObjectName"));
                        ClassName.RemoveFromStart(TEXT("Class'"));
                        ClassName.RemoveFromEnd(TEXT("'"));
                        UClass* ObjClass = FindObject<UClass>(nullptr, *ClassName);
                        if (ObjClass)
                        {
                            // Walk the hierarchy to find the function
                            for (UClass* C = ObjClass; C && !Func; C = C->GetSuperClass())
                            {
                                Func = C->FindFunctionByName(FName(*FuncName));
                            }
                        }
                    }
                }
            }
        }
    }

    if (ContextExpr->HasField(TEXT("Parameters")))
    {
        ParamsJson = ContextExpr->GetArrayField(TEXT("Parameters"));
    }

    // Create the call node
    UK2Node_CallFunction* CallNode = CreateCallNode(Builder, Func, ParamsJson, TargetPin);
    if (!CallNode)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Failed to create call node for statement at si=%d"), Stmt.StatementIndex);
        return nullptr;
    }

    // Wire exec pins
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    // Handle return value / RValuePointer
    FString RValuePointer;
    if (Json->HasField(TEXT("RValuePointer")) && Json->TryGetField(TEXT("RValuePointer")))
    {
        RValuePointer = Json->GetStringField(TEXT("RValuePointer"));
    }

    UEdGraphPin* ReturnValuePin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
    if (ReturnValuePin && !RValuePointer.IsEmpty())
    {
        Builder.ProducerPins.Add(RValuePointer, ReturnValuePin);
    }

    // Store position
    CallNode->NodePosX = 0;
    CallNode->NodePosY = 0;

    return CallNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitCallMath(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Resolve the function
    UFunction* Func = ResolveFunction(Json, TEXT(""));
    if (!Func)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Failed to resolve math function at si=%d"), Stmt.StatementIndex);
        return nullptr;
    }

    // Get parameters
    TArray<TSharedPtr<FJsonValue>> ParamsJson;
    if (Json->HasField(TEXT("Parameters")))
    {
        ParamsJson = Json->GetArrayField(TEXT("Parameters"));
    }

    // Create the call node
    UK2Node_CallFunction* CallNode = CreateCallNode(Builder, Func, ParamsJson, nullptr);
    if (!CallNode)
    {
        return nullptr;
    }

    // Wire exec pins
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    CallNode->NodePosX = 0;
    CallNode->NodePosY = 0;

    return CallNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitLet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get LHS variable
    // JSON structure: EX_LetObj.Variable = { Token: "EX_LocalVariable", Variable: { Owner, Property: { Name } } }
    const TSharedPtr<FJsonObject>& VarObj = Json->GetObjectField(TEXT("Variable"));
    if (!VarObj.IsValid()) return nullptr;

    // Navigate to inner Variable if present (EX_LocalVariable/EX_InstanceVariable wrapper)
    const TSharedPtr<FJsonObject>& InnerVar = VarObj->HasField(TEXT("Variable")) ? VarObj->GetObjectField(TEXT("Variable")) : VarObj;
    const TSharedPtr<FJsonObject>& PropObj = InnerVar->GetObjectField(TEXT("Property"));
    if (!PropObj.IsValid()) return nullptr;

    FString VarName = PropObj->GetStringField(TEXT("Name"));

    // Get RHS expression
    const TSharedPtr<FJsonObject>& ExprJson = Json->GetObjectField(TEXT("Expression"));
    FString RHSDebugToken = ExprJson.IsValid() ? ExprJson->GetStringField(TEXT("Token")) : TEXT("NONE");
    FPinValue RHSValue = ExprJson.IsValid() ? ResolveExpression(Builder, ExprJson) : FPinValue();

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitLet si=%d: var=%s, rhs=%s, hasPin=%d"),
        Stmt.StatementIndex, *VarName, *RHSDebugToken, RHSValue.Pin ? 1 : 0);

    // If ResolveExpression returned empty and the RHS is a side-effecting call,
    // emit it as an exec node to get its return value pin
    if (!RHSValue.Pin && ExprJson.IsValid())
    {
        const FString ExprToken = ExprJson->GetStringField(TEXT("Token"));
        if (ExprToken == TEXT("EX_Context") || ExprToken == TEXT("EX_Context_FailSilent") ||
            ExprToken == TEXT("EX_LocalFinalFunction") || ExprToken == TEXT("EX_FinalFunction") ||
            ExprToken == TEXT("EX_LocalVirtualFunction") || ExprToken == TEXT("EX_VirtualFunction"))
        {
            UEdGraphNode* CallNode = EmitExpressionAsExec(Builder, ExprJson, Stmt.StatementIndex);
            if (CallNode)
            {
                // The call node's return value pin becomes our RHS
                UEdGraphPin* ReturnPin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
                if (ReturnPin)
                {
                    RHSValue = FPinValue{ ReturnPin, false, TEXT(""), nullptr };
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> EmitExpressionAsExec succeeded, got return pin from %s"), *CallNode->GetClass()->GetName());
                }
                else
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> EmitExpressionAsExec succeeded but no return pin on %s"), *CallNode->GetClass()->GetName());
                }
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> EmitExpressionAsExec returned null"));
            }
        }
    }

    // Determine owner class
    UClass* OwnerClass = GeneratedClass;
    if (InnerVar->HasField(TEXT("Owner")))
    {
        const TSharedPtr<FJsonObject>& OwnerObj = VarObj->GetObjectField(TEXT("Owner"));
        if (OwnerObj.IsValid())
        {
            FString OwnerObjectName = OwnerObj->GetStringField(TEXT("ObjectName"));
            OwnerObjectName.RemoveFromStart(TEXT("Function'"));
            OwnerObjectName.RemoveFromEnd(TEXT("'"));
            int32 ColonIdx;
            if (OwnerObjectName.FindChar(TEXT(':'), ColonIdx))
            {
                OwnerObjectName = OwnerObjectName.Left(ColonIdx);
            }
            // For function-owned variables, try to find the function's outer class
            // For now, use GeneratedClass
        }
    }

    // Check if this is a CallFunc temp variable
    bool bIsTempVar = VarName.StartsWith(TEXT("CallFunc_")) || VarName.StartsWith(TEXT("Temp_"));

    // For CallFunc temps that receive call results, just store the producer pin
    if (bIsTempVar && RHSValue.Pin)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Storing producer pin for temp var %s"), *VarName);
        Builder.ProducerPins.Add(VarName, RHSValue.Pin);
        return nullptr; // No exec anchor needed for pure temp assignments
    }

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Creating SET node for %s (bIsTemp=%d, hasRhsPin=%d)"), *VarName, bIsTempVar, RHSValue.Pin ? 1 : 0);

    // Create variable set node
    UK2Node_VariableSet* SetNode = CreateVariableSet(Builder, VarName, OwnerClass);
    if (!SetNode)
    {
        return nullptr;
    }

    // Wire exec pins
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Wire RHS to value pin
    UEdGraphPin* ValuePin = FindPin(SetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Input);
    if (ValuePin)
    {
        if (RHSValue.Pin)
        {
            ConnectPins(RHSValue.Pin, ValuePin);
        }
        else if (RHSValue.bConstant)
        {
            ValuePin->DefaultValue = RHSValue.ConstString;
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    SetNode->NodePosX = 0;
    SetNode->NodePosY = 0;

    return SetNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitLetValueOnPersistentFrame(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get destination property
    const TSharedPtr<FJsonObject>& DestProp = Json->GetObjectField(TEXT("DestinationProperty"));
    if (!DestProp.IsValid()) return nullptr;

    const TSharedPtr<FJsonObject>& PropObj = DestProp->GetObjectField(TEXT("Property"));
    if (!PropObj.IsValid()) return nullptr;

    FString VarName = PropObj->GetStringField(TEXT("Name"));

    // Get assignment expression
    const TSharedPtr<FJsonObject>& ExprJson = Json->GetObjectField(TEXT("AssignmentExpression"));
    FPinValue RHSValue = ExprJson.IsValid() ? ResolveExpression(Builder, ExprJson) : FPinValue();

    // If ResolveExpression returned empty and the RHS is a side-effecting call,
    // emit it as an exec node to get its return value pin
    if (!RHSValue.Pin && ExprJson.IsValid())
    {
        const FString ExprToken = ExprJson->GetStringField(TEXT("Token"));
        if (ExprToken == TEXT("EX_Context") || ExprToken == TEXT("EX_Context_FailSilent") ||
            ExprToken == TEXT("EX_LocalFinalFunction") || ExprToken == TEXT("EX_FinalFunction") ||
            ExprToken == TEXT("EX_LocalVirtualFunction") || ExprToken == TEXT("EX_VirtualFunction"))
        {
            UEdGraphNode* CallNode = EmitExpressionAsExec(Builder, ExprJson, Stmt.StatementIndex);
            if (CallNode)
            {
                UEdGraphPin* ReturnPin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
                if (ReturnPin)
                {
                    RHSValue = FPinValue{ ReturnPin, false, TEXT(""), nullptr };
                }
            }
        }
    }

    // Use UK2Node_VariableSet for the persistent frame variable
    UK2Node_VariableSet* SetNode = CreateVariableSet(Builder, VarName, GeneratedClass);
    if (!SetNode)
    {
        return nullptr;
    }

    // Wire exec pins
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Wire RHS to value pin
    UEdGraphPin* ValuePin = FindPin(SetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Input);
    if (ValuePin)
    {
        if (RHSValue.Pin)
        {
            ConnectPins(RHSValue.Pin, ValuePin);
        }
        else if (RHSValue.bConstant)
        {
            ValuePin->DefaultValue = RHSValue.ConstString;
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    SetNode->NodePosX = 0;
    SetNode->NodePosY = 0;

    return SetNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitJump(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("CodeOffset")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("CodeOffset"));
    }
    else if (Json->HasField(TEXT("ObjectPath")))
    {
        FString ObjectPath = Json->GetStringField(TEXT("ObjectPath"));
        // Extract statement index from path like "ExecuteUbergraph_BP_CharCreation[15]"
        int32 BracketIdx;
        if (ObjectPath.FindChar(TEXT('['), BracketIdx))
        {
            FString IndexStr = ObjectPath.Mid(BracketIdx + 1);
            IndexStr.RemoveFromEnd(TEXT("]"));
            TargetStmtIdx = FCString::Atoi(*IndexStr);
        }
    }

    if (TargetStmtIdx >= 0)
    {
        // Wire exec to target statement's anchor
        WireExecFromPin(Builder, Builder.LastExecPin, TargetStmtIdx);
    }

    return nullptr; // Jump has no anchor node
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitJumpIfNot(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get condition
    const TSharedPtr<FJsonObject>& BoolExpr = Json->GetObjectField(TEXT("BooleanExpression"));
    FPinValue CondValue = BoolExpr.IsValid() ? ResolveExpression(Builder, BoolExpr) : FPinValue();

    // Get target
    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("CodeOffset")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("CodeOffset"));
    }
    else if (Json->HasField(TEXT("ObjectPath")))
    {
        FString ObjectPath = Json->GetStringField(TEXT("ObjectPath"));
        int32 BracketIdx;
        if (ObjectPath.FindChar(TEXT('['), BracketIdx))
        {
            FString IndexStr = ObjectPath.Mid(BracketIdx + 1);
            IndexStr.RemoveFromEnd(TEXT("]"));
            TargetStmtIdx = FCString::Atoi(*IndexStr);
        }
    }

    // Create branch node
    UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Builder.Graph);
    BranchNode->CreateNewGuid();
    BranchNode->SetFlags(RF_Transactional);
    BranchNode->NodePosX = 200;
    BranchNode->NodePosY = 0;
    Builder.Graph->AddNode(BranchNode, true, true);
    BranchNode->AllocateDefaultPins();

    // Wire exec
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(BranchNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Wire condition
    UEdGraphPin* ConditionPin = BranchNode->GetConditionPin();
    if (ConditionPin)
    {
        if (CondValue.Pin)
        {
            ConnectPins(CondValue.Pin, ConditionPin);
        }
        else if (CondValue.bConstant)
        {
            ConditionPin->DefaultValue = CondValue.ConstString;
        }
    }

    // Wire true branch (then) to next statement
    UEdGraphPin* ThenPin = FindPin(BranchNode, TEXT("then"), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    // Wire false branch (else) to target
    UEdGraphPin* ElsePin = BranchNode->GetElsePin();
    if (ElsePin && TargetStmtIdx >= 0)
    {
        WireExecFromPin(Builder, ElsePin, TargetStmtIdx);
    }

    return BranchNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitPushExecutionFlow(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    // PushExecutionFlow is a loop/branch marker - treat as no-op, exec continues
    return nullptr;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitPopExecutionFlow(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get the pushed address to jump to
    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("PushingAddress")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("PushingAddress"));
    }

    if (TargetStmtIdx >= 0)
    {
        WireExecFromPin(Builder, Builder.LastExecPin, TargetStmtIdx);
    }

    return nullptr;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitPopExecutionFlowIfNot(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get the pushed address
    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("PushingAddress")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("PushingAddress"));
    }

    // The condition is the last computed boolean value (from a preceding LetBool)
    // For simplicity, we'll use a branch with condition from the last produced boolean
    // In a full implementation, we'd track the last boolean produced
    // For now, create a branch with the condition being true (placeholder)

    UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Builder.Graph);
    BranchNode->CreateNewGuid();
    BranchNode->SetFlags(RF_Transactional);
    BranchNode->NodePosX = 200;
    BranchNode->NodePosY = 0;
    Builder.Graph->AddNode(BranchNode, true, true);
    BranchNode->AllocateDefaultPins();

    // Wire exec
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(BranchNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Wire true branch (then) to next statement (continue)
    UEdGraphPin* ThenPin = FindPin(BranchNode, TEXT("then"), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    // Wire false branch (else) to target (loop back/merge)
    UEdGraphPin* ElsePin = BranchNode->GetElsePin();
    if (ElsePin && TargetStmtIdx >= 0)
    {
        WireExecFromPin(Builder, ElsePin, TargetStmtIdx);
    }

    return BranchNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitComputedJump(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get the index expression (CodeOffsetExpression)
    const TSharedPtr<FJsonObject>& IndexExpr = Json->GetObjectField(TEXT("CodeOffsetExpression"));
    if (!IndexExpr.IsValid()) return nullptr;

    FPinValue IndexValue = ResolveExpression(Builder, IndexExpr);
    if (!IndexValue.Pin)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("ComputedJump without valid index expression at si=%d"), Stmt.StatementIndex);
        return nullptr;
    }

    // Collect all jump targets in the function to determine case count
    TSet<int32> CaseTargets;
    for (const FBytecodeToken* OtherStmt : Builder.Statements)
    {
        if (OtherStmt->Token == TEXT("EX_Jump") || OtherStmt->Token == TEXT("EX_JumpIfNot"))
        {
            const TSharedPtr<FJsonObject>& OtherJson = OtherStmt->JsonData;
            if (OtherJson->HasField(TEXT("CodeOffset")))
            {
                int32 Target = OtherJson->GetIntegerField(TEXT("CodeOffset"));
                if (Target != Stmt.StatementIndex) // Don't include self-references
                {
                    CaseTargets.Add(Target);
                }
            }
        }
    }

    // Create switch integer node
    UK2Node_SwitchInteger* SwitchNode = NewObject<UK2Node_SwitchInteger>(Builder.Graph);
    SwitchNode->CreateNewGuid();
    SwitchNode->SetFlags(RF_Transactional);
    SwitchNode->NodePosX = 200;
    SwitchNode->NodePosY = 0;
    Builder.Graph->AddNode(SwitchNode, true, true);

    // Add output pins for each case
    for (int32 i = 0; i < CaseTargets.Num(); ++i)
    {
        SwitchNode->AddPinToSwitchNode();
    }
    SwitchNode->AllocateDefaultPins();

    // Wire exec
    if (Builder.LastExecPin)
    {
        UEdGraphPin* ExecPin = FindPin(SwitchNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }

    // Wire selection index
    UEdGraphPin* SelectionPin = FindPin(SwitchNode, FString(TEXT("Selection")), EGPD_Input);
    if (SelectionPin && IndexValue.Pin)
    {
        ConnectPins(IndexValue.Pin, SelectionPin);
    }

    // Wire each case output to its target
    TArray<UEdGraphPin*> CasePins;
    for (UEdGraphPin* Pin : SwitchNode->Pins)
    {
        if (Pin->Direction == EGPD_Output && Pin->PinName != UEdGraphSchema_K2::PN_Then)
        {
            CasePins.Add(Pin);
        }
    }

    int32 CaseIdx = 0;
    for (int32 Target : CaseTargets)
    {
        if (CaseIdx < CasePins.Num())
        {
            WireExecFromPin(Builder, CasePins[CaseIdx], Target);
        }
        ++CaseIdx;
    }

    // No default exec (handled by the last exec pin continuing)
    Builder.LastExecPin = nullptr;

    return SwitchNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitReturn(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    if (!Builder.ResultNode)
    {
        // For events/ubergraph, return is just the end
        Builder.LastExecPin = nullptr;
        return nullptr;
    }

    // Wire exec to result node
    UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
    if (ResultExecPin && Builder.LastExecPin)
    {
        ConnectPins(Builder.LastExecPin, ResultExecPin);
    }

    Builder.LastExecPin = nullptr;
    return Builder.ResultNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitSwitchValue(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    // SwitchValue as a top-level statement is rare; usually part of a Let RHS
    // For now, treat as no-op
    return nullptr;
}

// ============================================================================
// Exec wiring
// ============================================================================

void FBlueprintBytecodeImporter::WireExec(FFunctionBuilder& Builder, UEdGraphNode* FromNode, int32 ToStmtIdx)
{
    if (!FromNode) return;

    // Find or create anchor for target statement
    UEdGraphNode** FoundAnchor = Builder.StatementAnchors.Find(ToStmtIdx);
    if (FoundAnchor && *FoundAnchor)
    {
        UEdGraphPin* ExecPin = FindPin(*FoundAnchor, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            UEdGraphPin* SourceExecPin = FindPin(FromNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
            if (!SourceExecPin)
            {
                SourceExecPin = FindPin(FromNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
            }
            if (SourceExecPin)
            {
                ConnectPins(SourceExecPin, ExecPin);
            }
        }
    }
}

void FBlueprintBytecodeImporter::WireExecFromPin(FFunctionBuilder& Builder, UEdGraphPin* FromPin, int32 ToStmtIdx)
{
    if (!FromPin) return;

    UEdGraphNode** FoundAnchor = Builder.StatementAnchors.Find(ToStmtIdx);
    if (FoundAnchor && *FoundAnchor)
    {
        UEdGraphPin* ExecPin = FindPin(*FoundAnchor, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        if (ExecPin)
        {
            ConnectPins(FromPin, ExecPin);
        }
    }
}

// ============================================================================
// Utility functions
// ============================================================================

UEdGraphPin* FBlueprintBytecodeImporter::FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node) return nullptr;

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName == FName(*PinName) && Pin->Direction == Direction)
        {
            return Pin;
        }
    }
    return nullptr;
}

void FBlueprintBytecodeImporter::ConnectPins(UEdGraphPin* PinA, UEdGraphPin* PinB)
{
    if (!PinA || !PinB) return;

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (Schema)
    {
        Schema->TryCreateConnection(PinA, PinB);
    }
}

UK2Node_CallFunction* FBlueprintBytecodeImporter::CreateStubCallNode(const FString& FunctionName, UEdGraph* Graph)
{
    UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
    CallNode->CreateNewGuid();
    CallNode->SetFlags(RF_Transient);
    CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), GeneratedClass);
    Graph->AddNode(CallNode, true, true);
    return CallNode;
}

const ParsedFunction* FBlueprintBytecodeImporter::FindFunction(const FString& Name) const
{
    return ParsedFunctions.Find(Name);
}

// ============================================================================
// Dynamic bindings (input events)
// ============================================================================

bool FBlueprintBytecodeImporter::ProcessDynamicBindings(const TSharedPtr<FJsonObject>& ClassProperties, const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!ClassProperties) return false;

    if (ClassProperties->HasField(TEXT("DynamicBindingObjects")))
    {
        const TArray<TSharedPtr<FJsonValue>>& Bindings = ClassProperties->GetArrayField(TEXT("DynamicBindingObjects"));
        for (const TSharedPtr<FJsonValue>& BindingValue : Bindings)
        {
            const TSharedPtr<FJsonObject>& BindingJson = BindingValue->AsObject();
            if (!BindingJson) continue;

            // ObjectName looks like: InputDebugKeyDelegateBinding'BP_CharCreation_C:InputDebugKeyDelegateBinding_0'
            const FString ObjectName = BindingJson->GetStringField(TEXT("ObjectName"));
            int32 QuoteIdx = INDEX_NONE;
            if (!ObjectName.FindChar(TEXT('\''), QuoteIdx))
            {
                continue;
            }

            const FString TypeName = ObjectName.Left(QuoteIdx);
            const TSharedPtr<FJsonObject> Resolved = ResolveBindingExport(BindingJson, JsonObjects);
            if (!Resolved)
            {
                continue;
            }

            if (TypeName == TEXT("InputDebugKeyDelegateBinding"))
            {
                ProcessInputDebugKeyBinding(Resolved, JsonObjects);
            }
            else if (TypeName == TEXT("EnhancedInputActionDelegateBinding"))
            {
                ProcessEnhancedInputBinding(Resolved, JsonObjects);
            }
        }
    }

    return true;
}

const TSharedPtr<FJsonObject> FBlueprintBytecodeImporter::ResolveBindingExport(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects) const
{
    // ObjectPath looks like: /Game/TouchyGame/BP/BP_CharCreation.17
    // The trailing number is the index of the export inside the JSON array.
    const FString ObjectPath = BindingJson->GetStringField(TEXT("ObjectPath"));
    int32 DotIdx = INDEX_NONE;
    if (!ObjectPath.FindChar(TEXT('.'), DotIdx))
    {
        return nullptr;
    }

    const FString IndexStr = ObjectPath.RightChop(DotIdx + 1);
    if (!IndexStr.IsNumeric())
    {
        return nullptr;
    }

    const int32 ExportIndex = FCString::Atoi(*IndexStr);
    if (ExportIndex < 0 || ExportIndex >= JsonObjects.Num())
    {
        return nullptr;
    }

    return JsonObjects[ExportIndex]->AsObject();
}

void FBlueprintBytecodeImporter::ProcessInputDebugKeyBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!BindingJson || !BindingJson->HasField(TEXT("Properties")))
    {
        return;
    }

    const TSharedPtr<FJsonObject>& Properties = BindingJson->GetObjectField(TEXT("Properties"));
    if (!Properties || !Properties->HasField(TEXT("InputDebugKeyDelegateBindings")))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>& Entries = Properties->GetArrayField(TEXT("InputDebugKeyDelegateBindings"));
    for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
    {
        const TSharedPtr<FJsonObject>& Entry = EntryValue->AsObject();
        if (!Entry) continue;

        FInputBindingInfo Info;
        Info.NodeType = TEXT("K2Node_InputDebugKeyEvent");
        Info.FunctionName = Entry->GetStringField(TEXT("FunctionNameToBind"));
        Info.InputKeyEvent = Entry->GetStringField(TEXT("InputKeyEvent"));

        if (Entry->HasField(TEXT("InputChord")))
        {
            const TSharedPtr<FJsonObject>& Chord = Entry->GetObjectField(TEXT("InputChord"));
            if (Chord && Chord->HasField(TEXT("Key")))
            {
                const TSharedPtr<FJsonObject>& Key = Chord->GetObjectField(TEXT("Key"));
                if (Key)
                {
                    Info.KeyName = Key->GetStringField(TEXT("KeyName"));
                }
            }
        }

        if (!Info.FunctionName.IsEmpty())
        {
            InputBindings.Add(Info.FunctionName, Info);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("InputDebugKey binding: %s -> Key=%s Event=%s"), *Info.FunctionName, *Info.KeyName, *Info.InputKeyEvent);
        }
    }
}

void FBlueprintBytecodeImporter::ProcessEnhancedInputBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!BindingJson || !BindingJson->HasField(TEXT("Properties")))
    {
        return;
    }

    const TSharedPtr<FJsonObject>& Properties = BindingJson->GetObjectField(TEXT("Properties"));
    if (!Properties || !Properties->HasField(TEXT("InputActionDelegateBindings")))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>& Entries = Properties->GetArrayField(TEXT("InputActionDelegateBindings"));
    for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
    {
        const TSharedPtr<FJsonObject>& Entry = EntryValue->AsObject();
        if (!Entry) continue;

        FInputBindingInfo Info;
        Info.NodeType = TEXT("K2Node_EnhancedInputActionEvent");
        Info.FunctionName = Entry->GetStringField(TEXT("FunctionNameToBind"));
        Info.TriggerEvent = Entry->GetStringField(TEXT("TriggerEvent"));

        if (Entry->HasField(TEXT("InputAction")))
        {
            const TSharedPtr<FJsonObject>& InputAction = Entry->GetObjectField(TEXT("InputAction"));
            if (InputAction)
            {
                const FString ObjectName = InputAction->GetStringField(TEXT("ObjectName"));
                // "InputAction'IA_LeftClick'" -> "IA_LeftClick"
                int32 StartQuote = INDEX_NONE;
                int32 EndQuote = INDEX_NONE;
                if (ObjectName.FindChar(TEXT('\''), StartQuote) && ObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd) != INDEX_NONE)
                {
                    const int32 EndIdx = ObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                    Info.InputActionName = ObjectName.Mid(StartQuote + 1, EndIdx - StartQuote - 1);
                }

                Info.InputActionPath = InputAction->GetStringField(TEXT("ObjectPath"));
                const int32 DotIdx = Info.InputActionPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (DotIdx != INDEX_NONE)
                {
                    Info.InputActionPath = Info.InputActionPath.Left(DotIdx);
                }
            }
        }

        if (!Info.FunctionName.IsEmpty())
        {
            InputBindings.Add(Info.FunctionName, Info);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EnhancedInput binding: %s -> Action=%s Trigger=%s"), *Info.FunctionName, *Info.InputActionName, *Info.TriggerEvent);
        }
    }
}
