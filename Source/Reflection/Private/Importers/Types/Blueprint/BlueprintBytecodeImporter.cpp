/* Copyright Reflection Contributors 2024-2026 */

#include "BlueprintBytecodeImporter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_Select.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_StructMemberGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Self.h"
#include "K2Node_Literal.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputKey.h"
#include "InputAction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/FieldIterator.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectHash.h"
#include "UObject/StructOnScope.h"
#include "Importers/Constructor/DependencyRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBytecodeImporter, Log, All);

/* Resolve a BlueprintGeneratedClass reference from an InterfaceClass/Class JSON object.
 * The JSON carries ObjectName ("BlueprintGeneratedClass'GI_Data_C'") plus an ObjectPath
 * ("/Game/TouchyGame/BP/GI_Data.0") whose ".0" is an export index, not part of the path.
 * Bare-name FindObject fails for freshly imported stub classes, so walk up to the full
 * path and load the package the class lives in. */
static UClass* ResolveCastClass(const TSharedPtr<FJsonObject>& ClassObj)
{
	if (!ClassObj.IsValid()) return nullptr;

	FString ClassName = ClassObj->GetStringField(TEXT("ObjectName"));
	ClassName.RemoveFromStart(TEXT("BlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("WidgetBlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("AnimBlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("Class'"));
	ClassName.RemoveFromEnd(TEXT("'"));

	if (UClass* Found = FindObject<UClass>(nullptr, *ClassName)) return Found;

	FString ObjPath = ClassObj->GetStringField(TEXT("ObjectPath"));
	if (!ObjPath.IsEmpty())
	{
		int32 Dot;
		if (ObjPath.FindLastChar(TEXT('.'), Dot)) ObjPath = ObjPath.Left(Dot);

		FString FullPath = ObjPath + TEXT(".") + ClassName;
		if (UClass* Found = FindObject<UClass>(nullptr, *FullPath)) return Found;
		if (UClass* Found = LoadObject<UClass>(nullptr, *FullPath)) return Found;

		UPackage* Pkg = LoadPackage(nullptr, *ObjPath, LOAD_None);
		if (Pkg)
		{
			if (UClass* Found = FindObject<UClass>(Pkg, *ClassName)) return Found;
		}
	}

	return nullptr;
}

static UEnum* ResolveEnumObj(const TSharedPtr<FJsonObject>& EnumObj)
{
	if (!EnumObj.IsValid()) return nullptr;

	FString EnumName = EnumObj->GetStringField(TEXT("ObjectName"));
	/* ObjectName comes wrapped in whatever prefix the exporter chose:
	 * "UserDefinedEnum'Enum_Race'", "Class'EGender'" - the actual name sits
	 * between the first and last quote. */
	const int32 FirstQuote = EnumName.Find(TEXT("'"), ESearchCase::CaseSensitive);
	const int32 LastQuote = EnumName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (FirstQuote != INDEX_NONE && LastQuote > FirstQuote)
	{
		EnumName = EnumName.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
	}

	if (UEnum* Found = FindObject<UEnum>(nullptr, *EnumName)) return Found;
	if (UEnum* FoundFirst = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::None)) return FoundFirst;

	/* Not in memory yet: the ObjectPath ("​/Game/Character/Enum_Race.0") names
	 * the defining package - strip the object index and load. */
	FString ObjectPath = EnumObj->GetStringField(TEXT("ObjectPath"));
	if (!ObjectPath.IsEmpty())
	{
		int32 Dot;
		if (ObjectPath.FindChar(TEXT('.'), Dot))
		{
			ObjectPath.LeftInline(Dot);
		}
		return LoadObject<UEnum>(nullptr, *ObjectPath);
	}
	return nullptr;
}

static UScriptStruct* ResolveStructObj(const TSharedPtr<FJsonObject>& StructObj)
{
	if (!StructObj.IsValid()) return nullptr;

	FString StructName = StructObj->GetStringField(TEXT("ObjectName"));
	StructName.RemoveFromStart(TEXT("UserDefinedStruct'"));
	StructName.RemoveFromStart(TEXT("ScriptStruct'"));
	/* Native structs are exported as Class'Key' / Class'InputActionValue' with a
	 * module-only ObjectPath ("/Script/InputCore"); the name after the prefix is
	 * the struct's short name, resolved below by FindObject / module-qualified path. */
	StructName.RemoveFromStart(TEXT("Class'"));
	StructName.RemoveFromEnd(TEXT("'"));
	int32 ColonIdx;
	if (StructName.FindChar(TEXT(':'), ColonIdx)) StructName = StructName.Left(ColonIdx);

	if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *StructName)) return Found;
	return FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::None);
}

/* Resolve a user-defined struct by JSON ObjectName/ObjectPath. The bytecode importer
 * runs against assets the batch importer may have just created in-memory (same-batch
 * shells, resolved via the registry) or may already have on disk (LoadObject). Bare-name
 * FindObject covers native structs and anything already resident. The ObjectPath carries a
 * trailing ".N" export index that must be stripped before building package references. */
static UScriptStruct* ResolveUserDefinedStruct(const FString& StructObjName, const FString& ObjectPath)
{
	if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *StructObjName)) return Found;
	if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*StructObjName, EFindFirstObjectOptions::None)) return Found;

	FString PkgPath = ObjectPath;
	int32 DotIdx;
	if (PkgPath.FindLastChar(TEXT('.'), DotIdx)) PkgPath = PkgPath.Left(DotIdx);
	if (PkgPath.IsEmpty()) return nullptr;

	if (FAssetEntry* Entry = FAssetDependencyRegistry::Get().FindByPackagePath(PkgPath))
	{
		if (UScriptStruct* Found = Cast<UScriptStruct>(Entry->CreatedObject)) return Found;
	}

	const FString FullPath = PkgPath + TEXT(".") + StructObjName;
	if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *FullPath)) return Found;
	return LoadObject<UScriptStruct>(nullptr, *FullPath);
}

/* Array element categories UK2Node_GetArrayItem refuses to return by reference
 * (SupportsReturnByRef excludes Object/Class/SoftObject/SoftClass/Interface).
 * For these the Output pin must not keep bIsReference=true: propagation flips
 * it live during connection and the editor notifies "Array Get node altered.
 * Now returning a copy." for every such getter. */
static bool IsNonRefCapableArrayCategory(const FEdGraphPinType& PinType)
{
	const FName& Category = PinType.PinCategory;
	return Category == UEdGraphSchema_K2::PC_Object
		|| Category == UEdGraphSchema_K2::PC_Class
		|| Category == UEdGraphSchema_K2::PC_SoftObject
		|| Category == UEdGraphSchema_K2::PC_SoftClass
		|| Category == UEdGraphSchema_K2::PC_Interface;
}

/* MinimalAPI-safe stand-in for UK2Node_GetArrayItem::SetDesiredReturnType:
 * the engine exports neither the method nor the class (LNK2019 on 08.25), but
 * bReturnByRefDesired is a reflected UPROPERTY. Flipping it through reflection
 * and reallocating the pins reproduces the setter exactly (AllocateDefaultPins
 * reads the bool into the output pin's bIsReference), and because the BOOL
 * persists, the setting survives every later compile-time reconstruction -
 * which is what pin-flag clearing alone never did. */
static void SetArrayItemReturnByRef(UK2Node_GetArrayItem* Node, bool bAsReference)
{
	if (!Node) return;
	if (FBoolProperty* Prop = CastField<FBoolProperty>(UK2Node_GetArrayItem::StaticClass()->FindPropertyByName(TEXT("bReturnByRefDesired"))))
	{
		Prop->SetValue_InContainer(Node, &bAsReference);
		/* ReconstructNode reallocation reads AllocateDefaultPins fresh, so the
		 * output pin is rebuilt from the flipped bool. Nothing is wired yet at
		 * both call sites, so link rewiring and PostReconstructNode are no-ops. */
		Node->ReconstructNode();
	}
}

/* Constant-only stub arguments have no producer pin to copy a type from; an
 * empty FEdGraphPinType makes the compiler reject the default value
 * ("Unsupported type on pin Param 0", 08.25: x11 in BP_CharacterUnit). Map the
 * constant token to its category. Object/struct constants never reach here -
 * the stub loop skips those tokens before resolving. */
static FEdGraphPinType PinTypeFromConstantToken(const FString& Token)
{
	FEdGraphPinType PinType;
	if (Token == TEXT("EX_IntConst") || Token == TEXT("EX_Int64Const") || Token == TEXT("EX_ByteConst") || Token == TEXT("EX_IntConstByte"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (Token == TEXT("EX_FloatConst") || Token == TEXT("EX_DoubleConst"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (Token == TEXT("EX_BooleanConst"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (Token == TEXT("EX_StringConst") || Token == TEXT("EX_UnicodeStringConst"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (Token == TEXT("EX_NameConst"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (Token == TEXT("EX_TextConst"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	return PinType;
}

/* Resolve an EX_ObjectConst "Value" object to a live UObject for call-argument
 * placement (plan 010 item 1). Two shapes:
 *  - native class: ObjectName "Class'ShortName'" with a /Script/<Module> package
 *    path (e.g. Class'EnhancedInputLocalPlayerSubsystem' + /Script/EnhancedInput)
 *    -> resolve <Module>.<ShortName> as a UClass;
 *  - asset: strip export suffixes from the ObjectPath and Find/Load generically.
 * Blueprint assets resolve to their generated class's CDO so literals/Literal
 * nodes reference the instance the original graph serialized. */
static UObject* ResolveObjectConstValue(const TSharedPtr<FJsonObject>& ValueObj)
{
	if (!ValueObj.IsValid()) return nullptr;

	const FString ObjectName = ValueObj->GetStringField(TEXT("ObjectName"));
	FString ObjectPath = ValueObj->GetStringField(TEXT("ObjectPath"));

	/* Generated-class constants ("BlueprintGeneratedClass'BP_CharCreation_C'"
	 * with ObjectPath "/Game/TouchyGame/BP/BP_CharCreation.0") are class
	 * references - the class lives at <package>.<ClassName>, not at the
	 * package path the generic path below resolves to (that returns the
	 * UPackage and gets rejected). */
	if (ObjectName.StartsWith(TEXT("BlueprintGeneratedClass'"))
		|| ObjectName.StartsWith(TEXT("WidgetBlueprintGeneratedClass'"))
		|| ObjectName.StartsWith(TEXT("AnimBlueprintGeneratedClass'")))
	{
		const int32 FirstQuote = ObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive);
		const int32 LastQuote = ObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (FirstQuote != INDEX_NONE && LastQuote > FirstQuote)
		{
			FString ShortName = ObjectName.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
			FString PackagePath = ObjectPath;
			int32 DotIdx;
			if (PackagePath.FindChar(TEXT('.'), DotIdx) && PackagePath.Mid(DotIdx + 1).IsNumeric())
			{
				PackagePath = PackagePath.Left(DotIdx);
			}
			const FString FullPath = PackagePath + TEXT(".") + ShortName;
			if (UClass* Found = FindObject<UClass>(nullptr, *FullPath)) return Found;
			return LoadObject<UClass>(nullptr, *FullPath);
		}
		return nullptr;
	}

	if (ObjectName.StartsWith(TEXT("Class'")) && ObjectPath.StartsWith(TEXT("/Script/")))
	{
		FString ShortName = ObjectName;
		ShortName.RemoveFromStart(TEXT("Class'"));
		ShortName.RemoveFromEnd(TEXT("'"));
		if (!ShortName.IsEmpty() && FCString::Strchr(*ShortName, TEXT(':')) == nullptr)
		{
			const FString FullPath = ObjectPath + TEXT(".") + ShortName;
			if (UClass* Found = FindObject<UClass>(nullptr, *FullPath)) return Found;
			return LoadObject<UClass>(nullptr, *FullPath);
		}
		return nullptr;
	}

	int32 BracketIdx;
	if (ObjectPath.FindChar(TEXT('['), BracketIdx))
	{
		ObjectPath = ObjectPath.Left(BracketIdx);
	}
	int32 DotIdx;
	if (ObjectPath.FindChar(TEXT('.'), DotIdx) && ObjectPath.Mid(DotIdx + 1).IsNumeric())
	{
		ObjectPath = ObjectPath.Left(DotIdx);
	}

	UObject* Resolved = FindObject<UObject>(nullptr, *ObjectPath);
	if (!Resolved || Resolved->IsA<UPackage>())
	{
		Resolved = LoadObject<UObject>(nullptr, *ObjectPath);
	}
	if (Resolved && !Resolved->IsA<UPackage>())
	{
		if (UBlueprint* BP = Cast<UBlueprint>(Resolved))
		{
			Resolved = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
		}
		return Resolved;
	}
	return nullptr;
}

/* Resolve a generated-class reference (e.g. BlueprintGeneratedClass'GI_Data_C' with
 * ObjectPath "/Game/TouchyGame/BP/GI_Data.0") using the same chain as
 * ResolveUserDefinedStruct: memory, same-batch registry, then disk. */
static UClass* ResolveClassFromJson(const TSharedPtr<FJsonObject>& OwnerObj)
{
	if (!OwnerObj.IsValid()) return nullptr;

	FString ClassName = OwnerObj->GetStringField(TEXT("ObjectName"));
	ClassName.RemoveFromStart(TEXT("BlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("WidgetBlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("AnimBlueprintGeneratedClass'"));
	ClassName.RemoveFromStart(TEXT("Class'"));
	ClassName.RemoveFromEnd(TEXT("'"));
	int32 ColonIdx;
	if (ClassName.FindChar(TEXT(':'), ColonIdx)) ClassName = ClassName.Left(ColonIdx);

	if (UClass* Found = FindObject<UClass>(nullptr, *ClassName)) return Found;

	FString PkgPath = OwnerObj->GetStringField(TEXT("ObjectPath"));
	int32 DotIdx;
	if (PkgPath.FindLastChar(TEXT('.'), DotIdx)) PkgPath = PkgPath.Left(DotIdx);
	if (PkgPath.IsEmpty()) return nullptr;

	if (FAssetEntry* Entry = FAssetDependencyRegistry::Get().FindByPackagePath(PkgPath))
	{
		if (UObject* Created = Entry->CreatedObject)
		{
			if (UClass* Found = Cast<UClass>(Created)) return Found;
			if (UBlueprint* CreatedBP = Cast<UBlueprint>(Created))
			{
				if (UClass* Found = CreatedBP->GeneratedClass) return Found;
			}
		}
	}

	const FString FullPath = PkgPath + TEXT(".") + ClassName;
	if (UClass* Found = FindObject<UClass>(nullptr, *FullPath)) return Found;
	return LoadObject<UClass>(nullptr, *FullPath);
}

/* Find the output pin on a call node that corresponds to an out/return parameter.
 * Stub functions can end up with pin names that don't exactly match the property
 * name - a K2 recompile may rename or dedupe them ("AsGI Data" vs "AsGI Data1"),
 * or the property may be the function's return value, in which case the pin is
 * named "ReturnValue" (typed) instead of after the parameter. Match in order of
 * preference:
 *   1. exact output pin name == property name
 *   2. any output pin whose name starts with the property name (dedup suffixes)
 *   3. any output pin whose object type matches the property's declared class */
static UEdGraphPin* FindCallNodeOutPinForParam(UK2Node_CallFunction* Node, FProperty* Prop)
{
	if (!Node || !Prop) return nullptr;

	const FString PropName = Prop->GetName();
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == FName(*PropName))
		{
			return Pin;
		}
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinName.ToString().StartsWith(PropName, ESearchCase::CaseSensitive))
		{
			return Pin;
		}
	}

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		if (UClass* PropClass = ObjProp->PropertyClass)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (UClass* PinClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
				{
					if (PinClass == PropClass || PinClass->IsChildOf(PropClass) || PropClass->IsChildOf(PinClass))
					{
						return Pin;
					}
				}
			}
		}
	}

	return nullptr;
}

// Object/class-family pins store their value in DefaultObject, not DefaultValue.
// Writing a string (e.g. "None" from an EX_NoObject param) into DefaultValue
// breaks pin validation with "String NewDefaultValue 'None' specified on object
// pin". Leave object pins untouched unless the constant is a real object path.
static void SetPinDefaultValueSafe(UEdGraphPin* Pin, const FString& Value)
{
	if (!Pin) return;

	const FName& Category = Pin->PinType.PinCategory;
	if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class ||
		Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass ||
		Category == UEdGraphSchema_K2::PC_Interface)
	{
		// "None" / empty == the default null object state; nothing to write.
		if (Value.IsEmpty() || Value == TEXT("None")) return;
		// A real object/class reference is stored via DefaultObject, not DefaultValue.
		Pin->DefaultValue.Reset();
		return;
	}

	Pin->DefaultValue = Value;
}

// Create a pin typed from a JSON property description. Used for function-scoped
// compiler temp locals (e.g. Temp_int_Loop_Counter_Variable) whose member
// reference cannot resolve to a class property, so AllocateDefaultPins makes no pin.
static UEdGraphPin* CreatePinForJsonProperty(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TSharedPtr<FJsonObject>& PropObj, const FName& PinName)
{
	if (!PropObj.IsValid()) return nullptr;

	const FString PropType = PropObj->GetStringField(TEXT("Type"));
	FName Category;
	FName SubCategory;
	UObject* SubCategoryObject = nullptr;

	if (PropType == TEXT("IntProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Int;
	}
	else if (PropType == TEXT("FloatProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Real;
		SubCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (PropType == TEXT("DoubleProperty"))
	{
		/* Engine-resolved double pins are PC_Real + PC_Double (a real pin carrying the
		 * double subcategory), not a bare PC_Double pin. Match that convention so
		 * TryCreateConnection accepts JSON-created double pins against engine ones. */
		Category = UEdGraphSchema_K2::PC_Real;
		SubCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (PropType == TEXT("BoolProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (PropType == TEXT("StrProperty"))
	{
		Category = UEdGraphSchema_K2::PC_String;
	}
	else if (PropType == TEXT("NameProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Name;
	}
	else if (PropType == TEXT("TextProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Text;
	}
	else if (PropType == TEXT("ByteProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Byte;
		if (PropObj->HasField(TEXT("Enum")))
		{
			SubCategoryObject = ResolveEnumObj(PropObj->GetObjectField(TEXT("Enum")));
		}
	}
	else if (PropType == TEXT("ObjectProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Object;
		if (PropObj->HasField(TEXT("PropertyClass")))
		{
			SubCategoryObject = ResolveCastClass(PropObj->GetObjectField(TEXT("PropertyClass")));
		}
	}
	else if (PropType == TEXT("SoftObjectProperty"))
	{
		Category = UEdGraphSchema_K2::PC_SoftObject;
		if (PropObj->HasField(TEXT("PropertyClass")))
		{
			SubCategoryObject = ResolveCastClass(PropObj->GetObjectField(TEXT("PropertyClass")));
		}
	}
	else if (PropType == TEXT("ClassProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Class;
		if (PropObj->HasField(TEXT("PropertyClass")))
		{
			SubCategoryObject = ResolveCastClass(PropObj->GetObjectField(TEXT("PropertyClass")));
		}
	}
	else if (PropType == TEXT("StructProperty"))
	{
		Category = UEdGraphSchema_K2::PC_Struct;
		if (PropObj->HasField(TEXT("Struct")))
		{
			SubCategoryObject = ResolveStructObj(PropObj->GetObjectField(TEXT("Struct")));
		}
	}

	if (Category.IsNone())
	{
		UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePinForJsonProperty: unhandled type %s"), *PropType);
    return nullptr;
}

	UEdGraphPin* Pin = Node->CreatePin(Direction, Category, SubCategory, SubCategoryObject, PinName);
	if (Pin)
	{
		Pin->PinFriendlyName = FText::FromName(PinName);
	}
	return Pin;
}

FBlueprintBytecodeImporter::FBlueprintBytecodeImporter(UBlueprint* InBlueprint, UBlueprintGeneratedClass* InGeneratedClass)
    : Blueprint(InBlueprint)
    , GeneratedClass(InGeneratedClass)
    , EventGraph(nullptr)
{
}

/* Static diagnostics shared across import instances (plan 013). */
FBlueprintImportDiagnostics FBlueprintBytecodeImporter::LastImportDiagnostics;

void FBlueprintBytecodeImporter::AddDiagnostic(const FString& Category, const FString& Text) const
{
    LastImportDiagnostics.Add(Category, Text);
}

bool FBlueprintBytecodeImporter::ProcessFunctions(const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!Blueprint || !GeneratedClass)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Error, TEXT("Invalid blueprint or generated class"));
        return false;
    }

    // Step 1: Parse all function JSON objects
    ThunkParamBindings.Reset();
    WrittenUbergraphSlots.Reset();
    LastImportDiagnostics.Reset(Blueprint->GetName());
    for (const TSharedPtr<FJsonValue>& JsonValue : JsonObjects)
    {
        const TSharedPtr<FJsonObject>& JsonObject = JsonValue->AsObject();
        if (!JsonObject) continue;

        if (!JsonObject->HasField(TEXT("Type"))) continue;
        const FString& TypeName = JsonObject->GetStringField(TEXT("Type"));

        /* Variable Registry (plan 011 item 0): classify every declared
         * variable up-front from ChildProperties flags. */
        if (TypeName == TEXT("BlueprintGeneratedClass"))
        {
            if (JsonObject->HasField(TEXT("ChildProperties")))
            {
                BuildVariableRegistry(JsonObject->GetArrayField(TEXT("ChildProperties")), EVarKind::GraphVariable);
            }
            continue;
        }
        if (TypeName != TEXT("Function")) continue;

        ParsedFunction Func = ParseFunction(JsonObject);

        /* AnimBPs export their anim-graph entry thunk as a Function named
         * "AnimGraph". Building a graph for it would collide with the real
         * AnimGraph ubergraph page - the anim importer owns that page. */
        if (Func.Name == TEXT("AnimGraph")) continue;
        /* EvaluateGraphExposedInputs_ExecuteUbergraph_<BP>_AnimGraphNode_<Type>_<GUID>
         * are compiler thunks for anim-node input pins. The anim importer binds
         * those inputs through PropertyBindings; surfacing them as functions
         * only pollutes the FUNCTIONS list. */
        if (Func.Name.StartsWith(TEXT("EvaluateGraphExposedInputs_"))) continue;

        if (!Func.Name.IsEmpty())
        {
            Func.Kind = ClassifyFunction(Func);
            Func.EntryIndex = ExtractEntryIndex(Func);

            /* Exact parameter->frame-slot bindings from the thunk (plan 011
             * follow-up B): EX_LetValueOnPersistentFrame assignments declare
             * which frame slot each incoming parameter feeds.
             * NOTE: must run BEFORE MoveTemp(Func) below. */
        for (const FBytecodeToken& Tok : Func.BytecodeTokens)
        {
            if (!Tok.JsonData.IsValid()) continue;

            if (Tok.Token == TEXT("EX_LetValueOnPersistentFrame"))
            {
                const TSharedPtr<FJsonObject>& Dest = Tok.JsonData->GetObjectField(TEXT("DestinationProperty"));
                const TSharedPtr<FJsonObject>& DProp = Dest.IsValid() ? Dest->GetObjectField(TEXT("Property")) : nullptr;
                FString Slot = DProp.IsValid() ? DProp->GetStringField(TEXT("Name")) : TEXT("");
                Slot = Slot.IsEmpty() && DProp.IsValid() && DProp->HasField(TEXT("Path"))
                    ? (DProp->GetArrayField(TEXT("Path")).Num() > 0 ? DProp->GetArrayField(TEXT("Path"))[0]->AsString() : TEXT(""))
                    : Slot;

                FString Parm;
                const TSharedPtr<FJsonObject>& SrcExpr = Tok.JsonData->GetObjectField(TEXT("AssignmentExpression"));
                if (SrcExpr.IsValid())
                {
                    const TSharedPtr<FJsonObject>& SV = SrcExpr->GetObjectField(TEXT("Variable"));
                    const TSharedPtr<FJsonObject>& SIV = SV.IsValid() ? (SV->HasField(TEXT("Variable")) ? SV->GetObjectField(TEXT("Variable")) : SV) : SV;
                    const TSharedPtr<FJsonObject>& SP = SIV.IsValid() ? SIV->GetObjectField(TEXT("Property")) : nullptr;
                    if (SP.IsValid()) Parm = SP->GetStringField(TEXT("Name"));
                }
                if (!Slot.IsEmpty() && !Parm.IsEmpty())
                {
                    ThunkParamBindings.FindOrAdd(Func.Name).Add(TPair<FString, FString>(Slot, Parm));
                    WrittenUbergraphSlots.Add(Slot);
                }
            }
            else if (Tok.Token.StartsWith(TEXT("EX_Let")))
            {
                const TSharedPtr<FJsonObject>& VarObj = Tok.JsonData->GetObjectField(TEXT("Variable"));
                const TSharedPtr<FJsonObject>& VIV = VarObj.IsValid() ? (VarObj->HasField(TEXT("Variable")) ? VarObj->GetObjectField(TEXT("Variable")) : VarObj) : VarObj;
                const TSharedPtr<FJsonObject>& VP = VIV.IsValid() ? VIV->GetObjectField(TEXT("Property")) : nullptr;
                if (VP.IsValid())
                {
                    const FString LhsName = VP->GetStringField(TEXT("Name"));
                    if (!LhsName.IsEmpty()) WrittenUbergraphSlots.Add(LhsName);
                }
            }
        }

        ParsedFunctions.Add(Func.Name, MoveTemp(Func));

        if (JsonObject->HasField(TEXT("ChildProperties")))
        {
            BuildVariableRegistry(JsonObject->GetArrayField(TEXT("ChildProperties")),
                Func.Kind == EFunctionKind::Ubergraph ? EVarKind::FrameTemp : EVarKind::FunctionParm);
        }
        }
    }

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Parsed %d functions"), ParsedFunctions.Num());

    // Step 1.25: Clear anything a previous import built so a re-import rebuilds
    // from scratch instead of stacking on top of stale nodes and UFunctions.
    ClearExistingGraphContent();

    // Step 1.5: Create UFunction objects with properties so entry/result nodes
    // get proper typed pins from ChildProperties.
    CreateFunctionProperties();

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
// Function property creation
// ============================================================================

void FBlueprintBytecodeImporter::CreateFunctionProperties()
{
    for (auto& Pair : ParsedFunctions)
    {
        const FString& FuncName = Pair.Key;
        ParsedFunction& Func = Pair.Value;

        /* Skip ubergraph and construction script — they have no user-visible params */
        if (Func.Kind == EFunctionKind::Ubergraph || Func.Kind == EFunctionKind::ConstructionScript)
        {
            continue;
        }

        /* Receive* events (ReceiveBeginPlay/ReceiveTick) are declared by the native
         * super class. The event node references the native function directly
         * (CreateEventNode), so a scaffold UFunction on the class with the same name
         * is both unnecessary and triggers the K2 compiler's
         * "name conflicts with a native 'Actor' function" error. */
        if (Func.Kind == EFunctionKind::NativeEvent)
        {
            continue;
        }

        /* Build a scaffold UFunction with its parameter properties, then register
         * it on BOTH the GeneratedClass and the SkeletonGeneratedClass. Call node
         * pin allocation (UK2Node_CallFunction::AllocateDefaultPins) resolves the
         * referenced function via the SkeletonGeneratedClass first, so a function
         * that only exists on the GeneratedClass yields a pinless call node. */
        auto BuildScaffoldFunction = [&](UClass* TargetClass) -> UFunction*
        {
            UFunction* FuncObj = NewObject<UFunction>(TargetClass, FName(*FuncName), RF_Public | RF_Standalone);

            /* Use the flags the export declared. Fall back to a sane default only if
             * the JSON carried none. The final compiled UFunction is regenerated from
             * the graph by Kismet, so these flags only drive in-graph behavior (call
             * node visibility, entry/result pin creation). */
            EFunctionFlags FnFlags = ParseFunctionFlags(Func.Flags);
            if (FnFlags == (EFunctionFlags)0)
            {
                FnFlags = (EFunctionFlags)(FUNC_Public | FUNC_BlueprintCallable | FUNC_BlueprintEvent);
            }
            FuncObj->FunctionFlags = FnFlags;

            /* Create FProperty entries for the real parameter/return properties. */
            PopulateFunctionProperties(FuncObj, Func.ChildProperties);

            /* UStruct::AddCppProperty prepends, so after PopulateFunctionProperties
             * the ChildProperties chain is the REVERSE of the JSON declaration.
             * TFieldIterator-based pin creation (call parameters, entry/result pins)
             * maps bytecode args positionally, so restore declaration order by
             * relinking the chain. Property offsets were already assigned by
             * StaticLink inside PopulateFunctionProperties and are unaffected by
             * the relink; the final compiled class regenerates both anyway. */
            FField* Prev = nullptr;
            FField* Cur = FuncObj->ChildProperties;
            FuncObj->ChildProperties = nullptr;
            while (Cur)
            {
                FField* Next = Cur->Next;
                Cur->Next = Prev;
                Prev = Cur;
                Cur = Next;
            }
            FuncObj->ChildProperties = Prev;

            TargetClass->AddFunctionToFunctionMap(FuncObj, FName(*FuncName));

            /* Link the scaffold function into the class's Children field chain (the
             * way the K2 compiler links compiled functions). ResolveMember resolves
             * function-scoped locals via FindUField<UStruct>(Class, FuncName), which
             * walks Children - without this link, local-scope variable pins cannot be
             * created during graph building. The final compile regenerates the class
             * (PurgeClass resets Children), so this never leaks into the cooked class. */
            FuncObj->Next = TargetClass->Children;
            TargetClass->Children = FuncObj;

            return FuncObj;
        };

        BuildScaffoldFunction(GeneratedClass);
        if (Blueprint && Blueprint->SkeletonGeneratedClass)
        {
            BuildScaffoldFunction(Blueprint->SkeletonGeneratedClass);
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Created UFunction: %s"),
            *FuncName);
    }
}

EFunctionFlags FBlueprintBytecodeImporter::ParseFunctionFlags(const FString& FlagsStr)
{
    EFunctionFlags Result = (EFunctionFlags)0;

    TArray<FString> Tokens;
    FlagsStr.ParseIntoArray(Tokens, TEXT("|"), true);
    for (const FString& RawTok : Tokens)
    {
        const FString Tok = RawTok.TrimStartAndEnd();
        if (Tok == TEXT("FUNC_Final"))                Result = (EFunctionFlags)(Result | FUNC_Final);
        else if (Tok == TEXT("FUNC_RequiredAPI"))     Result = (EFunctionFlags)(Result | FUNC_RequiredAPI);
        else if (Tok == TEXT("FUNC_BlueprintAuthorityOnly")) Result = (EFunctionFlags)(Result | FUNC_BlueprintAuthorityOnly);
        else if (Tok == TEXT("FUNC_BlueprintCosmetic")) Result = (EFunctionFlags)(Result | FUNC_BlueprintCosmetic);
        else if (Tok == TEXT("FUNC_Net"))              Result = (EFunctionFlags)(Result | FUNC_Net);
        else if (Tok == TEXT("FUNC_NetReliable"))     Result = (EFunctionFlags)(Result | FUNC_NetReliable);
        else if (Tok == TEXT("FUNC_NetRequest"))      Result = (EFunctionFlags)(Result | FUNC_NetRequest);
        else if (Tok == TEXT("FUNC_Exec"))            Result = (EFunctionFlags)(Result | FUNC_Exec);
        else if (Tok == TEXT("FUNC_Native"))          Result = (EFunctionFlags)(Result | FUNC_Native);
        else if (Tok == TEXT("FUNC_Event"))           Result = (EFunctionFlags)(Result | FUNC_Event);
        else if (Tok == TEXT("FUNC_NetResponse"))     Result = (EFunctionFlags)(Result | FUNC_NetResponse);
        else if (Tok == TEXT("FUNC_Static"))          Result = (EFunctionFlags)(Result | FUNC_Static);
        else if (Tok == TEXT("FUNC_NetMulticast"))    Result = (EFunctionFlags)(Result | FUNC_NetMulticast);
        else if (Tok == TEXT("FUNC_UbergraphFunction")) Result = (EFunctionFlags)(Result | FUNC_UbergraphFunction);
        else if (Tok == TEXT("FUNC_MulticastDelegate")) Result = (EFunctionFlags)(Result | FUNC_MulticastDelegate);
        else if (Tok == TEXT("FUNC_Public"))          Result = (EFunctionFlags)(Result | FUNC_Public);
        else if (Tok == TEXT("FUNC_Private"))         Result = (EFunctionFlags)(Result | FUNC_Private);
        else if (Tok == TEXT("FUNC_Protected"))       Result = (EFunctionFlags)(Result | FUNC_Protected);
        else if (Tok == TEXT("FUNC_Delegate"))        Result = (EFunctionFlags)(Result | FUNC_Delegate);
        else if (Tok == TEXT("FUNC_NetServer"))       Result = (EFunctionFlags)(Result | FUNC_NetServer);
        else if (Tok == TEXT("FUNC_HasOutParms"))     Result = (EFunctionFlags)(Result | FUNC_HasOutParms);
        else if (Tok == TEXT("FUNC_HasDefaults"))     Result = (EFunctionFlags)(Result | FUNC_HasDefaults);
        else if (Tok == TEXT("FUNC_NetClient"))       Result = (EFunctionFlags)(Result | FUNC_NetClient);
        else if (Tok == TEXT("FUNC_DLLImport"))       Result = (EFunctionFlags)(Result | FUNC_DLLImport);
        else if (Tok == TEXT("FUNC_BlueprintCallable")) Result = (EFunctionFlags)(Result | FUNC_BlueprintCallable);
        else if (Tok == TEXT("FUNC_BlueprintEvent"))  Result = (EFunctionFlags)(Result | FUNC_BlueprintEvent);
        else if (Tok == TEXT("FUNC_BlueprintPure"))   Result = (EFunctionFlags)(Result | FUNC_BlueprintPure);
        else if (Tok == TEXT("FUNC_Const"))           Result = (EFunctionFlags)(Result | FUNC_Const);
        else if (Tok == TEXT("FUNC_NetValidate"))     Result = (EFunctionFlags)(Result | FUNC_NetValidate);
        else if (Tok == TEXT("FUNC_EditorOnly"))      Result = (EFunctionFlags)(Result | FUNC_EditorOnly);
    }

    return Result;
}

FProperty* FBlueprintBytecodeImporter::CreatePropertyFromJson(FFieldVariant Owner, const FString& PropName,
    const TSharedPtr<FJsonObject>& PropJson)
{
    if (!PropJson.IsValid()) return nullptr;

    const FString PropType = PropJson->GetStringField(TEXT("Type"));
    const FName PropFName(*PropName);

    if (PropType == TEXT("BoolProperty"))
    {
        FBoolProperty* BoolProp = new FBoolProperty(Owner, PropFName, RF_Public);
        BoolProp->SetBoolSize(1, true);
        return BoolProp;
    }
    else if (PropType == TEXT("ObjectProperty"))
    {
        FObjectProperty* ObjProp = new FObjectProperty(Owner, PropFName, RF_Public);
        if (PropJson->HasField(TEXT("PropertyClass")))
        {
            const TSharedPtr<FJsonObject>& PropClass = PropJson->GetObjectField(TEXT("PropertyClass"));
            if (PropClass.IsValid())
            {
                UClass* ResolvedClass = ResolveCastClass(PropClass);
                if (!ResolvedClass)
                {
                    FString ClsName = PropClass->GetStringField(TEXT("ObjectName"));
                    UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePropertyFromJson: could not resolve class for '%s' (%s)"), *PropName, *ClsName);
                }
                ObjProp->SetPropertyClass(ResolvedClass);
            }
        }
        return ObjProp;
    }
    else if (PropType == TEXT("IntProperty") || PropType == TEXT("Int8Property")
        || PropType == TEXT("Int16Property") || PropType == TEXT("Int64Property"))
    {
        return new FIntProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("FloatProperty"))
    {
        return new FFloatProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("DoubleProperty"))
    {
        return new FDoubleProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("StrProperty"))
    {
        return new FStrProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("TextProperty"))
    {
        return new FTextProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("NameProperty"))
    {
        return new FNameProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("StructProperty"))
    {
        /* Resolve FIRST, allocate after: a null-Struct FStructProperty into the
         * Kismet compiler is an access violation waiting to happen (08.24:
         * S_ClothesStats outside the closure -> Set garment locals with null
         * struct type). Callers skip nullptr with a warning - the missing
         * dependency is the thing to fix, not the symptom to keep. */
        UScriptStruct* ResolvedStruct = nullptr;
        FString StructObjectName;
        if (PropJson->HasField(TEXT("Struct")))
        {
            const TSharedPtr<FJsonObject>& StructObj = PropJson->GetObjectField(TEXT("Struct"));
            if (StructObj.IsValid())
            {
                StructObjectName = StructObj->GetStringField(TEXT("ObjectName"));
                StructObjectName.RemoveFromStart(TEXT("UserDefinedStruct'"));
                StructObjectName.RemoveFromStart(TEXT("ScriptStruct'"));
                /* Native engine structs export as Class'Key' / Class'LinearColor' with
                 * ObjectPath = module only ("/Script/InputCore"). ResolveUserDefinedStruct
                 * rebuilds the module-qualified path from ObjectPath + short name. */
                StructObjectName.RemoveFromStart(TEXT("Class'"));
                StructObjectName.RemoveFromEnd(TEXT("'"));

                ResolvedStruct = ResolveUserDefinedStruct(StructObjectName, StructObj->GetStringField(TEXT("ObjectPath")));
            }
        }
        if (!ResolvedStruct)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePropertyFromJson: could not resolve struct for '%s' (%s) - property skipped"), *PropName, *StructObjectName);
            LastImportDiagnostics.Add(TEXT("UnresolvedStruct"), FString::Printf(TEXT("property '%s' (%s)"), *PropName, *StructObjectName));
            return nullptr;
        }

        FStructProperty* StructProp = new FStructProperty(Owner, PropFName, RF_Public);
        StructProp->Struct = ResolvedStruct;
        return StructProp;
    }
    else if (PropType == TEXT("ClassProperty"))
    {
        FClassProperty* ClassProp = new FClassProperty(Owner, PropFName, RF_Public);
        ClassProp->PropertyClass = UClass::StaticClass();
        if (PropJson->HasField(TEXT("PropertyClass")))
        {
            UClass* ResolvedClass = ResolveCastClass(PropJson->GetObjectField(TEXT("PropertyClass")));
            if (ResolvedClass)
            {
                ClassProp->SetMetaClass(ResolvedClass);
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePropertyFromJson: could not resolve class for '%s'"), *PropName);
            }
        }
        return ClassProp;
    }
    else if (PropType == TEXT("ByteProperty") || PropType == TEXT("EnumProperty"))
    {
        /* 5.7 keeps FByteProperty::Enum private with no public setter, so an
         * enum-backed parm must be modeled as FEnumProperty (public SetEnum) -
         * the K2 pin builder maps both to a PC_Byte pin carrying the enum
         * object, making them interchangeable for call-node pin creation.
         * A plain byte without an Enum reference stays FByteProperty. */
        UEnum* ResolvedEnum = nullptr;
        if (PropJson->HasField(TEXT("Enum")))
        {
            const TSharedPtr<FJsonObject>& EnumObj = PropJson->GetObjectField(TEXT("Enum"));
            if (EnumObj.IsValid())
            {
                ResolvedEnum = ResolveEnumObj(EnumObj);
                if (!ResolvedEnum)
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePropertyFromJson: could not resolve enum for '%s'"), *PropName);
                    LastImportDiagnostics.Add(TEXT("UnresolvedEnum"), FString::Printf(TEXT("property '%s'"), *PropName));
                }
            }
        }

        if (PropType == TEXT("EnumProperty"))
        {
            /* An EnumProperty whose enum failed to resolve is the same
             * null-typed hazard as a null struct - skip it. */
            if (!ResolvedEnum)
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreatePropertyFromJson: enum property '%s' has no resolvable enum - property skipped"), *PropName);
                LastImportDiagnostics.Add(TEXT("UnresolvedEnum"), FString::Printf(TEXT("property '%s' skipped"), *PropName));
                return nullptr;
            }
            FEnumProperty* EnumProp = new FEnumProperty(Owner, PropFName, RF_Public);
            /* Same construction order as FPropertyBag::AddProperty: SetEnum
             * first, then AddCppProperty installs the underlying byte storage
             * (it CHECKs the child's owner is the enum property itself). */
            EnumProp->SetEnum(ResolvedEnum);
            FNumericProperty* UnderlyingProp = new FByteProperty(EnumProp, TEXT("UnderlyingType"), RF_Transient);
            EnumProp->AddCppProperty(UnderlyingProp);
            return EnumProp;
        }

        return new FByteProperty(Owner, PropFName, RF_Public);
    }
    else if (PropType == TEXT("ArrayProperty"))
    {
        FArrayProperty* ArrayProp = new FArrayProperty(Owner, PropFName, RF_Public);
        if (PropJson->HasField(TEXT("Inner")))
        {
            const TSharedPtr<FJsonObject>& InnerJson = PropJson->GetObjectField(TEXT("Inner"));
            if (InnerJson.IsValid())
            {
                /* Recurse with the array as owner so the element property is a
                 * proper child field; StaticLink derives ElementSize from it. */
                ArrayProp->Inner = CreatePropertyFromJson(ArrayProp, PropName, InnerJson);
            }
        }
        return ArrayProp;
    }

    return nullptr;
}

void FBlueprintBytecodeImporter::PopulateFunctionProperties(UFunction* FuncObj, const TArray<TSharedPtr<FJsonObject>>& ChildProperties)
{
    if (!FuncObj) return;

    /* Walk ChildProperties - create FProperty entries for real parameters only */
    int32 RetValOffset = INDEX_NONE;
    FProperty* LastOutParam = nullptr;
    bool bSawExplicitReturnParm = false;
    for (const TSharedPtr<FJsonObject>& PropJson : ChildProperties)
    {
        if (!PropJson.IsValid()) continue;

        const FString PropName = PropJson->GetStringField(TEXT("Name"));
        const FString PropType = PropJson->GetStringField(TEXT("Type"));

        /* Skip internal compiler temps */
        if (PropName.StartsWith(TEXT("CallFunc_")) || PropName.StartsWith(TEXT("K2Node_"))
            || PropName.StartsWith(TEXT("Temp_")))
        {
            continue;
        }

        /* A parameter must be flagged Parm. Local variables also carry
         * Edit | BlueprintVisible (that is how BP marks function locals), so
         * those flags alone must NOT classify a property as a parameter -
         * otherwise every function local becomes a fake Parm on the UFunction
         * and is then skipped by the local-variable declaration pass. */
        const FString PropFlags = PropJson->HasField(TEXT("PropertyFlags"))
            ? PropJson->GetStringField(TEXT("PropertyFlags")) : TEXT("");

        const bool bIsParm = PropFlags.Contains(TEXT("Parm"));
        const bool bIsOutParm = PropFlags.Contains(TEXT("OutParm"));
        const bool bIsReturnParm = PropFlags.Contains(TEXT("ReturnParm"));
        const bool bIsReferenceParm = PropFlags.Contains(TEXT("ReferenceParm"));
        const bool bIsConstParm = PropFlags.Contains(TEXT("ConstParm"));
        const bool bIsRequiredParm = PropFlags.Contains(TEXT("RequiredParm"));
        const bool bIsBlueprintReadOnly = PropFlags.Contains(TEXT("BlueprintReadOnly"));

        if (!bIsParm && !bIsOutParm) continue;

        /* Create the right FProperty subclass */
        FProperty* Prop = CreatePropertyFromJson(FuncObj, PropName, PropJson);
        if (!Prop)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("PopulateFunctionProperties: skipping unsupported param '%s' of type %s"), *PropName, *PropType);
            continue;
        }

        /* Set parameter flags. ReferenceParm/ConstParm matter: the engine's
         * pin builder classifies a param as a function INPUT when it has
         * ReferenceParm set (EdGraphSchema_K2 / K2Node::CreatePinsForFunctionEntryExit:
         * bIsFunctionInput = !CPF_OutParm || CPF_ReferenceParm), so dropping
         * these flags would push const-ref struct params onto the result node
         * as plain out params instead of the entry node as pass-by-ref pins. */
        EPropertyFlags NewPropFlags = (EPropertyFlags)(CPF_Parm | CPF_BlueprintVisible | CPF_Edit);
        if (bIsOutParm) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_OutParm);
        if (bIsReturnParm) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_ReturnParm | CPF_OutParm);
        if (bIsReferenceParm) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_ReferenceParm);
        if (bIsConstParm) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_ConstParm);
        if (bIsRequiredParm) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_RequiredParm);
        if (bIsBlueprintReadOnly) NewPropFlags = (EPropertyFlags)(NewPropFlags | CPF_BlueprintReadOnly);
        Prop->SetPropertyFlags(NewPropFlags);

        /* Track the return value. The export is inconsistent about spelling
         * ReturnParm (Global Game Instance's 'AsGI Data' return is only
         * "Parm | OutParm"), so when no explicit ReturnParm exists the last
         * OutParm is the return value - that matches UE's layout where the
         * return is the trailing parameter. ConstParm/ReferenceParm out params
         * are const-ref inputs (e.g. Add New NPC (Input)'s struct params), NOT
         * returns, so they never count. */
        if (bIsReturnParm)
        {
            bSawExplicitReturnParm = true;
            RetValOffset = FuncObj->PropertiesSize;
        }
        else if (bIsOutParm && !PropFlags.Contains(TEXT("ReferenceParm")) && !PropFlags.Contains(TEXT("ConstParm")))
        {
            LastOutParam = Prop;
        }

        FuncObj->AddCppProperty(Prop);
    }

    /* Infer the return value from the last OutParm when the export omitted
     * ReturnParm. */
    if (!bSawExplicitReturnParm && LastOutParam)
    {
        LastOutParam->SetPropertyFlags((EPropertyFlags)(LastOutParam->PropertyFlags | CPF_ReturnParm));
        RetValOffset = LastOutParam->GetOffset_ForInternal();
    }

    /* Set the return value offset if we found an out param */
    if (RetValOffset != INDEX_NONE)
    {
        FuncObj->ReturnValueOffset = RetValOffset;
    }

    FuncObj->Bind();
    FuncObj->StaticLink(true);
}

/* Depth-limited scan for EX_LocalVariable tokens. Each carries the compiler temp's
 * Property name under Variable->Property. Used to discover which frame locals a
 * function body actually reads/writes (the ubergraph frame is shared by every event
 * and function compiled into it). */
static void CollectLocalVariableNames(const TSharedPtr<FJsonObject>& Obj, TSet<FString>& OutNames, int32 Depth = 0)
{
    if (!Obj.IsValid() || Depth > 16) return;

    if (Obj->HasField(TEXT("Token")) && Obj->GetStringField(TEXT("Token")) == TEXT("EX_LocalVariable"))
    {
        if (Obj->HasField(TEXT("Variable")))
        {
            const TSharedPtr<FJsonObject>& Var = Obj->GetObjectField(TEXT("Variable"));
            if (Var.IsValid() && Var->HasField(TEXT("Property")))
            {
                const TSharedPtr<FJsonObject>& Prop = Var->GetObjectField(TEXT("Property"));
                if (Prop.IsValid())
                {
                    const FString Name = Prop->GetStringField(TEXT("Name"));
                    if (!Name.IsEmpty())
                    {
                        OutNames.Add(Name);
                    }
                }
            }
        }
    }

    for (const auto& KV : Obj->Values)
    {
        if (KV.Value->Type == EJson::Object)
        {
            CollectLocalVariableNames(KV.Value->AsObject(), OutNames, Depth + 1);
        }
        else if (KV.Value->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Elem : KV.Value->AsArray())
            {
                if (Elem->Type == EJson::Object)
                {
                    CollectLocalVariableNames(Elem->AsObject(), OutNames, Depth + 1);
                }
            }
        }
    }
}

void FBlueprintBytecodeImporter::CreateFunctionLocalVariables(FFunctionBuilder& Builder, const ParsedFunction& Ubergraph,
    const TArray<const FBytecodeToken*>& Stmts)
{
    if (!Builder.Func || !Builder.EntryNode) return;

    UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Builder.EntryNode);
    if (!EntryNode) return;

    UFunction* FuncObj = GeneratedClass->FindFunctionByName(FName(*Builder.Func->Name));
    if (!FuncObj) return;

    /* Names of every frame local this body references. The ubergraph frame holds
     * temps for ALL events/functions compiled into it, so only the referenced ones
     * belong to this function as locals. */
    TSet<FString> ReferencedNames;
    for (const FBytecodeToken* Stmt : Stmts)
    {
        if (Stmt && Stmt->JsonData.IsValid())
        {
            CollectLocalVariableNames(Stmt->JsonData, ReferencedNames);
        }
    }
    if (ReferencedNames.Num() == 0) return;

    bool bAdded = false;

    /* The ubergraph frame holds temps for ALL events/functions compiled into it, so
     * local declarations come from the passed Ubergraph ChildProperties. Function
     * scoped locals declared directly on the UFunction (e.g. "SD loc" in
     * PassDataFromSaveGame) also need declaring, so merge those in - skipping any
     * name already handled so ubergraph-backed functions don't get duplicates. */
    TArray<TSharedPtr<FJsonObject>> PropSources;
    PropSources.Reserve(Ubergraph.ChildProperties.Num() + (Builder.Func ? Builder.Func->ChildProperties.Num() : 0));
    PropSources.Append(Ubergraph.ChildProperties);
    if (Builder.Func)
    {
        for (const TSharedPtr<FJsonObject>& PropJson : Builder.Func->ChildProperties)
        {
            if (!PropJson.IsValid()) continue;
            const FString Name = PropJson->GetStringField(TEXT("Name"));
            bool bDuplicate = false;
            for (const TSharedPtr<FJsonObject>& Existing : PropSources)
            {
                if (Existing.IsValid() && Existing->GetStringField(TEXT("Name")) == Name)
                {
                    bDuplicate = true;
                    break;
                }
            }
            if (!bDuplicate) PropSources.Add(PropJson);
        }
    }

    for (const TSharedPtr<FJsonObject>& PropJson : PropSources)
    {
        if (!PropJson.IsValid()) continue;

        const FString Name = PropJson->GetStringField(TEXT("Name"));
        if (!ReferencedNames.Contains(Name)) continue;

        /* Call results and node temps are pure expression slots handled through
         * ProducerPins - they are not declared local variables. */
        if (Name.StartsWith(TEXT("CallFunc_")) || Name.StartsWith(TEXT("K2Node_"))) continue;

        const FString PropFlags = PropJson->HasField(TEXT("PropertyFlags"))
            ? PropJson->GetStringField(TEXT("PropertyFlags")) : TEXT("");
        if (PropFlags.Contains(TEXT("Parm"))) continue;

        if (FindFProperty<FProperty>(FuncObj, FName(*Name))) continue;

        FProperty* Prop = CreatePropertyFromJson(FuncObj, Name, PropJson);
        if (!Prop) continue;

        /* Frame locals are written by the graph's Set nodes, so the property and its
         * declaration must be blueprint-writable or the compiler rejects the Set
         * ("X is not blueprint writable"). */
        Prop->PropertyFlags |= CPF_BlueprintVisible;
        FuncObj->AddCppProperty(Prop);

        /* UK2Node_Variable::GetPropertyForVariable resolves graph variable pins
         * against the SkeletonGeneratedClass first (until the generated class
         * layout is ready). The scaffold SkeletonGeneratedClass function only
         * carries the parameters, so mirror the local property onto it - otherwise
         * the local's Get/Set nodes allocate no pins. */
        if (Blueprint && Blueprint->SkeletonGeneratedClass)
        {
            if (UFunction* SkeletonFunc = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*Builder.Func->Name)))
            {
                if (!FindFProperty<FProperty>(SkeletonFunc, FName(*Name)))
                {
                    FProperty* SkeletonProp = CreatePropertyFromJson(SkeletonFunc, Name, PropJson);
                    if (SkeletonProp)
                    {
                        SkeletonProp->PropertyFlags |= CPF_BlueprintVisible;
                        SkeletonFunc->AddCppProperty(SkeletonProp);
                    }
                }
            }
        }

        FBPVariableDescription Desc;
        Desc.VarName = FName(*Name);
        Desc.VarGuid = FGuid::NewGuid();
        Desc.VarType = PinTypeFromJson(PropJson);
        Desc.FriendlyName = Name;
        Desc.Category = FText::GetEmpty();
        Desc.PropertyFlags = CPF_BlueprintVisible;
        EntryNode->LocalVariables.Add(Desc);

        Builder.FunctionLocalNames.Add(Name);
        Builder.FunctionLocalGuids.Add(Name, Desc.VarGuid);
        bAdded = true;

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Declared function local '%s' on %s"), *Name, *Builder.Func->Name);
    }

    if (bAdded)
    {
        /* Relink so ResolveMemberImpl finds the locals during pin allocation. */
        FuncObj->StaticLink(true);
    }
}

const ParsedFunction* FBlueprintBytecodeImporter::FindUbergraph() const
{
    const FString UbergraphName = FString::Printf(TEXT("ExecuteUbergraph_%s"), *Blueprint->GetName());
    if (const ParsedFunction* Found = ParsedFunctions.Find(UbergraphName))
    {
        return Found;
    }

    for (const auto& Pair : ParsedFunctions)
    {
        if (Pair.Key.StartsWith(TEXT("ExecuteUbergraph_")))
        {
            return &Pair.Value;
        }
    }
    return nullptr;
}

void FBlueprintBytecodeImporter::ClearExistingGraphContent()
{
    if (!Blueprint) return;

    /* Remove UFunction objects a previous import created on the generated class.
     * Only direct children of GeneratedClass are touched; parent/native functions
     * live on other classes and are left alone. Renaming them out of the class also
     * stops NewObject from reusing a stale UFunction when a fresh one is created
     * with the same name later. */
    auto ClearScaffoldFunctions = [this](UClass* ClassToClear)
    {
        if (!ClassToClear) return;

        TArray<UFunction*> OwnedFunctions;
        for (UFunction* Func : TFieldRange<UFunction>(ClassToClear))
        {
            if (Func->GetOuter() == ClassToClear)
            {
                OwnedFunctions.Add(Func);
            }
        }

        for (UFunction* Func : OwnedFunctions)
        {
            ClassToClear->RemoveFunctionFromFunctionMap(Func);
            /* Unlink the function from the class Children chain BEFORE renaming it
             * away. CreateFunctionProperties links each scaffold UFunction onto the
             * head of Children (FuncObj->Next = Children; Children = FuncObj). If a
             * previous import's function is left linked and NewObject then reuses
             * that same object, re-linking it onto the chain makes Next point at
             * itself -> a Children chain cycle that hangs the K2 compiler when it
             * walks Children during recompile. */
            if (ClassToClear->Children == Func)
            {
                ClassToClear->Children = Func->Next;
            }
            else
            {
                UField* Prev = ClassToClear->Children;
                while (Prev && Prev->Next != Func)
                {
                    Prev = Prev->Next;
                }
                if (Prev)
                {
                    Prev->Next = Func->Next;
                }
            }
            Func->Next = nullptr;

            Func->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
            Func->MarkAsGarbage();
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Cleared %d previously imported function(s) from %s"),
            OwnedFunctions.Num(), *ClassToClear->GetName());
    };

    if (GeneratedClass)
    {
        /* Tear down the uber graph state BEFORE the ExecuteUbergraph_* UFunction is
         * removed below. The CDO's persistent frame is laid out against the current
         * uber graph function; once that function is marked garbage and later
         * collected, GC walking the surviving class/CDO in
         * UBlueprintGeneratedClass::AddReferencedObjectsInUbergraphFrame dereferences
         * the (now null) UberGraphFunction and crashes reading Struct->RefLink.
         * Freeing the frame and clearing both pointers first leaves the old class/CDO
         * in a state GC can visit safely. */
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
    }

    ClearScaffoldFunctions(GeneratedClass);
    if (Blueprint && Blueprint->SkeletonGeneratedClass)
    {
        ClearScaffoldFunctions(Blueprint->SkeletonGeneratedClass);
    }

    /* Remove any ObjectRedirector left behind under the class (e.g. by a rename that was
     * interrupted or by a stale compile). They must not sit in the way of NewObject recreating
     * a UFunction with the same name. */
    if (GeneratedClass)
    {
        TArray<UObject*> OwnedObjects;
        GetObjectsWithOuter(GeneratedClass, OwnedObjects);
        for (UObject* Obj : OwnedObjects)
        {
            if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Obj))
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Removing stale ObjectRedirector: %s"), *Redirector->GetName());
                Redirector->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
                Redirector->MarkAsGarbage();
            }
        }
    }

    /* Clear every node from the ubergraph pages and function graphs so a re-import
     * rebuilds from scratch. The graph objects themselves are kept - GetOrCreateEventGraph
     * and GetOrCreateFunctionGraph reuse them by name. */
    TArray<UEdGraph*> AllGraphs;
    AllGraphs.Append(Blueprint->UbergraphPages);
    AllGraphs.Append(Blueprint->FunctionGraphs);

    int32 ClearedNodes = 0;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (!Graph) continue;

        Graph->Modify();
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            Node->BreakAllNodeLinks();
            Node->ConditionalBeginDestroy();
            ++ClearedNodes;
        }
        Graph->Nodes.Empty();
        Graph->SubGraphs.Empty();
    }

    /* Drop function graphs whose function no longer exists in the JSON, and any
     * graph that is not an editable user-function graph. Their nodes are cleared
     * above and rebuilding won't repopulate them, so an empty graph (or one whose
     * UFunction was removed) would otherwise linger and confuse the compiler.
     * Event thunks (InpActEvt_*), native overrides (Receive*) and the
     * construction/ubergraph functions have their bodies decompiled into the
     * EventGraph and their runtime functions generated by the K2 compiler from
     * the event nodes - an editable graph for them would either stay empty (stub
     * leftovers) or collide with the compiler's own stub generation. */
    for (int32 i = Blueprint->FunctionGraphs.Num() - 1; i >= 0; --i)
    {
        UEdGraph* Graph = Blueprint->FunctionGraphs[i];
        if (!Graph) continue;

        const FString GraphName = Graph->GetName();
        const ParsedFunction* Func = ParsedFunctions.Find(GraphName);
        if (!Func || Func->Kind != EFunctionKind::UserFunction)
        {
            Blueprint->FunctionGraphs.RemoveAt(i);
            Graph->MarkAsGarbage();
        }
    }

    EventGraph = nullptr;

    if (ClearedNodes > 0)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Cleared %d node(s) across %d graph(s) before rebuild"),
            ClearedNodes, AllGraphs.Num());
    }
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
        if (Name.Contains(TEXT("K2Node_InputKeyEvent")))
        {
            /* Plain key input (K2Node_InputKey editor node). The compiler stamps
             * "InpActEvt_<[Mods+]Key>_<IntermediateName>" in K2Node_InputKey.cpp
             * when expanding into the internal UK2Node_InputKeyEvent. */
            return EFunctionKind::InputKey;
        }
        if (Name.Contains(TEXT("K2Node_InputAxisEvent")))
        {
            return EFunctionKind::InputAxis;
        }
        return EFunctionKind::CustomEvent;
    }

    if (Name.StartsWith(TEXT("Receive")) && Func.Flags.Contains(TEXT("FUNC_Event")))
    {
        return EFunctionKind::NativeEvent;
    }

    // A real BlueprintEvent carries FUNC_Event in its FunctionFlags. Ordinary
    // user functions (whose bodies were compiled into the ubergraph frame) are
    // FUNC_BlueprintCallable | FUNC_BlueprintEvent WITHOUT FUNC_Event - they
    // must become UserFunction graphs, not event nodes.
    if (Func.Flags.Contains(TEXT("FUNC_Event")))
    {
        return EFunctionKind::CustomEvent;
    }

    return EFunctionKind::UserFunction;
}

bool FBlueprintBytecodeImporter::IsEventKind(EFunctionKind Kind) const
{
    return Kind == EFunctionKind::EnhancedInputAction
        || Kind == EFunctionKind::InputDebugKey
        || Kind == EFunctionKind::InputKey
        || Kind == EFunctionKind::LegacyInputAction
        || Kind == EFunctionKind::InputAxis
        || Kind == EFunctionKind::NativeEvent
        || Kind == EFunctionKind::CustomEvent;
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
    // Sort functions by EntryIndex so they're laid out left-to-right.
    // Exception: input thunks that have a DynamicBindingObjects entry are created
    // first, in binding-array order, so the auto-generated node names
    // (K2Node_EnhancedInputAction_0/_1 ...) match the original blueprint's
    // numbering instead of following bytecode label order.
    TArray<TPair<FString, ParsedFunction*>> SortedFuncs;
    for (auto& Pair : ParsedFunctions)
    {
        SortedFuncs.Add(TPair<FString, ParsedFunction*>(Pair.Key, &Pair.Value));
    }
    SortedFuncs.Sort([this](const TPair<FString, ParsedFunction*>& A, const TPair<FString, ParsedFunction*>& B)
    {
        auto SortKey = [this](const ParsedFunction* F)
        {
            const FInputBindingInfo* Binding = InputBindings.Find(F->Name);
            return (Binding && Binding->BindingOrder != INDEX_NONE)
                ? Binding->BindingOrder - 1000000
                : F->EntryIndex;
        };
        return SortKey(A.Value) < SortKey(B.Value);
    });

    const int32 FunctionSpacing = 2000; // Horizontal spacing between function groups

    /* Column assignment must be identical across both passes or event nodes
     * end up columns away from their own bodies: pass 2 also reserves columns
     * for non-event/non-UserFunction ubergraph-backed bodies, so the map below
     * is the single source of truth. */
    TMap<FName, int32> FunctionColumns;
    int32 NumExtraEventGraphColumns = 0;

    // First pass: create event nodes for event-like kinds, spaced horizontally
    int32 EventNodeIdx = 0;
    for (int32 Idx = 0; Idx < SortedFuncs.Num(); ++Idx)
    {
        const ParsedFunction& Func = *SortedFuncs[Idx].Value;
        if (IsEventKind(Func.Kind))
        {
            EventNodeY = 0; // Reset Y for each function group
            FunctionColumns.Add(FName(*Func.Name), EventNodeIdx * FunctionSpacing);
            UEdGraphNode* EventNode = CreateEventNode(Func, EventNodeIdx * FunctionSpacing);
            if (EventNode)
            {
                EventNodes.Add(Func.Name, EventNode);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Created event node for: %s"), *Func.Name);
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Failed to create event node for: %s"), *Func.Name);
            }
            ++EventNodeIdx;
        }
    }

    /* Ubergraph territory maps (plan 009): raw EntryIndex of every event-kind
     * function marks where its section begins in the shared ubergraph bytecode.
     * BuildLinearPath stops at a foreign section instead of absorbing it into
     * the walking event's body (the Label_6006-style leakage). */
    UbergraphEventEntrySis.Reset();
    ClaimedSiOwners.Reset();
    UbergraphPopTargets.Reset();
    for (const TPair<FString, ParsedFunction*>& Pair : SortedFuncs)
    {
        const ParsedFunction& EntryFunc = *Pair.Value;
        if (IsEventKind(EntryFunc.Kind) && EntryFunc.EntryIndex != INDEX_NONE)
        {
            UbergraphEventEntrySis.Add(EntryFunc.EntryIndex);
        }
    }

    // Second pass: decompile function bodies. Event bodies share the EventGraph
    // so they're spaced apart horizontally, counting only events. User functions
    // get their own graph, so each starts from a fresh local offset instead of
    // inheriting a position derived from the shared EventGraph indexing.
    for (const TPair<FString, ParsedFunction*>& Pair : SortedFuncs)
    {
        const ParsedFunction& Func = *Pair.Value;
        int32 FuncOffsetX = 0;
        if (const int32* ColumnX = FunctionColumns.Find(FName(*Func.Name)))
        {
            FuncOffsetX = *ColumnX;
        }
        else if (!IsEventKind(Func.Kind) && Func.Kind != EFunctionKind::UserFunction)
        {
            /* Non-event, non-user-function bodies (ubergraph with no events) also
             * live in the shared EventGraph - keep them on their own column,
             * continuing after every reserved event column. */
            FuncOffsetX = FunctionColumns.Num() > 0 ? (EventNodeIdx + NumExtraEventGraphColumns) * FunctionSpacing : 0;
            FunctionColumns.Add(FName(*Func.Name), FuncOffsetX);
            ++NumExtraEventGraphColumns;
        }
        BuildFunctionGraph(Func, FuncOffsetX);
    }
    return true;
}

int32 FBlueprintBytecodeImporter::NextSequentialStatementIndex(const ParsedFunction& Ubergraph, int32 StmtIdx) const
{
    for (int32 Idx = 0; Idx < Ubergraph.BytecodeTokens.Num(); ++Idx)
    {
        if (Ubergraph.BytecodeTokens[Idx].StatementIndex == StmtIdx)
        {
            if (Idx + 1 < Ubergraph.BytecodeTokens.Num())
            {
                return Ubergraph.BytecodeTokens[Idx + 1].StatementIndex;
            }
            break;
        }
    }
    return -1;
}

bool FBlueprintBytecodeImporter::BuildLinearPath(const ParsedFunction& Ubergraph, int32 StartIdx,
    TArray<const FBytecodeToken*>& OutPath, TSet<int32>& OutIndices)
{
    OutPath.Reset();
    OutIndices.Reset();

    // Find the function-entry push (return address). Event bodies pop this
    // address at their terminating EX_PopExecutionFlow, which is how the path
    // knows to stop.
    int32 EntryPush = -1;
    for (const FBytecodeToken& Tok : Ubergraph.BytecodeTokens)
    {
        if (Tok.Token == TEXT("EX_PushExecutionFlow") && Tok.JsonData.IsValid()
            && Tok.JsonData->HasField(TEXT("PushingAddress")))
        {
            EntryPush = Tok.JsonData->GetIntegerField(TEXT("PushingAddress"));
            break;
        }
    }

    // Worklist of linear regions. The first is the event body start; every
    // EX_JumpIfNot CodeOffset target (e.g. a switch-on-enum case body) becomes
    // another region to walk after the current one, so case bodies end up in
    // the emitted path and can receive the branch's else wire.
    TArray<int32> RegionQueue;
    TSet<int32> RegionQueued;
    RegionQueue.Add(StartIdx);
    RegionQueued.Add(StartIdx);

    int32 RegionSafety = 0;
    while (RegionQueue.Num() > 0 && RegionSafety++ < 500)
    {
        int32 CurIdx = RegionQueue[0];
        RegionQueue.RemoveAt(0);

        // Each region is entered through its own flow: seed the stack with the
        // entry push so the region's terminating PopExecutionFlow pops it.
        TArray<int32> FlowStack;
        if (EntryPush >= 0)
        {
            FlowStack.Add(EntryPush);
        }

        int32 Safety = 0;
        while (CurIdx >= 0 && Safety++ < 2000)
        {
            // Locate the token with this statement index (indices are ascending)
            const FBytecodeToken* Tok = nullptr;
            for (const FBytecodeToken& T : Ubergraph.BytecodeTokens)
            {
                if (T.StatementIndex == CurIdx)
                {
                    Tok = &T;
                    break;
                }
                if (T.StatementIndex > CurIdx)
                {
                    break;
                }
            }
            if (!Tok)
            {
                break;
            }

            /* Territory guard (plan 009): another event's section begins here.
             * Fall-through, a jump, or a popped return address landing on it
             * belongs to that event's body, not to the one being walked - stop
             * instead of absorbing foreign statements. */
            if (CurIdx != StartIdx && UbergraphEventEntrySis.Contains(CurIdx))
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning,
                    TEXT("linear path [%s]: stopping at other event's entry si=%d"),
                    *ActiveUbergraphOwnerName, CurIdx);
                break;
            }

            if (OutIndices.Contains(CurIdx))
            {
                // Loop back edge or a region already collected; stop here.
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  linear path: loop back edge at si=%d (-> %d)"), CurIdx, CurIdx);
                break;
            }
            OutIndices.Add(CurIdx);
            OutPath.Add(Tok);

            /* First-builder claims (diagnostic-only this round): when a later
             * walk reaches an index an earlier function already collected, log
             * the overlap so section ownership can be confirmed from the log. */
            if (const FString* ExistingOwner = ClaimedSiOwners.Find(CurIdx))
            {
                if (*ExistingOwner != ActiveUbergraphOwnerName)
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Log,
                        TEXT("linear path [%s]: si=%d already claimed by [%s] (cross-event overlap)"),
                        *ActiveUbergraphOwnerName, CurIdx, **ExistingOwner);
                }
            }
            else
            {
                ClaimedSiOwners.Add(CurIdx, ActiveUbergraphOwnerName);
            }

            const FString& TokenName = Tok->Token;
            if (TokenName == TEXT("EX_Return") || TokenName == TEXT("EX_EndOfScript") || TokenName == TEXT("EX_Nothing")
                || TokenName == TEXT("EX_ComputedJump"))
            {
                break;
            }

            const TSharedPtr<FJsonObject>& Json = Tok->JsonData;
            int32 NextIdx = -1;

            if (TokenName == TEXT("EX_Jump") && Json.IsValid() && Json->HasField(TEXT("CodeOffset")))
            {
                NextIdx = Json->GetIntegerField(TEXT("CodeOffset"));
            }
            else if (TokenName == TEXT("EX_JumpIfNot") && Json.IsValid() && Json->HasField(TEXT("CodeOffset")))
            {
                // The else branch jumps to a separate region (e.g. a switch-on-enum
                // case body). Enqueue it to walk after this region; the branch's
                // else pin is wired to the target's anchor during emission (deferred
                // if the target is emitted later in the path).
                const int32 Target = Json->GetIntegerField(TEXT("CodeOffset"));
                if (!OutIndices.Contains(Target) && !RegionQueued.Contains(Target))
                {
                    RegionQueue.Add(Target);
                    RegionQueued.Add(Target);
                }
                // Then branch falls through to the next statement
                NextIdx = NextSequentialStatementIndex(Ubergraph, CurIdx);
            }
            else if (TokenName == TEXT("EX_PushExecutionFlow") && Json.IsValid() && Json->HasField(TEXT("PushingAddress")))
            {
                FlowStack.Add(Json->GetIntegerField(TEXT("PushingAddress")));
                NextIdx = NextSequentialStatementIndex(Ubergraph, CurIdx);
            }
            else if (TokenName == TEXT("EX_PopExecutionFlow"))
            {
                if (FlowStack.Num() > 1)
                {
                    NextIdx = FlowStack.Pop();
                    /* Record the reconstructed target for the emit side - the
                     * JSON's pop opcodes carry no address. */
                    UbergraphPopTargets.Add(CurIdx, NextIdx);
                }
                else
                {
                    /* Function-terminal pop: the only stack entry is the seeded
                     * ubergraph invocation push, whose resume is the shared
                     * Return epilogue - not this function's body. Stop instead
                     * of resuming. (08.25: resuming chased cross-region gotos
                     * into the Change Garment section - foreign Work Uniform /
                     * Change Garment calls and a stray increment leaked into
                     * Call for NPC interaction, and the loop's Completed chained
                     * to the shared Return instead of its real continuation.) */
                    break;
                }
            }
            else if (TokenName == TEXT("EX_PopExecutionFlowIfNot"))
            {
                /* Conditional pop: only pops when the condition is false. The
                 * linear path follows the true (continue) branch, so the stack
                 * is untouched - but record where FALSE would land so
                 * EmitPopExecutionFlowIfNot can wire its else pin. */
                if (FlowStack.Num() > 0)
                {
                    UbergraphPopTargets.Add(CurIdx, FlowStack.Last());
                }
                NextIdx = NextSequentialStatementIndex(Ubergraph, CurIdx);
            }
            else
            {
                // Everything else falls through to the next statement
                NextIdx = NextSequentialStatementIndex(Ubergraph, CurIdx);
            }

            CurIdx = NextIdx;
        }
    }

    return OutPath.Num() > 0;
}

bool FBlueprintBytecodeImporter::BuildFunctionGraph(const ParsedFunction& Func, int32 FuncOffsetX)
{
    /* Linear walks attribute their collected statement indices to this function
     * in ClaimedSiOwners (cross-event overlap diagnostics, plan 009). */
    ActiveUbergraphOwnerName = Func.Name;

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
        const ParsedFunction* Ubergraph = nullptr;
        const FString UbergraphName = FString::Printf(TEXT("ExecuteUbergraph_%s"), *Blueprint->GetName());
        if (const ParsedFunction* Found = ParsedFunctions.Find(UbergraphName))
        {
            Ubergraph = Found;
        }
        else
        {
            // Fallback: search any function whose name starts with the prefix
            for (const auto& Pair : ParsedFunctions)
            {
                if (Pair.Key.StartsWith(TEXT("ExecuteUbergraph_")))
                {
                    Ubergraph = &Pair.Value;
                    break;
                }
            }
        }
        if (!Ubergraph)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No ubergraph found for event: %s"), *Func.Name);
            return false;
        }

        // The event's entry index can be a dispatch trampoline: a bare
        // EX_Jump to the section where the body was actually placed. Follow it.
        int32 BodyStart = Func.EntryIndex;
        for (const FBytecodeToken& Token : Ubergraph->BytecodeTokens)
        {
            if (Token.StatementIndex == Func.EntryIndex)
            {
                if (Token.Token == TEXT("EX_Jump") && Token.JsonData.IsValid()
                    && Token.JsonData->HasField(TEXT("CodeOffset")))
                {
                    BodyStart = Token.JsonData->GetIntegerField(TEXT("CodeOffset"));
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Event %s: followed trampoline %d -> %d"), *Func.Name, Func.EntryIndex, BodyStart);
                }
                break;
            }
        }

        // Follow the linear exec path from the body start. Unlike a simple
        // statement-index range, this follows jumps into shared regions (e.g. a
        // while loop body placed elsewhere in the ubergraph) and stops at loop
        // back-edges, body-terminating pops (tracked via the execution-flow
        // stack) and the final return.
        TArray<const FBytecodeToken*> EventStmts;
        TSet<int32> EventStmtIndices;
        if (!BuildLinearPath(*Ubergraph, BodyStart, EventStmts, EventStmtIndices))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No body statements for event: %s at offset %d"), *Func.Name, Func.EntryIndex);
            return false;
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Event %s: linear path from %d, collected %d statements:"), *Func.Name, BodyStart, EventStmts.Num());
        for (const FBytecodeToken* Tok : EventStmts)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  si=%d token=%s"), Tok->StatementIndex, *Tok->Token);
        }

        // Build the body using the event node as entry
        FFunctionBuilder Builder;
        Builder.Graph = EventGraph;
        Builder.Func = &Func;
        /* Statements come from the ubergraph case body (BuildLinearPath below);
         * keep a pointer to it so loop detection scans real tokens instead of
         * this 3-statement thunk. */
        Builder.TokenSource = Ubergraph;
        Builder.EntryNode = *FoundNode;
        Builder.ResultNode = nullptr;
        Builder.NextNodeX = FuncOffsetX;
        Builder.NextNodeY = 0;

        // Register event output pins as producers so the bytecode's reads of
        // event-parameter locals (K2Node_EnhancedInputActionEvent_ActionValue,
        // _SourceAction, _ElapsedTime, _TriggeredTime) resolve directly to
        // the event node's output pins instead of creating unnecessary SET
        // nodes for compiler temps. The bytecode uses mangled local names
        // like "K2Node_EnhancedInputActionEvent_ActionValue[_N]" where _N is
        // the event index suffix.
        if (Func.Kind == EFunctionKind::EnhancedInputAction
            || Func.Kind == EFunctionKind::InputDebugKey
            || Func.Kind == EFunctionKind::LegacyInputAction)
        {
            // Extract event index suffix from function name (e.g. "..._0" or "..._1")
            int32 EventIdx = 0;
            {
                const FString& FName = Func.Name;
                const int32 LastUnderscore = FName.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (LastUnderscore != INDEX_NONE)
                {
                    EventIdx = FCString::Atoi(*FName.Mid(LastUnderscore + 1));
                }
            }
            const FString IdxSuffix = (EventIdx > 0) ? FString::Printf(TEXT("_%d"), EventIdx) : FString();

            // Map bytecode local names → event node pin names
            struct { const TCHAR* BytecodeName; const TCHAR* PinName; } PinMap[] = {
                { TEXT("ActionValue"),   TEXT("ActionValue") },
                { TEXT("ElapsedTime"),   TEXT("ElapsedSeconds") },
                { TEXT("TriggeredTime"), TEXT("TriggeredSeconds") },
                { TEXT("SourceAction"),  TEXT("InputAction") },
            };
            for (auto& Entry : PinMap)
            {
                const FString LocalName = FString::Printf(TEXT("K2Node_EnhancedInputActionEvent_%s%s"), Entry.BytecodeName, *IdxSuffix);
                UEdGraphPin* Pin = FindPin(Builder.EntryNode, Entry.PinName, EGPD_Output);
                if (Pin)
                {
                    Builder.ProducerPins.Add(LocalName, Pin);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Registered event output producer %s (pin %s)"),
                        *LocalName, *Pin->PinName.ToString());
                }
            }
        }

        // Build statement index map
        for (int32 Idx = 0; Idx < EventStmts.Num(); ++Idx)
        {
            const FBytecodeToken* Stmt = EventStmts[Idx];
            Builder.Statements.Add(Stmt);
            Builder.StmtIndexToArrayPos.Add(Stmt->StatementIndex, Builder.Statements.Num() - 1);
        }

        // Mark switch case body starts (EX_JumpIfNot CodeOffset targets that are
        // in this path) so the emit loop doesn't chain them off the previous
        // statement's then pin; they are entered through their own exec wire.
        for (const FBytecodeToken* Stmt : Builder.Statements)
        {
            if (Stmt->Token == TEXT("EX_JumpIfNot") && Stmt->JsonData.IsValid() && Stmt->JsonData->HasField(TEXT("CodeOffset")))
            {
                const int32 Target = Stmt->JsonData->GetIntegerField(TEXT("CodeOffset"));
                if (Builder.StmtIndexToArrayPos.Contains(Target))
                {
                    Builder.RegionStartIndices.Add(Target);
                }
            }
        }

        // Determine the correct exec output pin name
        UEdGraphPin* EventExecPin = nullptr;
        if (Func.Kind == EFunctionKind::EnhancedInputAction)
        {
            const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
            if (Binding && !Binding->TriggerEvent.IsEmpty())
            {
                /* The export carries the enum with its C++ scope ("ETriggerEvent::Started")
                 * but UK2Node_EnhancedInputAction names its exec pins after the bare
                 * enumerator ("Started", "Completed", ...). Strip the scope before lookup. */
                FString TriggerEvent = Binding->TriggerEvent;
                const int32 ScopeIdx = TriggerEvent.Find(TEXT("::"));
                if (ScopeIdx != INDEX_NONE)
                {
                    TriggerEvent.RightChopInline(ScopeIdx + 2);
                }
                EventExecPin = FindPin(Builder.EntryNode, *TriggerEvent, EGPD_Output);
            }
        }
        if (!EventExecPin && Func.Kind == EFunctionKind::InputDebugKey)
        {
            /* The native K2Node_InputDebugKey has "Pressed" / "Released" exec output
             * pins (not "then"). Match the pin to the EInputEvent from the binding. */
            const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
            if (Binding && !Binding->InputKeyEvent.IsEmpty())
            {
                FString InputEvent = Binding->InputKeyEvent;
                const int32 ScopeIdx = InputEvent.Find(TEXT("::"));
                if (ScopeIdx != INDEX_NONE)
                {
                    InputEvent.RightChopInline(ScopeIdx + 2);
                }
                if (InputEvent == TEXT("IE_Pressed"))
                {
                    EventExecPin = FindPin(Builder.EntryNode, TEXT("Pressed"), EGPD_Output);
                }
                else if (InputEvent == TEXT("IE_Released"))
                {
                    EventExecPin = FindPin(Builder.EntryNode, TEXT("Released"), EGPD_Output);
                }
            }
            // Fallback: try "Pressed" if binding lookup failed
            if (!EventExecPin)
            {
                EventExecPin = FindPin(Builder.EntryNode, TEXT("Pressed"), EGPD_Output);
            }
        }
        if (!EventExecPin && Func.Kind == EFunctionKind::InputKey)
        {
            /* UK2Node_InputKey has the same "Pressed"/"Released" exec outputs
             * as InputDebugKey - pick the one the binding's EInputEvent names. */
            const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
            FString InputEvent = Binding ? Binding->InputKeyEvent : TEXT("");
            if (!InputEvent.IsEmpty())
            {
                const int32 ScopeIdx = InputEvent.Find(TEXT("::"));
                if (ScopeIdx != INDEX_NONE)
                {
                    InputEvent.RightChopInline(ScopeIdx + 2);
                }
                if (InputEvent == TEXT("IE_Released"))
                {
                    EventExecPin = FindPin(Builder.EntryNode, TEXT("Released"), EGPD_Output);
                }
            }
            // Fallback: IE_Pressed (or unknown) -> "Pressed"
            if (!EventExecPin)
            {
                EventExecPin = FindPin(Builder.EntryNode, TEXT("Pressed"), EGPD_Output);
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

        /* CustomEvent thunks declare their parameter->frame-slot bindings via
         * EX_LetValueOnPersistentFrame assignments (plan 011 follow-up B) -
         * bind each declared slot to this event node's matching output pin.
         * Exact names only; no sanitized/counter guessing. */
        if (const TArray<TPair<FString, FString>>* Bindings = ThunkParamBindings.Find(Func.Name))
        {
            for (const TPair<FString, FString>& B : *Bindings)
            {
                UEdGraphPin* ParamPin = FindPin(Builder.EntryNode, *B.Value, EGPD_Output);
                if (ParamPin)
                {
                    RegisterProducer(Builder, B.Key, ParamPin);
                }
                else
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Warning,
                        TEXT("Thunk binding %s: entry node has no output pin '%s'"),
                        *B.Key, *B.Value);
                }
            }
        }

        // Reconstruct StandardMacro loops on this decompiled event body before
        // the flat walk (plan 006) - BeginPlay/Tick flow through this path.
        DetectMacroLoops(Builder);

        /* Events have no function-local frame to declare (and their UFunctions
         * are inherited engine hooks - unsafe to mutate). Register ONLY the
         * detected loops' scaffold temps so their body reads emit resolvable
         * VariableGets that EmitMacroLoopNodes repoints onto the instance
         * outputs and removes; every other compiler temp keeps resolving to
         * pin defaults exactly as before. */
        for (const FDetectedLoop& Loop : Builder.DetectedLoops)
        {
            const FString ScaffoldLocals[] = { Loop.CounterLocal, Loop.IndexLocal, Loop.BreakFlagLocal };
            for (const FString& LocalName : ScaffoldLocals)
            {
                if (!LocalName.IsEmpty() && !Builder.FunctionLocalNames.Contains(LocalName))
                {
                    Builder.FunctionLocalNames.Add(LocalName);
                    Builder.FunctionLocalGuids.Add(LocalName, FGuid::NewGuid());
                }
            }
        }

        // Process each statement
        bool bPrevWasSuppressed = false;
        for (int32 Idx = 0; Idx < Builder.Statements.Num(); ++Idx)
        {
            const FBytecodeToken* Stmt = Builder.Statements[Idx];

            if (Stmt->Token == TEXT("EX_EndOfScript") || Stmt->Token == TEXT("EX_Nothing"))
            {
                continue;
            }

            // Skip macro-loop scaffold (pushes/pops/gate math/back-edge)
            if (Builder.LoopSuppressedSis.Contains(Stmt->StatementIndex))
            {
                bPrevWasSuppressed = true;
                continue;
            }

            // A statement that begins a jump-target region (e.g. a switch case
            // body) is entered through its own incoming exec wire, not chained
            // off the previous statement's then pin. Only drop the chain when
            // the previous statement actually transferred control (jump /
            // return / loop back-edge). A region start that is the
            // straight-line fall-through of the previous statement must keep
            // its chain or the fall-through exec is silently dropped.
            if (Builder.RegionStartIndices.Contains(Stmt->StatementIndex))
            {
                const bool bPrevTransferredControl = Idx > 0 && (
                    Builder.Statements[Idx - 1]->Token == TEXT("EX_Jump")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_Return")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_PopExecutionFlow")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_ComputedJump")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_SwitchValue")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_EndOfScript")
                    || Builder.Statements[Idx - 1]->Token == TEXT("EX_Nothing"));
                if (bPrevTransferredControl || bPrevWasSuppressed)
                {
                    Builder.LastExecPin = nullptr;
                }
            }
            else if (bPrevWasSuppressed)
            {
                // Never chain straight across a suppressed scaffold run - the
                // macro instance (or a deferred wire) owns that connection.
                // EXCEPTION: the first real statement of a detected loop body
                // is the splice point; the pre-loop chain must reach it so the
                // emitter can move that wire onto Macro Exec (plan 006).
                if (!Builder.LoopChainBridgeSis.Contains(Stmt->StatementIndex))
                {
                    Builder.LastExecPin = nullptr;
                }
            }
            bPrevWasSuppressed = false;

            UEdGraphNode* Anchor = EmitStatement(Builder, *Stmt);
            if (Anchor)
            {
                Builder.StatementAnchors.Add(Stmt->StatementIndex, Anchor);
                Builder.VisitedAnchors.Add(Stmt->StatementIndex);
            }
        }

        // Wire any deferred exec jumps (forward jump targets emitted later in
        // the path, e.g. switch case bodies) now that all anchors exist.
        ResolvePendingExecWires(Builder);

        // Splice reconstructed StandardMacro loop instances into the graph
        EmitMacroLoopNodes(Builder);

        ResolvePendingDataWires(Builder);
        LogWiringAudit(Builder, Func.Name);

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Decompiled event body: %s (%d statements, pin=%s)"),
            *Func.Name, EventStmts.Num(),
            EventExecPin ? *EventExecPin->PinName.ToString() : TEXT("NONE"));
        return true;
    }

    // The ubergraph is the shared body store for every event thunk. Its content
    // is decompiled per-event (BuildLinearPath from each event's EntryIndex), so
    // emitting it here as a flat chain would duplicate every event body and chain
    // them all from a single ExecuteUbergraph entry, corrupting the graph.
    if (Func.Kind == EFunctionKind::Ubergraph)
    {
        bool bHasEvents = false;
        for (const auto& Pair : ParsedFunctions)
        {
            if (IsEventKind(Pair.Value.Kind))
            {
                bHasEvents = true;
                break;
            }
        }
        if (bHasEvents)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Skipping ubergraph flat emission (%s): %d event bodies consume it"), *Func.Name, EventNodes.Num());
            return true;
        }
    }

    // The UserConstructionScript lives in its own FunctionGraph, not in the
    // EventGraph. Decompiling it into the EventGraph creates a stray
    // UK2Node_FunctionEntry that collides with the K2 compiler's own
    // ExecuteUbergraph entry node ("Expected only one function entry node"),
    // which cascades into 40+ "Temp variable could not be found" errors.
    if (Func.Kind == EFunctionKind::ConstructionScript)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Skipping construction script (%s): handled natively by the engine"), *Func.Name);
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
    Builder.NextNodeX = FuncOffsetX;
    Builder.NextNodeY = 0;

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

    /* A user function whose body was compiled into the ubergraph frame (its
     * EntryIndex points at a trampoline in ExecuteUbergraph_*) is decompiled from
     * the ubergraph into its own function graph, exactly like event bodies. Its
     * own bytecode is only the thunk that jumps into the ubergraph. */
    if (Func.Kind == EFunctionKind::UserFunction && Func.EntryIndex != INDEX_NONE)
    {
        const ParsedFunction* Ubergraph = FindUbergraph();
        if (!Ubergraph)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No ubergraph found for ubergraph-backed function: %s"), *Func.Name);
            return false;
        }

        int32 BodyStart = Func.EntryIndex;
        for (const FBytecodeToken& Token : Ubergraph->BytecodeTokens)
        {
            if (Token.StatementIndex == Func.EntryIndex)
            {
                if (Token.Token == TEXT("EX_Jump") && Token.JsonData.IsValid()
                    && Token.JsonData->HasField(TEXT("CodeOffset")))
                {
                    BodyStart = Token.JsonData->GetIntegerField(TEXT("CodeOffset"));
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Function %s: followed trampoline %d -> %d"), *Func.Name, Func.EntryIndex, BodyStart);
                }
                break;
            }
        }

        TArray<const FBytecodeToken*> BodyStmts;
        TSet<int32> BodyStmtIndices;
        if (!BuildLinearPath(*Ubergraph, BodyStart, BodyStmts, BodyStmtIndices))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("No body statements for function: %s at offset %d"), *Func.Name, Func.EntryIndex);
            return false;
        }

        Builder.Statements.Reserve(BodyStmts.Num());
        for (const FBytecodeToken* Stmt : BodyStmts)
        {
            Builder.Statements.Add(Stmt);
            Builder.StmtIndexToArrayPos.Add(Stmt->StatementIndex, Builder.Statements.Num() - 1);
        }
        /* Body lives in the ubergraph (Func itself is only the trampoline thunk) */
        Builder.TokenSource = Ubergraph;

        /* Declare the frame locals this body uses as real function-local variables
         * (FProperty on the function UFunction + FBPVariableDescription on the
         * entry node) BEFORE pins allocate, so variable get/set nodes resolve
         * through SetLocalMember. */
        CreateFunctionLocalVariables(Builder, *Ubergraph, BodyStmts);

        /* FunctionEntry parameter outputs feed body reads through EXACT
         * thunk-declared frame-slot bindings (plan 011 follow-up B) - no
         * sanitized/counter guessing. */
        if (const TArray<TPair<FString, FString>>* Bindings = ThunkParamBindings.Find(Func.Name))
        {
            for (const TPair<FString, FString>& B : *Bindings)
            {
                UEdGraphPin* ParamPin = FindPin(Builder.EntryNode, *B.Value, EGPD_Output);
                if (ParamPin)
                {
                    RegisterProducer(Builder, B.Key, ParamPin);
                }
                else
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Warning,
                        TEXT("Thunk binding %s: entry node has no output pin '%s'"),
                        *B.Key, *B.Value);
                }
            }
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Function %s: linear path from %d, collected %d statements"),
            *Func.Name, BodyStart, BodyStmts.Num());
    }
    else
    {
        // Build statement list
        Builder.Statements.Reserve(Func.BytecodeTokens.Num());
        for (const FBytecodeToken& Token : Func.BytecodeTokens)
        {
            Builder.Statements.Add(&Token);
            Builder.StmtIndexToArrayPos.Add(Token.StatementIndex, Builder.Statements.Num() - 1);
        }

        /* Standalone functions (own bytecode - e.g. function-library functions)
         * do not share the ubergraph frame, so declare their locals from their
         * own ChildProperties. Without this, function-local variables such as
         * BPL_DollSystem's 'As GI Data' never appear and their VariableGet nodes
         * cannot resolve. */
        TArray<const FBytecodeToken*> FuncStmts;
        FuncStmts.Reserve(Func.BytecodeTokens.Num());
        for (const FBytecodeToken& Token : Func.BytecodeTokens)
        {
            FuncStmts.Add(&Token);
        }
        CreateFunctionLocalVariables(Builder, Func, FuncStmts);
    }

    if (Builder.Statements.Num() == 0)
    {
        return false;
    }

    // Mark switch case body starts (EX_JumpIfNot CodeOffset targets that are in
    // this path) so the emit loop doesn't chain them off the previous statement.
    for (const FBytecodeToken* Stmt : Builder.Statements)
    {
        if (Stmt->Token == TEXT("EX_JumpIfNot") && Stmt->JsonData.IsValid() && Stmt->JsonData->HasField(TEXT("CodeOffset")))
        {
            const int32 Target = Stmt->JsonData->GetIntegerField(TEXT("CodeOffset"));
            if (Builder.StmtIndexToArrayPos.Contains(Target))
            {
                Builder.RegionStartIndices.Add(Target);
            }
        }
    }

    // Wire entry exec to first statement
    UEdGraphPin* EntryExecPin = FindPin(Builder.EntryNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (!EntryExecPin)
    {
        EntryExecPin = FindPin(Builder.EntryNode, TEXT("then"), EGPD_Output);
    }
    Builder.LastExecPin = EntryExecPin;

    // Reconstruct StandardMacro loops before the flat walk so their scaffold
    // statements can be skipped (plan 006). The K2Node_MacroInstance spliced
    // in after the walk owns that control flow instead.
    DetectMacroLoops(Builder);

    // Process each statement in order
    bool bPrevWasSuppressed = false;
    for (int32 Idx = 0; Idx < Builder.Statements.Num(); ++Idx)
    {
        const FBytecodeToken* Stmt = Builder.Statements[Idx];

        // Skip EndOfScript and Nothing
        if (Stmt->Token == TEXT("EX_EndOfScript") || Stmt->Token == TEXT("EX_Nothing"))
        {
            continue;
        }

        // Skip macro-loop scaffold (pushes/pops/gate math/back-edge)
        if (Builder.LoopSuppressedSis.Contains(Stmt->StatementIndex))
        {
            bPrevWasSuppressed = true;
            continue;
        }

        // A statement that begins a jump-target region (e.g. a switch case
        // body) is entered through its own incoming exec wire, not chained
        // off the previous statement's then pin. Only drop the chain when the
        // previous statement actually transferred control (jump / return / loop
        // back-edge). A region start that is the straight-line fall-through of
        // the previous statement (e.g. the PrintString label reached after
        // CreateSaveGameObject, or an EX_Return after the last call) must keep
        // its chain or the fall-through exec is silently dropped.
        if (Builder.RegionStartIndices.Contains(Stmt->StatementIndex))
        {
            const bool bPrevTransferredControl = Idx > 0 && (
                Builder.Statements[Idx - 1]->Token == TEXT("EX_Jump")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_Return")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_PopExecutionFlow")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_ComputedJump")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_SwitchValue")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_EndOfScript")
                || Builder.Statements[Idx - 1]->Token == TEXT("EX_Nothing"));
            if (bPrevTransferredControl || bPrevWasSuppressed)
            {
                // EXCEPTION: a loop body head (chain bridge) must keep the
                // pre-loop exec chain so EmitMacroLoopNodes can splice it
                // onto Macro Exec (plan 006).
                if (!Builder.LoopChainBridgeSis.Contains(Stmt->StatementIndex))
                {
                    Builder.LastExecPin = nullptr;
                }
            }
        }
        else if (bPrevWasSuppressed)
        {
            // Never chain straight across a suppressed scaffold run - the
            // macro instance (or a deferred wire) owns that connection.
            // EXCEPTION: the first real statement of a detected loop body
            // is the splice point; the pre-loop chain must reach it so the
            // emitter can move that wire onto Macro Exec (plan 006).
            if (!Builder.LoopChainBridgeSis.Contains(Stmt->StatementIndex))
            {
                Builder.LastExecPin = nullptr;
            }
        }
        bPrevWasSuppressed = false;

        UEdGraphNode* Anchor = EmitStatement(Builder, *Stmt);
        if (Anchor)
        {
            Builder.StatementAnchors.Add(Stmt->StatementIndex, Anchor);
            Builder.VisitedAnchors.Add(Stmt->StatementIndex);
        }
    }

    // Wire any deferred exec jumps (forward jump targets emitted later in the
    // path, e.g. switch case bodies) now that all anchors exist.
    ResolvePendingExecWires(Builder);

    // Splice reconstructed StandardMacro loop instances into the graph
    // (exec/Completed/Break rewiring + Element/Index consumer repointing).
    EmitMacroLoopNodes(Builder);

    ResolvePendingDataWires(Builder);
    LogWiringAudit(Builder, Func.Name);

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

    // Link to the UFunction so pins can be created from its signature.
    // The scaffold UFunction is registered on the SkeletonGeneratedClass, so
    // setting the member reference BEFORE AllocateDefaultPins would let the
    // engine's UK2Node_FunctionEntry::AllocateDefaultPins resolve it and also
    // create the parameter pins via CreatePinsForFunctionEntryExit - and the
    // CreateUserDefinedPinsForFunctionEntryExit call below would then duplicate
    // them. Match the engine's own path for user-defined functions
    // (UEdGraphSchema_K2::CreateFunctionGraphTerminators with a UFunction*):
    // AllocateDefaultPins must run with an unresolved reference so it only
    // creates the exec pin; set the member ref afterwards, then create the
    // user-defined pins from the UFunction signature directly.
    UFunction* FuncObj = GeneratedClass->FindFunctionByName(FName(*Func.Name));
    if (FuncObj)
    {
        EntryNode->bIsEditable = true;
    }

    // The engine's user-created function flow (FBlueprintEditorUtils::CreateFunctionGraph)
    // calls AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable|FUNC_BlueprintEvent|FUNC_Public)
    // on the entry node. The skeleton class derives its function flags from
    // UK2Node_FunctionEntry::GetFunctionFlags() == ResolveMember()->FunctionFlags | ExtraFlags.
    // During skeleton generation the FunctionReference cannot resolve (null member parent),
    // so without ExtraFlags the imported functions end up with zero flags and are NOT
    // Blueprint Callable - they vanish from the palette / My Blueprint / drag-drop.
    EFunctionFlags EntryFlags = ParseFunctionFlags(Func.Flags);
    if (EntryFlags == (EFunctionFlags)0)
    {
        EntryFlags = (EFunctionFlags)(FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    }
    EntryNode->AddExtraFlags(EntryFlags);

    Graph->AddNode(EntryNode, true, true);
    EntryNode->AllocateDefaultPins();

    if (FuncObj)
    {
        // Set the member reference only now, after AllocateDefaultPins created
        // just the exec pin - the reference lets the skeleton derive the function
        // flags (GetFunctionFlags) and the world-context checks run correctly.
        EntryNode->FunctionReference.SetExternalMember(FuncObj->GetFName(), nullptr);

        // Create the function's input pins (as outputs on the entry node)
        // directly from the UFunction's properties - no class resolution needed.
        // CreateUserDefinedPinsForFunctionEntryExit routes through
        // CreateUniquePinName, which suffixes a pin whenever its name collides
        // with a UFunction property - and since these pins ARE the function's
        // params, every param pin would come out "LoadingSource1" / "Save Data1",
        // breaking ResolveExpression's exact-name lookup against the entry node.
        // Create them with bUseUniqueName=false so pin names equal the param names.
        const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
        for (TFieldIterator<FProperty> PropIt(FuncObj); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
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
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateEntryNode: %s - user-defined pins created from UFunction"), *Func.Name);
    }
    else
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreateEntryNode: UFunction not found for %s"), *Func.Name);
    }

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

    UFunction* FuncObj = GeneratedClass->FindFunctionByName(FName(*Func.Name));
    if (FuncObj)
    {
        // Mirror the engine: member ref with no parent class, pins made from
        // the UFunction signature directly (works regardless of class hierarchy).
        ResultNode->FunctionReference.SetExternalMember(FuncObj->GetFName(), nullptr);
        ResultNode->bIsEditable = true;
    }

    Graph->AddNode(ResultNode, true, true);
    ResultNode->AllocateDefaultPins();

    if (FuncObj)
    {
        // Create the function's out-param pins (as inputs on the result node)
        // directly from the UFunction's properties. Same bUseUniqueName=false
        // treatment as the entry node so pins keep the exact parameter names.
        const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
        for (TFieldIterator<FProperty> PropIt(FuncObj); PropIt && (PropIt->PropertyFlags & CPF_Parm); ++PropIt)
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
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateResultNode: %s - user-defined pins created from UFunction"), *Func.Name);
    }
    else
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreateResultNode: UFunction not found for %s"), *Func.Name);
    }

    return ResultNode;
}

// ============================================================================
// Node positioning
// ============================================================================

void FBlueprintBytecodeImporter::PositionNode(UEdGraphNode* Node, FFunctionBuilder& Builder)
{
    if (!Node) return;
    Node->NodePosX = Builder.NextNodeX;
    Node->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120; // Vertical spacing between nodes
}

// ============================================================================
// Event node creation
// ============================================================================

UEdGraphNode* FBlueprintBytecodeImporter::CreateEventNode(const ParsedFunction& Func, int32 OffsetX)
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
        ActionNode->NodePosX = OffsetX;
        ActionNode->NodePosY = EventNodeY;
        EventNodeY += 120;

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
        /* Use the engine's native UK2Node_InputDebugKey (InputBlueprintNodes plugin)
         * instead of our UReflectionK2Node_InputDebugKey. The native node:
         *  - Has Pressed/Released exec output pins (not delegate/then)
         *  - Registers in FBlueprintActionDatabase so it appears in context menus
         *  - ExpandNode() creates UK2Node_InputDebugKeyEvent intermediates during compile,
         *    which generate the InpActEvt_* functions the runtime binding system expects
         *
         * The header is in Private/ but the class is API-exported (INPUTBLUEPRINTNODES_API),
         * so we load it by name via reflection. */
        static UClass* NativeDebugKeyClass = nullptr;
        if (!NativeDebugKeyClass)
        {
            NativeDebugKeyClass = LoadObject<UClass>(nullptr, TEXT("/Script/InputBlueprintNodes.K2Node_InputDebugKey"));
            if (!NativeDebugKeyClass)
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("InputDebugKey: Could not find K2Node_InputDebugKey class from InputBlueprintNodes"));
                return nullptr;
            }
        }

        const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
        if (Binding)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("InputDebugKey event: %s -> Key=%s Event=%s"), *Func.Name, *Binding->KeyName, *Binding->InputKeyEvent);
        }

        UK2Node* DebugKeyNode = NewObject<UK2Node>(Graph, NativeDebugKeyClass);
        DebugKeyNode->CreateNewGuid();
        DebugKeyNode->SetFlags(RF_Transactional);
        DebugKeyNode->NodePosX = OffsetX;
        DebugKeyNode->NodePosY = EventNodeY;
        EventNodeY += 120;

        /* Set properties via FProperty reflection since the header is private.
         * Native UK2Node_InputDebugKey has:
         *   FKey InputKey
         *   bool bControl, bAlt, bShift, bCommand, bExecuteWhenPaused */
        auto SetBoolProp = [&](const TCHAR* PropName, bool bValue)
        {
            if (FProperty* Prop = NativeDebugKeyClass->FindPropertyByName(FName(PropName)))
            {
                Prop->SetValue_InContainer(DebugKeyNode, &bValue);
            }
        };

        if (Binding)
        {
            if (!Binding->KeyName.IsEmpty())
            {
                FKey Key(*Binding->KeyName);
                if (FProperty* InputKeyProp = NativeDebugKeyClass->FindPropertyByName(FName("InputKey")))
                {
                    InputKeyProp->SetValue_InContainer(DebugKeyNode, &Key);
                }
            }
            SetBoolProp(TEXT("bControl"), Binding->bCtrl);
            SetBoolProp(TEXT("bAlt"), Binding->bAlt);
            SetBoolProp(TEXT("bShift"), Binding->bShift);
            SetBoolProp(TEXT("bCommand"), Binding->bCmd);
            SetBoolProp(TEXT("bExecuteWhenPaused"), Binding->bExecuteWhenPaused);
        }

        Graph->AddNode(DebugKeyNode, true, true);
        DebugKeyNode->AllocateDefaultPins();
        return DebugKeyNode;
    }

    case EFunctionKind::InputKey:
    {
        /* Plain key input (K2Node_InputKey editor node). Unlike InputDebugKey
         * the header is public (BlueprintGraph/Classes), so members are set
         * directly. The compiler's name grammar is
         * "InpActEvt_<[Ctrl+Alt+Shift+Cmd+]Key>_K2Node_InputKeyEvent_<N>"
         * (K2Node_InputKey.cpp ExpandNode); the authoritative key/chord/event
         * data comes from the class's InputKeyDelegateBinding export when
         * present, with the function name as fallback. */
        UK2Node_InputKey* KeyNode = NewObject<UK2Node_InputKey>(Graph);
        KeyNode->CreateNewGuid();
        KeyNode->SetFlags(RF_Transactional);
        KeyNode->NodePosX = OffsetX;
        KeyNode->NodePosY = EventNodeY;
        EventNodeY += 120;

        FInputBindingInfo NameFallback;
        const FInputBindingInfo* Binding = InputBindings.Find(Func.Name);
        if (!Binding)
        {
            /* Parse the key (+optional modifier segment) out of the name:
             * InpActEvt_[<Mods+>]<Key>_K2Node_... */
            const FString Body = Func.Name.Mid(10); /* strlen("InpActEvt_") */
            int32 MarkerIdx = INDEX_NONE;
            if (Body.FindChar(TEXT('_'), MarkerIdx) && MarkerIdx > 0)
            {
                FString FirstSeg = Body.Left(MarkerIdx);
                FString KeyValue = FirstSeg;
                if (FirstSeg.Contains(TEXT("+")))
                {
                    /* Modifier segment: "Ctrl+Alt+Key" - the last '+'-separated
                     * token is the key itself; earlier tokens are modifiers. */
                    TArray<FString> Parts;
                    FirstSeg.ParseIntoArray(Parts, TEXT("+"), true);
                    if (Parts.Num() >= 2)
                    {
                        KeyValue = Parts.Last();
                        for (int32 Idx = 0; Idx < Parts.Num() - 1; ++Idx)
                        {
                            const FString& Mod = Parts[Idx];
                            if (Mod == TEXT("Ctrl")) NameFallback.bCtrl = true;
                            else if (Mod == TEXT("Alt")) NameFallback.bAlt = true;
                            else if (Mod == TEXT("Shift")) NameFallback.bShift = true;
                            else if (Mod == TEXT("Cmd")) NameFallback.bCmd = true;
                            else KeyValue = Mod; /* unknown token was the key */
                        }
                    }
                }
                NameFallback.KeyName = KeyValue;
                Binding = &NameFallback;
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("InputKey %s: no binding export, name-parsed Key=%s"), *Func.Name, *KeyValue);
            }
        }

        if (Binding && !Binding->KeyName.IsEmpty())
        {
            FKey ParsedKey(*Binding->KeyName);
            if (ParsedKey.IsValid())
            {
                KeyNode->InputKey = ParsedKey;
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("InputKey %s: unresolvable FKey '%s' - leaving node default"), *Func.Name, *Binding->KeyName);
            }
            KeyNode->bControl = Binding->bCtrl;
            KeyNode->bAlt = Binding->bAlt;
            KeyNode->bShift = Binding->bShift;
            KeyNode->bCommand = Binding->bCmd;
            KeyNode->bConsumeInput = Binding->bConsumeInput;
            KeyNode->bExecuteWhenPaused = Binding->bExecuteWhenPaused;
            KeyNode->bOverrideParentBinding = Binding->bOverrideParentBinding;
        }

        Graph->AddNode(KeyNode, true, true);
        KeyNode->AllocateDefaultPins();
        return KeyNode;
    }

    case EFunctionKind::LegacyInputAction:
    {
        /* UK2Node_InputAction: legacy ActionBindings, no trigger sub-pins. */
        UK2Node_InputAction* InputActionNode = NewObject<UK2Node_InputAction>(Graph);
        InputActionNode->CreateNewGuid();
        InputActionNode->SetFlags(RF_Transactional);
        InputActionNode->NodePosX = OffsetX;
        InputActionNode->NodePosY = EventNodeY;
        EventNodeY += 120;

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
        InputAxisNode->NodePosX = OffsetX;
        InputAxisNode->NodePosY = EventNodeY;
        EventNodeY += 120;

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
        EventNode->NodePosX = OffsetX;
        EventNode->NodePosY = EventNodeY;
        EventNodeY += 120;

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
        CustomEventNode->NodePosX = OffsetX;
        CustomEventNode->NodePosY = EventNodeY;
        EventNodeY += 120;
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

    /* Containers first: the element decides the type, the container decides the
     * shape. Without these, array locals declared VarType wildcard and every
     * Get/Set on them failed with "specified as an array, but does not have a
     * valid array property" (08.25: Origin Condition/Mesh/Lewdness x8). */
    if (TypeName == TEXT("ArrayProperty") && PropObj->HasField(TEXT("Inner")))
    {
        PinType = PinTypeFromJson(PropObj->GetObjectField(TEXT("Inner")));
        PinType.ContainerType = EPinContainerType::Array;
        return PinType;
    }
    if (TypeName == TEXT("SetProperty") && PropObj->HasField(TEXT("ElementProp")))
    {
        PinType = PinTypeFromJson(PropObj->GetObjectField(TEXT("ElementProp")));
        PinType.ContainerType = EPinContainerType::Set;
        return PinType;
    }
    if (TypeName == TEXT("MapProperty") && PropObj->HasField(TEXT("KeyProp")) && PropObj->HasField(TEXT("ValueProp")))
    {
        PinType = PinTypeFromJson(PropObj->GetObjectField(TEXT("KeyProp")));
        const FEdGraphPinType ValueType = PinTypeFromJson(PropObj->GetObjectField(TEXT("ValueProp")));
        PinType.ContainerType = EPinContainerType::Map;
        PinType.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
        return PinType;
    }

    if (TypeName == TEXT("IntProperty") || TypeName == TEXT("Int8Property") || TypeName == TEXT("Int16Property") || TypeName == TEXT("Int64Property") || TypeName == TEXT("ByteProperty") || TypeName == TEXT("UInt16Property") || TypeName == TEXT("UInt32Property") || TypeName == TEXT("UInt64Property"))
    {
        /* A byte carrying an Enum reference is an enum pin (byte + UEnum
         * subcategory), not a plain int - Select option naming and enumerator
         * defaults both key off that. */
        if (TypeName == TEXT("ByteProperty") && PropObj->HasField(TEXT("Enum")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            PinType.PinSubCategoryObject = ResolveEnumObj(PropObj->GetObjectField(TEXT("Enum")));
        }
        else
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        }
    }
    else if (TypeName == TEXT("EnumProperty"))
    {
        /* Enum values are byte pins carrying the enum as subcategory object
         * (UEdGraphSchema_K2 convention); UnderlyingProp repeats the byte
         * backing and adds nothing to typing. */
        PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        if (PropObj->HasField(TEXT("Enum")))
        {
            PinType.PinSubCategoryObject = ResolveEnumObj(PropObj->GetObjectField(TEXT("Enum")));
        }
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
    else if (TypeName == TEXT("StrProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_String;
    }
    else if (TypeName == TEXT("TextProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
    }
    else if (TypeName == TEXT("StructProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        if (PropObj->HasField(TEXT("Struct")))
        {
            PinType.PinSubCategoryObject = ResolveStructObj(PropObj->GetObjectField(TEXT("Struct")));
        }
    }
    else if (TypeName == TEXT("ObjectProperty"))
    {
        PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        if (PropObj->HasField(TEXT("PropertyClass")))
        {
            const TSharedPtr<FJsonObject>& PropClass = PropObj->GetObjectField(TEXT("PropertyClass"));
            if (PropClass)
            {
                /* The JSON carries ObjectName ("BlueprintGeneratedClass'BP_Save_C'") plus an
                 * ObjectPath, not a bare Name field - FindObject on "Name" never resolves.
                 * ResolveCastClass handles the full memory/registry/disk chain. */
                PinType.PinSubCategoryObject = ResolveCastClass(PropClass);
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
    else if (Token == TEXT("EX_SetArray"))
    {
        return EmitSetArray(Builder, Stmt);
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

    // Shortcut: reads of EnhancedInputAction event outputs arrive as local
    // function calls (e.g. K2Node_EnhancedInputActionEvent_ActionValue);
    // resolve directly to the event node's output pin via ProducerPins.
    if (Token == TEXT("EX_LocalVirtualFunction") || Token == TEXT("EX_LocalFinalFunction"))
    {
        const FString FuncName = ExprJson->GetStringField(TEXT("Function"));
        if (FuncName.StartsWith(TEXT("K2Node_EnhancedInputActionEvent_")))
        {
            if (UEdGraphPin** FoundPin = Builder.ProducerPins.Find(FuncName))
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("%s -> producer pin %s (no call node)"),
                    *FuncName, *(*FoundPin)->PinName.ToString());
                return FPinValue{ *FoundPin, false, TEXT(""), nullptr };
            }
        }
    }

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

        // Function parameters and locals are output pins on the entry node (created
        // from the UFunction signature). A read resolves to that pin - not a bogus
        // self-context VariableGet, which would have no pins since the name is not a
        // class member.
        if (Builder.EntryNode)
        {
            for (UEdGraphPin* Pin : Builder.EntryNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == FName(*VarName))
                {
                    return FPinValue{ Pin, false, TEXT(""), nullptr };
                }
            }
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

        // Compiler temp vars (CallFunc_, Temp_, K2Node_) have no real Get node;
        // if no producer pin was found, return empty rather than a bogus VariableGet.
        // Function locals (e.g. Temp_int_Loop_Counter_Variable) ARE declared, so
        // they fall through to a real Get node resolved via SetLocalMember.
        // Value-guard temps recorded by EmitLet resolve as literal constants -
        // they are pin values, not variables (Select options, ternary branches).
        if (const FString* ConstVal = Builder.TempConstants.Find(VarName))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Resolved temp var %s as constant '%s'"), *VarName, **ConstVal);
            return FPinValue{ nullptr, true, *ConstVal, nullptr };
        }
        if (!Builder.FunctionLocalNames.Contains(VarName)
            && (VarName.StartsWith(TEXT("CallFunc_")) || VarName.StartsWith(TEXT("Temp_")) || VarName.StartsWith(TEXT("K2Node_"))))
        {
            /* Classified diagnostics + suppression rule (plan 011 follow-up B):
             * a frame slot that NO Let-family statement ever writes is compiler
             * bookkeeping or an entry input - it must never become a node. */
            EVarKind Kind;
            const FVarInfo* Info = FindVariableInfo(VarName, Kind);
            if (Info && Kind == EVarKind::FrameTemp && !WrittenUbergraphSlots.Contains(VarName))
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log,
                    TEXT("  FrameTemp '%s' (%s) is never written in bytecode - bookkeeping/input, suppressing node creation"),
                    *VarName, *Info->PinType.PinCategory.ToString());
                AddDiagnostic(TEXT("SuppressedTemp"),
                    FString::Printf(TEXT("'%s' (%s) never written - node creation suppressed"), *VarName, *Info->PinType.PinCategory.ToString()));
                return FPinValue();
            }
            FindProducerExact(Builder, VarName);
            return FPinValue();
        }

        UK2Node_VariableGet* GetNode = CreateVariableGet(Builder, VarName, OwnerClass, PropObj);
        if (GetNode && GetNode->Pins.Num() > 0)
        {
            // For bSelfContext variables, the output pin is named after the variable, not "ReturnValue"
            UEdGraphPin* ValuePin = FindPin(GetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
            if (!ValuePin)
            {
                ValuePin = FindPin(GetNode, *VarName, EGPD_Output);
            }
            if (ValuePin)
            {
                return FPinValue{ ValuePin, false, TEXT(""), nullptr };
            }
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_SwitchValue"))
    {
        /* Compiled K2Node_Select / ternary - without this every Select
         * expression silently vanished, leaving SET nodes valueless and
         * downstream reads defaulted. */
        return ResolveSwitchValue(Builder, ExprJson);
    }
    else if (Token == TEXT("EX_Self"))
    {
        // Self as a value (e.g. RHS of "Widget.GM = Self", or a WorldContextObject
        // parameter): create a visible UK2Node_Self and return its output pin so
        // downstream pins can be wired. The hidden "self" param of member calls is
        // skipped by CreateCallNode and never reaches here.
        UK2Node_Self* SelfNode = NewObject<UK2Node_Self>(Builder.Graph);
        SelfNode->CreateNewGuid();
        SelfNode->SetFlags(RF_Transactional);
        SelfNode->NodePosX = Builder.NextNodeX - 200;
        SelfNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        Builder.Graph->AddNode(SelfNode, true, true);
        SelfNode->AllocateDefaultPins();

        UEdGraphPin* SelfPin = FindPin(SelfNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Output);
        if (!SelfPin)
        {
            // UK2Node_Self's output pin can be named "self" (PN_Self)
            for (UEdGraphPin* Pin : SelfNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output)
                {
                    SelfPin = Pin;
                    break;
                }
            }
        }
        if (SelfPin)
        {
            return FPinValue{ SelfPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_VectorConst"))
    {
        /* Literal vector constant (e.g. camera base offset (-1,55,163) feeding
         * Add_VectorVector): Kismet pin defaults are FLAT comma text with no
         * component labels - "(X=..)" labeled form fails the graph widget's
         * parse and renders 0 (stock nodes store "0, 0, 0" / "1.0,1.0,1.0").
         * Component order X,Y,Z. */
        const TSharedPtr<FJsonObject> Val = ExprJson->HasField(TEXT("Value")) ? ExprJson->GetObjectField(TEXT("Value")) : nullptr;
        if (!Val.IsValid()) return FPinValue();
        const double X = Val->HasField(TEXT("X")) ? Val->GetNumberField(TEXT("X")) : 0.0;
        const double Y = Val->HasField(TEXT("Y")) ? Val->GetNumberField(TEXT("Y")) : 0.0;
        const double Z = Val->HasField(TEXT("Z")) ? Val->GetNumberField(TEXT("Z")) : 0.0;
        const FString Text = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_VectorConst -> literal %s"), *Text);
        return FPinValue{ nullptr, true, Text, nullptr };
    }
    else if (Token == TEXT("EX_RotationConst"))
    {
        /* Literal rotator constant. Kismet pin defaults are FLAT comma text
         * (no labels); t3d rotator component order is Roll,Yaw,Pitch - the
         * only permutation matching the original asset
         * ({P:-2,Y:-103,R:0} -> "0.000000,-103.000000,-2.000000"). */
        const TSharedPtr<FJsonObject> Val = ExprJson->HasField(TEXT("Value")) ? ExprJson->GetObjectField(TEXT("Value")) : nullptr;
        if (!Val.IsValid()) return FPinValue();
        const double Pitch = Val->HasField(TEXT("Pitch")) ? Val->GetNumberField(TEXT("Pitch")) : 0.0;
        const double Yaw = Val->HasField(TEXT("Yaw")) ? Val->GetNumberField(TEXT("Yaw")) : 0.0;
        const double Roll = Val->HasField(TEXT("Roll")) ? Val->GetNumberField(TEXT("Roll")) : 0.0;
        const FString Text = FString::Printf(TEXT("%f,%f,%f"), Roll, Yaw, Pitch);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_RotationConst -> literal %s"), *Text);
        return FPinValue{ nullptr, true, Text, nullptr };
    }
    else if (Token == TEXT("EX_ObjectConst"))
    {
        const TSharedPtr<FJsonObject>& ValueObj = ExprJson->GetObjectField(TEXT("Value"));
        if (!ValueObj.IsValid()) return FPinValue();

        FString ObjectName = ValueObj->GetStringField(TEXT("ObjectName"));
        FString ObjectPath = ValueObj->GetStringField(TEXT("ObjectPath"));

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_ObjectConst: Name=%s, Path=%s"), *ObjectName, *ObjectPath);

        // Native static-library CDOs (e.g. Class'Default__KismetArrayLibrary') are used
        // only as the implicit self of static/pure library calls whose self pin is
        // hidden. Do NOT create a literal for them: their module path (e.g.
        // "/Script/Engine") would otherwise resolve to the class object (Engine) and
        // produce a bogus stray literal in the graph.
        if (ObjectName.StartsWith(TEXT("Class'Default__")))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Native CDO ref %s -> skipping literal (hidden static self)"), *ObjectName);
            return FPinValue();
        }

        // Try to resolve the object
        UObject* ResolvedObject = nullptr;

        // Strip [index] and .N suffix from ObjectPath
        FString CleanPath = ObjectPath;
        int32 BracketIdx;
        if (CleanPath.FindChar(TEXT('['), BracketIdx))
        {
            CleanPath = CleanPath.Left(BracketIdx);
        }
        int32 DotIdx;
        if (CleanPath.FindChar(TEXT('.'), DotIdx))
        {
            // Check if what follows is a number (subobject index)
            FString AfterDot = CleanPath.Mid(DotIdx + 1);
            if (AfterDot.IsNumeric())
            {
                CleanPath = CleanPath.Left(DotIdx);
            }
        }

        // 1. Try LoadObject for specific asset types first (not Package)
        if (!CleanPath.IsEmpty())
        {
            ResolvedObject = LoadObject<UMaterialInterface>(nullptr, *CleanPath);
            if (!ResolvedObject)
            {
                ResolvedObject = LoadObject<UTexture>(nullptr, *CleanPath);
            }
        }
        
        // 2. Try FindObject (already loaded objects) - skip Packages
        if (!ResolvedObject)
        {
            UObject* Found = FindObject<UObject>(nullptr, *CleanPath);
            if (Found && !Found->IsA<UPackage>())
            {
                ResolvedObject = Found;
            }
        }
        
        // 3. Try LoadObject<UObject> as last resort - skip Packages
        if (!ResolvedObject && !CleanPath.IsEmpty())
        {
            UObject* Loaded = LoadObject<UObject>(nullptr, *CleanPath);
            if (Loaded && !Loaded->IsA<UPackage>())
            {
                ResolvedObject = Loaded;
            }
        }

        // 3. Try to find by class name (CDO references)
        if (!ResolvedObject)
        {
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
                if (!ObjClass)
                {
                    FString EnginePath = FString::Printf(TEXT("/Script/Engine.%s"), *ClassName);
                    ObjClass = FindObject<UClass>(nullptr, *EnginePath);
                }
                if (ObjClass)
                {
                    ResolvedObject = ObjClass->GetDefaultObject();
                }
            }
        }

        if (ResolvedObject)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Resolved object: %s (%s)"), *ResolvedObject->GetName(), *ResolvedObject->GetClass()->GetName());

            // If we resolved to a Blueprint asset, reference the CDO of its generated
            // class instead so the literal's output pin is typed as the BP class
            // (e.g. BPL_DollSystem_C) rather than as a generic Blueprint asset.
            if (UBlueprint* BP = Cast<UBlueprint>(ResolvedObject))
            {
                if (UClass* GenClass = BP->GeneratedClass)
                {
                    ResolvedObject = GenClass->GetDefaultObject();
                }
            }

            UK2Node_Literal* LiteralNode = NewObject<UK2Node_Literal>(Builder.Graph);
            LiteralNode->CreateNewGuid();
            LiteralNode->SetFlags(RF_Transactional);
            LiteralNode->NodePosX = Builder.NextNodeX - 200;
            LiteralNode->NodePosY = Builder.NextNodeY;
            Builder.NextNodeY += 120;
            Builder.Graph->AddNode(LiteralNode, true, true);
            LiteralNode->SetObjectRef(ResolvedObject);
            LiteralNode->AllocateDefaultPins();

            UEdGraphPin* ValuePin = LiteralNode->GetValuePin();
            if (ValuePin)
            {
                return FPinValue{ ValuePin, false, TEXT(""), nullptr };
            }
        }
        else
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  Failed to resolve object: %s (path: %s)"), *ObjectName, *CleanPath);
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
    /* NOTE: EX_VectorConst / EX_RotationConst (and any future numeric *Const)
     * are handled by the GENERIC numeric-struct literal branch earlier in this
     * chain - do not re-add per-token duplicates here. */
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
        UScriptStruct* StructType = ResolveUserDefinedStruct(StructObjectName, StructObj->GetStringField(TEXT("ObjectPath")));

        if (!StructType)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Could not resolve struct: %s"), *StructObjectName);
            return FPinValue();
        }

        UK2Node_MakeStruct* MakeStructNode = NewObject<UK2Node_MakeStruct>(Builder.Graph);
        MakeStructNode->CreateNewGuid();
        MakeStructNode->SetFlags(RF_Transactional);
        MakeStructNode->StructType = StructType;
        MakeStructNode->NodePosX = Builder.NextNodeX - 200;
        MakeStructNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        Builder.Graph->AddNode(MakeStructNode, true, true);
        /* Nodes created programmatically (not placed in the editor) skip
         * PostPlacedNewNode/PreSave, so bMadeAfterOverridePinRemoval stays false and
         * ValidateNodeDuringCompilation emits a benign "Override pins have been
         * removed" note for every importer-created MakeStruct. The flag exists purely
         * to suppress that message - set it so re-imports import clean. */
        MakeStructNode->bMadeAfterOverridePinRemoval = true;
        MakeStructNode->AllocateDefaultPins();

        // Wire struct member pins. EX_StructConst's Properties array maps
        // positionally to the struct's fields, which appear on the MakeStruct
        // node as INPUT member pins (e.g. R/G/B/A for LinearColor).
        const TArray<TSharedPtr<FJsonValue>>& Properties = ExprJson->GetArrayField(TEXT("Properties"));
        TArray<UEdGraphPin*> MemberPins;
        for (UEdGraphPin* Pin : MakeStructNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                MemberPins.Add(Pin);
            }
        }

        for (int32 i = 0; i < Properties.Num() && i < MemberPins.Num(); ++i)
        {
            FPinValue MemberValue = ResolveExpression(Builder, Properties[i]->AsObject());
            if (MemberValue.Pin)
            {
                ConnectPins(MemberValue.Pin, MemberPins[i]);
            }
            else if (MemberValue.bConstant)
            {
                SetPinDefaultValueSafe(MemberPins[i], MemberValue.ConstString);
            }
        }

        UEdGraphPin* ReturnValuePin = FindPin(MakeStructNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        if (!ReturnValuePin)
        {
            // MakeStruct output pin is named after the struct type (e.g. "LinearColor")
            for (UEdGraphPin* Pin : MakeStructNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                {
                    ReturnValuePin = Pin;
                    break;
                }
            }
        }
        if (ReturnValuePin)
        {
            return FPinValue{ ReturnValuePin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_StructMemberContext"))
    {
        // Struct member read. Two encodings:
        //  - { Struct: <struct expr>, Property: { Name } }  (old)
        //  - { Property: { Owner: <struct>, Property: <field> }, StructExpression: <source struct> } (exporter)
        const TSharedPtr<FJsonObject>& MemberObj = ExprJson->GetObjectField(TEXT("Property"));
        if (!MemberObj.IsValid()) return FPinValue();
        const TSharedPtr<FJsonObject>& MemberPropObj = MemberObj->GetObjectField(TEXT("Property"));
        if (!MemberPropObj.IsValid()) return FPinValue();
        FString MemberName = MemberPropObj->GetStringField(TEXT("Name"));
        if (MemberName.IsEmpty()) return FPinValue();

        // Resolve the source struct expression
        FPinValue SourceValue;
        const TSharedPtr<FJsonObject>& StructExpr = ExprJson->HasField(TEXT("StructExpression"))
            ? ExprJson->GetObjectField(TEXT("StructExpression"))
            : ExprJson->GetObjectField(TEXT("Struct"));
        if (StructExpr.IsValid())
        {
            SourceValue = ResolveExpression(Builder, StructExpr);
        }

        // Derive the source temp variable name so multiple field reads share one BreakStruct node
        FString SourceVarName;
        if (StructExpr.IsValid())
        {
            const TSharedPtr<FJsonObject>& StructExprVar = StructExpr->GetObjectField(TEXT("Variable"));
            if (StructExprVar.IsValid())
            {
                const TSharedPtr<FJsonObject>& StructExprProp = StructExprVar->GetObjectField(TEXT("Property"));
                if (StructExprProp.IsValid())
                {
                    SourceVarName = StructExprProp->GetStringField(TEXT("Name"));
                }
            }
        }

        // Struct type from Property.Owner ("UserDefinedStruct'S_CharData'")
        UScriptStruct* StructType = nullptr;
        if (MemberObj->HasField(TEXT("Owner")))
        {
            const TSharedPtr<FJsonObject>& OwnerObj = MemberObj->GetObjectField(TEXT("Owner"));
            if (OwnerObj.IsValid())
            {
                FString StructObjName = OwnerObj->GetStringField(TEXT("ObjectName"));
                StructObjName.RemoveFromStart(TEXT("UserDefinedStruct'"));
                StructObjName.RemoveFromStart(TEXT("Class'"));
                StructObjName.RemoveFromEnd(TEXT("'"));
                int32 ColonIdx;
                if (StructObjName.FindChar(TEXT(':'), ColonIdx)) StructObjName = StructObjName.Left(ColonIdx);
                StructType = ResolveUserDefinedStruct(StructObjName, OwnerObj->GetStringField(TEXT("ObjectPath")));
            }
        }
        if (!StructType)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EX_StructMemberContext: no struct type for member %s"), *MemberName);
            return FPinValue();
        }

        // Find or create the BreakStruct node for the source struct
        UK2Node_BreakStruct* BreakNode = nullptr;
        if (!SourceVarName.IsEmpty())
        {
            if (UK2Node_BreakStruct** Found = Builder.BreakStructNodes.Find(SourceVarName))
            {
                BreakNode = *Found;
            }
        }
        if (!BreakNode)
        {
            BreakNode = NewObject<UK2Node_BreakStruct>(Builder.Graph);
            BreakNode->CreateNewGuid();
            BreakNode->SetFlags(RF_Transactional);
            BreakNode->StructType = StructType;
            Builder.Graph->AddNode(BreakNode, true, true);
            BreakNode->AllocateDefaultPins();
            BreakNode->NodePosX = Builder.NextNodeX - 600;
            BreakNode->NodePosY = Builder.NextNodeY;
            Builder.NextNodeY += 120;

            UEdGraphPin* StructPin = FindPin(BreakNode, TEXT("Struct"), EGPD_Input);
            if (!StructPin) StructPin = FindPin(BreakNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Input);
            if (!StructPin)
            {
                // UK2Node_BreakStruct's input pin is named after the struct type (e.g. "S_CharData")
                for (UEdGraphPin* Pin : BreakNode->Pins)
                {
                    if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                    {
                        StructPin = Pin;
                        break;
                    }
                }
            }
            if (StructPin && SourceValue.Pin)
            {
                // Array_Get wildcard ReturnValue pins stay untyped until node reconstruction
                // (which never runs here) - force the concrete struct type so the link resolves
                if (SourceValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
                    || (SourceValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
                        && !SourceValue.Pin->PinType.PinSubCategoryObject.IsValid())
                    || (SourceValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
                        && !SourceValue.Pin->PinType.PinSubCategoryObject.IsValid()))
                {
                    SourceValue.Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                    SourceValue.Pin->PinType.PinSubCategory = TEXT("");
                    SourceValue.Pin->PinType.PinSubCategoryObject = StructType;
                }
                ConnectPins(SourceValue.Pin, StructPin);
            }
            if (!SourceVarName.IsEmpty())
            {
                Builder.BreakStructNodes.Add(SourceVarName, BreakNode);
            }
        }

        UEdGraphPin* MemberPin = FindPin(BreakNode, *MemberName, EGPD_Output);
        if (!MemberPin)
        {
            // Match by base name (pins use the target struct's hashed member names)
            const FString BaseName = StripStructMemberSuffix(MemberName);
            for (UEdGraphPin* Pin : BreakNode->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinName != UEdGraphSchema_K2::PN_ReturnValue
                    && StripStructMemberSuffix(Pin->PinName.ToString()) == BaseName)
                {
                    MemberPin = Pin;
                    break;
                }
            }
        }
        if (MemberPin)
        {
            return FPinValue{ MemberPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_Context") || Token == TEXT("EX_Context_FailSilent"))
    {
        // Member access on an object instance: GI_Data_3.NPC Character Data.
        // ObjectExpression = the object (producer pin), ContextExpression =
        // EX_InstanceVariable { Variable: { Owner, Property } }.
        const TSharedPtr<FJsonObject>& CtxExpr = ExprJson->GetObjectField(TEXT("ContextExpression"));
        if (!CtxExpr.IsValid()) return FPinValue();
        const FString CtxToken = CtxExpr->GetStringField(TEXT("Token"));
        if (CtxToken != TEXT("EX_InstanceVariable") && CtxToken != TEXT("EX_StructMemberContext"))
        {
            return FPinValue(); // call context - handled by the statement emitters
        }

        const TSharedPtr<FJsonObject>& CtxVarObj = CtxExpr->GetObjectField(TEXT("Variable"));
        if (!CtxVarObj.IsValid()) return FPinValue();
        const TSharedPtr<FJsonObject>& CtxPropObj = CtxVarObj->GetObjectField(TEXT("Property"));
        if (!CtxPropObj.IsValid()) return FPinValue();
        const FString MemberName = CtxPropObj->GetStringField(TEXT("Name"));
        if (MemberName.IsEmpty() && !CtxVarObj->HasField(TEXT("Path"))) return FPinValue();

        // Member owner class (GI_Data_C)
        UClass* MemberOwner = GeneratedClass;
        bool bOwnerResolved = false;
        if (UClass* FoundClass = ResolveClassFromJson(CtxVarObj->GetObjectField(TEXT("Owner"))))
        {
            MemberOwner = FoundClass;
            bOwnerResolved = true;
        }

        /* Pure member READS (e.g. Conv_BoolToString(PC->bEnableClickEvents))
         * describe the member as Path[] + ResolvedOwner with NO Property
         * object - synthesize both, mirroring the write-side handling in
         * EmitLet. */
        FString EffectiveMember = MemberName;
        if (EffectiveMember.IsEmpty())
        {
            const TArray<TSharedPtr<FJsonValue>>& MemPath = CtxVarObj->GetArrayField(TEXT("Path"));
            if (MemPath.Num() == 0 || MemPath[0]->AsString().IsEmpty()) return FPinValue();
            EffectiveMember = MemPath[0]->AsString();
            if (!bOwnerResolved && CtxVarObj->HasField(TEXT("ResolvedOwner")))
            {
                if (UClass* PathOwner = ResolveClassFromJson(CtxVarObj->GetObjectField(TEXT("ResolvedOwner"))))
                {
                    MemberOwner = PathOwner;
                }
            }
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_Context member read synthesized: %s::%s (from Path)"),
                *MemberOwner->GetName(), *EffectiveMember);
        }

        // Resolve the object expression (e.g. the Global Game Instance call result)
        FPinValue ObjValue;
        if (ExprJson->HasField(TEXT("ObjectExpression")))
        {
            ObjValue = ResolveExpression(Builder, ExprJson->GetObjectField(TEXT("ObjectExpression")));
        }

        UK2Node_VariableGet* GetNode = CreateVariableGet(Builder, EffectiveMember, MemberOwner,
            EffectiveMember == MemberName ? CtxPropObj : TSharedPtr<FJsonObject>());
        if (GetNode && GetNode->Pins.Num() > 0)
        {
            UEdGraphPin* ValuePin = FindPin(GetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
            if (!ValuePin) ValuePin = FindPin(GetNode, *EffectiveMember, EGPD_Output);
            if (ValuePin && ObjValue.Pin)
            {
                UEdGraphPin* SelfPin = FindPin(GetNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input);
                if (!SelfPin) SelfPin = FindPin(GetNode, TEXT("self"), EGPD_Input);
                if (SelfPin)
                {
                    if (!SelfPin->PinType.PinSubCategoryObject.IsValid() && ObjValue.Pin->PinType.PinSubCategoryObject.IsValid())
                    {
                        SelfPin->PinType.PinSubCategoryObject = ObjValue.Pin->PinType.PinSubCategoryObject;
                    }
                    else if (ObjValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
                        && !ObjValue.Pin->PinType.PinSubCategoryObject.IsValid()
                        && SelfPin->PinType.PinSubCategoryObject.IsValid())
                    {
                        // GI instance call ReturnValue pins are untyped "object" - coerce to the
                        // member owner type so the connection to the typed self pin succeeds
                        ObjValue.Pin->PinType.PinSubCategoryObject = SelfPin->PinType.PinSubCategoryObject;
                    }
                    ConnectPins(ObjValue.Pin, SelfPin);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_Context member access: wired %s -> %s (%s)"),
                        *ObjValue.Pin->PinName.ToString(), *SelfPin->PinName.ToString(), *EffectiveMember);
                }
            }
            return FPinValue{ ValuePin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
    else if (Token == TEXT("EX_ArrayGetByRef"))
    {
        // Array get by index. The exporter stores the source array as
        // "ArrayVariable" and the index as "ArrayIndex".
        const TSharedPtr<FJsonObject>& ArrayExpr = ExprJson->HasField(TEXT("ArrayVariable"))
            ? ExprJson->GetObjectField(TEXT("ArrayVariable"))
            : (ExprJson->HasField(TEXT("ArrayExpression")) ? ExprJson->GetObjectField(TEXT("ArrayExpression")) : nullptr);
        const TSharedPtr<FJsonObject>& IndexExpr = ExprJson->HasField(TEXT("ArrayIndex"))
            ? ExprJson->GetObjectField(TEXT("ArrayIndex"))
            : (ExprJson->HasField(TEXT("IndexTerm")) ? ExprJson->GetObjectField(TEXT("IndexTerm")) : nullptr);

        if (!ArrayExpr.IsValid()) return FPinValue();

        FPinValue ArrayValue = ResolveExpression(Builder, ArrayExpr);
        FPinValue IndexValue = IndexExpr.IsValid() ? ResolveExpression(Builder, IndexExpr) : FPinValue();

        /* UK2Node_GetArrayItem (not UK2Node_CallArrayFunction - that node type
         * requires a target array function and ensures when none is set) has the
         * "Array" / "Dimension 1" / "Output" pins the bytecode expects. */
        UK2Node_GetArrayItem* ArrayGetNode = NewObject<UK2Node_GetArrayItem>(Builder.Graph);
        ArrayGetNode->CreateNewGuid();
        ArrayGetNode->SetFlags(RF_Transactional);
        ArrayGetNode->NodePosX = Builder.NextNodeX - 200;
        ArrayGetNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        Builder.Graph->AddNode(ArrayGetNode, true, true);
        ArrayGetNode->AllocateDefaultPins();

        /* Object-like arrays never return elements by reference. EX_ArrayGetByRef
         * is the BY-REF op: mark the RESULT pin after the array is wired (typed)
         * so the saved shape matches the original "Get (a ref)" and compile-time
         * propagation keeps the reference instead of downgrading it to a copy
         * (08.25: BP_WorldPawn's struct-array gets were flipped, one
         * notification per node). Marking BEFORE wiring would fire the
         * "Array Get node altered" notification during wildcard propagation. */
        if (ArrayValue.Pin)
        {
            /* Non-ref-capable element categories must not return by reference.
             * Clearing only the pin flag does NOT survive: compile-time node
             * reconstruction rebuilds pins from the private bReturnByRefDesired
             * (08.25: the "Array Get node altered" notification came back in
             * BP_CharCreation). The public setter flips the bool itself; its
             * internal reconstruct is safe here because nothing is wired yet. */
            if (IsNonRefCapableArrayCategory(ArrayValue.Pin->PinType))
            {
                SetArrayItemReturnByRef(ArrayGetNode, false);
            }
            /* Exporter flattens array frame properties to a plain
             * ObjectProperty (element class kept, container lost) - that is
             * the only flattening signature, so restore the container ONLY for
             * object-category producers. Scalar returns mis-fed into a Get
             * (08.25: an Array_Length result wired as an array) must stay
             * scalar - typing them as arrays broke their other consumers
             * ("Array of Integers is not compatible with Integer"). */
            if (ArrayValue.Pin->PinType.ContainerType != EPinContainerType::Array
                && ArrayValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
            {
                ArrayValue.Pin->PinType.ContainerType = EPinContainerType::Array;
            }
            UEdGraphPin* ArrayPin = ArrayGetNode->GetTargetArrayPin();
            if (ArrayPin) ConnectPins(ArrayValue.Pin, ArrayPin);
        }
        if (IndexValue.Pin)
        {
            UEdGraphPin* IndexPin = ArrayGetNode->GetIndexPin();
            if (IndexPin) ConnectPins(IndexValue.Pin, IndexPin);
        }
        else if (IndexValue.bConstant)
        {
            UEdGraphPin* IndexPin = ArrayGetNode->GetIndexPin();
            if (IndexPin) IndexPin->DefaultValue = IndexValue.ConstString;
        }

        if (ArrayValue.Pin && !IsNonRefCapableArrayCategory(ArrayValue.Pin->PinType))
        {
            if (UEdGraphPin* ResultPin = ArrayGetNode->GetResultPin())
            {
                ResultPin->PinType.bIsReference = true;
            }
        }

        UEdGraphPin* OutputPin = ArrayGetNode->GetResultPin();
        if (OutputPin)
        {
            return FPinValue{ OutputPin, false, TEXT(""), nullptr };
        }
        return FPinValue();
    }
	else if (Token == TEXT("EX_Cast") || Token == TEXT("EX_DynamicCast"))
	{
		// CST_ObjectToBool is a simple is-not-null check on the object, used to
		// extract the "Cast Succeeded" boolean from a DynamicCast node.
		const FString ConversionType = ExprJson->HasField(TEXT("ConversionType"))
			? ExprJson->GetStringField(TEXT("ConversionType")) : TEXT("");

		if (Token == TEXT("EX_Cast") && ConversionType == TEXT("CST_ObjectToBool"))
		{
			const TSharedPtr<FJsonObject>& TargetExpr = ExprJson->HasField(TEXT("Target"))
				? ExprJson->GetObjectField(TEXT("Target"))
				: (ExprJson->HasField(TEXT("ObjectExpression")) ? ExprJson->GetObjectField(TEXT("ObjectExpression")) : nullptr);
			if (!TargetExpr.IsValid()) return FPinValue();

			// The target is the cast result variable. We need its producer pin,
			// then find the UK2Node_DynamicCast that produced it and return its
			// "Cast Succeeded" pin instead.
			FPinValue TargetValue = ResolveExpression(Builder, TargetExpr);
			if (TargetValue.Pin)
			{
				// Walk backwards from the source pin to find the DynamicCast node
				UEdGraphNode* SourceNode = TargetValue.Pin->GetOwningNode();
				if (SourceNode && SourceNode->IsA<UK2Node_DynamicCast>())
				{
					UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(SourceNode);
					UEdGraphPin* SuccessPin = FindPin(CastNode, TEXT("bSuccess"), EGPD_Output);
					if (!SuccessPin) SuccessPin = FindPin(CastNode, TEXT("Cast Succeeded"), EGPD_Output);
					if (SuccessPin)
					{
						return FPinValue{ SuccessPin, false, TEXT(""), nullptr };
					}
				}
				// Fallback: create a "not equal to null" check
				UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_Cast CST_ObjectToBool: source is not a DynamicCast, using IsValid fallback"));
			}
			return FPinValue();
		}

		// EX_Cast with non-ObjectToBool conversion (CST_None = numeric cast like
		// Cast<double>, Cast<float>) is a pure type conversion handled internally
		// by the compiler.  Do NOT fall through to the DynamicCast creator below
		// which would create orphaned nodes with no exec wiring.
		if (Token == TEXT("EX_Cast"))
		{
			UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_Cast conversionType=%s: pure cast, resolving target"), *ConversionType);
			const TSharedPtr<FJsonObject>& TargetExpr = ExprJson->HasField(TEXT("Target"))
				? ExprJson->GetObjectField(TEXT("Target"))
				: (ExprJson->HasField(TEXT("ObjectExpression")) ? ExprJson->GetObjectField(TEXT("ObjectExpression")) : nullptr);
			if (TargetExpr.IsValid())
			{
				return ResolveExpression(Builder, TargetExpr);
			}
			return FPinValue();
		}

		// Dynamic cast - JSON fields: InterfaceClass + Target
		const TSharedPtr<FJsonObject>& ClassObj = ExprJson->HasField(TEXT("InterfaceClass"))
			? ExprJson->GetObjectField(TEXT("InterfaceClass"))
			: (ExprJson->HasField(TEXT("Class")) ? ExprJson->GetObjectField(TEXT("Class")) : nullptr);
		const TSharedPtr<FJsonObject>& ObjectExpr = ExprJson->HasField(TEXT("Target"))
			? ExprJson->GetObjectField(TEXT("Target"))
			: (ExprJson->HasField(TEXT("ObjectExpression")) ? ExprJson->GetObjectField(TEXT("ObjectExpression")) : nullptr);

		if (!ObjectExpr.IsValid()) return FPinValue();

		FPinValue ObjectValue = ResolveExpression(Builder, ObjectExpr);

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Builder.Graph);
		CastNode->CreateNewGuid();
		CastNode->SetFlags(RF_Transactional);
		CastNode->NodePosX = Builder.NextNodeX - 200;
		CastNode->NodePosY = Builder.NextNodeY;
		Builder.NextNodeY += 120;

		if (ClassObj.IsValid())
		{
			UClass* CastClass = ResolveCastClass(ClassObj);
			if (CastClass)
			{
				CastNode->TargetType = CastClass;
				UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_DynamicCast: resolved class %s -> %s"),
					*ClassObj->GetStringField(TEXT("ObjectName")), *CastClass->GetPathName());
			}
			else
			{
				UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EX_DynamicCast: failed to resolve class %s"),
					*ClassObj->GetStringField(TEXT("ObjectName")));
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
        /* Pure math/function call - SINGLE FUNNEL through CreateCallNode
         * (plan 011 item 1): identical param placement to statement-level
         * calls (out-producers via tail pairing, ObjectConst defaults,
         * StructConst split consumption, pending-wire fallbacks). */
        const TSharedPtr<FJsonObject>& FuncObj = ExprJson->GetObjectField(TEXT("Function"));
        if (!FuncObj.IsValid()) return FPinValue();

        UFunction* Func = ResolveFunction(ExprJson, TEXT(""));
        if (!Func) return FPinValue();

        const TArray<TSharedPtr<FJsonValue>>& Params = ExprJson->GetArrayField(TEXT("Parameters"));
        UK2Node_CallFunction* CallNode = CreateCallNode(Builder, Func, Params, nullptr);
        if (!CallNode)
        {
            return FPinValue();
        }
        CallNode->NodePosX = Builder.NextNodeX - 200;
        CallNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;

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
	else if (Token == TEXT("EX_DynamicCast") || Token == TEXT("EX_Cast"))
	{
		// Dynamic cast is side-effecting (has exec pins): create node with exec wiring
		const TSharedPtr<FJsonObject>& ClassObj = ExprJson->HasField(TEXT("InterfaceClass"))
			? ExprJson->GetObjectField(TEXT("InterfaceClass"))
			: (ExprJson->HasField(TEXT("Class")) ? ExprJson->GetObjectField(TEXT("Class")) : nullptr);
		const TSharedPtr<FJsonObject>& ObjectExpr = ExprJson->HasField(TEXT("Target"))
			? ExprJson->GetObjectField(TEXT("Target"))
			: (ExprJson->HasField(TEXT("ObjectExpression")) ? ExprJson->GetObjectField(TEXT("ObjectExpression")) : nullptr);

		if (!ObjectExpr.IsValid()) return nullptr;

		FPinValue ObjectValue = ResolveExpression(Builder, ObjectExpr);

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Builder.Graph);
		CastNode->CreateNewGuid();
		CastNode->SetFlags(RF_Transactional);
		CastNode->NodePosX = Builder.NextNodeX;
		CastNode->NodePosY = Builder.NextNodeY;
		Builder.NextNodeY += 120;

		if (ClassObj.IsValid())
		{
			UClass* CastClass = ResolveCastClass(ClassObj);
			if (CastClass) CastNode->TargetType = CastClass;
			UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_DynamicCast(Exec): raw ObjectName=%s -> %s"),
				*ClassObj->GetStringField(TEXT("ObjectName")),
				CastClass ? *CastClass->GetPathName() : TEXT("NOT FOUND"));
		}

		Builder.Graph->AddNode(CastNode, true, true);
		CastNode->AllocateDefaultPins();

		// Wire exec input
		if (Builder.LastExecPin)
		{
			UEdGraphPin* ExecPin = FindPin(CastNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
			if (ExecPin) ConnectPins(Builder.LastExecPin, ExecPin);
		}

		// Wire source pin
		if (ObjectValue.Pin)
		{
			UEdGraphPin* SourcePin = CastNode->GetCastSourcePin();
			if (SourcePin) ConnectPins(ObjectValue.Pin, SourcePin);
		}

		// Wire exec output to next statement (success path)
		UEdGraphPin* ThenPin = FindPin(CastNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
		if (ThenPin) Builder.LastExecPin = ThenPin;

		// Wire CastFailed to return node (cast failure = early return)
		if (Builder.ResultNode)
		{
			UEdGraphPin* CastFailedPin = FindPin(CastNode, TEXT("CastFailed"), EGPD_Output);
			if (!CastFailedPin) CastFailedPin = FindPin(CastNode, TEXT("Failed"), EGPD_Output);
			if (CastFailedPin)
			{
				UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
				if (ResultExecPin)
				{
					ConnectPins(CastFailedPin, ResultExecPin);
					UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CastFailed -> Return Node exec wired"));
				}
			}
		}

		return CastNode;
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
        if (!TargetClass)
        {
            // Use ObjectPath from JSON (e.g. "/Script/Engine") + ClassName
            FString FullPath = FString::Printf(TEXT("%s.%s"), *FuncObjectPath, *ClassName);
            TargetClass = FindObject<UClass>(nullptr, *FullPath);
        }
        if (!TargetClass)
        {
            TargetClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassName));
        }
        if (!TargetClass)
        {
            TargetClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *ClassName));
        }
        if (!TargetClass)
        {
            TargetClass = FindFirstObject<UClass>(*ClassName);
        }
        // For engine classes, try LoadClass with ObjectPath as the package
        if (!TargetClass && !FuncObjectPath.IsEmpty())
        {
            FString LoadPath = FString::Printf(TEXT("%s.%s"), *FuncObjectPath, *ClassName);
            TargetClass = LoadObject<UClass>(nullptr, *LoadPath);
        }
        if (!TargetClass && !FuncObjectPath.IsEmpty())
        {
            // Try just the ObjectPath (it may point to the class directly)
            TargetClass = LoadObject<UClass>(nullptr, *FuncObjectPath);
        }
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
        UFunction* FoundFunc = TargetClass->FindFunctionByName(FName(*FuncName));
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("ResolveFunction: class=%s, func=%s, found=%d"), *TargetClass->GetName(), *FuncName, FoundFunc ? 1 : 0);
        return FoundFunc;
    }

    UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("ResolveFunction: no class found for ClassName=%s, ContextClassName=%s"), *ClassName, *ContextClassName);
    return nullptr;
}

// ============================================================================
// Call node creation
// ============================================================================

// ============================================================================
// Variable Registry + temp-resolution helpers (plan 011)
// ============================================================================

void FBlueprintBytecodeImporter::BuildVariableRegistry(const TArray<TSharedPtr<FJsonValue>>& Properties, EVarKind DefaultKind)
{
    for (const TSharedPtr<FJsonValue>& V : Properties)
    {
        const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
        if (!Obj.IsValid()) continue;
        const FString Name = Obj->GetStringField(TEXT("Name"));
        if (Name.IsEmpty()) continue;

        FVarInfo Info;
        Info.PinType = PinTypeFromJson(Obj);
        const FString Flags = Obj->GetStringField(TEXT("PropertyFlags"));
        Info.Kind = Flags.Contains(TEXT("Parm")) ? EVarKind::FunctionParm : DefaultKind;

        // Frame temps are ubergraph-scoped; class members are blueprint-wide.
        // Re-imports overwrite - last writer wins is correct per parse order.
        if (DefaultKind == EVarKind::GraphVariable)
        {
            ClassMemberVars.Add(Name, Info);
        }
        else
        {
            UbergraphFrameLocals.Add(Name, Info);
        }
    }
}

const FBlueprintBytecodeImporter::FVarInfo* FBlueprintBytecodeImporter::FindVariableInfo(const FString& Name, EVarKind& OutKind) const
{
    if (const FVarInfo* M = ClassMemberVars.Find(Name)) { OutKind = M->Kind; return M; }
    if (const FVarInfo* F = UbergraphFrameLocals.Find(Name)) { OutKind = F->Kind; return F; }
    return nullptr;
}

void FBlueprintBytecodeImporter::RegisterProducer(FFunctionBuilder& Builder, const FString& Name, UEdGraphPin* Pin) const
{
    if (!Pin || Name.IsEmpty()) return;
    /* Identity conflict alarm: a temp name must map to exactly one producer
     * pin. A conflicting re-registration means two sites claim the same temp
     * (the old Phase-1 fallback stealing MakeRotator_ReturnValue_1 for
     * SweepHitResult was exactly this shape and silently miswired the graph). */
    if (UEdGraphPin** Existing = Builder.ProducerPins.Find(Name))
    {
        if (*Existing != Pin)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning,
                TEXT("[TempRegistry] CONFLICT: producer '%s' re-registered to pin '%s' (was '%s' on %s)"),
                *Name, *Pin->PinName.ToString(), *(*Existing)->PinName.ToString(),
                (*Existing)->GetOwningNode() ? *(*Existing)->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("<null>"));
            AddDiagnostic(TEXT("ProducerConflict"),
                FString::Printf(TEXT("'%s' re-registered to '%s' (was '%s')"),
                    *Name, *Pin->PinName.ToString(), *(*Existing)->PinName.ToString()));
        }
    }
    Builder.ProducerPins.Add(Name, Pin);
    UE_LOG(LogBlueprintBytecodeImporter, Verbose, TEXT("[TempRegistry] producer '%s' -> %s"), *Name, *Pin->PinName.ToString());
}

UEdGraphPin* FBlueprintBytecodeImporter::FindProducerExact(FFunctionBuilder& Builder, const FString& Name)
{
    if (UEdGraphPin** P = Builder.ProducerPins.Find(Name))
    {
        return *P;
    }

    /* Diagnostic only (plan 011 follow-up B): near-miss names are reported,
     * never auto-wired - heuristic unification (counter-strip, sanitization)
     * could silently merge distinct temps. */
    for (const TPair<FString, UEdGraphPin*>& KV : Builder.ProducerPins)
    {
        const bool bNearMiss =
            KV.Key.Equals(Name, ESearchCase::IgnoreCase)
            || StripStructMemberSuffix(KV.Key).Equals(StripStructMemberSuffix(Name), ESearchCase::IgnoreCase);
        if (bNearMiss)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning,
                TEXT("[TempRegistry] no exact producer for '%s'; nearest registered key is '%s' (diagnostic only - not auto-wired)"),
                *Name, *KV.Key);
            break;
        }
    }
    return nullptr;
}

void FBlueprintBytecodeImporter::ResolvePendingDataWires(FFunctionBuilder& Builder)
{
    int32 Resolved = 0;
    TArray<TPair<UEdGraphPin*, FString>> Leftovers;
    for (const TPair<UEdGraphPin*, FString>& PW : Builder.PendingDataWires)
    {
        UEdGraphPin* Producer = nullptr;
        if (UEdGraphPin** P = Builder.ProducerPins.Find(PW.Value)) { Producer = *P; }
        else { Producer = FindProducerExact(Builder, PW.Value); }

        if (Producer && PW.Key && !PW.Key->LinkedTo.Contains(Producer))
        {
            ConnectPins(Producer, PW.Key);
            ++Resolved;
        }
        else if (!Producer)
        {
            Leftovers.Add(PW);
        }
    }
    Builder.PendingDataWires.Reset();

    if (Resolved > 0)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log,
            TEXT("[PendingDataWires] resolved %d deferred value connections"), Resolved);
    }
    for (const TPair<UEdGraphPin*, FString>& L : Leftovers)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning,
            TEXT("[PendingDataWires] UNRESOLVED temp '%s' (pin %s still unlinked)"),
            *L.Value, L.Key ? *L.Key->PinName.ToString() : TEXT("null"));
        AddDiagnostic(TEXT("UnresolvedWire"),
            FString::Printf(TEXT("temp '%s' -> pin '%s' still unlinked"),
                *L.Value, L.Key ? *L.Key->PinName.ToString() : TEXT("null")));
    }
}

void FBlueprintBytecodeImporter::LogWiringAudit(FFunctionBuilder& Builder, const FString& GraphLabel)
{
    int32 UnlinkedExec = 0;
    for (UEdGraphNode* Node : Builder.Graph->Nodes)
    {
        const UK2Node* K2Node = Cast<UK2Node>(Node);
        if (!K2Node || K2Node->IsA<UK2Node_FunctionEntry>() || K2Node->IsA<UK2Node_Event>()
            || K2Node->IsA<UK2Node_CustomEvent>()) continue;

        for (UEdGraphPin* Pin : K2Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->LinkedTo.Num() == 0 && Pin->PinName == UEdGraphSchema_K2::PN_Execute)
            {
                ++UnlinkedExec;
            }
        }
    }
    UE_LOG(LogBlueprintBytecodeImporter, Log,
        TEXT("[WiringAudit] %s: %d impure node(s) with unlinked execute input (entry points / branch ends are expected)"),
        *GraphLabel, UnlinkedExec);

    /* Diagnostics report (plan 013): bytecode si -> emitted node cross-reference
     * plus the audit result, per graph. */
    TMap<int32, FString>& SiMap = LastImportDiagnostics.StatementNodes.FindOrAdd(GraphLabel);
    for (const TPair<int32, UEdGraphNode*>& Anchor : Builder.StatementAnchors)
    {
        if (Anchor.Value)
        {
            SiMap.Add(Anchor.Key, Anchor.Value->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        }
    }
    LastImportDiagnostics.Add(TEXT("WiringAudit"),
        FString::Printf(TEXT("%s: %d impure node(s) with unlinked execute input"), *GraphLabel, UnlinkedExec));
}

UK2Node_CallFunction* FBlueprintBytecodeImporter::CreateCallNode(FFunctionBuilder& Builder, UFunction* Func, const TArray<TSharedPtr<FJsonValue>>& ParamsJson, UEdGraphPin* TargetPin)
{
    if (!Func)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreateCallNode: Func is null, returning nullptr"));
        return nullptr;
    }

    // Array functions (KismetArrayLibrary::Array_Length / Array_Set / ...) are
    // CustomThunk functions with ArrayParm meta that the editor only exposes as
    // UK2Node_CallArrayFunction. A plain UK2Node_CallFunction leaves the array
    // pin as a wildcard that never resolves its inner type, producing
    // "undetermined type" compile errors. Create the array-specific node instead.
    UK2Node_CallFunction* CallNode = nullptr;
    if (Func->HasMetaData(FName(TEXT("ArrayParm"))))
    {
        CallNode = NewObject<UK2Node_CallArrayFunction>(Builder.Graph);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateCallNode %s: using UK2Node_CallArrayFunction (ArrayParm)"),
            *Func->GetName());
    }
    else
    {
        CallNode = NewObject<UK2Node_CallFunction>(Builder.Graph);
    }
    CallNode->CreateNewGuid();
    CallNode->SetFlags(RF_Transactional);
    CallNode->NodePosX = Builder.NextNodeX - 200;
    CallNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

    // Get the function's owner class
    UClass* OwnerClass = Func->GetOwnerClass();
    if (!OwnerClass) OwnerClass = GeneratedClass;

    // SetFromFunction auto-detects self-context (function on the blueprint's own
    // class -> bSelfContext=True, self pin PinSubCategory="self") and fills the
    // MemberGuid from the function's registered GUID.
    Builder.Graph->AddNode(CallNode, true, true);
    CallNode->SetFromFunction(Func);
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateCallNode %s: owner=%s, selfContext=%d"),
        *Func->GetName(), *OwnerClass->GetName(), CallNode->FunctionReference.IsSelfContext() ? 1 : 0);
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
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateCallNode %s: wired target %s (%s) -> self %s"),
                *Func->GetName(),
                *TargetPin->PinName.ToString(), *TargetPin->PinType.PinCategory.ToString(),
                *SelfPin->PinName.ToString());
        }
        else
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("CreateCallNode %s: no self pin found for target"), *Func->GetName());
        }
    }

    // Wire parameters using name-based matching (order-independent).
    //
    // The JSON Parameters array is in the bytecode's canonical order, but
    // TFieldIterator may return UFunction properties in a different order
    // (e.g. when the K2 compiler reorders stub function properties). Match
    // each JSON param to its call-node pin by name instead of by positional
    // index so the wiring is correct regardless of property order.
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  CreateCallNode %s: wiring %d params (name-based)"),
        *Func->GetName(), ParamsJson.Num());

    // Phase 1: Register out-param result producers.
    // Exact composite-name identity: the compiler names out-temps
    // "CallFunc_<FuncName>_<ParmName>[_N]" deterministically, so each out-only
    // parm is paired with the temp whose normalized name EQUALS that composite
    // key (optional trailing _<digits> duplicate counter stripped). No
    // positional floor/cursor math and no fuzzy suffix matching - those
    // mispaired neighbors (SweepHitResult stealing MakeRotator_ReturnValue_1)
    // and consumed the stolen arg, shifting every later param one slot.
    TSet<int32> ConsumedJsonParams;
    {
        TArray<FProperty*> OutProps;
        for (TFieldIterator<FProperty> It(Func); It; ++It)
        {
            FProperty* Prop = *It;
            if (!(Prop->PropertyFlags & CPF_Parm)) continue;
            if (Prop->PropertyFlags & CPF_ReturnParm) continue;
            if (Prop->GetFName() == TEXT("self")) continue;
            if (Prop->GetFName() == TEXT("__WorldContext")) continue;
            // Only pure OutParm (not by-ref in/out) is a result slot.
            if ((Prop->PropertyFlags & CPF_OutParm) && !(Prop->PropertyFlags & CPF_ReferenceParm))
            {
                OutProps.Add(Prop);
            }
        }

        /* Normalize to alphanumerics only: UDS props carry spaces ('Hair
         * Mesh') while compiler temps embed underscores
         * (CallFunc_Initial_Hair_Hair_Mesh). */
        auto NormKey = [](const FString& In) -> FString
        {
            FString S;
            for (TCHAR Ch : In)
            {
                const bool bAlnum = (Ch >= TEXT('0') && Ch <= TEXT('9'))
                    || (Ch >= TEXT('a') && Ch <= TEXT('z'))
                    || (Ch >= TEXT('A') && Ch <= TEXT('Z'));
                if (bAlnum)
                {
                    S.AppendChar(Ch);
                }
            }
            return S;
        };

        /* Strip one trailing "_<digits>" duplicate counter (CallFunc_X_1 ->
         * CallFunc_X). Only strips when the tail after the last underscore is
         * non-empty and all digits, so names ending in '_' or words are kept. */
        auto StripDupCounter = [](const FString& In) -> FString
        {
            const int32 LastUS = In.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (LastUS == INDEX_NONE || LastUS + 1 >= In.Len()) return In;
            const FString Tail = In.Mid(LastUS + 1);
            for (const TCHAR Ch : Tail)
            {
                if (Ch < TEXT('0') || Ch > TEXT('9')) return In;
            }
            return In.Left(LastUS);
        };

        for (FProperty* Prop : OutProps)
        {
            /* Expected composite key: CallFunc_<Func>_<Parm>, normalized. */
            const FString Expected = FString::Printf(TEXT("CallFunc_%s_%s"), *Func->GetName(), *Prop->GetName());
            const FString ExpectedKey = NormKey(Expected);

            int32 Best = -1;
            FString SlotName;
            for (int32 Idx = 0; Idx < ParamsJson.Num(); ++Idx)
            {
                if (ConsumedJsonParams.Contains(Idx)) continue;
                const TSharedPtr<FJsonObject>& SlotObj = ParamsJson[Idx]->AsObject();
                if (!SlotObj.IsValid() || SlotObj->GetStringField(TEXT("Token")) != TEXT("EX_LocalVariable")) continue;
                const TSharedPtr<FJsonObject>& SV = SlotObj->GetObjectField(TEXT("Variable"));
                const TSharedPtr<FJsonObject>& SP = SV.IsValid() ? SV->GetObjectField(TEXT("Property")) : nullptr;
                const FString Candidate = SP.IsValid() ? SP->GetStringField(TEXT("Name")) : TEXT("");
                if (Candidate.IsEmpty() || !Candidate.StartsWith(TEXT("CallFunc_")))
                {
                    continue;
                }
                if (NormKey(StripDupCounter(Candidate)) == ExpectedKey)
                {
                    Best = Idx;
                    SlotName = Candidate;
                    break;
                }
            }
            if (Best < 0)
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning,
                    TEXT("CreateCallNode %s: no out-temp found for parm '%s' (expected %s[_N])"),
                    *Func->GetName(), *Prop->GetName(), *Expected);
                AddDiagnostic(TEXT("MissingOutTemp"),
                    FString::Printf(TEXT("%s: no out-temp for parm '%s' (expected %s[_N])"), *Func->GetName(), *Prop->GetName(), *Expected));
                continue;
            }

            ConsumedJsonParams.Add(Best);

            UEdGraphPin* OutPin = FindCallNodeOutPinForParam(CallNode, Prop);
            if (OutPin)
            {
                RegisterProducer(Builder, SlotName, OutPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log,
                    TEXT("  -> Registered out-param producer %s from %s (pin %s)"),
                    *SlotName, *Prop->GetName(), *OutPin->PinName.ToString());
            }
        }
    }

    // Phase 2: Wire input params (JSON-params-first, name-based matching).

    /* Bytecode emits call args in the function's declaration order. Build the
     * full ordered input-slot list (including hidden context parms) so unnamed
     * args - inline literals AND structured reads (EX_StructMemberContext /
     * EX_ArrayGetByRef / EX_Context chains) - can be placed positionally.
     * Return-value slots are excluded: their result temps are matched by the
     * producer registration above and never consume an input slot. */
    TArray<FProperty*> ParmSlots;
    for (TFieldIterator<FProperty> It(Func); It; ++It)
    {
        FProperty* Prop = *It;
        if (!(Prop->PropertyFlags & CPF_Parm)) continue;
        if (Prop->PropertyFlags & CPF_ReturnParm) continue;
        ParmSlots.Add(Prop);
    }
    int32 PosSlot = 0;

    for (int32 i = 0; i < ParamsJson.Num(); ++i)
    {
        if (ConsumedJsonParams.Contains(i)) continue;

        const TSharedPtr<FJsonObject>& ParamExpr = ParamsJson[i]->AsObject();
        if (!ParamExpr.IsValid()) continue;

        const FString ParamToken = ParamExpr->GetStringField(TEXT("Token"));

        // Extract the param name from the expression. EX_LocalVariable and
        // EX_InstanceVariable carry Variable.Property.Name which matches the
        // call-node pin name.
        FString ParamName;
        if (ParamToken == TEXT("EX_LocalVariable") || ParamToken == TEXT("EX_InstanceVariable"))
        {
            const TSharedPtr<FJsonObject>& VarObj = ParamExpr->GetObjectField(TEXT("Variable"));
            if (VarObj.IsValid())
            {
                const TSharedPtr<FJsonObject>& InnerVar = VarObj->HasField(TEXT("Variable"))
                    ? VarObj->GetObjectField(TEXT("Variable")) : VarObj;
                if (InnerVar.IsValid() && InnerVar->HasField(TEXT("Property")))
                {
                    const TSharedPtr<FJsonObject>& PropObj = InnerVar->GetObjectField(TEXT("Property"));
                    if (PropObj.IsValid())
                    {
                        ParamName = PropObj->GetStringField(TEXT("Name"));
                    }
                }
            }
        }

        /* EX_StructConst-fed frame temps consumed as struct-by-ref arguments
         * (plan 010 item 3): turn the recorded literal fields into split-subpin
         * defaults on the slot - e.g. ModifyContextOptions on AddMappingContext. */
        if (ParamName.IsEmpty() && ParamToken == TEXT("EX_LocalVariable") && PosSlot < ParmSlots.Num())
        {
            FString ArgTemp;
            {
                const TSharedPtr<FJsonObject>& AV = ParamExpr->GetObjectField(TEXT("Variable"));
                const TSharedPtr<FJsonObject>& AIV = AV.IsValid() ? (AV->HasField(TEXT("Variable")) ? AV->GetObjectField(TEXT("Variable")) : AV) : AV;
                const TSharedPtr<FJsonObject>& AP = AIV.IsValid() ? AIV->GetObjectField(TEXT("Property")) : AIV;
                if (AP.IsValid()) ArgTemp = AP->GetStringField(TEXT("Name"));
            }
            if (!ArgTemp.IsEmpty() && Builder.TempStructFields.Contains(ArgTemp))
            {
                UEdGraphPin* StructSlot = FindPin(CallNode, *ParmSlots[PosSlot]->GetName(), EGPD_Input);
                ++PosSlot;
                if (StructSlot && StructSlot->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                {
                    TMap<FString, FString>* Fields = Builder.TempStructFields.Find(ArgTemp);
                    StructSlot->DefaultValue = TEXT("()");
                    GetMutableDefault<UEdGraphSchema_K2>()->SplitPin(StructSlot);
                    const bool bSplit = StructSlot->SubPins.Num() > 0;
                    if (bSplit)
                    {
                        for (UEdGraphPin* SubPin : StructSlot->SubPins)
                        {
                            FString* Found = Fields ? Fields->Find(StripStructMemberSuffix(SubPin->PinName.ToString())) : nullptr;
                            if (Found) SubPin->DefaultValue = *Found;
                        }
                        StructSlot->DefaultValue = TEXT("()");
                    }
                    UE_LOG(LogBlueprintBytecodeImporter, Log,
                        TEXT("  -> Param idx %d: struct literal %s applied to %s (%s, %d fields)"),
                        i, *ArgTemp, *StructSlot->PinName.ToString(),
                        bSplit ? TEXT("split subpins") : TEXT("collapsed ()"), Fields ? Fields->Num() : 0);
                }
                continue;
            }
        }

        // EX_ObjectConst: place the constant where the original graph kept it.
        //  - visible object/class pin -> pin DefaultObject (IMC asset, mesh,
        //    material, native subsystem class...);
        //  - hidden self/WorldContext fed by a /Game BP-library CDO -> Literal
        //    node wired in (original shows K2Node_Literal -> hidden self);
        //  - Class'Default__' engine-lib CDOs keep the legacy skip (auto-filled).
        if (ParamName.IsEmpty() && ParamToken == TEXT("EX_ObjectConst"))
        {
            UObject* ConstObj = nullptr;
            bool bEngineLibCdo = false;
            {
                const TSharedPtr<FJsonObject>* ValuePtr = nullptr;
                if (ParamExpr->TryGetObjectField(TEXT("Value"), ValuePtr) && ValuePtr)
                {
                    ConstObj = ResolveObjectConstValue(*ValuePtr);
                    /* Only Default__ CDO refs are auto-filled engine-library
                     * selves. A plain Class'X' const on a /Script/ path is a
                     * REAL class argument (e.g. the LocalPlayerSubsystem
                     * class passed to GetLocalPlayerSubSystemFromPlayerController)
                     * and must be placed. */
                    FString ObjName = (*ValuePtr)->GetStringField(TEXT("ObjectName"));
                    bEngineLibCdo = ObjName.StartsWith(TEXT("Class'Default__"));
                }
            }

            UEdGraphPin* Positional = nullptr;
            if (PosSlot < ParmSlots.Num())
            {
                Positional = FindPin(CallNode, *ParmSlots[PosSlot]->GetName(), EGPD_Input);
            }
            ++PosSlot;

            if (ConstObj)
            {
                /* Candidate selection: positional slot first, but hidden
                 * self/WorldContext parms can occupy earlier positions - fall
                 * back to the first visible, unlinked, type-compatible input. */
                auto SlotTakesDefault = [this, ConstObj](UEdGraphPin* P) -> bool
                {
                    if (!P || P->bHidden || P->LinkedTo.Num() > 0) return false;
                    UClass* WantClass = Cast<UClass>(P->PinType.PinSubCategoryObject.Get());
                    if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
                    {
                        UClass* AsClass = Cast<UClass>(ConstObj);
                        return AsClass != nullptr && (!WantClass || AsClass->IsChildOf(WantClass));
                    }
                    return WantClass ? ConstObj->IsA(WantClass) : true;
                };

                UEdGraphPin* Target = SlotTakesDefault(Positional) ? Positional : nullptr;
                if (!Target)
                {
                    for (FProperty* Prop : ParmSlots)
                    {
                        UEdGraphPin* Candidate = FindPin(CallNode, *Prop->GetName(), EGPD_Input);
                        if (SlotTakesDefault(Candidate))
                        {
                            Target = Candidate;
                            break;
                        }
                    }
                }

                if (Target)
                {
                    Target->DefaultObject = ConstObj;
                    UE_LOG(LogBlueprintBytecodeImporter, Log,
                        TEXT("  -> Param idx %d: object const %s placed as DefaultObject on pin %s"),
                        i, *ConstObj->GetName(), *Target->PinName.ToString());
                    continue;
                }

                // Hidden BP-library-CDO self -> Literal node (original Literal_N shape)
                if (!bEngineLibCdo && Positional && Positional->bHidden && Positional->LinkedTo.Num() == 0)
                {
                    UK2Node_Literal* LitNode = NewObject<UK2Node_Literal>(Builder.Graph);
                    LitNode->CreateNewGuid();
                    LitNode->SetFlags(RF_Transactional);
                    LitNode->NodePosX = Builder.NextNodeX - 200;
                    LitNode->NodePosY = Builder.NextNodeY;
                    Builder.NextNodeY += 120;
                    Builder.Graph->AddNode(LitNode, true, true);
                    LitNode->SetObjectRef(ConstObj);
                    LitNode->AllocateDefaultPins();
                    if (UEdGraphPin* LitOut = LitNode->GetValuePin())
                    {
                        ConnectPins(LitOut, Positional);
                        UE_LOG(LogBlueprintBytecodeImporter, Log,
                            TEXT("  -> Param idx %d: BP CDO literal %s wired to hidden pin %s"),
                            i, *ConstObj->GetName(), *Positional->PinName.ToString());
                        continue;
                    }
                }
            }

            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param idx %d: skipping EX_ObjectConst (auto-filled hidden / unresolved)"),
                i);
            continue;
        }

        // EX_Self / EX_NoObject map to hidden context slots: the engine fills
        // these at compile time. Skip wiring, but still OCCUPY the positional
        // slot so later unnamed args stay aligned with parameter order.
        if (ParamName.IsEmpty() && (ParamToken == TEXT("EX_Self") || ParamToken == TEXT("EX_NoObject")))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param idx %d: skipping %s (auto-filled hidden)"),
                i, *ParamToken);
            if (PosSlot < ParmSlots.Num())
            {
                ++PosSlot;
            }
            continue;
        }

        if (ParamName.IsEmpty())
        {
            /* Bytecode args carry no name unless they are locals. Place the
             * expression positionally: constants become default values,
             * structured reads resolve to producer pins and connect. */
            UEdGraphPin* SlotPin = nullptr;
            if (PosSlot < ParmSlots.Num())
            {
                SlotPin = FindPin(CallNode, *ParmSlots[PosSlot]->GetName(), EGPD_Input);
                ++PosSlot;
            }
            if (!SlotPin || SlotPin->bHidden)
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param idx %d: token %s has no placeable slot, skipping"),
                    i, *ParamToken);
                AddDiagnostic(TEXT("NoSlot"),
                    FString::Printf(TEXT("%s: arg idx %d (%s) has no placeable slot"), *Func->GetName(), i, *ParamToken));
                continue;
            }

            /* Native struct consts (LinearColor etc.) are PIN DEFAULTS in the
             * original graph - the compiler inlines them as the argument.
             * Serializing onto the slot keeps that shape; emitting a MakeStruct
             * node strands an external node per call (08.25: five Make
             * LinearColor nodes for five PrintStrings carrying the same color).
             * UserDefinedStruct consts keep their node/split-pin paths. */
            if (ParamToken == TEXT("EX_StructConst"))
            {
                const TSharedPtr<FJsonObject>* StructPtr = nullptr;
                FString StructPath;
                if (ParamExpr->TryGetObjectField(TEXT("Struct"), StructPtr) && StructPtr && StructPtr->IsValid())
                {
                    StructPath = (*StructPtr)->GetStringField(TEXT("ObjectPath"));
                }
                if (StructPath.StartsWith(TEXT("/Script/")))
                {
                    FString StructObjectName = (*StructPtr)->GetStringField(TEXT("ObjectName"));
                    StructObjectName.RemoveFromStart(TEXT("Class'"));
                    StructObjectName.RemoveFromEnd(TEXT("'"));
                    UScriptStruct* NativeStruct = ResolveUserDefinedStruct(StructObjectName, StructPath);
                    const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
                    if (NativeStruct && ParamExpr->TryGetArrayField(TEXT("Properties"), Members) && Members)
                    {
                        FString Text = TEXT("(");
                        bool bAllNumeric = true;
                        int32 FieldIdx = 0;
                        for (TFieldIterator<FProperty> FieldIt(NativeStruct); FieldIt; ++FieldIt, ++FieldIdx)
                        {
                            if (FieldIdx >= Members->Num()) { bAllNumeric = false; break; }
                            const TSharedPtr<FJsonObject> Member = (*Members)[FieldIdx]->AsObject();
                            const FString MemberTok = Member.IsValid() ? Member->GetStringField(TEXT("Token")) : FString();
                            if (Member.IsValid()
                                && (MemberTok == TEXT("EX_FloatConst") || MemberTok == TEXT("EX_DoubleConst")
                                    || MemberTok == TEXT("EX_IntConst") || MemberTok == TEXT("EX_ByteConst")))
                            {
                                const double Val = Member->HasField(TEXT("Value")) ? Member->GetNumberField(TEXT("Value")) : 0.0;
                                if (FieldIdx > 0) Text += TEXT(",");
                                Text += FString::Printf(TEXT("%s=%f"), *FieldIt->GetName(), Val);
                            }
                            else
                            {
                                bAllNumeric = false;
                                break;
                            }
                        }
                        Text += TEXT(")");
                        if (bAllNumeric)
                        {
                            SetPinDefaultValueSafe(SlotPin, Text);
                            UE_LOG(LogBlueprintBytecodeImporter, Log,
                                TEXT("  -> Param idx %d: native struct const %s placed as default on pin %s = '%s'"),
                                i, *StructObjectName, *SlotPin->PinName.ToString(), *Text);
                            continue;
                        }
                    }
                }
            }

            FPinValue SlotValue = ResolveExpression(Builder, ParamExpr);
            if (SlotValue.Pin)
            {
                ConnectPins(SlotValue.Pin, SlotPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param idx %d: %s connected positionally to pin %s"),
                    i, *ParamToken, *SlotPin->PinName.ToString());
            }
            else if (SlotValue.bConstant)
            {
                SetPinDefaultValueSafe(SlotPin, SlotValue.ConstString);
                // Enum (byte) pins must store the enumerator NAME, not the raw index
                if (SlotPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && SlotPin->PinType.PinSubCategoryObject.IsValid())
                {
                    if (UEnum* Enum = Cast<UEnum>(SlotPin->PinType.PinSubCategoryObject.Get()))
                    {
                        const FString EnumName = Enum->GetNameStringByIndex(FCString::Atoi(*SlotValue.ConstString));
                        if (!EnumName.IsEmpty())
                        {
                            SlotPin->DefaultValue = EnumName;
                        }
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param idx %d: literal %s placed positionally on pin %s = '%s'"),
                    i, *ParamToken, *SlotPin->PinName.ToString(), *SlotValue.ConstString);
            }
            else
            {
                /* PendingDataWires (plan 011 item 2): a temp whose producer was
                 * not emitted YET - queue and resolve after the full walk. */
                if (ParamExpr->GetStringField(TEXT("Token")) == TEXT("EX_LocalVariable"))
                {
                    const TSharedPtr<FJsonObject>& PV = ParamExpr->GetObjectField(TEXT("Variable"));
                    const TSharedPtr<FJsonObject>& PIV = PV.IsValid() ? (PV->HasField(TEXT("Variable")) ? PV->GetObjectField(TEXT("Variable")) : PV) : PV;
                    const TSharedPtr<FJsonObject>& PP = PIV.IsValid() ? PIV->GetObjectField(TEXT("Property")) : PIV;
                    if (PP.IsValid())
                    {
                        Builder.PendingDataWires.Add(TPair<UEdGraphPin*, FString>(SlotPin, PP->GetStringField(TEXT("Name"))));
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  -> Param idx %d: token %s unresolved for pin %s (queued)"),
                    i, *ParamToken, *SlotPin->PinName.ToString());
            }
            continue;
        }

        // Skip internal names
        if (ParamName == TEXT("self") || ParamName == TEXT("__WorldContext"))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s: skipping internal name"), *ParamName);
            continue;
        }

        UEdGraphPin* ParamPin = FindPin(CallNode, *ParamName, EGPD_Input);
        if (!ParamPin)
        {
            /* Result temps (registered producers) can also FEED value slots -
             * e.g. GetMousePosition's out locals passed straight into
             * MakeVector2D. Connect the producer pin to the next positional
             * slot instead of dropping the arg. */
            if (UEdGraphPin** ProducerPin = Builder.ProducerPins.Find(ParamName))
            {
                UEdGraphPin* SlotPin = nullptr;
                if (PosSlot < ParmSlots.Num())
                {
                    SlotPin = FindPin(CallNode, *ParmSlots[PosSlot]->GetName(), EGPD_Input);
                    if (!SlotPin || SlotPin->bHidden ||
                        SlotPin->PinType.PinCategory != (*ProducerPin)->PinType.PinCategory)
                    {
                        ++PosSlot;
                        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s: producer type %s does not fit slot %d, advancing"),
                            *ParamName, *(*ProducerPin)->PinType.PinCategory.ToString(), PosSlot);
                        continue;
                    }
                    ++PosSlot;
                    ConnectPins(*ProducerPin, SlotPin);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s: producer pin wired positionally to %s"),
                        *ParamName, *SlotPin->PinName.ToString());
                }
                continue;
            }

            /* Bytecode temps rarely match engine pin names ("Temp_int_Array_
             * Index_Variable" feeds "Index"): resolve the expression into the
             * next positional slot instead of dropping the arg - locals emit
             * VariableGets, contexts chain targets, constants become defaults. */
            UEdGraphPin* SlotPin = nullptr;
            if (PosSlot < ParmSlots.Num())
            {
                SlotPin = FindPin(CallNode, *ParmSlots[PosSlot]->GetName(), EGPD_Input);
                ++PosSlot;
            }
            if (!SlotPin || SlotPin->bHidden)
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  -> Param %s (%s): no placeable slot on call node"),
                    *ParamName, *ParamToken);
                AddDiagnostic(TEXT("NoSlot"),
                    FString::Printf(TEXT("%s: param '%s' (%s) has no placeable slot"), *Func->GetName(), *ParamName, *ParamToken));
                continue;
            }
            FPinValue SlotValue = ResolveExpression(Builder, ParamExpr);
            if (SlotValue.Pin)
            {
                ConnectPins(SlotValue.Pin, SlotPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s (%s): resolved positionally onto %s"),
                    *ParamName, *ParamToken, *SlotPin->PinName.ToString());
            }
            else if (SlotValue.bConstant)
            {
                SetPinDefaultValueSafe(SlotPin, SlotValue.ConstString);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s (%s): constant '%s' onto %s"),
                    *ParamName, *ParamToken, *SlotValue.ConstString, *SlotPin->PinName.ToString());
            }
            else
            {
                /* Named struct-by-ref arg fed by a recorded EX_StructConst
                 * temp (plan 010 item 3): apply split-subpin defaults on the
                 * first unlinked struct input slot instead of queuing. */
                if (ParamName.StartsWith(TEXT("Temp_")))
                {
                    TMap<FString, FString>* Fields = Builder.TempStructFields.Find(ParamName);
                    if (Fields)
                    {
                        UEdGraphPin* StructSlot = nullptr;
                        for (UEdGraphPin* Pin : CallNode->Pins)
                        {
                            if (Pin && Pin->Direction == EGPD_Input && !Pin->bHidden
                                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct
                                && Pin->LinkedTo.Num() == 0)
                            {
                                StructSlot = Pin;
                                break;
                            }
                        }
                        if (StructSlot)
                        {
                            StructSlot->DefaultValue = TEXT("()");
                            GetMutableDefault<UEdGraphSchema_K2>()->SplitPin(StructSlot);
                            const bool bSplit = StructSlot->SubPins.Num() > 0;
                            if (bSplit)
                            {
                                for (UEdGraphPin* SubPin : StructSlot->SubPins)
                                {
                                    FString* Found = Fields->Find(StripStructMemberSuffix(SubPin->PinName.ToString()));
                                    if (Found) SubPin->DefaultValue = *Found;
                                }
                                StructSlot->DefaultValue = TEXT("()");
                            }
                            UE_LOG(LogBlueprintBytecodeImporter, Log,
                                TEXT("  -> Param %s: struct literal applied to %s (%s, %d fields)"),
                                *ParamName, *StructSlot->PinName.ToString(),
                                bSplit ? TEXT("split subpins") : TEXT("collapsed ()"), Fields->Num());
                            continue;
                        }
                    }
                }
                if (ParamToken == TEXT("EX_LocalVariable"))
                {
                    const TSharedPtr<FJsonObject>& NV = ParamExpr->GetObjectField(TEXT("Variable"));
                    const TSharedPtr<FJsonObject>& NIV = NV.IsValid() ? (NV->HasField(TEXT("Variable")) ? NV->GetObjectField(TEXT("Variable")) : NV) : NV;
                    const TSharedPtr<FJsonObject>& NP = NIV.IsValid() ? NIV->GetObjectField(TEXT("Property")) : NIV;
                    if (NP.IsValid())
                    {
                        Builder.PendingDataWires.Add(TPair<UEdGraphPin*, FString>(SlotPin, NP->GetStringField(TEXT("Name"))));
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  -> Param %s (%s): unresolved for pin %s (queued)"),
                    *ParamName, *ParamToken, *SlotPin->PinName.ToString());
            }
            continue;
        }

        // Keep the positional cursor aligned with successfully wired named args.
        for (int32 V = 0; V < ParmSlots.Num(); ++V)
        {
            if (ParmSlots[V]->GetFName() == ParamPin->PinName)
            {
                PosSlot = V + 1;
                break;
            }
        }

        // Hidden pin + EX_Self: engine-filled, skip.
        if (ParamPin->bHidden && ParamToken == TEXT("EX_Self"))
        {
            continue;
        }

        FPinValue ParamValue = ResolveExpression(Builder, ParamExpr);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Param %s (idx %d): token=%s, resolvedPin=%s (%s), const=%d [%s]"),
            *ParamName, i, *ParamToken,
            ParamValue.Pin ? *ParamValue.Pin->PinName.ToString() : TEXT("NULL"),
            ParamValue.Pin ? *ParamValue.Pin->PinType.PinCategory.ToString() : TEXT(""),
            (int32)ParamValue.bConstant, *ParamValue.ConstString);
        if (ParamValue.Pin)
        {
            ConnectPins(ParamValue.Pin, ParamPin);
        }
        else if (ParamValue.bConstant)
        {
            SetPinDefaultValueSafe(ParamPin, ParamValue.ConstString);
            // Enum (byte) pins must store the enumerator NAME, not the raw index
            if (ParamPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && ParamPin->PinType.PinSubCategoryObject.IsValid())
            {
                if (UEnum* Enum = Cast<UEnum>(ParamPin->PinType.PinSubCategoryObject.Get()))
                {
                    const FString EnumName = Enum->GetNameStringByIndex(FCString::Atoi(*ParamValue.ConstString));
                    if (!EnumName.IsEmpty())
                    {
                        ParamPin->DefaultValue = EnumName;
                    }
                }
            }
        }
    }

    // Post-loop snapshot for ArrayParm nodes: the array pin's type propagates to
    // dependent pins (e.g. Array_Set.Item) when it is connected. Log each input
    // pin's final type and link count so a dropped/wildcard dependent pin is
    // visible in the log before the graph is saved.
    if (Func->HasMetaData(FName(TEXT("ArrayParm"))))
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateCallNode %s: post-loop snapshot (%d pins):"),
            *Func->GetName(), CallNode->Pins.Num());
        for (UEdGraphPin* SnapPin : CallNode->Pins)
        {
            if (!SnapPin) continue;
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  pin %s dir=%d cat=%s subcat=%s obj=%s ref=%d const=%d links=%d"),
                *SnapPin->PinName.ToString(), (int32)SnapPin->Direction,
                *SnapPin->PinType.PinCategory.ToString(), *SnapPin->PinType.PinSubCategory.ToString(),
                SnapPin->PinType.PinSubCategoryObject.IsValid() ? *SnapPin->PinType.PinSubCategoryObject->GetName() : TEXT("null"),
                (int32)SnapPin->PinType.bIsReference, (int32)SnapPin->PinType.bIsConst, SnapPin->LinkedTo.Num());
        }
    }

    return CallNode;
}

// ============================================================================
// Variable node creation
// ============================================================================

UK2Node_VariableGet* FBlueprintBytecodeImporter::CreateVariableGet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass, const TSharedPtr<FJsonObject>& PropObj)
{
    /* Pure-read reuse (plan 013): return the cached GET node for repeat reads
     * of the same variable - VariableGet is pure, so reuse is semantically
     * identical and matches the original graph shape (one GET feeding a Break
     * whose members fan out) instead of stranding unwired duplicates. */
    if (UK2Node_VariableGet** Cached = Builder.VariableGetNodes.Find(VarName))
    {
        if (*Cached && (*Cached)->Pins.Num() > 0)
        {
            return *Cached;
        }
        Builder.VariableGetNodes.Remove(VarName);
    }

    UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Builder.Graph);
    GetNode->CreateNewGuid();
    GetNode->SetFlags(RF_Transactional);
    GetNode->NodePosX = Builder.NextNodeX - 400;
    GetNode->NodePosY = Builder.NextNodeY;
    
    UClass* VarClass = OwnerClass ? OwnerClass : GeneratedClass;
    FGuid VarGuid;
    
    // Try property table first
    UBlueprint::GetGuidFromClassByFieldName<FProperty>(VarClass, FName(*VarName), VarGuid);
    
    // If not found, try SCS nodes (components like "Pubic hair" are stored here)
    if (!VarGuid.IsValid() && Blueprint && Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->GetVariableName() == FName(*VarName))
            {
                VarGuid = SCSNode->VariableGuid;
                break;
            }
        }
    }
    
    // A declared local of this function resolves against the function UFunction
    // (scope name = function name), so pins are created from the local FProperty.
    if (Builder.FunctionLocalNames.Contains(VarName))
    {
        GetNode->VariableReference.SetLocalMember(FName(*VarName), Builder.Func->Name, Builder.FunctionLocalGuids.FindRef(VarName));
    }
    else if (VarClass == GeneratedClass)
    {
        if (VarGuid.IsValid())
        {
            GetNode->VariableReference.SetSelfMember(FName(*VarName), VarGuid);
        }
        else
        {
            GetNode->VariableReference.SetSelfMember(FName(*VarName));
        }
    }
    else
    {
        if (VarGuid.IsValid())
        {
            GetNode->VariableReference.SetExternalMember(FName(*VarName), VarClass, VarGuid);
        }
        else
        {
            GetNode->VariableReference.SetExternalMember(FName(*VarName), VarClass);
        }
    }
    
    Builder.Graph->AddNode(GetNode, true, true);
    GetNode->AllocateDefaultPins();
    Builder.NextNodeY += 120;

    /* A freshly imported blueprint's skeleton class has no properties yet, so
     * AllocateDefaultPins creates no pins for self/external members (the variable
     * reference can only be resolved against the generated class once the skeleton
     * is recompiled). Create the output pin (and a self pin for member access)
     * directly from the property JSON so downstream consumers can be wired. */
    UEdGraphPin* OutPin = FindPin(GetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
    if (!OutPin) OutPin = FindPin(GetNode, *VarName, EGPD_Output);
    if (!OutPin && PropObj.IsValid())
    {
        OutPin = CreatePinForJsonProperty(GetNode, EGPD_Output, PropObj, FName(*VarName));
        if (OutPin)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("CreateVariableGet: created fallback output pin '%s' (%s)"), *VarName, *OutPin->PinType.PinCategory.ToString());
        }
    }

    if (OutPin && !Builder.FunctionLocalNames.Contains(VarName))
    {
        UEdGraphPin* SelfPin = FindPin(GetNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input);
        if (!SelfPin) SelfPin = FindPin(GetNode, TEXT("self"), EGPD_Input);
        if (!SelfPin)
        {
            UEdGraphPin* NewSelfPin = GetNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, FName(), VarClass, UEdGraphSchema_K2::PN_Self);
            if (NewSelfPin)
            {
                NewSelfPin->PinFriendlyName = NSLOCTEXT("K2Node", "Target", "Target");
                if (VarClass == GeneratedClass)
                {
                    NewSelfPin->bHidden = true;
                }
            }
        }
    }

    Builder.VariableGetNodes.Add(VarName, GetNode);
    return GetNode;
}

UK2Node_VariableSet* FBlueprintBytecodeImporter::CreateVariableSet(FFunctionBuilder& Builder, const FString& VarName, UClass* OwnerClass)
{
    UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(Builder.Graph);
    SetNode->CreateNewGuid();
    SetNode->SetFlags(RF_Transactional);
    SetNode->NodePosX = Builder.NextNodeX - 200;
    SetNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;
    
    UClass* VarClass = OwnerClass ? OwnerClass : GeneratedClass;
    FGuid VarGuid;
    
    UBlueprint::GetGuidFromClassByFieldName<FProperty>(VarClass, FName(*VarName), VarGuid);
    
    if (!VarGuid.IsValid() && Blueprint && Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* SCSNode : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (SCSNode && SCSNode->GetVariableName() == FName(*VarName))
            {
                VarGuid = SCSNode->VariableGuid;
                break;
            }
        }
    }
    
    if (Builder.FunctionLocalNames.Contains(VarName))
    {
        SetNode->VariableReference.SetLocalMember(FName(*VarName), Builder.Func->Name, Builder.FunctionLocalGuids.FindRef(VarName));
    }
    else if (VarClass == GeneratedClass)
    {
        if (VarGuid.IsValid())
        {
            SetNode->VariableReference.SetSelfMember(FName(*VarName), VarGuid);
        }
        else
        {
            SetNode->VariableReference.SetSelfMember(FName(*VarName));
        }
    }
    else
    {
        if (VarGuid.IsValid())
        {
            SetNode->VariableReference.SetExternalMember(FName(*VarName), VarClass, VarGuid);
        }
        else
        {
            SetNode->VariableReference.SetExternalMember(FName(*VarName), VarClass);
        }
    }
    
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
            if (ObjExpr->GetStringField(TEXT("Token")) == TEXT("EX_InterfaceContext"))
            {
                /* Calling through an interface (e.g.
                 * EnhancedInputSubsystemInterface::AddMappingContext on the
                 * subsystem net) - resolve the implementing object's net. */
                const TSharedPtr<FJsonObject>* IfaceValPtr = nullptr;
                if (ObjExpr->TryGetObjectField(TEXT("InterfaceValue"), IfaceValPtr) && IfaceValPtr)
                {
                    FPinValue IfaceValue = ResolveExpression(Builder, *IfaceValPtr);
                    if (IfaceValue.Pin)
                    {
                        TargetPin = IfaceValue.Pin;
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Log,
                    TEXT("EmitContextCall si=%d: EX_InterfaceContext target resolved=%d"),
                    Stmt.StatementIndex, TargetPin ? 1 : 0);
            }
            else
            {
                FPinValue TargetValue = ResolveExpression(Builder, ObjExpr);
                if (TargetValue.Pin)
                {
                    TargetPin = TargetValue.Pin;
                }
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitContextCall si=%d: object token=%s, targetPin=%d (%s)"),
                    Stmt.StatementIndex,
                    *ObjExpr->GetStringField(TEXT("Token")),
                    TargetPin ? 1 : 0,
                    TargetPin ? *TargetPin->PinName.ToString() : TEXT("none"));
            }
        }
    }

    // Resolve the function call. NOTE: FJsonObject::GetObjectField never returns
    // null in UE5.7 for a missing field (GetField yields FJsonValueNull whose
    // AsObject() is a static empty object), so IsValid() is always true. Use
    // TryGetObjectField so a truly absent ContextExpression yields null.
    const TSharedPtr<FJsonObject>* ContextExprField = nullptr;
    const bool bHasContextExpr = Json->TryGetObjectField(TEXT("ContextExpression"), ContextExprField);
    const TSharedPtr<FJsonObject> ContextExpr = (bHasContextExpr && ContextExprField) ? *ContextExprField : TSharedPtr<FJsonObject>();

    // Bare statement-level virtual function (e.g. a call to a local custom event
    // like "Update All Character INFO") has no ContextExpression wrapper - the
    // function name and parameters sit directly on the statement JSON.
    const bool bBareVirtualFunction = !ContextExpr.IsValid() && Json->HasField(TEXT("Function"));
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitContextCall si=%d: hasObjExpr=%d, hasCtxExpr=%d, hasFunction=%d, bBareVirtualFunction=%d"),
        Stmt.StatementIndex,
        Json->HasField(TEXT("ObjectExpression")) ? 1 : 0,
        Json->HasField(TEXT("ContextExpression")) ? 1 : 0,
        Json->HasField(TEXT("Function")) ? 1 : 0,
        bBareVirtualFunction ? 1 : 0);
    if (!ContextExpr.IsValid() && !bBareVirtualFunction) return nullptr;

    const FString CallToken = ContextExpr.IsValid() ? ContextExpr->GetStringField(TEXT("Token")) : TEXT("");

    UFunction* Func = nullptr;
    TArray<TSharedPtr<FJsonValue>> ParamsJson;
    FString ResolvedFuncName;

    if (bBareVirtualFunction)
    {
        // Bare statement-level virtual function: name + params sit directly on
        // the statement JSON (no ContextExpression wrapper).
        ResolvedFuncName = Json->GetStringField(TEXT("Function"));
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Bare VirtualFunction: %s"), *ResolvedFuncName);
        Func = GeneratedClass->FindFunctionByName(FName(*ResolvedFuncName));
        if (Func)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Found in GeneratedClass: %s"), *GeneratedClass->GetName());
        }
        else
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  NOT found in GeneratedClass: %s"), *GeneratedClass->GetName());
        }
        if (Json->HasField(TEXT("Parameters")))
        {
            ParamsJson = Json->GetArrayField(TEXT("Parameters"));
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Bare VirtualFunction Params count: %d"), ParamsJson.Num());
        }
    }
    else if (CallToken == TEXT("EX_FinalFunction") || CallToken == TEXT("EX_LocalFinalFunction"))
    {
        Func = ResolveFunction(ContextExpr, TEXT(""));
        const TSharedPtr<FJsonObject>& FuncObj = ContextExpr->GetObjectField(TEXT("Function"));
        if (FuncObj.IsValid())
        {
            FString FuncObjectName = FuncObj->GetStringField(TEXT("ObjectName"));
            int32 ColonIdx;
            if (FuncObjectName.FindChar(TEXT(':'), ColonIdx))
            {
                ResolvedFuncName = FuncObjectName.Mid(ColonIdx + 1);
            }
            ResolvedFuncName.RemoveFromEnd(TEXT("'"));
        }
    }
    else if (CallToken == TEXT("EX_VirtualFunction") || CallToken == TEXT("EX_LocalVirtualFunction"))
    {
        // Virtual functions - use the function name
        ResolvedFuncName = ContextExpr->GetStringField(TEXT("Function"));
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EX_VirtualFunction: %s"), *ResolvedFuncName);
        
        // Try to find in GeneratedClass first
        Func = GeneratedClass->FindFunctionByName(FName(*ResolvedFuncName));
        if (Func)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Found in GeneratedClass: %s"), *GeneratedClass->GetName());
        }
        
        // If not found, try resolving from the ObjectExpression's resolved object
        if (!Func && Json->HasField(TEXT("ObjectExpression")))
        {
            const TSharedPtr<FJsonObject>& ObjExpr = Json->GetObjectField(TEXT("ObjectExpression"));
            if (ObjExpr.IsValid())
            {
                // Direct load from ObjectConst JSON (do NOT call ResolveExpression here -
                // it would create a duplicate literal node)
                UObject* ResolvedObj = nullptr;
                if (ObjExpr->HasField(TEXT("Value")))
                {
                    const TSharedPtr<FJsonObject>& ValueObj = ObjExpr->GetObjectField(TEXT("Value"));
                    FString ObjName = ValueObj->GetStringField(TEXT("ObjectName"));
                    FString ObjPath = ValueObj->HasField(TEXT("ObjectPath")) ? ValueObj->GetStringField(TEXT("ObjectPath")) : TEXT("");

                    // Try loading the object directly
                    ResolvedObj = LoadObject<UObject>(nullptr, *ObjPath);
                    if (!ResolvedObj)
                    {
                        // Try stripping .N suffix
                        int32 DotIdx;
                        if (ObjPath.FindChar(TEXT('.'), DotIdx))
                        {
                            FString BasePath = ObjPath.Left(DotIdx);
                            ResolvedObj = LoadObject<UObject>(nullptr, *BasePath);
                        }
                    }
                }

                // Get UClass from resolved object
                UClass* ObjClass = nullptr;
                if (UBlueprint* BP = Cast<UBlueprint>(ResolvedObj))
                {
                    ObjClass = BP->GeneratedClass;
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Got GeneratedClass from Blueprint: %s"), ObjClass ? *ObjClass->GetName() : TEXT("null"));
                }
                else if (UClass* AsClass = Cast<UClass>(ResolvedObj))
                {
                    ObjClass = AsClass;
                }
                else if (ResolvedObj)
                {
                    ObjClass = ResolvedObj->GetClass();
                }

                // Also try class name lookup as fallback
                if (!ObjClass)
                {
                    FString ClassName;
                    if (ObjExpr->HasField(TEXT("Variable")))
                    {
                        const TSharedPtr<FJsonObject>& InnerVarObj = ObjExpr->GetObjectField(TEXT("Variable"));
                        const TSharedPtr<FJsonObject>& InnerInner = InnerVarObj->HasField(TEXT("Variable")) ? InnerVarObj->GetObjectField(TEXT("Variable")) : InnerVarObj;
                        if (InnerInner->HasField(TEXT("Property")))
                        {
                            const TSharedPtr<FJsonObject>& PropObj = InnerInner->GetObjectField(TEXT("Property"));
                            if (PropObj->HasField(TEXT("PropertyClass")))
                            {
                                const TSharedPtr<FJsonObject>& PropClass = PropObj->GetObjectField(TEXT("PropertyClass"));
                                ClassName = PropClass->GetStringField(TEXT("ObjectName"));
                                ClassName.RemoveFromStart(TEXT("Class'"));
                                ClassName.RemoveFromEnd(TEXT("'"));
                            }
                        }
                    }
                    else if (ObjExpr->HasField(TEXT("Value")))
                    {
                        const TSharedPtr<FJsonObject>& ValueObj = ObjExpr->GetObjectField(TEXT("Value"));
                        FString ObjectName = ValueObj->GetStringField(TEXT("ObjectName"));
                        int32 ApostropheIdx;
                        if (ObjectName.FindChar(TEXT('\''), ApostropheIdx))
                        {
                            ClassName = ObjectName.Left(ApostropheIdx);
                        }
                    }

                    if (!ClassName.IsEmpty())
                    {
                        ObjClass = FindObject<UClass>(nullptr, *ClassName);
                        if (!ObjClass)
                        {
                            ObjClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None);
                        }
                    }
                }

                if (ObjClass)
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Searching class hierarchy: %s"), *ObjClass->GetName());
                    UClass* C = ObjClass;
                    int32 Depth = 0;
                    while (C && !Func && Depth < 128)
                    {
                        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("    [%d] class=%s (looking up func)"), Depth, *C->GetName());
                        Func = C->FindFunctionByName(FName(*ResolvedFuncName));
                        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("    [%d] class=%s lookup returned %d"), Depth, *C->GetName(), Func ? 1 : 0);
                        if (Func)
                        {
                            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  Found function in: %s"), *C->GetName());
                            break;
                        }
                        C = C->GetSuperClass();
                        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("    [%d] super=%s"), Depth, C ? *C->GetName() : TEXT("null"));
                        ++Depth;
                    }
                    if (Depth >= 128)
                    {
                        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  Class hierarchy walk exceeded 128 levels (possible cycle) for class %s, func %s"),
                            *ObjClass->GetName(), *ResolvedFuncName);
                    }
                }
                else
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  Failed to resolve class from ObjectExpression for function: %s"), *ResolvedFuncName);
                }
            }
        }
    }

    /* Implemented-interface fallback (plan 010 item 2): functions declared on
     * a Blueprint interface (e.g. AddMappingContext on
     * EnhancedInputSubsystemInterface) are invisible to the concrete class's
     * hierarchy walk - scan the target class's implemented interfaces. */
    if (!Func && !ResolvedFuncName.IsEmpty() && TargetPin)
    {
        if (UClass* TargetClass = Cast<UClass>(TargetPin->PinType.PinSubCategoryObject.Get()))
        {
            UClass* C = TargetClass;
            int32 IfaceDepth = 0;
            while (C && !Func && IfaceDepth < 32)
            {
                for (const FImplementedInterface& Iface : C->Interfaces)
                {
                    if (!Iface.Class) continue;
                    Func = Iface.Class->FindFunctionByName(FName(*ResolvedFuncName));
                    if (Func)
                    {
                        UE_LOG(LogBlueprintBytecodeImporter, Log,
                            TEXT("EmitContextCall si=%d: %s found on implemented interface %s"),
                            Stmt.StatementIndex, *ResolvedFuncName, *Iface.Class->GetName());
                        break;
                    }
                }
                C = C->GetSuperClass();
                ++IfaceDepth;
            }
        }
    }

    if (ContextExpr.IsValid() && ContextExpr->HasField(TEXT("Parameters")))
    {
        ParamsJson = ContextExpr->GetArrayField(TEXT("Parameters"));
    }

    // Create the call node. WidgetBlueprintLibrary::Create is marked
    // BlueprintInternalUseOnly and must never surface as a raw call node - the
    // editor replaces it with a UK2Node_CreateWidget. Emit that node instead so
    // the graph matches the original (Class/OwningPlayer pins, no WorldContext).
    UEdGraphNode* CreateWidgetNode = nullptr;
    if (Func && Func->GetOwnerClass() == UWidgetBlueprintLibrary::StaticClass() && Func->GetFName() == TEXT("Create"))
    {
        // UK2Node_CreateWidget has no public header (UMGEditor/Private), so create
        // it by class name; only generic UEdGraphNode API is used below.
        UClass* CreateWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
        if (!CreateWidgetClass)
        {
            CreateWidgetClass = LoadClass<UEdGraphNode>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
        }
        if (CreateWidgetClass)
        {
            CreateWidgetNode = NewObject<UEdGraphNode>(Builder.Graph, CreateWidgetClass);
        }
        if (!CreateWidgetNode)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  Failed to create UK2Node_CreateWidget (class not found)"));
            return nullptr;
        }
        CreateWidgetNode->CreateNewGuid();
        CreateWidgetNode->SetFlags(RF_Transactional);
        Builder.Graph->AddNode(CreateWidgetNode, true, true);
        CreateWidgetNode->AllocateDefaultPins();

        // Params: [EX_Self(WorldContextObject), EX_ObjectConst(WidgetType), EX_NoObject(OwningPlayer)]
        if (ParamsJson.Num() >= 2)
        {
            const TSharedPtr<FJsonObject>& WidgetParam = ParamsJson[1]->AsObject();
            if (WidgetParam.IsValid())
            {
                const TSharedPtr<FJsonObject>& ValueObj = WidgetParam->GetObjectField(TEXT("Value"));
                if (UClass* WidgetClass = ResolveClassFromJson(ValueObj))
                {
                    if (UEdGraphPin* ClassPin = FindPin(CreateWidgetNode, TEXT("Class"), EGPD_Input))
                    {
                        ClassPin->DefaultObject = WidgetClass;
                    }
                }
            }
        }
        // EX_NoObject OwningPlayer stays unlinked with no default value.

        // Wire exec pins
        if (Builder.LastExecPin)
        {
            if (UEdGraphPin* ExecPin = FindPin(CreateWidgetNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input))
            {
                ConnectPins(Builder.LastExecPin, ExecPin);
            }
        }
        if (UEdGraphPin* ThenPin = FindPin(CreateWidgetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output))
        {
            Builder.LastExecPin = ThenPin;
        }

        // RValuePointer can be a string temp name or an object (Owner/Property).
        // Register the node's ReturnValue pin as the producer so consumers (widget
        // member SET targets, VariableGet self pins) can wire, and retype it to the
        // concrete widget class so those consumers see Initialize_C, not UserWidget.
        FString CreateRValue;
        if (Json->HasField(TEXT("RValuePointer")))
        {
            const TSharedPtr<FJsonValue>& RVField = Json->TryGetField(TEXT("RValuePointer"));
            if (RVField.IsValid())
            {
                if (RVField->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject>& RValueObj = RVField->AsObject();
                    if (RValueObj.IsValid() && RValueObj->HasField(TEXT("Property")))
                    {
                        const TSharedPtr<FJsonObject>& PropObj = RValueObj->GetObjectField(TEXT("Property"));
                        if (PropObj.IsValid())
                        {
                            CreateRValue = PropObj->GetStringField(TEXT("Name"));
                        }
                    }
                }
                else
                {
                    CreateRValue = Json->GetStringField(TEXT("RValuePointer"));
                }
            }
        }
        if (!CreateRValue.IsEmpty())
        {
            if (UEdGraphPin* RetPin = FindPin(CreateWidgetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output))
            {
                if (UEdGraphPin* ClassPin = FindPin(CreateWidgetNode, TEXT("Class"), EGPD_Input))
                {
                    if (UClass* WidgetClass = Cast<UClass>(ClassPin->DefaultObject))
                    {
                        RetPin->PinType.PinSubCategoryObject = WidgetClass;
                    }
                }
                Builder.ProducerPins.Add(CreateRValue, RetPin);
            }
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Emitted UK2Node_CreateWidget for WidgetBlueprintLibrary::Create"));
        CreateWidgetNode->NodePosX = Builder.NextNodeX;
        CreateWidgetNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        return CreateWidgetNode;
    }

    // Array_Get / Array_GetByRef are BlueprintInternalUseOnly CustomThunk calls
    // the editor replaces with a UK2Node_GetArrayItem. Emit that node instead so
    // the graph matches the original (Array / Dimension 1 / Output pins) and the
    // output pin derives its type from the wired array instead of staying an
    // unresolved wildcard. Params: [0] array, [1] index, [2] out-param result
    // temp (CallFunc_Array_Get_Item).
    if (Func && (Func->GetFName() == TEXT("Array_Get") || Func->GetFName() == TEXT("Array_GetByRef"))
        && ParamsJson.Num() >= 2)
    {
        UK2Node_GetArrayItem* ArrayGetNode = NewObject<UK2Node_GetArrayItem>(Builder.Graph);
        ArrayGetNode->CreateNewGuid();
        ArrayGetNode->SetFlags(RF_Transactional);
        ArrayGetNode->NodePosX = Builder.NextNodeX - 200;
        ArrayGetNode->NodePosY = Builder.NextNodeY;
        Builder.Graph->AddNode(ArrayGetNode, true, true);
        ArrayGetNode->AllocateDefaultPins();

        // Wire the array (param 0) into the Array pin.
        FPinValue ArrayValue = ResolveExpression(Builder, ParamsJson[0]->AsObject());

        /* Object-like arrays never return elements by reference. Array_GetByRef
         * is the BY-REF function form: mark the RESULT pin after the array is
         * wired (typed) so the saved shape matches the original "Get (a ref)".
         * Marking before wiring fires the "Array Get node altered"
         * notification during wildcard propagation. */
        if (ArrayValue.Pin)
        {
            /* Durable by-ref clear for non-ref-capable categories - see the
             * EX_ArrayGetByRef site: pin-flag clearing is reset by compile-time
             * reconstruction, flipping bReturnByRefDesired survives it. */
            if (IsNonRefCapableArrayCategory(ArrayValue.Pin->PinType))
            {
                SetArrayItemReturnByRef(ArrayGetNode, false);
            }
            /* Exporter flattens array frame properties to a plain ObjectProperty;
             * object-category producers only - scalar returns mis-fed into a Get
             * must stay scalar (see the EX_ArrayGetByRef site). */
            if (ArrayValue.Pin->PinType.ContainerType != EPinContainerType::Array
                && ArrayValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
            {
                ArrayValue.Pin->PinType.ContainerType = EPinContainerType::Array;
            }
            if (UEdGraphPin* ArrayPin = ArrayGetNode->GetTargetArrayPin())
            {
                ConnectPins(ArrayValue.Pin, ArrayPin);
            }
        }

        // Wire the index (param 1) into the Dimension 1 pin (constant or pin).
        FPinValue IndexValue = ResolveExpression(Builder, ParamsJson[1]->AsObject());
        UEdGraphPin* IndexPin = ArrayGetNode->GetIndexPin();
        if (IndexValue.Pin)
        {
            if (IndexPin) ConnectPins(IndexValue.Pin, IndexPin);
        }
        else if (IndexValue.bConstant && IndexPin)
        {
            IndexPin->DefaultValue = IndexValue.ConstString;
        }

        if (ArrayValue.Pin && !IsNonRefCapableArrayCategory(ArrayValue.Pin->PinType)
            && Func->GetFName() == TEXT("Array_GetByRef"))
        {
            if (UEdGraphPin* ResultPin = ArrayGetNode->GetResultPin())
            {
                ResultPin->PinType.bIsReference = true;
            }
        }

        // GetArrayItem is a pure node (IsNodePure() == true): it has no exec
        // pins, so it does not participate in the statement flow and
        // Builder.LastExecPin is left untouched.

        // Register the Output pin as the producer for the out-param result temp
        if (ParamsJson.Num() >= 3)
        {
            const TSharedPtr<FJsonObject>& OutObj = ParamsJson[2]->AsObject();
            if (OutObj.IsValid() && OutObj->GetStringField(TEXT("Token")) == TEXT("EX_LocalVariable"))
            {
                const TSharedPtr<FJsonObject>& OutVarObj = OutObj->GetObjectField(TEXT("Variable"));
                if (OutVarObj.IsValid())
                {
                    const TSharedPtr<FJsonObject>& OutPropObj = OutVarObj->GetObjectField(TEXT("Property"));
                    if (OutPropObj.IsValid())
                    {
                        const FString TempVarName = OutPropObj->GetStringField(TEXT("Name"));
                        if (!TempVarName.IsEmpty())
                        {
                            if (UEdGraphPin* OutputPin = ArrayGetNode->GetResultPin())
                            {
                                // Retype the output to the temp's declared struct
                                // type so consumers see the concrete type.
                                if (OutPropObj->HasField(TEXT("Struct")))
                                {
                                    const TSharedPtr<FJsonObject>& StructObj = OutPropObj->GetObjectField(TEXT("Struct"));
                                    FString StructObjectName = StructObj->GetStringField(TEXT("ObjectName"));
                                    StructObjectName.RemoveFromStart(TEXT("UserDefinedStruct'"));
                                    StructObjectName.RemoveFromStart(TEXT("ScriptStruct'"));
                                    StructObjectName.RemoveFromEnd(TEXT("'"));
                                    if (UScriptStruct* DeclaredStruct = ResolveUserDefinedStruct(StructObjectName, StructObj->GetStringField(TEXT("ObjectPath"))))
                                    {
                                        OutputPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                                        OutputPin->PinType.PinSubCategory = NAME_None;
                                        OutputPin->PinType.PinSubCategoryObject = DeclaredStruct;
                                    }
                                }
                                Builder.ProducerPins.Add(TempVarName, OutputPin);
                                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Registered Array_Get result producer %s (pin %s)"),
                                    *TempVarName, *OutputPin->PinName.ToString());
                            }
                        }
                    }
                }
            }
        }

        ArrayGetNode->NodePosX = Builder.NextNodeX;
        ArrayGetNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Emitted UK2Node_GetArrayItem for Array_Get (si=%d)"),
            Stmt.StatementIndex);
        return ArrayGetNode;
    }

    // A pure static BPL call (e.g. BPL_DollSystem::Global Game Instance) can be
    // emitted more than once for the same result temp (one stmt per use of its
    // result, e.g. the "_3" call in GM_Touchy::Update All Character INFO is
    // compiled at si=1262 and si=1469). The original graph holds ONE call node
    // per bytecode emission (CallFunction_0 + CallFunction_2), so emit a fresh
    // node here. Statements are scanned linearly, so the producer map must
    // always point at the LATEST emission: re-registration OVERWRITES the
    // earlier entry (no Contains guard), so reads that follow the re-emission
    // (e.g. Array_Get si=1515 on "_3") wire to the new node while reads before
    // it (Array_Length si=1308) keep their already-made connection to the first
    // node.

    UK2Node_CallFunction* CallNode = Func ? CreateCallNode(Builder, Func, ParamsJson, TargetPin) : nullptr;
    if (!CallNode && !ResolvedFuncName.IsEmpty())
    {
        // Unresolved function (e.g. virtual function on a stub class) - emit a stub call node
        CallNode = CreateStubCallNode(ResolvedFuncName, Builder.Graph);
        if (CallNode)
        {
            if (TargetPin)
            {
                // Retarget the function reference to the resolved target class so the
                // node shows the correct owner (e.g. BPL_DollSystem_C, not the current BP)
                if (UClass* TargetClass = Cast<UClass>(TargetPin->PinType.PinSubCategoryObject.Get()))
                {
                    CallNode->FunctionReference.SetExternalMember(FName(*ResolvedFuncName), TargetClass);
                }

                UEdGraphPin* SelfPin = FindPin(CallNode, TEXT("self"), EGPD_Input);
                if (!SelfPin) SelfPin = FindPin(CallNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input);
                if (SelfPin)
                {
                    // Coerce the stub self pin's object type so the connection is compatible
                    if (!SelfPin->PinType.PinSubCategoryObject.IsValid() && TargetPin->PinType.PinSubCategoryObject.IsValid())
                    {
                        SelfPin->PinType.PinSubCategoryObject = TargetPin->PinType.PinSubCategoryObject;
                    }
                    ConnectPins(TargetPin, SelfPin);
                }
            }

            /* Stub nodes carry no function signature pins, so materialize one
             * typed input pin per real bytecode argument - otherwise producers
             * feeding stub args (e.g. a MakeStruct passed as AddMappingContext
             * options) stay orphaned and the whole data chain dangles. */
            int32 StubSlot = 0;
            for (const TSharedPtr<FJsonValue>& ParamVal : ParamsJson)
            {
                const TSharedPtr<FJsonObject> ParamObj = ParamVal.IsValid() ? ParamVal->AsObject() : nullptr;
                if (!ParamObj.IsValid()) continue;
                const FString ParamTok = ParamObj->GetStringField(TEXT("Token"));
                if (ParamTok == TEXT("EX_Self") || ParamTok == TEXT("EX_ObjectConst") || ParamTok == TEXT("EX_NoObject")) continue;
                FPinValue ArgValue = ResolveExpression(Builder, ParamObj);
                const FName ArgPinName = FName(*FString::Printf(TEXT("Param %d"), StubSlot));
                UEdGraphPin* ArgPin = CallNode->CreatePin(EGPD_Input, ArgValue.Pin ? ArgValue.Pin->PinType : PinTypeFromConstantToken(ParamTok), ArgPinName);
                if (ArgPin && ArgValue.Pin)
                {
                    ConnectPins(ArgValue.Pin, ArgPin);
                }
                else if (ArgPin && ArgValue.bConstant)
                {
                    SetPinDefaultValueSafe(ArgPin, ArgValue.ConstString);
                }
                ++StubSlot;
            }
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("Created stub call node for unresolved function: %s"), *ResolvedFuncName);
        }
    }
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

    // Some exports store the return-value temp as an EX_LocalVariable in the
    // parameters instead of an RValuePointer; register it as the producer. Only do
    // this for functions that actually have a return value - otherwise the first
    // EX_LocalVariable param is a real input (e.g. SetInputMode_GameAndUIEx's
    // PlayerController) and claiming it as a return temp corrupts the producer map.
    // Prefer the EX_LocalVariable whose name matches the return parameter, then
    // fall back to the first one for exports that spell it differently.
    FProperty* ReturnProp = nullptr;
    if (RValuePointer.IsEmpty() && Func)
    {
        for (TFieldIterator<FProperty> It(Func); It; ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_ReturnParm)) { ReturnProp = *It; break; }
        }
        if (ReturnProp)
        {
            const FString SanitizedReturnName = ReturnProp->GetName().Replace(TEXT(" "), TEXT("_"));
            const TSharedPtr<FJsonValue>* MatchedParam = nullptr;
            const TSharedPtr<FJsonValue>* FirstParam = nullptr;
            for (const TSharedPtr<FJsonValue>& Param : ParamsJson)
            {
                const TSharedPtr<FJsonObject>& ParamObj = Param->AsObject();
                if (!ParamObj.IsValid()) continue;
                if (ParamObj->GetStringField(TEXT("Token")) != TEXT("EX_LocalVariable")) continue;
                const TSharedPtr<FJsonObject>& PVarObj = ParamObj->GetObjectField(TEXT("Variable"));
                if (!PVarObj.IsValid()) continue;
                const TSharedPtr<FJsonObject>& PPropObj = PVarObj->GetObjectField(TEXT("Property"));
                if (!PPropObj.IsValid()) continue;
                if (!FirstParam) FirstParam = &Param;
                const FString PVarName = PPropObj->GetStringField(TEXT("Name"));
                if (!PVarName.IsEmpty() && PVarName.Contains(SanitizedReturnName))
                {
                    MatchedParam = &Param;
                    break;
                }
            }
            const TSharedPtr<FJsonValue>* SelectedParam = MatchedParam ? MatchedParam : FirstParam;
            if (SelectedParam)
            {
                const TSharedPtr<FJsonObject>& ParamObj = (*SelectedParam)->AsObject();
                const TSharedPtr<FJsonObject>& PVarObj = ParamObj->GetObjectField(TEXT("Variable"));
                if (PVarObj.IsValid())
                {
                    const TSharedPtr<FJsonObject>& PPropObj = PVarObj->GetObjectField(TEXT("Property"));
                    if (PPropObj.IsValid())
                    {
                        FString PVarName = PPropObj->GetStringField(TEXT("Name"));
                        if (!PVarName.IsEmpty()) RValuePointer = PVarName;
                    }
                }
            }
        }
    }

    UEdGraphPin* ReturnValuePin = nullptr;
    if (!RValuePointer.IsEmpty())
    {
        // Prefer the typed, param-named output pin (e.g. the stub Global Game
        // Instance's "AsGI Data" pin typed GI_Data_C) over a synthesized untyped
        // ReturnValue pin so consumers (member SET/VariableGet self pins) see the
        // correct object type instead of a generic Object.
        if (ReturnProp)
        {
            ReturnValuePin = FindCallNodeOutPinForParam(CallNode, ReturnProp);
        }
        if (!ReturnValuePin)
        {
            ReturnValuePin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        }
        if (!ReturnValuePin)
        {
            // Function has a return value but the node didn't get a ReturnValue pin
            // (e.g. stub/unresolved call) - synthesize one so downstream producers
            // (e.g. EX_Context SET targets) can wire to it.
            ReturnValuePin = CallNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Object, FName(), nullptr, UEdGraphSchema_K2::PN_ReturnValue);
        }
        if (ReturnValuePin)
        {
            // Overwrite unconditionally: a re-emitted call (same result temp in
            // a later statement) supersedes any earlier producer. Reads before
            // this emission are already wired to the earlier node.
            Builder.ProducerPins.Add(RValuePointer, ReturnValuePin);
        }
    }

    // Some stub functions end up with a mangled signature (duplicated
    // __WorldContext params, by-ref out params exposing both an input and a
    // deduped "AsGI Data1" output) that breaks the 1:1 param-slot mapping in
    // CreateCallNode, so this call's result temp is never registered as a
    // producer. Any EX_LocalVariable temp whose name embeds THIS call's
    // function name (CallFunc_Global_Game_Instance_AsGI_Data_N) is this call's
    // result slot - register the matching output pin (name-prefix, then type
    // match, then synthesize) so downstream reads (EX_Context targets, member
    // SET self pins) can wire. Temps from earlier calls carry a different
    // function name (CallFunc_GetGameInstance_ReturnValue) and are skipped.
    if (Func && CallNode->IsA<UK2Node_CallFunction>())
    {
        FString FuncNameSanitized = ResolvedFuncName;
        FuncNameSanitized.ReplaceInline(TEXT(" "), TEXT("_"));
        for (const TSharedPtr<FJsonValue>& Param : ParamsJson)
        {
            const TSharedPtr<FJsonObject>& ParamObj = Param->AsObject();
            if (!ParamObj.IsValid()) continue;
            if (ParamObj->GetStringField(TEXT("Token")) != TEXT("EX_LocalVariable")) continue;
            const TSharedPtr<FJsonObject>& PVarObj = ParamObj->GetObjectField(TEXT("Variable"));
            if (!PVarObj.IsValid()) continue;
            const TSharedPtr<FJsonObject>& PPropObj = PVarObj->GetObjectField(TEXT("Property"));
            if (!PPropObj.IsValid()) continue;
            const FString PVarName = PPropObj->GetStringField(TEXT("Name"));
            if (PVarName.IsEmpty()) continue;
            if (!PVarName.StartsWith(TEXT("CallFunc_")) && !PVarName.StartsWith(TEXT("K2Node_"))) continue;
            if (!PVarName.Contains(FuncNameSanitized)) continue;
            /* Implicit-cast temps (CallFunc_<Func>_<Parm>_ImplicitCast) are the
             * compiler's conversion slots for THIS call's INPUTS - on a void
             * function like SetMorphTarget the scan below would otherwise
             * synthesize a bogus "<Parm>_ImplicitCast" output pin the UFunction
             * has no parameter for (compile error: "doesn't match any
             * parameters"). They are never result slots. */
            if (PVarName.Contains(TEXT("_ImplicitCast"))) continue;
            // No Contains guard: a re-emitted call (same temp in a later
            // statement) overwrites the producer so later reads use the new node.
            UEdGraphPin* OutPin = nullptr;

            // Name-prefix match: strip CallFunc_<Func>_ / K2Node_ prefixes and a
            // trailing _<digits>, then compare against output pins ignoring
            // spaces/underscores so "AsGI_Data" matches "AsGI Data1".
            FString Base = PVarName;
            Base.RemoveFromStart(TEXT("CallFunc_"), ESearchCase::CaseSensitive);
            Base.RemoveFromStart(TEXT("K2Node_"), ESearchCase::CaseSensitive);
            Base.RemoveFromStart(FuncNameSanitized + TEXT("_"), ESearchCase::CaseSensitive);
            int32 UndIdx = INDEX_NONE;
            if (Base.FindLastChar(TEXT('_'), UndIdx) && FCString::IsNumeric(*Base.Mid(UndIdx + 1)))
            {
                Base = Base.Left(UndIdx);
            }
            const FString BaseNorm = Base.Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
            for (UEdGraphPin* Pin : CallNode->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output) continue;
                const FString PinNorm = Pin->PinName.ToString().Replace(TEXT("_"), TEXT("")).Replace(TEXT(" "), TEXT(""));
                if (!BaseNorm.IsEmpty() && PinNorm.StartsWith(BaseNorm, ESearchCase::CaseSensitive))
                {
                    OutPin = Pin;
                    break;
                }
            }

            // Type match against the temp's declared class.
            if (!OutPin && PPropObj->HasField(TEXT("PropertyClass")))
            {
                const TSharedPtr<FJsonObject>& DeclClassObj = PPropObj->GetObjectField(TEXT("PropertyClass"));
                if (UClass* DeclaredClass = ResolveClassFromJson(DeclClassObj))
                {
                    for (UEdGraphPin* Pin : CallNode->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Output) continue;
                        if (UClass* PinClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get()))
                        {
                            if (PinClass == DeclaredClass || PinClass->IsChildOf(DeclaredClass) || DeclaredClass->IsChildOf(PinClass))
                            {
                                OutPin = Pin;
                                break;
                            }
                        }
                    }
                }
            }

            // Last resort: synthesize a typed output pin from the temp's class.
            if (!OutPin && !Base.IsEmpty())
            {
                OutPin = CallNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Object, FName(), nullptr, *Base);
                if (PPropObj->HasField(TEXT("PropertyClass")))
                {
                    const TSharedPtr<FJsonObject>& DeclClassObj = PPropObj->GetObjectField(TEXT("PropertyClass"));
                    if (UClass* DeclaredClass = ResolveClassFromJson(DeclClassObj))
                    {
                        OutPin->PinType.PinSubCategoryObject = DeclaredClass;
                    }
                }
            }

            if (OutPin)
            {
                Builder.ProducerPins.Add(PVarName, OutPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Registered call-result producer %s (pin %s)"),
                    *PVarName, *OutPin->PinName.ToString());
            }
        }
    }

    // Stub calls (unresolved functions) have no real signature, so the
    // RValuePointer scan above (gated on Func) never runs and out-param temps
    // like CallFunc_Global_Game_Instance_AsGI_Data are left without producers.
    // Synthesize a typed output pin per EX_LocalVariable call-result temp so
    // consumers (EX_LocalVirtualFunction targets, member SET self pins) can wire.
    if (!Func && CallNode->IsA<UK2Node_CallFunction>())
    {
        for (const TSharedPtr<FJsonValue>& Param : ParamsJson)
        {
            const TSharedPtr<FJsonObject>& ParamObj = Param->AsObject();
            if (!ParamObj.IsValid()) continue;
            if (ParamObj->GetStringField(TEXT("Token")) != TEXT("EX_LocalVariable")) continue;
            const TSharedPtr<FJsonObject>& PVarObj = ParamObj->GetObjectField(TEXT("Variable"));
            if (!PVarObj.IsValid()) continue;
            const TSharedPtr<FJsonObject>& PPropObj = PVarObj->GetObjectField(TEXT("Property"));
            if (!PPropObj.IsValid()) continue;
            FString PVarName = PPropObj->GetStringField(TEXT("Name"));
            if (!PVarName.StartsWith(TEXT("CallFunc_")) && !PVarName.StartsWith(TEXT("K2Node_"))) continue;
            if (FindPin(CallNode, *PVarName, EGPD_Output)) continue;

            UEdGraphPin* SynthPin = CallNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Object, FName(), nullptr, *PVarName);
            if (PPropObj->HasField(TEXT("PropertyClass")))
            {
                const TSharedPtr<FJsonObject>& DeclClassObj = PPropObj->GetObjectField(TEXT("PropertyClass"));
                if (UClass* DeclaredClass = ResolveClassFromJson(DeclClassObj))
                {
                    SynthPin->PinType.PinSubCategoryObject = DeclaredClass;
                }
            }
            Builder.ProducerPins.Add(PVarName, SynthPin);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Synthesized stub output pin %s on %s"),
                *PVarName, *CallNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
        }
    }

    // Store position
    CallNode->NodePosX = Builder.NextNodeX;
    CallNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

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

    CallNode->NodePosX = Builder.NextNodeX;
    CallNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

    return CallNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitLet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get LHS variable
    // JSON structure: EX_LetObj.Variable = { Token: "EX_LocalVariable", Variable: { Owner, Property: { Name } } }
    const TSharedPtr<FJsonObject>& VarObj = Json->GetObjectField(TEXT("Variable"));
    if (!VarObj.IsValid()) return nullptr;

    // EX_Context variables set a member on a context object: the property name lives
    // in RValuePointer and the context object in ObjectExpression.
    const FString VarToken = VarObj->GetStringField(TEXT("Token"));
    const bool bIsContextVar = (VarToken == TEXT("EX_Context") || VarToken == TEXT("EX_Context_FailSilent"));
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitLet si=%d ENTRY: VarToken=%s, bIsContextVar=%d"),
        Stmt.StatementIndex, *VarToken, bIsContextVar ? 1 : 0);

    // EX_StructMemberContext LHS = writing a field into a struct. Writes into
    // MakeStruct temps reconstruct the MakeStruct node; any other struct ref
    // (an array element fetched by-ref, an instance struct variable) compiled
    // from a modify-in-place UK2Node_SetFieldsInStruct and is reversed by
    // EmitStructFieldRefWrite.
    if (VarToken == TEXT("EX_StructMemberContext"))
    {
        const TSharedPtr<FJsonObject>& DispatchSE = VarObj->GetObjectField(TEXT("StructExpression"));
        const TSharedPtr<FJsonObject>& DispatchSEVar = DispatchSE.IsValid() ? DispatchSE->GetObjectField(TEXT("Variable")) : nullptr;
        const TSharedPtr<FJsonObject>& DispatchSEProp = DispatchSEVar.IsValid() ? DispatchSEVar->GetObjectField(TEXT("Property")) : nullptr;
        const FString DispatchBase = DispatchSEProp.IsValid() ? DispatchSEProp->GetStringField(TEXT("Name")) : TEXT("");
        if (DispatchBase.StartsWith(TEXT("K2Node_MakeStruct_")))
        {
            return EmitMakeStructFieldSet(Builder, Stmt);
        }
        return EmitStructFieldRefWrite(Builder, Stmt);
    }

    /* EX_StructConst assigned to a frame temp (plan 010 item 3): pure literal
     * field data - e.g. Temp_struct_Variable = ModifyContextOptions{1,0,0} -
     * that the original graph kept as split-subpin defaults on the consuming
     * call. Record the fields and emit nothing; CreateCallNode Phase 2 turns
     * them into subpin defaults when the temp feeds a struct-by-ref arg. */
    {
        const TSharedPtr<FJsonObject>* RhsPtr = nullptr;
        if (VarToken == TEXT("EX_LocalVariable")
            && Json->TryGetObjectField(TEXT("Expression"), RhsPtr) && RhsPtr
            && (*RhsPtr)->GetStringField(TEXT("Token")) == TEXT("EX_StructConst"))
        {
            const TSharedPtr<FJsonObject>& SCVar = VarObj->GetObjectField(TEXT("Variable"));
            const TSharedPtr<FJsonObject>& SCProp = SCVar.IsValid() ? SCVar->GetObjectField(TEXT("Property")) : nullptr;
            const FString TempName = SCProp.IsValid() ? SCProp->GetStringField(TEXT("Name")) : TEXT("");
            if (!TempName.IsEmpty())
            {
                TMap<FString, FString>& FieldMap = Builder.TempStructFields.FindOrAdd(TempName);
                const TArray<TSharedPtr<FJsonValue>>* Props = nullptr;
                if ((*RhsPtr)->TryGetArrayField(TEXT("Properties"), Props) && Props)
                {
                    for (const TSharedPtr<FJsonValue>& PV : *Props)
                    {
                        const TSharedPtr<FJsonObject> PObj = PV.IsValid() ? PV->AsObject() : nullptr;
                        if (!PObj.IsValid()) continue;
                        const FString PTok = PObj->GetStringField(TEXT("Token"));
                        FString FieldName;
                        if (PObj->HasField(TEXT("InnerProperty")))
                        {
                            const TSharedPtr<FJsonObject>& IP = PObj->GetObjectField(TEXT("InnerProperty"));
                            if (IP->HasField(TEXT("Path")))
                            {
                                const TArray<TSharedPtr<FJsonValue>>& PP = IP->GetArrayField(TEXT("Path"));
                                if (PP.Num() > 0 && !PP[0]->AsString().IsEmpty())
                                {
                                    FieldName = StripStructMemberSuffix(PP[0]->AsString());
                                }
                            }
                        }
                        if (FieldName.IsEmpty()) continue;

                        FString ValueText;
                        if (PTok == TEXT("EX_BitFieldConst") || PTok == TEXT("EX_True") || PTok == TEXT("EX_False"))
                        {
                            const bool bOn = (PTok == TEXT("EX_True"))
                                || (PObj->HasField(TEXT("ConstValue")) && PObj->GetIntegerField(TEXT("ConstValue")) != 0);
                            ValueText = bOn ? TEXT("True") : TEXT("False");
                        }
                        else if (PTok == TEXT("EX_IntConst") || PTok == TEXT("EX_ByteConst"))
                        {
                            ValueText = FString::FromInt(PObj->GetIntegerField(TEXT("Value")));
                        }
                        else if (PTok == TEXT("EX_FloatConst") || PTok == TEXT("EX_DoubleConst"))
                        {
                            ValueText = FString::SanitizeFloat(PObj->GetNumberField(TEXT("Value")));
                        }
                        else
                        {
                            continue;
                        }
                        FieldMap.Add(FieldName, ValueText);
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Log,
                    TEXT("EmitLet si=%d: %s := EX_StructConst recorded as literal struct data (%d fields, no node)"),
                    Stmt.StatementIndex, *TempName, FieldMap.Num());
                return nullptr;
            }
        }
    }

    // Navigate to inner Variable if present (EX_LocalVariable/EX_InstanceVariable wrapper)
    const TSharedPtr<FJsonObject>& InnerVar = VarObj->HasField(TEXT("Variable")) ? VarObj->GetObjectField(TEXT("Variable")) : VarObj;
    TSharedPtr<FJsonObject> PropObj;
    if (bIsContextVar)
    {
        const TSharedPtr<FJsonObject>& RValuePtr = VarObj->GetObjectField(TEXT("RValuePointer"));
        if (RValuePtr.IsValid())
        {
            // NOTE: GetObjectField never returns null in UE5.7 for a missing
            // field, so IsValid() can't detect absence. Use HasField.
            PropObj = RValuePtr->HasField(TEXT("Property")) ? RValuePtr->GetObjectField(TEXT("Property")) : TSharedPtr<FJsonObject>();
            // Context member SETs (e.g. PC->bEnableClickEvents) may describe the
            // member as a Path array with no Property object - synthesize the
            // property info from the path and the statement token.
            if (!PropObj.IsValid() && RValuePtr->HasField(TEXT("Path")))
            {
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  si=%d SYNTHESIS: RValuePtr Path present, hasProperty=%d"),
                    Stmt.StatementIndex, RValuePtr->HasField(TEXT("Property")) ? 1 : 0);
                const TArray<TSharedPtr<FJsonValue>>& PathArr = RValuePtr->GetArrayField(TEXT("Path"));
                if (PathArr.Num() > 0 && !PathArr[0]->AsString().IsEmpty())
                {
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  si=%d SYNTHESIS: Path[0]='%s'"), Stmt.StatementIndex, *PathArr[0]->AsString());
                    PropObj = MakeShared<FJsonObject>();
                    PropObj->SetStringField(TEXT("Name"), PathArr[0]->AsString());
                    const FString StmtToken = Json->GetStringField(TEXT("Token"));
                    if (StmtToken == TEXT("EX_LetBool"))     PropObj->SetStringField(TEXT("Type"), TEXT("BoolProperty"));
                    else if (StmtToken == TEXT("EX_LetObj")) PropObj->SetStringField(TEXT("Type"), TEXT("ObjectProperty"));
                    else if (StmtToken == TEXT("EX_Let"))    PropObj->SetStringField(TEXT("Type"), TEXT("IntProperty"));
                }
            }
        }
    }
    else
    {
        PropObj = InnerVar->GetObjectField(TEXT("Property"));
    }
    if (!PropObj.IsValid()) return nullptr;

    FString VarName = PropObj->GetStringField(TEXT("Name"));
    if (VarName.IsEmpty()) return nullptr;

    // Get RHS expression
    const TSharedPtr<FJsonObject>& ExprJson = Json->GetObjectField(TEXT("Expression"));
    FString RHSDebugToken = ExprJson.IsValid() ? ExprJson->GetStringField(TEXT("Token")) : TEXT("NONE");

    // EX_DynamicCast is always side-effecting (needs exec wiring), so handle
    // it here before ResolveExpression would create a pure node without exec.
    const FString RHSToken = ExprJson.IsValid() ? ExprJson->GetStringField(TEXT("Token")) : TEXT("");
    FPinValue RHSValue;
    UEdGraphNode* RHSCallNode = nullptr;
    if (RHSToken == TEXT("EX_DynamicCast"))
    {
        UEdGraphNode* CastNode = EmitExpressionAsExec(Builder, ExprJson, Stmt.StatementIndex);
        if (CastNode)
        {
            UEdGraphPin* ResultPin = nullptr;
            if (UK2Node_DynamicCast* DC = Cast<UK2Node_DynamicCast>(CastNode))
            {
                ResultPin = DC->GetCastResultPin();
            }
            if (!ResultPin) ResultPin = FindPin(CastNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
            if (!ResultPin) ResultPin = FindPin(CastNode, TEXT("ReturnValue"), EGPD_Output);
            if (ResultPin)
            {
                RHSValue = FPinValue{ ResultPin, false, TEXT(""), nullptr };
            }

            // Store "Cast Succeeded" pin for the K2Node_DynamicCast_bSuccess variable
            if (CastNode->IsA<UK2Node_DynamicCast>())
            {
                UK2Node_DynamicCast* DC = Cast<UK2Node_DynamicCast>(CastNode);
                UEdGraphPin* SuccessPin = FindPin(DC, TEXT("bSuccess"), EGPD_Output);
                if (!SuccessPin) SuccessPin = FindPin(DC, TEXT("Cast Succeeded"), EGPD_Output);
                if (SuccessPin)
                {
                    Builder.ProducerPins.Add(TEXT("K2Node_DynamicCast_bSuccess"), SuccessPin);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Stored Cast Succeeded pin for bSuccess"));
                }
            }
        }
    }
    else if (RHSToken == TEXT("EX_CallMath") && ExprJson.IsValid())
    {
        // Impure static calls (e.g. GameUserSettings::GetGameUserSettings or
        // GameplayStatics::GetActorOfClass) arrive as EX_CallMath in a LetObj
        // RHS. The pure ResolveExpression path would create the node without
        // exec wiring, orphaning it from the exec chain. Emit as an exec node
        // so the call chains in and becomes the statement anchor (needed when
        // the statement is a switch case body entry).
        UFunction* CallMathFunc = ResolveFunction(ExprJson, TEXT(""));
        const bool bIsImpureCallMath = CallMathFunc && !CallMathFunc->HasAnyFunctionFlags(FUNC_BlueprintPure);
        if (bIsImpureCallMath)
        {
            UEdGraphNode* CallNode = EmitExpressionAsExec(Builder, ExprJson, Stmt.StatementIndex);
            if (CallNode)
            {
                RHSCallNode = CallNode;
                UEdGraphPin* RetPin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
                if (RetPin)
                {
                    RHSValue = FPinValue{ RetPin, false, TEXT(""), nullptr };
                }
            }
        }
        else
        {
            // Check if this is a no-op type conversion: the function is a
            // BlueprintPure conversion (e.g. Conv_InputActionValueToBool) and
            // the sole parameter already resolves to a producer pin of the
            // target type. In that case skip the call node entirely — the
            // visual graph doesn't need it (the event's ActionValue pin is
            // already bool, so the conversion is redundant).
            bool bSkippedNoop = false;
            if (CallMathFunc && CallMathFunc->HasAnyFunctionFlags(FUNC_BlueprintPure))
            {
                TArray<TSharedPtr<FJsonValue>> CmParams;
                if (ExprJson->HasField(TEXT("Parameters")))
                {
                    CmParams = ExprJson->GetArrayField(TEXT("Parameters"));
                }
                if (CmParams.Num() == 1)
                {
                    const TSharedPtr<FJsonObject>& SingleParam = CmParams[0]->AsObject();
                    if (SingleParam.IsValid() && SingleParam->GetStringField(TEXT("Token")) == TEXT("EX_LocalVariable"))
                    {
                        const TSharedPtr<FJsonObject>& PVar = SingleParam->GetObjectField(TEXT("Variable"));
                        if (PVar.IsValid() && PVar->HasField(TEXT("Property")))
                        {
                            const TSharedPtr<FJsonObject>& PProp = PVar->GetObjectField(TEXT("Property"));
                            if (PProp.IsValid())
                            {
                                const FString PName = PProp->GetStringField(TEXT("Name"));
                                if (UEdGraphPin** InputPin = Builder.ProducerPins.Find(PName))
                                {
                                    // Check if the function's return value type
                                    // matches the producer pin's type. Find the
                                    // ReturnValue property in the function.
                                    FName OutPinCategory;
                                    for (TFieldIterator<FProperty> PropIt(CallMathFunc); PropIt; ++PropIt)
                                    {
                                        if (PropIt->HasAnyPropertyFlags(CPF_ReturnParm))
                                        {
                                            if (FBoolProperty* BoolProp = CastField<FBoolProperty>(*PropIt))
                                            {
                                                OutPinCategory = UEdGraphSchema_K2::PC_Boolean;
                                            }
                                            else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(*PropIt))
                                            {
                                                OutPinCategory = UEdGraphSchema_K2::PC_Object;
                                            }
                                            break;
                                        }
                                    }
                                    const FName InType = (*InputPin)->PinType.PinCategory;
                                    if (!OutPinCategory.IsNone() && OutPinCategory == InType)
                                    {
                                        RHSValue = FPinValue{ *InputPin, false, TEXT(""), nullptr };
                                        bSkippedNoop = true;
                                        UE_LOG(LogBlueprintBytecodeImporter, Log,
                                            TEXT("  -> Skipped no-op conversion %s: input %s already %s"),
                                            *CallMathFunc->GetName(), *PName, *InType.ToString());
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (!bSkippedNoop)
            {
                RHSValue = ResolveExpression(Builder, ExprJson);
            }
        }
    }
    else
    {
        RHSValue = ExprJson.IsValid() ? ResolveExpression(Builder, ExprJson) : FPinValue();
    }

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitLet si=%d: var=%s, rhs=%s, hasPin=%d"),
        Stmt.StatementIndex, *VarName, *RHSDebugToken, RHSValue.Pin ? 1 : 0);

    // Shortcut: if the RHS is EX_LocalVirtualFunction reading an event output
    // (e.g. K2Node_EnhancedInputActionEvent_ActionValue), resolve directly to
    // the event node's output pin via ProducerPins.  The event pin is already
    // the correct type — no Conv or call node is needed.
    if (!RHSValue.Pin && ExprJson.IsValid()
        && (RHSDebugToken == TEXT("EX_LocalVirtualFunction") || RHSDebugToken == TEXT("EX_LocalFinalFunction")))
    {
        const FString FuncName = ExprJson->HasField(TEXT("Function"))
            ? ExprJson->GetStringField(TEXT("Function")) : FString();
        if (FuncName.StartsWith(TEXT("K2Node_EnhancedInputActionEvent_")))
        {
            if (UEdGraphPin** FoundPin = Builder.ProducerPins.Find(FuncName))
            {
                RHSValue = FPinValue{ *FoundPin, false, TEXT(""), nullptr };
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Event output %s resolved to producer pin %s (no call node)"),
                    *FuncName, *(*FoundPin)->PinName.ToString());
            }
        }
    }

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
				RHSCallNode = CallNode;
				UEdGraphPin* ReturnPin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
				if (!ReturnPin)
				{
					ReturnPin = FindPin(CallNode, TEXT("ReturnValue"), EGPD_Output);
				}
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
	TSharedPtr<FJsonObject> OwnerObj;
	if (bIsContextVar)
	{
		const TSharedPtr<FJsonObject>& RValuePtr = VarObj->GetObjectField(TEXT("RValuePointer"));
		if (RValuePtr.IsValid())
		{
			if (RValuePtr->HasField(TEXT("Owner")))
			{
				OwnerObj = RValuePtr->GetObjectField(TEXT("Owner"));
			}
			// Native member SETs (e.g. PlayerController->bEnableClickEvents) expose
			// the owning class as ResolvedOwner instead of Owner.
			else if (RValuePtr->HasField(TEXT("ResolvedOwner")))
			{
				OwnerObj = RValuePtr->GetObjectField(TEXT("ResolvedOwner"));
			}
		}
	}
	else if (InnerVar->HasField(TEXT("Owner")))
	{
		OwnerObj = InnerVar->GetObjectField(TEXT("Owner"));
	}
	if (OwnerObj.IsValid())
	{
		// Robust resolution: handles BlueprintGeneratedClass'/WidgetBlueprintGeneratedClass'/
		// AnimBlueprintGeneratedClass'/Class' prefixes plus the ObjectPath dependency lookup.
		if (UClass* FoundClass = ResolveClassFromJson(OwnerObj))
		{
			OwnerClass = FoundClass;
		}
	}

	// Check if this is a compiler temp variable (CallFunc_, K2Node_, Temp_).
	// All three are ubergraph-local variables that do NOT exist in the visual
	// graph — creating VariableSet/VariableGet nodes for them causes compile errors.
	// Instead, just store the producer pin so downstream reads resolve correctly.
	bool bIsTempVar = VarName.StartsWith(TEXT("CallFunc_")) || VarName.StartsWith(TEXT("Temp_")) || VarName.StartsWith(TEXT("K2Node_"));
	const bool bIsCallResultTemp = bIsTempVar;

	// For CallFunc temps that receive call results, just store the producer pin
	if (bIsCallResultTemp && RHSValue.Pin)
	{
		/* The canonical SetFieldsInStruct copy-out local ("..._StructOut =
		 * <base element>") is compiler bookkeeping: EmitStructFieldRefWrite
		 * already registered the node's StructOut as this name's producer.
		 * Keep it so the downstream variable SET wires from StructOut like the
		 * original graph instead of being repointed at the unmodified getter. */
		if (VarName == TEXT("K2Node_SetFieldsInStruct_StructOut")
			&& Builder.ProducerPins.Contains(VarName))
		{
			UE_LOG(LogBlueprintBytecodeImporter, Log,
				TEXT("EmitLet si=%d: keeping SetFieldsInStruct StructOut producer for %s (copy-out skipped)"),
				Stmt.StatementIndex, *VarName);
			return RHSCallNode;
		}

		// The temp's declared class is often more derived than the call node's
		// native return type (e.g. WidgetBlueprintLibrary::Create returns
		// UserWidget, but DynamicOutputType makes the editor temp an
		// Initialize_C). Retype the producer so consumers that expect the
		// derived type (widget member SET targets) can connect.
		if (PropObj.IsValid() && PropObj->HasField(TEXT("PropertyClass")))
		{
			const TSharedPtr<FJsonObject>& DeclClassObj = PropObj->GetObjectField(TEXT("PropertyClass"));
			if (DeclClassObj.IsValid())
			{
				if (UClass* DeclaredClass = ResolveClassFromJson(DeclClassObj))
				{
					if (UClass* CurClass = Cast<UClass>(RHSValue.Pin->PinType.PinSubCategoryObject.Get()))
					{
						if (CurClass != DeclaredClass && DeclaredClass->IsChildOf(CurClass))
						{
							UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Retyping producer %s from %s to %s"),
								*VarName, *CurClass->GetName(), *DeclaredClass->GetName());
							RHSValue.Pin->PinType.PinSubCategoryObject = DeclaredClass;
						}
					}
				}
			}
		}

		UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Storing producer pin for temp var %s"), *VarName);
		Builder.ProducerPins.Add(VarName, RHSValue.Pin);
		// Track the most recent boolean so EX_PopExecutionFlowIfNot can use it
		// as its branch condition (e.g. CallFunc_Less_IntInt_ReturnValue).
		if (RHSValue.Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			Builder.LastBoolPin = RHSValue.Pin;
		}
		// Return the emitted call node as the statement's anchor so deferred
		// exec wires (switch case body entries) can wire into it.
		return RHSCallNode;
	}

	/* Value-guard temps: a temp whose only write is a literal (Select option
	 * constants, ternary branches, bool flags) exists solely to feed pins.
	 * Record the constant and skip node creation - reads resolve through
	 * TempConstants as pin default values, exactly like the Set Array Elem
	 * bSizeToFit='true' case. Must run BEFORE the Parm-flag branch: exporter
	 * JSON for some frame locals carries Parm flags even though they are
	 * plain value temps (e.g. Temp_bool_Variable_5 feeding Array_Set Item). */
	if (bIsTempVar && !RHSValue.Pin && RHSValue.bConstant)
	{
		Builder.TempConstants.Add(VarName, RHSValue.ConstString);
		UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitLet si=%d: temp %s := '%s' recorded as value guard (no SET node)"),
			Stmt.StatementIndex, *VarName, *RHSValue.ConstString);
		return nullptr;
	}

	// Function out parameters (Parm flag) should wire to the Return Node, not a SET node
	if (PropObj.IsValid() && PropObj->HasField(TEXT("PropertyFlags")))
	{
		FString PropFlags = PropObj->GetStringField(TEXT("PropertyFlags"));
		if (PropFlags.Contains(TEXT("Parm")))
		{
			UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Out param %s: wiring to Return Node"), *VarName);
			if (Builder.ResultNode && RHSValue.Pin)
			{
				UEdGraphPin* ReturnPin = FindPin(Builder.ResultNode, *VarName, EGPD_Input);
				if (ReturnPin)
				{
					ConnectPins(RHSValue.Pin, ReturnPin);
					UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Wired %s to Return Node (rhs=%s links=%d, return=%s links=%d)"),
						*VarName,
						*RHSValue.Pin->PinName.ToString(), RHSValue.Pin->LinkedTo.Num(),
						*ReturnPin->PinName.ToString(), ReturnPin->LinkedTo.Num());
				}
				else
				{
					UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("  -> Return Node has no pin for '%s'"), *VarName);
				}
			}
			return nullptr;
		}
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

    // Wire RHS to value pin (on VariableSet, the input pin is named after the variable)
    UEdGraphPin* ValuePin = FindPin(SetNode, *VarName, EGPD_Input);
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET value pin lookup: varName=%s, foundByName=%d"), *VarName, ValuePin ? 1 : 0);
    if (!ValuePin)
    {
        ValuePin = FindPin(SetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Input);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET fallback to ReturnValue: found=%d"), ValuePin ? 1 : 0);
    }
    if (!ValuePin)
    {
        // Local compiler temp variable (e.g. Temp_int_Loop_Counter_Variable) whose
        // member reference cannot resolve - create the value pin manually from JSON.
        ValuePin = CreatePinForJsonProperty(SetNode, EGPD_Input, PropObj, FName(*VarName));
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET manual pin for local var %s: %d"), *VarName, ValuePin ? 1 : 0);
    }
    if (ValuePin)
    {
        if (RHSValue.Pin)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Wiring RHS pin (%s) to SET value pin (%s)"), *RHSValue.Pin->PinName.ToString(), *ValuePin->PinName.ToString());
            ConnectPins(RHSValue.Pin, ValuePin);
            if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
            {
                Builder.LastBoolPin = RHSValue.Pin;
            }
        }
        else if (RHSValue.bConstant)
        {
            SetPinDefaultValueSafe(ValuePin, RHSValue.ConstString);
            // Enum (byte) pins must store the enumerator NAME, not the raw index,
            // or the graph shows "1" instead of the enum string.
            if (ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && ValuePin->PinType.PinSubCategoryObject.IsValid())
            {
                if (UEnum* Enum = Cast<UEnum>(ValuePin->PinType.PinSubCategoryObject.Get()))
                {
                    const int32 EnumIndex = FCString::Atoi(*RHSValue.ConstString);
                    const FString EnumName = Enum->GetNameStringByIndex(EnumIndex);
                    if (!EnumName.IsEmpty())
                    {
                        ValuePin->DefaultValue = EnumName;
                        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Enum pin: %s (%s)"), *EnumName, *Enum->GetName());
                    }
                }
            }
        }
        else
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET value pin found but no RHS pin or constant to wire"));
        }
    }

    // For EX_Context variables, wire the set node's target to the context object
    // (e.g. setting "Load Scene" on the GI_Data instance returned by a call, or
    // setting Widget.GM on a widget created earlier in the same event).
    if (bIsContextVar)
    {
        const TSharedPtr<FJsonObject>& ObjExpr = VarObj->GetObjectField(TEXT("ObjectExpression"));
        if (ObjExpr.IsValid())
        {
            FPinValue CtxValue = ResolveExpression(Builder, ObjExpr);
            if (CtxValue.Pin)
            {
                UEdGraphPin* TargetPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Self.ToString(), EGPD_Input);
                if (!TargetPin)
                {
                    // External member sets on dependency-imported classes whose property
                    // isn't present on the class don't get a self pin from
                    // AllocateDefaultPins - create one so the target can still wire.
                    TargetPin = SetNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, FName(), OwnerClass, UEdGraphSchema_K2::PN_Self);
                }
                if (TargetPin)
                {
                    // Coerce the producer pin's object type so the connection is compatible
                    if (!CtxValue.Pin->PinType.PinSubCategoryObject.IsValid())
                    {
                        CtxValue.Pin->PinType.PinSubCategoryObject = OwnerClass;
                    }
                    ConnectPins(CtxValue.Pin, TargetPin);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Wired context target for %s (%s links=%d)"), *VarName, *CtxValue.Pin->PinName.ToString(), CtxValue.Pin->LinkedTo.Num());
                }
            }
        }
    }

    // Expose the variable's current value through the SET node's "Output_Get"
    // getter, matching the reference graph shape: editor-created Set nodes carry
    // an Output_Get output pin whose tooltip says it can replace a separate Get
    // node, and reads of the variable in bytecode use that getter. At import time
    // the local's member reference cannot resolve (its FProperty is only created
    // during compilation), so AllocateDefaultPins creates no pins and no
    // Output_Get. We create the Output_Get pin manually and publish it as the
    // producer. At compile time the local resolves via the skeleton class (the
    // compilation manager recompiles the skeleton before the class layout pass,
    // and IsGeneratedClassLayoutReady() is false during class layout, so
    // GetPropertyForVariable() falls back to the skeleton), which means
    // UK2Node_VariableSet::ExpandNode removes the Output_Get pin and routes
    // consumers through an intermediate UK2Node_VariableGet.
    if (bIsTempVar && ValuePin)
    {
        UEdGraphPin* OutGetPin = FindPin(SetNode, TEXT("Output_Get"), EGPD_Output);
        if (!OutGetPin)
        {
            OutGetPin = CreatePinForJsonProperty(SetNode, EGPD_Output, PropObj, FName(TEXT("Output_Get")));
            if (OutGetPin)
            {
                OutGetPin->PinToolTip = TEXT("Retrieves the value of the variable, can use instead of a separate Get node");
                if (OutGetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
                {
                    OutGetPin->DefaultValue = TEXT("0");
                    OutGetPin->AutogeneratedDefaultValue = TEXT("0");
                }
            }
        }
        if (OutGetPin)
        {
            Builder.ProducerPins.Add(VarName, OutGetPin);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Registered Output_Get producer for temp var %s (pin=%s)"), *VarName, *OutGetPin->PinName.ToString());
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    SetNode->NodePosX = Builder.NextNodeX;
    SetNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

    return SetNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitMakeStructFieldSet(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;
    const TSharedPtr<FJsonObject>& VarObj = Json->GetObjectField(TEXT("Variable"));
    if (!VarObj.IsValid()) return nullptr;

    // Only reconstruct writes into a MakeStruct temp (K2Node_MakeStruct_*). Other
    // struct-member writes (fields of instance/context struct variables) are not
    // part of the MakeStruct pattern and are left unemitted.
    const TSharedPtr<FJsonObject>& LhsStructExpr = VarObj->GetObjectField(TEXT("StructExpression"));
    if (!LhsStructExpr.IsValid()) return nullptr;
    const TSharedPtr<FJsonObject>& LhsStructExprVar = LhsStructExpr->GetObjectField(TEXT("Variable"));
    const TSharedPtr<FJsonObject>& LhsStructExprProp = LhsStructExprVar.IsValid() ? LhsStructExprVar->GetObjectField(TEXT("Property")) : nullptr;
    const FString LhsStructExprName = LhsStructExprProp.IsValid() ? LhsStructExprProp->GetStringField(TEXT("Name")) : TEXT("");
    if (!LhsStructExprName.StartsWith(TEXT("K2Node_MakeStruct_")))
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMakeStructFieldSet: non-MakeStruct struct member write %s, skipping"), *LhsStructExprName);
        return nullptr;
    }

    // LHS: EX_StructMemberContext { Property: { Owner: <struct>, Property: <field> }, StructExpression: <make temp> }
    const TSharedPtr<FJsonObject>& LhsPropObj = VarObj->GetObjectField(TEXT("Property"));
    if (!LhsPropObj.IsValid()) return nullptr;
    const TSharedPtr<FJsonObject>& LhsOwnerObj = LhsPropObj->GetObjectField(TEXT("Owner"));
    if (!LhsOwnerObj.IsValid()) return nullptr;

    FString StructObjName = LhsOwnerObj->GetStringField(TEXT("ObjectName"));
    StructObjName.RemoveFromStart(TEXT("UserDefinedStruct'"));
    StructObjName.RemoveFromStart(TEXT("Class'"));
    StructObjName.RemoveFromEnd(TEXT("'"));
    int32 ColonIdx;
    if (StructObjName.FindChar(TEXT(':'), ColonIdx)) StructObjName = StructObjName.Left(ColonIdx);
    if (StructObjName.IsEmpty()) return nullptr;

    UScriptStruct* StructType = ResolveUserDefinedStruct(StructObjName, LhsOwnerObj->GetStringField(TEXT("ObjectPath")));
    if (!StructType)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMakeStructFieldSet: cannot find struct %s"), *StructObjName);
        return nullptr;
    }

    // The MakeStruct temp this field-set writes into (K2Node_MakeStruct_*)
    const TSharedPtr<FJsonObject>& StructExpr = VarObj->GetObjectField(TEXT("StructExpression"));
    FString MakeTempName;
    if (StructExpr.IsValid())
    {
        const TSharedPtr<FJsonObject>& StructExprVar = StructExpr->GetObjectField(TEXT("Variable"));
        if (StructExprVar.IsValid())
        {
            const TSharedPtr<FJsonObject>& StructExprProp = StructExprVar->GetObjectField(TEXT("Property"));
            if (StructExprProp.IsValid())
            {
                MakeTempName = StructExprProp->GetStringField(TEXT("Name"));
            }
        }
    }

    /* A MakeStruct temp can be constructed in more than one bytecode branch (e.g.
     * BuildSaveData builds MakeStruct_S_Save in both the success and the fallback
     * path, reusing the same frame-slot name). Field-sets belonging to one
     * construction run are consecutive statements in the statement stream; an
     * intervening statement (a call/jump/return) starts a new run, which needs its
     * own MakeStruct node - otherwise both branches fight over one node and the
     * later branch's wiring replaces the earlier branch's, leaving its field reads
     * dangling. StatementIndex is a bytecode offset (arbitrary gaps), so detect
     * contiguity through the ordered statement array. */
    bool bContinuesRun = false;
    {
        const int32 CurPos = Builder.StmtIndexToArrayPos.FindRef(Stmt.StatementIndex);
        if (CurPos > 0 && CurPos < Builder.Statements.Num())
        {
            const FBytecodeToken* PrevStmt = Builder.Statements[CurPos - 1];
            if (PrevStmt && PrevStmt->JsonData.IsValid())
            {
                const TSharedPtr<FJsonObject>& PrevVarObj = PrevStmt->JsonData->GetObjectField(TEXT("Variable"));
                if (PrevVarObj.IsValid())
                {
                    const TSharedPtr<FJsonObject>& PrevStructExpr = PrevVarObj->GetObjectField(TEXT("StructExpression"));
                    if (PrevStructExpr.IsValid())
                    {
                        const TSharedPtr<FJsonObject>& PrevStructExprVar = PrevStructExpr->GetObjectField(TEXT("Variable"));
                        if (PrevStructExprVar.IsValid())
                        {
                            const TSharedPtr<FJsonObject>& PrevStructExprProp = PrevStructExprVar->GetObjectField(TEXT("Property"));
                            if (PrevStructExprProp.IsValid())
                            {
                                bContinuesRun = (PrevStructExprProp->GetStringField(TEXT("Name")) == MakeTempName);
                            }
                        }
                    }
                }
            }
        }
    }
    if (!bContinuesRun)
    {
        ++Builder.MakeStructRunId;
    }

    const FString MakeNodeKey = StructObjName + TEXT("_run") + FString::FromInt(Builder.MakeStructRunId);

    // Find or create the MakeStruct node for this construction run
    UK2Node_MakeStruct* MakeNode = nullptr;
    if (UK2Node_MakeStruct** Found = Builder.MakeStructNodes.Find(MakeNodeKey))
    {
        MakeNode = *Found;
    }
    else
    {
        MakeNode = NewObject<UK2Node_MakeStruct>(Builder.Graph);
        MakeNode->CreateNewGuid();
        MakeNode->SetFlags(RF_Transactional);
        MakeNode->StructType = StructType;
        Builder.Graph->AddNode(MakeNode, true, true);
        MakeNode->AllocateDefaultPins();
        MakeNode->NodePosX = Builder.NextNodeX - 200;
        MakeNode->NodePosY = Builder.NextNodeY;
        Builder.NextNodeY += 120;
        Builder.MakeStructNodes.Add(MakeNodeKey, MakeNode);

        // Register the MakeStruct temp as the producer of the built struct value
        // (consumed by Array_Set.Item = K2Node_MakeStruct_S_CharData). Re-registering
        // the same temp name on a later run overwrites the producer, which is correct
        // because the later branch's consumer resolves after this run's field-sets.
        if (!MakeTempName.IsEmpty())
        {
            UEdGraphPin* MakeReturn = nullptr;
            for (UEdGraphPin* Pin : MakeNode->Pins)
            {
                if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                {
                    MakeReturn = Pin;
                    break;
                }
            }
            if (!MakeReturn)
            {
                MakeReturn = FindPin(MakeNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
            }
            if (MakeReturn)
            {
                Builder.ProducerPins.Add(MakeTempName, MakeReturn);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMakeStructFieldSet: registered producer %s -> MakeStruct %s (run %d)"),
                    *MakeTempName, *StructObjName, Builder.MakeStructRunId);
            }
        }
    }

    // Target field (LHS) of this struct write - used to find the MakeStruct input pin
    const TSharedPtr<FJsonObject>& LhsFieldObj = LhsPropObj->GetObjectField(TEXT("Property"));
    const FString LhsFieldName = LhsFieldObj.IsValid() ? LhsFieldObj->GetStringField(TEXT("Name")) : TEXT("");
    const FString LhsBase = LhsFieldName.IsEmpty() ? TEXT("") : StripStructMemberSuffix(LhsFieldName);

    // Resolve the RHS (field of the source struct / constant) -> BreakStruct field pin
    const TSharedPtr<FJsonObject>& ExprJson = Json->GetObjectField(TEXT("Expression"));
    FPinValue RhsValue = ExprJson.IsValid() ? ResolveExpression(Builder, ExprJson) : FPinValue();

    // Find the MakeStruct input pin matching the target field (match by base name)
    UEdGraphPin* TargetPin = nullptr;
    for (UEdGraphPin* Pin : MakeNode->Pins)
    {
        if (Pin->Direction == EGPD_Input && Pin->PinName != UEdGraphSchema_K2::PN_Execute
            && !LhsBase.IsEmpty() && StripStructMemberSuffix(Pin->PinName.ToString()) == LhsBase)
        {
            TargetPin = Pin;
            break;
        }
    }

    if (!TargetPin)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMakeStructFieldSet: no MakeStruct input pin matching base '%s'"), *LhsBase);
        return nullptr;
    }

    if (RhsValue.Pin)
    {
        ConnectPins(RhsValue.Pin, TargetPin);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMakeStructFieldSet: wired %s -> %s (base=%s)"),
            *RhsValue.Pin->PinName.ToString(), *TargetPin->PinName.ToString(), *LhsBase);
    }
    else if (RhsValue.bConstant)
    {
        FString DefaultValue = RhsValue.ConstString;
        // String pin DefaultValue is stored raw (no surrounding quotes)
        if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String
            && DefaultValue.Len() >= 2 && DefaultValue.StartsWith(TEXT("\"")) && DefaultValue.EndsWith(TEXT("\"")))
        {
            DefaultValue = DefaultValue.Mid(1, DefaultValue.Len() - 2);
        }
        SetPinDefaultValueSafe(TargetPin, DefaultValue);
        // Enum (byte) pins must store the enumerator NAME, not the raw index
        if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && TargetPin->PinType.PinSubCategoryObject.IsValid())
        {
            if (UEnum* Enum = Cast<UEnum>(TargetPin->PinType.PinSubCategoryObject.Get()))
            {
                const int32 EnumIndex = FCString::Atoi(*RhsValue.ConstString);
                const FString EnumName = Enum->GetNameStringByIndex(EnumIndex);
                if (!EnumName.IsEmpty())
                {
                    TargetPin->DefaultValue = EnumName;
                }
            }
        }
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMakeStructFieldSet: set default '%s' on %s (base=%s)"),
            *DefaultValue, *TargetPin->PinName.ToString(), *LhsBase);
    }
    else
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMakeStructFieldSet: no RHS pin or constant for %s at si=%d"), *StructObjName, Stmt.StatementIndex);
    }

    return nullptr;
}

/* Field-member writes whose base is not a MakeStruct temp compiled from a
 * modify-in-place UK2Node_SetFieldsInStruct: StructRef receives a struct
 * reference (most often an array element getter's by-ref Output), field input
 * pins receive the new values, and StructOut flows on to the consuming
 * variable SET. The compiler lowers that node into EX_StructMemberContext LETs
 * followed by a copy-out LET into the frame local
 * "K2Node_SetFieldsInStruct_StructOut"; this emitter reverses the lowering.
 * Field pins are NOT created by AllocateDefaultPins - its pin manager flags
 * every record bShowPin=false, so the written fields must be enabled in
 * ShowPinForProperties and the node reconstructed to become connectable. */
UEdGraphNode* FBlueprintBytecodeImporter::EmitStructFieldRefWrite(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;
    const TSharedPtr<FJsonObject>& VarObj = Json->GetObjectField(TEXT("Variable"));
    if (!VarObj.IsValid()) return nullptr;

    // LHS member owner struct - same JSON shape as the MakeStruct field-set path
    const TSharedPtr<FJsonObject>& LhsPropObj = VarObj->GetObjectField(TEXT("Property"));
    const TSharedPtr<FJsonObject>& LhsOwnerObj = LhsPropObj.IsValid() ? LhsPropObj->GetObjectField(TEXT("Owner")) : nullptr;
    if (!LhsOwnerObj.IsValid()) return nullptr;

    FString StructObjName = LhsOwnerObj->GetStringField(TEXT("ObjectName"));
    StructObjName.RemoveFromStart(TEXT("UserDefinedStruct'"));
    StructObjName.RemoveFromStart(TEXT("ScriptStruct'"));
    StructObjName.RemoveFromStart(TEXT("Class'"));
    StructObjName.RemoveFromEnd(TEXT("'"));
    int32 ColonIdx;
    if (StructObjName.FindChar(TEXT(':'), ColonIdx)) StructObjName = StructObjName.Left(ColonIdx);
    if (StructObjName.IsEmpty()) return nullptr;

    UScriptStruct* StructType = ResolveUserDefinedStruct(StructObjName, LhsOwnerObj->GetStringField(TEXT("ObjectPath")));
    if (!StructType)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitStructFieldRefWrite: cannot find struct %s (si=%d)"), *StructObjName, Stmt.StatementIndex);
        return nullptr;
    }

    const TSharedPtr<FJsonObject>& LhsFieldObj = LhsPropObj->GetObjectField(TEXT("Property"));
    const FString FieldName = LhsFieldObj.IsValid() ? LhsFieldObj->GetStringField(TEXT("Name")) : TEXT("");
    const FString FieldBase = FieldName.IsEmpty() ? TEXT("") : StripStructMemberSuffix(FieldName);

    // Base struct reference: whatever produces it (by-ref array getter output,
    // instance-variable get, another node's StructOut, ...)
    UEdGraphPin* BasePin = nullptr;
    FString BaseTempName;
    const TSharedPtr<FJsonObject>& LhsStructExpr = VarObj->GetObjectField(TEXT("StructExpression"));
    if (LhsStructExpr.IsValid())
    {
        const FPinValue BaseValue = ResolveExpression(Builder, LhsStructExpr);
        BasePin = BaseValue.Pin;
        const TSharedPtr<FJsonObject>& SEVar = LhsStructExpr->GetObjectField(TEXT("Variable"));
        const TSharedPtr<FJsonObject>& SEProp = SEVar.IsValid() ? SEVar->GetObjectField(TEXT("Property")) : nullptr;
        if (SEProp.IsValid()) BaseTempName = SEProp->GetStringField(TEXT("Name"));
    }
    if (!BasePin)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitStructFieldRefWrite: no base struct pin for %s.%s (si=%d)"), *BaseTempName, *FieldName, Stmt.StatementIndex);
        return nullptr;
    }

    UK2Node_SetFieldsInStruct* SetNode = NewObject<UK2Node_SetFieldsInStruct>(Builder.Graph);
    SetNode->CreateNewGuid();
    SetNode->SetFlags(RF_Transactional);
    SetNode->StructType = StructType;
    Builder.Graph->AddNode(SetNode, true, true);
    SetNode->AllocateDefaultPins();

    /* AllocateDefaultPins rebuilds ShowPinForProperties with every record
     * bShowPin=false (FSetFieldsInStructPinManager::GetRecordDefaults), so no
     * field pins exist yet. Enable the written field(s) and reconstruct -
     * RebuildPropertyList preserves flags by property name, mirroring what the
     * editor's RestoreAllPins/RemoveFieldPins actions do. */
    bool bFieldEnabled = false;
    for (FOptionalPinFromProperty& Opt : SetNode->ShowPinForProperties)
    {
        const FString OptName = Opt.PropertyName.ToString();
        if ((!FieldName.IsEmpty() && OptName == FieldName)
            || (!FieldBase.IsEmpty() && StripStructMemberSuffix(OptName) == FieldBase))
        {
            Opt.bShowPin = true;
            bFieldEnabled = true;
        }
    }
    if (bFieldEnabled)
    {
        SetNode->ReconstructNode();
    }

    // Exec chain (the node is impure: Execute / then)
    if (Builder.LastExecPin)
    {
        if (UEdGraphPin* ExecPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input))
        {
            ConnectPins(Builder.LastExecPin, ExecPin);
        }
    }
    if (UEdGraphPin* ThenPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output))
    {
        Builder.LastExecPin = ThenPin;
    }

    // StructRef <- base struct reference
    if (UEdGraphPin* RefPin = FindPin(SetNode, TEXT("StructRef"), EGPD_Input))
    {
        ConnectPins(BasePin, RefPin);
    }

    // Field input <- resolved RHS expression (Select / call result / constant)
    const TSharedPtr<FJsonObject>& ExprJson = Json->GetObjectField(TEXT("Expression"));
    FPinValue RhsValue = ExprJson.IsValid() ? ResolveExpression(Builder, ExprJson) : FPinValue();

    UEdGraphPin* TargetPin = nullptr;
    for (UEdGraphPin* Pin : SetNode->Pins)
    {
        if (Pin->Direction == EGPD_Input
            && Pin->PinName != UEdGraphSchema_K2::PN_Execute
            && Pin->PinName != TEXT("StructRef")
            && !FieldBase.IsEmpty()
            && StripStructMemberSuffix(Pin->PinName.ToString()) == FieldBase)
        {
            TargetPin = Pin;
            break;
        }
    }
    if (TargetPin && RhsValue.Pin)
    {
        ConnectPins(RhsValue.Pin, TargetPin);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitStructFieldRefWrite: wired %s -> %s.%s"),
            *RhsValue.Pin->PinName.ToString(), *StructObjName, *TargetPin->PinName.ToString());
    }
    else if (TargetPin && RhsValue.bConstant)
    {
        FString DefaultValue = RhsValue.ConstString;
        // String pin DefaultValue is stored raw (no surrounding quotes)
        if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String
            && DefaultValue.Len() >= 2 && DefaultValue.StartsWith(TEXT("\"")) && DefaultValue.EndsWith(TEXT("\"")))
        {
            DefaultValue = DefaultValue.Mid(1, DefaultValue.Len() - 2);
        }
        SetPinDefaultValueSafe(TargetPin, DefaultValue);
        // Enum (byte) pins must store the enumerator NAME, not the raw index
        if (TargetPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && TargetPin->PinType.PinSubCategoryObject.IsValid())
        {
            if (UEnum* Enum = Cast<UEnum>(TargetPin->PinType.PinSubCategoryObject.Get()))
            {
                const int32 EnumIndex = FCString::Atoi(*RhsValue.ConstString);
                const FString EnumName = Enum->GetNameStringByIndex(EnumIndex);
                if (!EnumName.IsEmpty())
                {
                    TargetPin->DefaultValue = EnumName;
                }
            }
        }
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitStructFieldRefWrite: set default '%s' on %s.%s"),
            *DefaultValue, *StructObjName, *TargetPin->PinName.ToString());
    }
    else if (!RhsValue.Pin && !RhsValue.bConstant)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitStructFieldRefWrite: no RHS for %s.%s (si=%d)"), *BaseTempName, *FieldName, Stmt.StatementIndex);
    }

    /* Register StructOut under the canonical copy-out local so the follow-up
     * assignment ("Character Apparance = K2Node_SetFieldsInStruct_StructOut")
     * wires straight from StructOut as in the original graph. The copy-out LET
     * itself keeps this producer (skip guard in EmitLet). */
    if (UEdGraphPin* OutPin = FindPin(SetNode, TEXT("StructOut"), EGPD_Output))
    {
        Builder.ProducerPins.Add(TEXT("K2Node_SetFieldsInStruct_StructOut"), OutPin);
    }

    SetNode->NodePosX = Builder.NextNodeX;
    SetNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Emitted UK2Node_SetFieldsInStruct (%s.%s <- base %s) si=%d"),
        *StructObjName, *FieldName, *BaseTempName, Stmt.StatementIndex);
    return SetNode;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitSetArray(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    /* UK2Node_MakeArray's compiled form: an EX_SetArray of the node's frame
     * temp ("K2Node_MakeArray_Array"). Several SetArray sites = the same pure
     * node evaluated in multiple contexts (loop gate + loop body), one editor
     * node, one net - cache by the assigning temp name. (08.25: Call for NPC
     * interaction had no handler at all - the MakeArray fell to the
     * unhandled-token path, its consumers starved, and the loop's
     * SetSkinnedAssetAndUpdate lost its Target.) */
    if (!Stmt.JsonData.IsValid()) return nullptr;

    FString TempName;
    const TSharedPtr<FJsonObject>* AssigningProp = nullptr;
    if (Stmt.JsonData->TryGetObjectField(TEXT("AssigningProperty"), AssigningProp) && AssigningProp && (*AssigningProp)->HasField(TEXT("Variable")))
    {
        const TSharedPtr<FJsonObject>& VarObj = (*AssigningProp)->GetObjectField(TEXT("Variable"));
        const TSharedPtr<FJsonObject>* PropObj = nullptr;
        if (VarObj.IsValid() && VarObj->TryGetObjectField(TEXT("Property"), PropObj) && PropObj)
        {
            TempName = (*PropObj)->GetStringField(TEXT("Name"));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
    Stmt.JsonData->TryGetArrayField(TEXT("Elements"), Elements);

    if (TempName.IsEmpty() || !Elements || Elements->Num() == 0)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EX_SetArray at si=%d: missing assigning temp or elements"), Stmt.StatementIndex);
        return nullptr;
    }

    if (UK2Node_MakeArray** Existing = Builder.MakeArrayNodes.Find(TempName))
    {
        return *Existing;
    }

    UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Builder.Graph);
    MakeArrayNode->CreateNewGuid();
    MakeArrayNode->SetFlags(RF_Transactional);
    MakeArrayNode->NodePosX = Builder.NextNodeX - 200;
    MakeArrayNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;
    MakeArrayNode->NumInputs = Elements->Num();
    Builder.Graph->AddNode(MakeArrayNode, true, true);
    MakeArrayNode->AllocateDefaultPins();

    for (int32 Idx = 0; Idx < Elements->Num(); ++Idx)
    {
        const TSharedPtr<FJsonObject> ElemObj = (*Elements)[Idx]->AsObject();
        if (!ElemObj.IsValid()) continue;
        FPinValue ElemValue = ResolveExpression(Builder, ElemObj);
        UEdGraphPin* ElemPin = MakeArrayNode->FindPin(FName(*FString::Printf(TEXT("[%d]"), Idx)), EGPD_Input);
        if (ElemPin && ElemValue.Pin)
        {
            ConnectPins(ElemValue.Pin, ElemPin);
        }
        else if (ElemPin && ElemValue.bConstant)
        {
            SetPinDefaultValueSafe(ElemPin, ElemValue.ConstString);
        }
    }

    UEdGraphPin* OutputPin = MakeArrayNode->FindPin(TEXT("Array"), EGPD_Output);
    if (OutputPin)
    {
        Builder.ProducerPins.Add(TempName, OutputPin);
    }
    Builder.MakeArrayNodes.Add(TempName, MakeArrayNode);

    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Emitted UK2Node_MakeArray (%s, %d element(s)) si=%d"),
        *TempName, Elements->Num(), Stmt.StatementIndex);
    return MakeArrayNode;
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
			ExprToken == TEXT("EX_LocalVirtualFunction") || ExprToken == TEXT("EX_VirtualFunction") ||
			ExprToken == TEXT("EX_DynamicCast") || ExprToken == TEXT("EX_Cast"))
		{
			UEdGraphNode* CallNode = EmitExpressionAsExec(Builder, ExprJson, Stmt.StatementIndex);
			if (CallNode)
			{
				UEdGraphPin* ReturnPin = FindPin(CallNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
				if (!ReturnPin)
				{
					ReturnPin = FindPin(CallNode, TEXT("ReturnValue"), EGPD_Output);
				}
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

    // Wire RHS to value pin (on VariableSet, the input pin is named after the variable)
    UEdGraphPin* ValuePin = FindPin(SetNode, *VarName, EGPD_Input);
    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET value pin lookup: varName=%s, foundByName=%d"), *VarName, ValuePin ? 1 : 0);
    if (!ValuePin)
    {
        ValuePin = FindPin(SetNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Input);
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET fallback to ReturnValue: found=%d"), ValuePin ? 1 : 0);
    }
    if (ValuePin)
    {
        if (RHSValue.Pin)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> Wiring RHS pin (%s) to SET value pin (%s)"), *RHSValue.Pin->PinName.ToString(), *ValuePin->PinName.ToString());
            ConnectPins(RHSValue.Pin, ValuePin);
        }
        else if (RHSValue.bConstant)
        {
            SetPinDefaultValueSafe(ValuePin, RHSValue.ConstString);
        }
        else
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> SET value pin found but no RHS pin or constant to wire"));
        }
    }

    // Update last exec pin
    UEdGraphPin* ThenPin = FindPin(SetNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
    if (ThenPin)
    {
        Builder.LastExecPin = ThenPin;
    }

    SetNode->NodePosX = Builder.NextNodeX;
    SetNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;

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
        // A jump transfers control unconditionally - there is no fall-through
        // to the statement after it. Drop the chain pin so the tail result-node
        // wiring doesn't also wire the jump source to the function return (which
        // would turn a loop back edge into a straight-line return). The target
        // receives its wire directly (anchor already emitted) or via the
        // deferred wire resolution below.
        if (Builder.StatementAnchors.Contains(TargetStmtIdx) || Builder.StmtIndexToArrayPos.Contains(TargetStmtIdx))
        {
            Builder.LastExecPin = nullptr;
        }
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
    BranchNode->NodePosX = Builder.NextNodeX + 200;
    BranchNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;
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

    // Wire false branch (else) to target, or to return node if no explicit target
    UEdGraphPin* ElsePin = BranchNode->GetElsePin();
    if (ElsePin)
    {
        bool bWired = false;
        if (TargetStmtIdx >= 0)
        {
            WireExecFromPin(Builder, ElsePin, TargetStmtIdx);
            // Wired immediately (anchor already emitted, e.g. a loop back edge)
            // or deferred because the target is a forward jump target emitted
            // later in the path (e.g. a switch case body). In both cases the
            // else branch is accounted for, so don't fall through to return.
            bWired = ElsePin->LinkedTo.Num() > 0 || Builder.StmtIndexToArrayPos.Contains(TargetStmtIdx);
        }
        if (!bWired && Builder.ResultNode)
        {
            // Target had no anchor (e.g. EX_Return) or no explicit target —
            // false branch means early return
            UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
            if (ResultExecPin)
            {
                ConnectPins(ElsePin, ResultExecPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Branch false -> Return Node exec wired (no anchor / no explicit target)"));
            }
        }
    }

    // When the condition is a DynamicCast's bSuccess output, the cast's failure
    // path must follow the same destination as this branch's else (the
    // EX_JumpIfNot target). The cast node was emitted with a default
    // CastFailed -> return wire; the real failure destination is the jump
    // target (e.g. a "no save data" PrintString in the Load functions), while
    // Build functions jump straight to the return and keep the return wire.
    if (CondValue.Pin && CondValue.Pin->GetOwningNode())
    {
        if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(CondValue.Pin->GetOwningNode()))
        {
            UEdGraphPin* CastFailedPin = FindPin(CastNode, TEXT("CastFailed"), EGPD_Output);
            if (!CastFailedPin)
            {
                CastFailedPin = FindPin(CastNode, TEXT("Failed"), EGPD_Output);
            }
            if (CastFailedPin)
            {
                CastFailedPin->BreakAllPinLinks();
                if (TargetStmtIdx >= 0)
                {
                    WireExecFromPin(Builder, CastFailedPin, TargetStmtIdx);
                }
                else if (Builder.ResultNode)
                {
                    UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
                    if (ResultExecPin)
                    {
                        ConnectPins(CastFailedPin, ResultExecPin);
                    }
                }
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("DynamicCast CastFailed routed to else target si=%d"), TargetStmtIdx);
            }
        }
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

    // Get the pushed address to jump to. The export JSON carries none - the
    // target is reconstructed from BuildLinearPath's FlowStack.
    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("PushingAddress")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("PushingAddress"));
    }
    else if (const int32* Reconstructed = UbergraphPopTargets.Find(Stmt.StatementIndex))
    {
        TargetStmtIdx = *Reconstructed;
    }

    if (TargetStmtIdx >= 0)
    {
        /* The popped address is a branch-merge point, not a fall-through:
         * mark it so the walk breaks the incoming chain (the explicit wire
         * below provides the exec input). */
        Builder.RegionStartIndices.Add(TargetStmtIdx);
        WireExecFromPin(Builder, Builder.LastExecPin, TargetStmtIdx);
    }

    return nullptr;
}

UEdGraphNode* FBlueprintBytecodeImporter::EmitPopExecutionFlowIfNot(FFunctionBuilder& Builder, const FBytecodeToken& Stmt)
{
    const TSharedPtr<FJsonObject>& Json = Stmt.JsonData;

    // Get the pushed address. The export JSON carries none - the target is
    // reconstructed from BuildLinearPath's FlowStack (where FALSE would pop).
    int32 TargetStmtIdx = -1;
    if (Json->HasField(TEXT("PushingAddress")))
    {
        TargetStmtIdx = Json->GetIntegerField(TEXT("PushingAddress"));
    }
    else if (const int32* Reconstructed = UbergraphPopTargets.Find(Stmt.StatementIndex))
    {
        TargetStmtIdx = *Reconstructed;
    }

    // The condition is the most recently produced boolean (from a preceding
    // LetBool, e.g. CallFunc_Less_IntInt_ReturnValue). Wire it to the branch.
    // When the condition is FALSE the execution flow pops (loop/early exit);
    // when TRUE the path continues (fall-through below).
    UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Builder.Graph);
    BranchNode->CreateNewGuid();
    BranchNode->SetFlags(RF_Transactional);
    BranchNode->NodePosX = Builder.NextNodeX + 200;
    BranchNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;
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

    // Wire the condition pin. Prefer the token's own BooleanExpression -
    // inline member reads (e.g. GI_Instance->NewGame) compile without a
    // preceding LetBool, so LastBoolPin is stale or null there and the whole
    // chain (member VariableGet + its object producer) would dangle. Fall
    // back to the last-bool heuristic for gate shapes that rely on it.
    UEdGraphPin* CondPin = BranchNode->GetConditionPin();
    bool bConditionWired = false;
    const TSharedPtr<FJsonObject>* BoolExprPtr = nullptr;
    if (CondPin && Json->TryGetObjectField(TEXT("BooleanExpression"), BoolExprPtr) && BoolExprPtr && BoolExprPtr->IsValid())
    {
        FPinValue CondValue = ResolveExpression(Builder, *BoolExprPtr);
        if (CondValue.Pin)
        {
            ConnectPins(CondValue.Pin, CondPin);
            Builder.LastBoolPin = CondValue.Pin;
            bConditionWired = true;
        }
    }
    if (!bConditionWired && CondPin && Builder.LastBoolPin)
    {
        ConnectPins(Builder.LastBoolPin, CondPin);
    }

    // Wire false branch (else) to target (loop back/merge)
    UEdGraphPin* ElsePin = BranchNode->GetElsePin();
    if (ElsePin)
    {
        bool bWired = false;
        if (TargetStmtIdx >= 0)
        {
            WireExecFromPin(Builder, ElsePin, TargetStmtIdx);
            // Wired immediately (anchor already emitted, e.g. a loop back edge)
            // or deferred because the target is emitted later in the path. In
            // both cases the else branch is accounted for, so don't fall through
            // to the return node.
            bWired = ElsePin->LinkedTo.Num() > 0 || Builder.StmtIndexToArrayPos.Contains(TargetStmtIdx);
        }
        if (!bWired && Builder.ResultNode)
        {
            // PopExecutionFlowIfNot with no PushingAddress pops the address
            // pushed at function entry (the function's return). For loops the
            // compiler emits the false branch as the loop exit, which returns
            // from the function - wire it to the result node.
            UEdGraphPin* ResultExecPin = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
            if (ResultExecPin)
            {
                ConnectPins(ElsePin, ResultExecPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("PopExecutionFlowIfNot false -> Return Node exec wired (no explicit target)"));
            }
        }
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
    SwitchNode->NodePosX = Builder.NextNodeX + 200;
    SwitchNode->NodePosY = Builder.NextNodeY;
    Builder.NextNodeY += 120;
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
            // The target anchor already exists, so the jump points backwards
            // into already-emitted code - a loop back edge (e.g. EX_Jump back
            // to the condition). UE accepts multiple inputs on an exec pin, so
            // wire the back edge to re-enter the loop head.
            ConnectPins(FromPin, ExecPin);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> wire exec to si=%d (loop back edge)"), ToStmtIdx);
            return;
        }
    }

    // The target is part of this path but its anchor has not been emitted yet
    // (a forward jump, e.g. a switch case body emitted later in the path).
    // Record a deferred wire, resolved after the emit loop when all anchors
    // exist.
    if (Builder.StmtIndexToArrayPos.Contains(ToStmtIdx))
    {
        Builder.PendingExecWires.Add(TPair<UEdGraphPin*, int32>(FromPin, ToStmtIdx));
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> deferred exec wire to si=%d (forward jump target)"), ToStmtIdx);
    }
}

void FBlueprintBytecodeImporter::ResolvePendingExecWires(FFunctionBuilder& Builder)
{
    for (const TPair<UEdGraphPin*, int32>& Wire : Builder.PendingExecWires)
    {
        UEdGraphPin* FromPin = Wire.Key;
        const int32 ToStmtIdx = Wire.Value;
        if (!FromPin)
        {
            continue;
        }

        // The jump target may be a pure statement (e.g. a LetObj creating a
        // temp) with no anchor. Scan forward from the target's position to the
        // first statement that produced an anchor node (the region's real
        // entry), stopping at the next region boundary so we don't wire into a
        // different case body.
        int32 TargetPos = Builder.StmtIndexToArrayPos.FindRef(ToStmtIdx);
        if (TargetPos == INDEX_NONE)
        {
            continue;
        }

        UEdGraphNode* TargetAnchor = nullptr;
        int32 AnchorStmtIdx = -1;
        for (int32 Idx = TargetPos; Idx < Builder.Statements.Num(); ++Idx)
        {
            const FBytecodeToken* Stmt = Builder.Statements[Idx];
            if (Idx != TargetPos && Builder.RegionStartIndices.Contains(Stmt->StatementIndex))
            {
                break;
            }
            UEdGraphNode** FoundAnchor = Builder.StatementAnchors.Find(Stmt->StatementIndex);
            if (FoundAnchor && *FoundAnchor)
            {
                // Only accept anchors that carry an exec input pin (real
                // statement nodes: branches, calls, variable sets). Pure
                // statements (GetArrayItem, Let temp producers, pure function
                // calls like the Global Game Instance accessor) have no exec
                // pin, so a jump/back-edge targeting them must keep scanning
                // forward to the first real exec entry point (e.g. the
                // IfThenElse condition of a loop whose head starts with a pure
                // call). Otherwise the deferred exec wire is silently dropped.
                UEdGraphPin* ExecPin = FindPin(*FoundAnchor, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
                if (ExecPin)
                {
                    TargetAnchor = *FoundAnchor;
                    AnchorStmtIdx = Stmt->StatementIndex;
                    break;
                }
            }
        }

        if (TargetAnchor)
        {
            UEdGraphPin* ExecPin = FindPin(TargetAnchor, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
            if (ExecPin)
            {
                ConnectPins(FromPin, ExecPin);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("  -> resolved deferred exec wire to si=%d (anchor si=%d)"), ToStmtIdx, AnchorStmtIdx);
            }
        }
    }
    Builder.PendingExecWires.Reset();
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
        const bool bConnected = Schema->TryCreateConnection(PinA, PinB);
        if (!bConnected)
        {
            const FString FailDesc = FString::Printf(TEXT("'%s' (%s) -> '%s' (%s) on %s"),
                *PinA->PinName.ToString(), *PinA->PinType.PinCategory.ToString(),
                *PinB->PinName.ToString(), *PinB->PinType.PinCategory.ToString(),
                PinB->GetOwningNode() ? *PinB->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : TEXT("null"));
            UE_LOG(LogBlueprintBytecodeImporter, Warning,
                TEXT("  -> ConnectPins FAILED: %s (%s) [%s] {%s} -> %s (%s) [%s] {%s}"),
                *PinA->PinName.ToString(), *PinA->PinType.PinCategory.ToString(), *PinA->PinType.PinSubCategory.ToString(),
                PinA->PinType.PinSubCategoryObject.IsValid() ? *PinA->PinType.PinSubCategoryObject->GetName() : TEXT("null"),
                *PinB->PinName.ToString(), *PinB->PinType.PinCategory.ToString(), *PinB->PinType.PinSubCategory.ToString(),
                PinB->PinType.PinSubCategoryObject.IsValid() ? *PinB->PinType.PinSubCategoryObject->GetName() : TEXT("null"));
            AddDiagnostic(TEXT("ConnectFailed"), FailDesc);
        }
    }
}

FString FBlueprintBytecodeImporter::StripStructMemberSuffix(const FString& FullName) const
{
    // User-defined struct members are named "<Base>_<Index>_<32hex>" (e.g.
    // "Race_2_174850054493D496B6AA269101837FED"). Strip the trailing two
    // segments so fields across different structs can be matched by base name.
    FString Result = FullName;
    const int32 HashIdx = Result.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (HashIdx != INDEX_NONE)
    {
        const FString HashPart = Result.Mid(HashIdx + 1);
        bool bIsHex = HashPart.Len() == 32;
        if (bIsHex)
        {
            for (const TCHAR Ch : HashPart)
            {
                const bool bDigit = (Ch >= TEXT('0') && Ch <= TEXT('9'));
                const bool bLower = (Ch >= TEXT('a') && Ch <= TEXT('f'));
                const bool bUpper = (Ch >= TEXT('A') && Ch <= TEXT('F'));
                if (!(bDigit || bLower || bUpper)) { bIsHex = false; break; }
            }
        }
        if (bIsHex)
        {
            const int32 NumIdx = Result.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd, HashIdx - 1);
            if (NumIdx != INDEX_NONE)
            {
                const FString NumPart = Result.Mid(NumIdx + 1, HashIdx - NumIdx - 1);
                if (NumPart.IsNumeric())
                {
                    Result = Result.Left(NumIdx);
                }
            }
        }
    }
    return Result;
}

UK2Node_CallFunction* FBlueprintBytecodeImporter::CreateStubCallNode(const FString& FunctionName, UEdGraph* Graph)
{
    UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
    CallNode->CreateNewGuid();
    CallNode->SetFlags(RF_Transactional);
    CallNode->FunctionReference.SetExternalMember(FName(*FunctionName), GeneratedClass);
    Graph->AddNode(CallNode, true, true);

    // NOTE: deliberately do NOT call AllocateDefaultPins() here. For an
    // unresolvable reference it runs UK2Node_CallFunction's global fallback which
    // sweeps EVERY UClass in memory looking for the function in a function library
    // (K2Node_CallFunction.cpp) - with the imported class count that hangs for a
    // very long time. The stub pins are created manually below instead.
    // AllocateDefaultPins only creates pins when the referenced function resolves.
    // For unresolved (stub) functions add the essential pins manually so the node
    // can still be wired into the exec/data graph.
    if (!CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input))
    {
        CallNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, FName(), nullptr, UEdGraphSchema_K2::PN_Execute);
    }
    if (!CallNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output))
    {
        CallNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, FName(), nullptr, UEdGraphSchema_K2::PN_Then);
    }
    if (!CallNode->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input))
    {
        // Plain object pin (NOT PSC_Self) so the target object can be wired to it;
        // the type is set later from the resolved target pin.
        CallNode->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, FName(), nullptr, UEdGraphSchema_K2::PN_Self);
    }
    return CallNode;
}

FPinValue FBlueprintBytecodeImporter::ResolveSwitchValue(FFunctionBuilder& Builder, const TSharedPtr<FJsonObject>& ExprJson)
{
    /* Compiled Select: pick Cases[i].CaseTerm where IndexTerm equals
     * Cases[i].CaseIndexValueTerm. Reconstructed as a plain non-enum
     * UK2Node_Select whose option order matches case order - the compiler
     * emits contiguous 0..N-1 case literals for that shape, which is exactly
     * what the node's KCST_SwitchValue handler regenerates. The bytecode's
     * DefaultTerm has no dedicated pin on this node version (compiled default
     * falls back to option 0), so it is intentionally not wired. */
    const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
    if (!ExprJson->TryGetArrayField(TEXT("Cases"), Cases) || !Cases || Cases->Num() == 0)
    {
        return FPinValue();
    }

    UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Builder.Graph);
    SelectNode->CreateNewGuid();
    SelectNode->SetFlags(RF_Transactional);
    SelectNode->NodePosX = Builder.NextNodeX - 200;
    SelectNode->NodePosY = Builder.NextNodeY;
    Builder.Graph->AddNode(SelectNode, true, true);
    Builder.NextNodeY += 120;

    /* Index type detection UP FRONT: an enum-indexed select (IndexTerm is an
     * EnumProperty) must be created through SetEnum + AllocateDefaultPins so
     * the option pins are named after the enum entries. GetOptionPins()
     * switches to name-matching against EnumEntries as soon as IndexPinType
     * becomes an enum, and plain "Option N" pins then match NOTHING - the
     * compile dies with "No option pin in Select". */
    const TSharedPtr<FJsonObject>* IndexTermEarly = nullptr;
    const bool bHaveIndexTermEarly = ExprJson->TryGetObjectField(TEXT("IndexTerm"), IndexTermEarly) && IndexTermEarly && IndexTermEarly->IsValid();
    FEdGraphPinType EnumIndexType;
    UEnum* SelectEnum = nullptr;
    if (bHaveIndexTermEarly)
    {
        const TSharedPtr<FJsonObject>* EarlyVar = nullptr;
        if ((*IndexTermEarly)->TryGetObjectField(TEXT("Variable"), EarlyVar) && EarlyVar)
        {
            const TSharedPtr<FJsonObject>* EarlyProp = nullptr;
            if ((*EarlyVar)->TryGetObjectField(TEXT("Property"), EarlyProp) && EarlyProp)
            {
                EnumIndexType = PinTypeFromJson(*EarlyProp);
                if (EnumIndexType.PinCategory == UEdGraphSchema_K2::PC_Byte)
                {
                    SelectEnum = Cast<UEnum>(EnumIndexType.PinSubCategoryObject.Get());
                }
            }
        }
    }

    if (SelectEnum)
    {
        /* Engine reconstruct-entry pattern (K2Node_Select.cpp): SetEnum
         * populates EnumEntries, AllocateDefaultPins then creates the option
         * pins named after them and clears the deferred-reconstruct flag.
         * The Index pin stays WILDCARD here: when the enum net connects (last,
         * below) the node's own OnPinTypeChanged sets the private IndexPinType
         * - the field the compile-time GetOptionPins() dispatch reads. Pre-
         * typing the pin would suppress that callback and leave IndexPinType
         * wildcard, so the enum branch would match none of the enum-named
         * option pins ("No option pin in Select"). */
        SelectNode->SetEnum(SelectEnum, true);
        SelectNode->AllocateDefaultPins();
    }
    else
    {
        SelectNode->AllocateDefaultPins();
    }

    /* Wire ORDER matters (K2Node_Select.cpp semantics):
     * - Connecting a CASE term while option pins are still wildcards makes the
     *   node's NotifyPinConnectionListChanged retype Return + every Option to
     *   the case net's type (byte + Enum_Race here); the Index pin is NOT
     *   touched by that path.
     * - Connecting the INDEX pin while IT is still a wildcard makes the node
     *   adopt the index type (int) into the private IndexPinType and defer a
     *   reconstruct that discards/rebuilds the option pins. Doing this FIRST
     *   used to retype the whole select to int and break every enum case
     *   wire - so cases always resolve and wire before the index connects. */
    if (!SelectEnum)
    {
        TArray<UEdGraphPin*> OptionPins;
        SelectNode->GetOptionPins(OptionPins);
        while (OptionPins.Num() < Cases->Num())
        {
            SelectNode->AddInputPin();
            SelectNode->GetOptionPins(OptionPins);
        }
        while (OptionPins.Num() > Cases->Num())
        {
            SelectNode->RemoveOptionPinToNode();
            SelectNode->GetOptionPins(OptionPins);
        }
    }

    /* Pass A: resolve every case term up front so the select's value type is
     * derived from the CASE data, never from the index. */
    TArray<FPinValue> CaseValues;
    CaseValues.Init(FPinValue(), Cases->Num());
    FEdGraphPinType ResolvedType;
    bool bHaveResolvedType = false;
    for (int32 CaseIdx = 0; CaseIdx < Cases->Num(); ++CaseIdx)
    {
        const TSharedPtr<FJsonObject> CaseObj = (*Cases)[CaseIdx]->AsObject();
        const TSharedPtr<FJsonObject>* TermPtr = nullptr;
        if (!CaseObj.IsValid()
            || !CaseObj->TryGetObjectField(TEXT("CaseTerm"), TermPtr) || !TermPtr || !TermPtr->IsValid())
        {
            continue;
        }
        CaseValues[CaseIdx] = ResolveExpression(Builder, *TermPtr);
        if (!bHaveResolvedType && CaseValues[CaseIdx].Pin
            && CaseValues[CaseIdx].Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
        {
            ResolvedType = CaseValues[CaseIdx].Pin->PinType;
            bHaveResolvedType = true;
        }
        else if (!bHaveResolvedType && !CaseValues[CaseIdx].Pin)
        {
            /* Literal-only term (TempConstants): harvest its DECLARED type
             * from JSON so literal selects don't inherit the index type
             * (plan 010 item 5a - e.g. bool case temps on an int-indexed
             * select must produce a BOOL-valued node). */
            const TSharedPtr<FJsonObject>* DeclProp = nullptr;
            const TSharedPtr<FJsonObject>* TermVar = nullptr;
            if ((*TermPtr)->TryGetObjectField(TEXT("Variable"), TermVar) && TermVar)
            {
                (*TermVar)->TryGetObjectField(TEXT("Property"), DeclProp);
            }
            if (!DeclProp)
            {
                (*TermPtr)->TryGetObjectField(TEXT("Property"), DeclProp); // EX_StructMemberContext form
            }
            if (DeclProp && *DeclProp)
            {
                FEdGraphPinType DeclType = PinTypeFromJson(*DeclProp);
                if (DeclType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
                {
                    ResolvedType = DeclType;
                    bHaveResolvedType = true;
                    UE_LOG(LogBlueprintBytecodeImporter, Log,
                        TEXT("ResolveSwitchValue: literal case %d declares type %s/%s"),
                        CaseIdx, *DeclType.PinCategory.ToString(), *DeclType.PinSubCategory.ToString());
                }
            }
        }
    }

    /* Secondary source: DefaultTerm's declared property carries the true value
     * type even when every case is a constant (e.g. K2Node_Select_Default_3). */
    if (!bHaveResolvedType)
    {
        const TSharedPtr<FJsonObject>* DefaultPtr = nullptr;
        if (ExprJson->TryGetObjectField(TEXT("DefaultTerm"), DefaultPtr) && DefaultPtr)
        {
            const TSharedPtr<FJsonObject>* DVar = nullptr;
            if ((*DefaultPtr)->TryGetObjectField(TEXT("Variable"), DVar) && DVar)
            {
                const TSharedPtr<FJsonObject>* DProp = nullptr;
                if ((*DVar)->TryGetObjectField(TEXT("Property"), DProp) && DProp)
                {
                    FEdGraphPinType DeclType = PinTypeFromJson(*DProp);
                    if (DeclType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
                    {
                        ResolvedType = DeclType;
                        bHaveResolvedType = true;
                    }
                }
            }
        }
    }

    const TSharedPtr<FJsonObject>* IndexTermPtr = nullptr;
    const bool bHaveIndexTerm = ExprJson->TryGetObjectField(TEXT("IndexTerm"), IndexTermPtr) && IndexTermPtr && IndexTermPtr->IsValid();
    FPinValue IndexValue;

    /* Literal-only cases: harvest the index net's type WITHOUT connecting -
     * the index itself is still connected last, below. */
    if (!bHaveResolvedType && bHaveIndexTerm)
    {
        IndexValue = ResolveExpression(Builder, *IndexTermPtr);
        if (IndexValue.Pin && IndexValue.Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
        {
            ResolvedType = IndexValue.Pin->PinType;
            bHaveResolvedType = true;
        }
    }

    /* Type the option slots and return value BEFORE wiring/constants:
     * UEdGraphPin::PinType is public and mutating it mirrors what the editor's
     * own wildcard propagation does internally. Constants then land as typed
     * defaults (enumerator names for byte-enum pins) and case wires connect
     * against matching types. Every input except Index is an option slot -
     * this covers both "Option N" pins and enum-entry-named pins. */
    const UEdGraphPin* IndexPinEarly = SelectNode->GetIndexPin();
    if (bHaveResolvedType)
    {
        for (UEdGraphPin* Pin : SelectNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin == IndexPinEarly) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
            {
                Pin->PinType = ResolvedType;
            }
        }
        UEdGraphPin* RetEarly = FindPin(SelectNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
        if (RetEarly && RetEarly->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
        {
            RetEarly->PinType = ResolvedType;
        }
    }

    /* Pass B: wire the cases or set their typed constants. Enum selects map
     * each case to its option pin through the CaseIndexValueTerm byte literal
     * (the exact KCST_SwitchValue semantics); plain selects map by position. */
    auto FindOptionPinForCase = [this, SelectNode, SelectEnum](int32 CaseIdx, const TSharedPtr<FJsonObject>* CaseObj) -> UEdGraphPin*
    {
        if (SelectEnum)
        {
            if (CaseObj && CaseObj->IsValid())
            {
                const TSharedPtr<FJsonObject> IdxTerm = (*CaseObj)->HasField(TEXT("CaseIndexValueTerm"))
                    ? (*CaseObj)->GetObjectField(TEXT("CaseIndexValueTerm")) : nullptr;
                if (IdxTerm.IsValid())
                {
                    const FString IdxToken = IdxTerm->GetStringField(TEXT("Token"));
                    if (IdxToken == TEXT("EX_ByteConst") || IdxToken == TEXT("EX_IntConst"))
                    {
                        const FString EntryName = SelectEnum->GetNameStringByIndex((int32)IdxTerm->GetNumberField(TEXT("Value")));
                        if (!EntryName.IsEmpty())
                        {
                            if (UEdGraphPin* Named = FindPin(SelectNode, *EntryName, EGPD_Input))
                            {
                                return Named;
                            }
                        }
                    }
                }
            }
            /* No usable literal - fall back to positional enum entry. */
            const FString FallbackName = SelectEnum->GetNameStringByIndex(CaseIdx);
            return FindPin(SelectNode, *FallbackName, EGPD_Input);
        }
        return FindPin(SelectNode, *FString::Printf(TEXT("Option %d"), CaseIdx), EGPD_Input);
    };

    for (int32 CaseIdx = 0; CaseIdx < Cases->Num(); ++CaseIdx)
    {
        const TSharedPtr<FJsonObject> CaseObj = (*Cases)[CaseIdx]->AsObject();
        UEdGraphPin* OptionPin = FindOptionPinForCase(CaseIdx, &CaseObj);
        if (!OptionPin) continue;

        const FPinValue& OptionValue = CaseValues[CaseIdx];
        if (OptionValue.Pin)
        {
            ConnectPins(OptionValue.Pin, OptionPin);
        }
        else if (OptionValue.bConstant)
        {
            FString DefaultValue = OptionValue.ConstString;
            // String pin DefaultValue is stored raw (no surrounding quotes)
            if (OptionPin->PinType.PinCategory == UEdGraphSchema_K2::PC_String
                && DefaultValue.Len() >= 2 && DefaultValue.StartsWith(TEXT("\"")) && DefaultValue.EndsWith(TEXT("\"")))
            {
                DefaultValue = DefaultValue.Mid(1, DefaultValue.Len() - 2);
            }
            SetPinDefaultValueSafe(OptionPin, DefaultValue);
            // Enum (byte) pins must store the enumerator NAME, not the raw index
            if (OptionPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte && OptionPin->PinType.PinSubCategoryObject.IsValid())
            {
                if (UEnum* EnumObj = Cast<UEnum>(OptionPin->PinType.PinSubCategoryObject.Get()))
                {
                    const int32 EnumIndex = FCString::Atoi(*DefaultValue);
                    const FString EnumName = EnumObj->GetNameStringByIndex(EnumIndex);
                    if (!EnumName.IsEmpty())
                    {
                        OptionPin->DefaultValue = EnumName;
                    }
                }
            }
        }
        else
        {
            /* Case term is a temp whose producer is not emitted YET (the case
             * body's call can live at a later statement index). Queue the wire
             * so the post-walk resolver connects it once the producer exists. */
            const TSharedPtr<FJsonObject>* TermPtr = nullptr;
            if (CaseObj.IsValid() && CaseObj->TryGetObjectField(TEXT("CaseTerm"), TermPtr) && TermPtr && (*TermPtr).IsValid()
                && (*TermPtr)->GetStringField(TEXT("Token")) == TEXT("EX_LocalVariable"))
            {
                const TSharedPtr<FJsonObject>& CV = (*TermPtr)->GetObjectField(TEXT("Variable"));
                const TSharedPtr<FJsonObject>& CIV = CV.IsValid() ? (CV->HasField(TEXT("Variable")) ? CV->GetObjectField(TEXT("Variable")) : CV) : CV;
                const TSharedPtr<FJsonObject>& CP = CIV.IsValid() ? CIV->GetObjectField(TEXT("Property")) : CIV;
                if (CP.IsValid())
                {
                    const FString CaseTemp = CP->GetStringField(TEXT("Name"));
                    if (!CaseTemp.IsEmpty())
                    {
                        Builder.PendingDataWires.Add(TPair<UEdGraphPin*, FString>(OptionPin, CaseTemp));
                        UE_LOG(LogBlueprintBytecodeImporter, Log,
                            TEXT("ResolveSwitchValue: case %d temp '%s' queued for pin %s"),
                            CaseIdx, *CaseTemp, *OptionPin->PinName.ToString());
                    }
                }
            }
        }
    }

    /* Index LAST: the still-wildcard Index pin adopts the linked net's type
     * through the node's own notification, which updates the private
     * IndexPinType (so the export serializes "IndexPinType=(int)") without
     * touching the already-typed option/return pins. */
    UEdGraphPin* IndexPin = SelectNode->GetIndexPin();
    if (!IndexPin)
    {
        IndexPin = FindPin(SelectNode, TEXT("Index"), EGPD_Input);
    }
    if (bHaveIndexTerm && IndexPin)
    {
        if (!IndexValue.Pin && bHaveResolvedType)
        {
            // Normal path: pass A only resolved cases; get the index net now
            IndexValue = ResolveExpression(Builder, *IndexTermPtr);
        }
        if (IndexValue.Pin)
        {
            ConnectPins(IndexValue.Pin, IndexPin);
        }
        else if (IndexValue.bConstant)
        {
            SetPinDefaultValueSafe(IndexPin, IndexValue.ConstString);
        }
    }

    UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
    if (!ReturnPin)
    {
        ReturnPin = FindPin(SelectNode, UEdGraphSchema_K2::PN_ReturnValue.ToString(), EGPD_Output);
    }
    if (!ReturnPin)
    {
        return FPinValue();
    }
    return FPinValue{ ReturnPin, false, TEXT(""), nullptr };
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
            else if (TypeName == TEXT("InputKeyDelegateBinding"))
            {
                /* Plain K2Node_InputKey events - same entry schema as the debug
                 * key variant (InputKeyDelegateBindings[] with InputChord +
                 * EInputEvent + FunctionNameToBind). */
                ProcessInputKeyBinding(Resolved, JsonObjects);
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
    int32 OrderIdx = 0;
    for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
    {
        const TSharedPtr<FJsonObject>& Entry = EntryValue->AsObject();
        if (!Entry) continue;

        FInputBindingInfo Info;
        Info.BindingOrder = OrderIdx++;
        Info.NodeType = TEXT("K2Node_InputDebugKeyEvent");
        Info.FunctionName = Entry->GetStringField(TEXT("FunctionNameToBind"));
        Info.InputKeyEvent = Entry->GetStringField(TEXT("InputKeyEvent"));
        Info.bExecuteWhenPaused = Entry->GetBoolField(TEXT("bExecuteWhenPaused"));

        if (Entry->HasField(TEXT("InputChord")))
        {
            const TSharedPtr<FJsonObject>& Chord = Entry->GetObjectField(TEXT("InputChord"));
            if (Chord)
            {
                Info.bShift = Chord->GetBoolField(TEXT("bShift"));
                Info.bCtrl = Chord->GetBoolField(TEXT("bCtrl"));
                Info.bAlt = Chord->GetBoolField(TEXT("bAlt"));
                Info.bCmd = Chord->GetBoolField(TEXT("bCmd"));

                if (Chord->HasField(TEXT("Key")))
                {
                    const TSharedPtr<FJsonObject>& Key = Chord->GetObjectField(TEXT("Key"));
                    if (Key)
                    {
                        Info.KeyName = Key->GetStringField(TEXT("KeyName"));
                    }
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

void FBlueprintBytecodeImporter::ProcessInputKeyBinding(const TSharedPtr<FJsonObject>& BindingJson, const TArray<TSharedPtr<FJsonValue>>& JsonObjects)
{
    if (!BindingJson || !BindingJson->HasField(TEXT("Properties")))
    {
        return;
    }

    const TSharedPtr<FJsonObject>& Properties = BindingJson->GetObjectField(TEXT("Properties"));
    if (!Properties || !Properties->HasField(TEXT("InputKeyDelegateBindings")))
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>& Entries = Properties->GetArrayField(TEXT("InputKeyDelegateBindings"));
    int32 OrderIdx = 0;
    for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
    {
        const TSharedPtr<FJsonObject>& Entry = EntryValue->AsObject();
        if (!Entry) continue;

        FInputBindingInfo Info;
        Info.BindingOrder = OrderIdx++;
        Info.NodeType = TEXT("K2Node_InputKeyEvent");
        Info.FunctionName = Entry->GetStringField(TEXT("FunctionNameToBind"));
        Info.InputKeyEvent = Entry->GetStringField(TEXT("InputKeyEvent"));
        Info.bExecuteWhenPaused = Entry->GetBoolField(TEXT("bExecuteWhenPaused"));
        Info.bConsumeInput = Entry->GetBoolField(TEXT("bConsumeInput"));
        Info.bOverrideParentBinding = Entry->GetBoolField(TEXT("bOverrideParentBinding"));

        if (Entry->HasField(TEXT("InputChord")))
        {
            const TSharedPtr<FJsonObject>& Chord = Entry->GetObjectField(TEXT("InputChord"));
            if (Chord)
            {
                Info.bShift = Chord->GetBoolField(TEXT("bShift"));
                Info.bCtrl = Chord->GetBoolField(TEXT("bCtrl"));
                Info.bAlt = Chord->GetBoolField(TEXT("bAlt"));
                Info.bCmd = Chord->GetBoolField(TEXT("bCmd"));

                if (Chord->HasField(TEXT("Key")))
                {
                    const TSharedPtr<FJsonObject>& Key = Chord->GetObjectField(TEXT("Key"));
                    if (Key)
                    {
                        Info.KeyName = Key->GetStringField(TEXT("KeyName"));
                    }
                }
            }
        }

        if (!Info.FunctionName.IsEmpty())
        {
            InputBindings.Add(Info.FunctionName, Info);
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("InputKey binding: %s -> Key=%s Event=%s"), *Info.FunctionName, *Info.KeyName, *Info.InputKeyEvent);
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
    int32 OrderIdx = 0;
    for (const TSharedPtr<FJsonValue>& EntryValue : Entries)
    {
        const TSharedPtr<FJsonObject>& Entry = EntryValue->AsObject();
        if (!Entry) continue;

        FInputBindingInfo Info;
        Info.BindingOrder = OrderIdx++;
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

/* ============================================================================
 * StandardMacro loop reconstruction (plan 006)
 * ============================================================================ */

/* Display name for a detected loop family (plain enum - no reflection) */
static const TCHAR* MacroLoopTypeName(EMacroLoopType Type)
{
    switch (Type)
    {
        case EMacroLoopType::ForEachLoopWithBreak: return TEXT("ForEachLoopWithBreak");
        case EMacroLoopType::ForEachLoop:          return TEXT("ForEachLoop");
        case EMacroLoopType::ForLoopWithBreak:     return TEXT("ForLoopWithBreak");
        case EMacroLoopType::ForLoop:              return TEXT("ForLoop");
        case EMacroLoopType::WhileLoop:            return TEXT("WhileLoop");
        case EMacroLoopType::ReverseForEachLoop:   return TEXT("ReverseForEachLoop");
        default:                                   return TEXT("None");
    }
}

FString FBlueprintBytecodeImporter::CallMathFunctionName(const FBytecodeToken& Tok)
{
    if (Tok.Token != TEXT("EX_CallMath") || !Tok.JsonData.IsValid())
    {
        return FString();
    }
    const TSharedPtr<FJsonObject>* Fn = nullptr;
    if (!Tok.JsonData->TryGetObjectField(TEXT("Function"), Fn) || !Fn->IsValid())
    {
        return FString();
    }
    const FString ObjName = (*Fn)->GetStringField(TEXT("ObjectName"));
    /* ObjectName looks like "Class'KismetMathLibrary:Add_IntInt'" */
    const int32 Colon = ObjName.Find(TEXT(":"));
    if (Colon == INDEX_NONE)
    {
        return FString();
    }
    FString Out = ObjName.Mid(Colon + 1);
    Out.RemoveFromEnd(TEXT("'"));
    return Out;
}

/* Recursively collect EX_CallMath short names anywhere inside a statement's
 * JSON payload (plan 006 R1/F1): loop math is nested inside EX_Let/LetBool
 * ".Expression" and EX_Context chains, invisible to top-level scans. When
 * OutCalls is provided it receives (ShortName, call object) pairs. */
static void CollectMathCalls(const TSharedPtr<FJsonObject>& Obj, TArray<FString>& OutNames,
    TArray<TPair<FString, TSharedPtr<FJsonObject>>>* OutCalls = nullptr)
{
    if (!Obj.IsValid())
    {
        return;
    }
    FString Token;
    /* Pure math arrives as EX_CallMath, but context-dispatched calls (the
     * corpus norm: EX_Let > EX_Context > EX_FinalFunction wrapping
     * KismetArrayLibrary:Array_Length) arrive as EX_FinalFunction - accept
     * both plus virtual interface calls (plan 006 R1, diag_v2_trace). */
    if (Obj->TryGetStringField(TEXT("Token"), Token) &&
        (Token == TEXT("EX_CallMath") || Token == TEXT("EX_FinalFunction") || Token == TEXT("EX_VirtualFunction")))
    {
        const TSharedPtr<FJsonObject>* Fn = nullptr;
        if (Obj->TryGetObjectField(TEXT("Function"), Fn) && Fn->IsValid())
        {
            const FString ObjName = (*Fn)->GetStringField(TEXT("ObjectName"));
            const int32 Colon = ObjName.Find(TEXT(":"));
            if (Colon != INDEX_NONE)
            {
                FString Short = ObjName.Mid(Colon + 1);
                Short.RemoveFromEnd(TEXT("'"));
                OutNames.AddUnique(Short);
                if (OutCalls)
                {
                    OutCalls->Add({ Short, Obj });
                }
            }
        }
    }
    for (const auto& KV : Obj->Values)
    {
        if (KV.Value->Type == EJson::Object)
        {
            CollectMathCalls(KV.Value->AsObject(), OutNames, OutCalls);
        }
        else if (KV.Value->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Elem : KV.Value->AsArray())
            {
                if (Elem.IsValid() && Elem->Type == EJson::Object)
                {
                    CollectMathCalls(Elem->AsObject(), OutNames, OutCalls);
                }
            }
        }
    }
}

/* Does an EX_Let/EX_LetBool write the literal TRUE? Break sites and exhaust
 * paths store the break flag as EX_True; other writes are ordinary scaffolds. */
static bool LetWritesLiteralTrue(const FBytecodeToken& Tok)
{
    if (!Tok.JsonData.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Expr = nullptr;
    if (!Tok.JsonData->TryGetObjectField(TEXT("Expression"), Expr))
    {
        return true; /* constant folded - treated as unconditional */
    }
    FString ExprToken;
    if ((*Expr)->TryGetStringField(TEXT("Token"), ExprToken))
    {
        return ExprToken == TEXT("EX_True");
    }
    return false;
}

/* Target local name of an EX_Let / EX_LetBool / EX_LetObj statement */
static bool GetLetTargetLocal(const FBytecodeToken& Tok, FString& OutName, FString& OutType)
{
    if (Tok.Token != TEXT("EX_Let") && Tok.Token != TEXT("EX_LetBool") && Tok.Token != TEXT("EX_LetObj"))
    {
        return false;
    }
    if (!Tok.JsonData.IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* VarWrapper = nullptr;
    if (!Tok.JsonData->TryGetObjectField(TEXT("Variable"), VarWrapper) || !VarWrapper->IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* InnerVar = nullptr;
    if (!(*VarWrapper)->TryGetObjectField(TEXT("Variable"), InnerVar) || !InnerVar->IsValid())
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* Prop = nullptr;
    if (!(*InnerVar)->TryGetObjectField(TEXT("Property"), Prop) || !Prop->IsValid())
    {
        return false;
    }
    OutName = (*Prop)->GetStringField(TEXT("Name"));
    OutType = (*Prop)->GetStringField(TEXT("Type"));
    return !OutName.IsEmpty();
}

/* Property name carried by an EX_LocalVariable expression, else empty */
static FString GetLocalVarNameFromExpr(const TSharedPtr<FJsonObject>& Expr)
{
    if (!Expr.IsValid() || Expr->GetStringField(TEXT("Token")) != TEXT("EX_LocalVariable"))
    {
        return FString();
    }
    const TSharedPtr<FJsonObject>* Var = nullptr;
    if (!Expr->TryGetObjectField(TEXT("Variable"), Var) || !Var->IsValid())
    {
        return FString();
    }
    const TSharedPtr<FJsonObject>* Prop = nullptr;
    if (!(*Var)->TryGetObjectField(TEXT("Property"), Prop) || !Prop->IsValid())
    {
        return FString();
    }
    return (*Prop)->GetStringField(TEXT("Name"));
}

void FBlueprintBytecodeImporter::DetectMacroLoops(FFunctionBuilder& Builder)
{
    Builder.DetectedLoops.Reset();
    Builder.LoopSuppressedSis.Reset();
    Builder.LoopChainBridgeSis.Reset();

    /* Scan the function that OWNS the statements being walked: event thunks
     * and trampolined user functions carry 3-token stubs while their bodies
     * live in the ubergraph (Builder.TokenSource). Without this the token
     * guard below silently disabled loop detection corpus-wide. */
    const ParsedFunction* Func = Builder.TokenSource ? Builder.TokenSource : Builder.Func;
    if (!Func || Func->BytecodeTokens.Num() < 8)
    {
        return;
    }

    /* Linear si-ordered view + quick lookup */
    TArray<const FBytecodeToken*> Ordered;
    Ordered.Reserve(Func->BytecodeTokens.Num());
    for (const FBytecodeToken& T : Func->BytecodeTokens)
    {
        Ordered.Add(&T);
    }
    Ordered.Sort([](const FBytecodeToken& A, const FBytecodeToken& B) { return A.StatementIndex < B.StatementIndex; });
    TMap<int32, int32> PosOfSi;
    for (int32 i = 0; i < Ordered.Num(); ++i)
    {
        PosOfSi.Add(Ordered[i]->StatementIndex, i);
    }
    auto NextSequential = [&Ordered, &PosOfSi](int32 Si) -> const FBytecodeToken*
    {
        const int32* P = PosOfSi.Find(Si);
        if (!P || (*P + 1) >= Ordered.Num())
        {
            return nullptr;
        }
        return Ordered[*P + 1];
    };

    /* Flow events */
    struct FEv { int32 Si; int32 Addr; };
    TArray<FEv> Pushes;
    TArray<int32> PlainPops;
    TArray<int32> PopIfNots;
    TArray<int32> JumpIfNots;
    TArray<FEv> BackJumps;

    for (const FBytecodeToken* T : Ordered)
    {
        const FBytecodeToken& Tok = *T;
        const TSharedPtr<FJsonObject>& J = Tok.JsonData;
        if (Tok.Token == TEXT("EX_PushExecutionFlow") && J.IsValid() && J->HasField(TEXT("PushingAddress")))
        {
            Pushes.Add({ Tok.StatementIndex, J->GetIntegerField(TEXT("PushingAddress")) });
        }
        else if (Tok.Token == TEXT("EX_PopExecutionFlow"))
        {
            PlainPops.Add(Tok.StatementIndex);
        }
        else if (Tok.Token == TEXT("EX_PopExecutionFlowIfNot"))
        {
            PopIfNots.Add(Tok.StatementIndex);
        }
        else if ((Tok.Token == TEXT("EX_Jump") || Tok.Token == TEXT("EX_JumpIfNot")) && J.IsValid() && J->HasField(TEXT("CodeOffset")))
        {
            /* Both conditional-exit spellings gate loops: the StandardMacro
             * families compile their gate as EX_PopExecutionFlowIfNot, but a
             * ForEachLoop over a pure expression chain (08.25: Call for NPC
             * interaction, MakeArray->Length->Less) emits a bare EX_JumpIfNot.
             * Without collecting it the gate scan reported "no conditional
             * exit inside window" and the loop stayed flat. */
            if (Tok.Token == TEXT("EX_JumpIfNot"))
            {
                JumpIfNots.Add(Tok.StatementIndex);
            }
            const int32 Target = J->GetIntegerField(TEXT("CodeOffset"));
            if (Target < Tok.StatementIndex)
            {
                BackJumps.Add({ Tok.StatementIndex, Target });
            }
        }
    }
    if (BackJumps.Num() == 0 || (PopIfNots.Num() == 0 && JumpIfNots.Num() == 0))
    {
        return;
    }

    /* Math-op signal counter over a si range - recursive (plan 006 R1) */
    auto CountSignals = [&](int32 From, int32 To, int32& GetArr, int32& ArrLen, int32& LessI, int32& GreaterI,
                            int32& AndB, int32& NotPreB, int32& AddI, int32& SubI)
    {
        GetArr = ArrLen = LessI = GreaterI = AndB = NotPreB = AddI = SubI = 0;
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < From || Si > To) continue;
            TArray<FString> Names;
            CollectMathCalls(T->JsonData, Names);
            for (const FString& M : Names)
            {
                if (M.StartsWith(TEXT("Array_Get")) || M.Contains(TEXT("GetArrayItem"))) ++GetArr;
                else if (M == TEXT("Array_Length")) ++ArrLen;
                else if (M.StartsWith(TEXT("Less_IntInt"))) ++LessI;
                else if (M.StartsWith(TEXT("Greater_IntInt")) || M.StartsWith(TEXT("GreaterEqual_IntInt"))) ++GreaterI;
                else if (M.StartsWith(TEXT("BooleanAND"))) ++AndB;
                else if (M.StartsWith(TEXT("Not_PreBool"))) ++NotPreB;
                else if (M.StartsWith(TEXT("Add_IntInt"))) ++AddI;
                else if (M.StartsWith(TEXT("Subtract_IntInt"))) ++SubI;
            }
        }
    };

    /* Match clusters off each backward jump (latch). Skeleton anchors per
     * plan 006 R2: window = [outer-Push .. latch]; CondLast = PopIfNot inside
     * the window; InnerPush = a push whose resume address lies INSIDE the
     * window (the increment/recheck). Depending on bytecode layout it sits
     * right before the body (pre-body gate: GM_Touchy push@1464 -> 2349) or in
     * the emitted prologue (post-body gate: BeginPlay push@10 -> 1730); both
     * shapes satisfy "Addr in (HeadSi..LatchSi]". */
    TArray<FDetectedLoop> Candidates;
    for (const FEv& L : BackJumps)
    {
        const int32 HeadSi = L.Addr;

        /* Outer push: earliest push before the head whose resume address lies
         * beyond the latch (the Completed chain). */
        const FEv* Outer = nullptr;
        for (const FEv& P : Pushes)
        {
            if (P.Si < HeadSi && P.Addr > L.Si && (!Outer || P.Si < Outer->Si))
            {
                Outer = &P;
            }
        }
        if (!Outer)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Loop cluster at si=%d: no outer push past latch, keeping flat"), HeadSi);
            continue;
        }

        /* Conditional exit between head and latch - take the LAST conditional:
         * nested user branches inside the body can carry their own earlier
         * gate (BeginPlay @177 vs the real gate @2031, diag_v2_trace). Both
         * gate spellings count - see the collector comment above. */
        int32 CondLast = INDEX_NONE;
        for (int32 C : PopIfNots)
        {
            if (C >= HeadSi && C <= L.Si && (CondLast == INDEX_NONE || C > CondLast))
            {
                CondLast = C;
            }
        }
        for (int32 C : JumpIfNots)
        {
            if (C >= HeadSi && C <= L.Si && (CondLast == INDEX_NONE || C > CondLast))
            {
                CondLast = C;
            }
        }
        if (CondLast == INDEX_NONE)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Loop cluster at si=%d: no conditional exit inside window (PopExecutionFlowIfNot/JumpIfNot), keeping flat"), HeadSi);
            continue;
        }

        /* Inner push resuming at the increment - required by every emittable
         * family. Its ABSENCE is the hand-rolled/gate-hybrid shape (plan 006
         * R6: the Initialize 7210 DoOnce guard pushes an exit-side resume). */
        const FEv* Inner = nullptr;
        for (const FEv& P : Pushes)
        {
            if (&P == Outer || P.Si < Outer->Si || P.Si > L.Si) continue;
            if (P.Addr > HeadSi && P.Addr <= L.Si)
            {
                Inner = &P;
                break;
            }
        }
        if (!Inner)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Loop cluster at si=%d: no increment-resume push inside window, keeping flat"), HeadSi);
            continue;
        }

        /* The body ends with plain pops mapping back into the increment (both
         * normal completion and break-site exits); remember the last one. */
        int32 BodyLast = INDEX_NONE;
        for (int32 E : PlainPops)
        {
            if (E > Inner->Si && E < L.Si && (BodyLast == INDEX_NONE || E > BodyLast))
            {
                BodyLast = E;
            }
        }
        if (BodyLast == INDEX_NONE)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Loop cluster at si=%d: no body pop before latch, keeping flat"), HeadSi);
            continue;
        }

        FDetectedLoop Loop;
        Loop.PushSi = Outer->Si;
        Loop.ExitAddr = Outer->Addr;
        Loop.InnerPushSi = Inner->Si;
        Loop.IncrementAddr = Inner->Addr;
        Loop.BodyFirst = HeadSi;
        Loop.BodyLast = BodyLast;
        Loop.CondLast = CondLast;
        Loop.LatchSi = L.Si;

        /* A bare EX_JumpIfNot gate exits to ITS OWN CodeOffset, which is the
         * loop's Completed continuation - NOT the outer push resume. The two
         * only coincide for EX_PopExecutionFlowIfNot gates (the op pops to the
         * outer resume as it jumps). (08.25: Call for NPC interaction's
         * ForEachLoop chained Completed to the shared Return at 26406 and left
         * its real continuation - the AssigneOrFindFocusIndexToNPC call at
         * 20506 - unwired.) */
        if (CondLast != INDEX_NONE)
        {
            for (const FBytecodeToken* T : Ordered)
            {
                if (T->StatementIndex == CondLast && T->Token == TEXT("EX_JumpIfNot")
                    && T->JsonData.IsValid() && T->JsonData->HasField(TEXT("CodeOffset")))
                {
                    Loop.ExitAddr = T->JsonData->GetIntegerField(TEXT("CodeOffset"));
                    break;
                }
            }
        }

        /* Canonical temp-pair discovery INSIDE the cluster window: the
         * increment lets carry both canonical names in every corpus layout
         * (even where init Lets sit outside - GM counter@2660 vs latch@2418),
         * and window scoping keeps sibling instances' suffixed temps from
         * cross-wiring in multi-loop functions (MainGame_UI ~30 loops). */
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < Outer->Si || Si > L.Si) continue;
            FString Name, TypeStr;
            if (!GetLetTargetLocal(*T, Name, TypeStr)) continue;
            if (Loop.CounterLocal.IsEmpty() && Name.Contains(TEXT("Loop_Counter")))
            {
                Loop.CounterLocal = Name;
            }
            if (Loop.IndexLocal.IsEmpty() && Name.Contains(TEXT("Array_Index")))
            {
                Loop.IndexLocal = Name;
            }
            if (Loop.BreakFlagLocal.IsEmpty() && Name.Contains(TEXT("break_was_hit"), ESearchCase::IgnoreCase))
            {
                Loop.BreakFlagLocal = Name;
            }
        }

        /* Break sites: literal-true writes of the break flag INSIDE the cluster
         * window (user breaks and in-window exhaust paths alike). */
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < Outer->Si || Si > L.Si) continue;
            FString Name, TypeStr;
            if (!GetLetTargetLocal(*T, Name, TypeStr)) continue;
            if (!Loop.BreakFlagLocal.IsEmpty() && Name == Loop.BreakFlagLocal &&
                (TypeStr == TEXT("BoolProperty") || TypeStr.IsEmpty()) && LetWritesLiteralTrue(*T))
            {
                Loop.BreakSites.AddUnique(Si);
            }
        }

        /* Direction signals come from the two scaffold-only windows (plan 006
         * R3): gate [Head..CondLast] carries Length+compare, increment
         * [Increment..Latch] carries the +/- step. User code inside the body
         * cannot pollute either window. */
        int32 GetArr, ArrLen, LessI, GreaterI, AndB, NotPreB, AddI, SubI;
        CountSignals(HeadSi, CondLast, GetArr, ArrLen, LessI, GreaterI, AndB, NotPreB, AddI, SubI);
        const bool bGateLess = LessI > 0;
        const bool bGateGreater = GreaterI > 0;
        const bool bGateLen = ArrLen > 0;
        CountSignals(Loop.IncrementAddr, L.Si, GetArr, ArrLen, LessI, GreaterI, AndB, NotPreB, AddI, SubI);
        const bool bIncrAdd = AddI > 0;
        const bool bIncrSub = SubI > 0;

        /* Classification: the canonical temp PAIR is mandatory for the ForEach
         * family (strongest corpus discriminator); direction picks the subtype.
         * WhileLoop is NEVER emitted (R6 - zero verified instances, fingerprint
         * indistinguishable from hand-rolled control flow). ForLoop family is
         * kept but requires the canonical counter temp (zero corpus instances).
         * No Reverse-with-break variant ships in StandardMacros. */
        const bool bPair = !Loop.CounterLocal.IsEmpty() && !Loop.IndexLocal.IsEmpty();
        const bool bHasBreak = !Loop.BreakFlagLocal.IsEmpty();
        EMacroLoopType Type = EMacroLoopType::None;
        if (bPair && bGateLen && bGateLess && bIncrAdd)
        {
            Type = bHasBreak ? EMacroLoopType::ForEachLoopWithBreak : EMacroLoopType::ForEachLoop;
        }
        else if (bPair && bGateGreater && bIncrSub && !bHasBreak)
        {
            Type = EMacroLoopType::ReverseForEachLoop;
        }
        else if (!Loop.CounterLocal.IsEmpty() && Loop.IndexLocal.IsEmpty() &&
                 (bGateLess || bGateGreater) && (bIncrAdd || bIncrSub))
        {
            Type = bHasBreak ? EMacroLoopType::ForLoopWithBreak : EMacroLoopType::ForLoop;
        }

        if (Type == EMacroLoopType::None)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log,
                TEXT("Loop cluster at si=%d: skeleton/temp-pair mismatch (pair=%d len=%d less=%d greater=%d add=%d sub=%d brk=%d) - keeping flat"),
                HeadSi, bPair ? 1 : 0, bGateLen ? 1 : 0, bGateLess ? 1 : 0, bGateGreater ? 1 : 0,
                bIncrAdd ? 1 : 0, bIncrSub ? 1 : 0, bHasBreak ? 1 : 0);
            continue;
        }

        /* Per-event decompilation only walks a subset of the ubergraph - skip
         * clusters whose body head is not on THIS path (their si values can
         * never match anchors here). */
        if (!Builder.StmtIndexToArrayPos.Contains(HeadSi))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Verbose, TEXT("Loop cluster body=%d not on this event's path - belongs to another case"), HeadSi);
            continue;
        }

        /* Iterated array expression: Parameters[0] of the Array_Length call,
         * found RECURSIVELY anywhere inside the gate statements (plan 006 F5:
         * GM nests it under Let.Expression -> EX_Context). */
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < HeadSi || Si > CondLast || Loop.ArrayExpr.IsValid()) continue;
            TArray<FString> Names;
            TArray<TPair<FString, TSharedPtr<FJsonObject>>> Calls;
            CollectMathCalls(T->JsonData, Names, &Calls);
            for (const TPair<FString, TSharedPtr<FJsonObject>>& Call : Calls)
            {
                if (Call.Key != TEXT("Array_Length")) continue;
                const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
                if (Call.Value->TryGetArrayField(TEXT("Parameters"), Params) && Params && Params->Num() > 0)
                {
                    Loop.ArrayExpr = (*Params)[0]->AsObject();
                }
                break;
            }
        }

        Loop.Type = Type;
        Candidates.Add(Loop);
    }

    /* Non-overlap resolution (plan 006 Step 2): outer pseudo-latches swallow
     * real loops (GM 2683->1239 contains [1262..2418]; RemovePatreon 6541->624
     * contains all three of its loops; diag_v2_trace). Tightest window wins;
     * anything intersecting an accepted cluster is a decoy and stays flat. */
    Candidates.Sort([](const FDetectedLoop& A, const FDetectedLoop& B)
    {
        return (A.LatchSi - A.BodyFirst) < (B.LatchSi - B.BodyFirst);
    });
    for (const FDetectedLoop& Cand : Candidates)
    {
        bool bOverlaps = false;
        for (const FDetectedLoop& Acc : Builder.DetectedLoops)
        {
            if (Cand.BodyFirst <= Acc.LatchSi && Acc.BodyFirst <= Cand.LatchSi)
            {
                bOverlaps = true;
                break;
            }
        }
        if (bOverlaps)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Log,
                TEXT("Loop candidate body=%d..%d overlaps an accepted cluster - keeping flat"),
                Cand.BodyFirst, Cand.LatchSi);
            continue;
        }
        Builder.DetectedLoops.Add(Cand);
        UE_LOG(LogBlueprintBytecodeImporter, Log,
            TEXT("Detected %s loop: body=%d..%d cond-exit=%d inner-push=%d incr=%d latch=%d->%d exit=%d counter='%s' index='%s' breakflag='%s' breaks=%d array=%d"),
            MacroLoopTypeName(Cand.Type), Cand.BodyFirst, Cand.BodyLast, Cand.CondLast, Cand.InnerPushSi,
            Cand.IncrementAddr, Cand.LatchSi, Cand.BodyFirst, Cand.ExitAddr,
            *Cand.CounterLocal, *Cand.IndexLocal, *Cand.BreakFlagLocal, Cand.BreakSites.Num(),
            Cand.ArrayExpr.IsValid() ? 1 : 0);
    }

    /* Suppression set: scaffold statements the walk must skip */
    for (const FDetectedLoop& Loop : Builder.DetectedLoops)
    {
        Builder.LoopSuppressedSis.Add(Loop.PushSi);
        Builder.LoopSuppressedSis.Add(Loop.InnerPushSi);
        Builder.LoopSuppressedSis.Add(Loop.CondLast);
        Builder.LoopSuppressedSis.Add(Loop.LatchSi);
        /* EVERY plain pop mapping into the increment is scaffold - the normal
         * body-end pop AND break-site exit pops (plan 006 R2). */
        for (int32 E : PlainPops)
        {
            if (E > Loop.InnerPushSi && E < Loop.LatchSi)
            {
                Builder.LoopSuppressedSis.Add(E);
            }
        }
        /* Entire increment zone (BodyLast+1..LatchSi-1) is macro scaffolding:
         * counter/index Add_IntInt calls, temp stores, gate jump-back. None of
         * this is user code — the macro instance exposes Array Index output. */
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si > Loop.BodyLast && Si < Loop.LatchSi)
            {
                Builder.LoopSuppressedSis.Add(Si);
            }
        }

        /* Macro-internal temp writes anywhere in the function (counter/index/
         * break-flag juggling lives before, inside and after the cluster -
         * e.g. GM counter init@2660 sits beyond its latch@2418), but NOT the
         * break-site lets themselves - the emitter rewires those exec inputs
         * onto the instance Break pin and removes the nodes. */
        for (const FBytecodeToken& TokRef : Func->BytecodeTokens)
        {
            const FBytecodeToken* T = &TokRef;
            FString Name, TypeStr;
            if (!GetLetTargetLocal(*T, Name, TypeStr)) continue;
            const bool bCounterWrite = !Loop.CounterLocal.IsEmpty() && Name == Loop.CounterLocal;
            const bool bIndexWrite = !Loop.IndexLocal.IsEmpty() && Name == Loop.IndexLocal;
            const bool bFlagWrite = !Loop.BreakFlagLocal.IsEmpty() && Name == Loop.BreakFlagLocal;
            if (!bCounterWrite && !bIndexWrite && !bFlagWrite) continue;
            bool bIsBreakSite = Loop.BreakSites.Contains(T->StatementIndex);
            if (bIsBreakSite) continue;
            Builder.LoopSuppressedSis.Add(T->StatementIndex);

            /* A break/exhaust write chains into a bare pop - suppress it too */
            if (bFlagWrite)
            {
                if (const FBytecodeToken* Nx = NextSequential(T->StatementIndex))
                {
                    if (Nx->Token == TEXT("EX_PopExecutionFlow"))
                    {
                        Builder.LoopSuppressedSis.Add(Nx->StatementIndex);
                    }
                }
            }

            /* Init/exhaust-region counter/index Lets chain into thunk gotos
             * straight into the loop head (plan 006 R4) - suppress that goto
             * too so the walk bridges the gap instead of emitting a stale
             * jump node whose wire would be spliced onto Macro Exec. */
            if ((bCounterWrite || bIndexWrite) &&
                (T->StatementIndex < Loop.BodyFirst || T->StatementIndex > Loop.LatchSi))
            {
                if (const FBytecodeToken* Nx = NextSequential(T->StatementIndex))
                {
                    if (Nx->Token == TEXT("EX_Jump") && Nx->JsonData.IsValid() &&
                        Nx->JsonData->HasField(TEXT("CodeOffset")) &&
                        Nx->JsonData->GetIntegerField(TEXT("CodeOffset")) == Loop.BodyFirst)
                    {
                        Builder.LoopSuppressedSis.Add(Nx->StatementIndex);
                    }
                }
            }
        }

        /* Gate compiler temps (len-Let, cmp-LetBool, AND/Not results) exist
         * solely for the flattened gate - suppress across the whole cluster
         * EXCEPT the body span (InnerPush..BodyLast], which is user code.
         * Covers both layouts: pre-body gate (GM len/cmp at 1308/1389 between
         * head 1262 and inner-push 1464) and post-body gate (BeginPlay cond
         * eval 1799..2030 after BodyLast 1729). */
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < Loop.BodyFirst || Si > Loop.CondLast) continue;
            if (Si > Loop.InnerPushSi && Si <= Loop.BodyLast) continue;
            FString Name, TypeStr;
            if (!GetLetTargetLocal(*T, Name, TypeStr)) continue;
            if (Name.StartsWith(TEXT("Temp_")) || Name.StartsWith(TEXT("CallFunc_")))
            {
                Builder.LoopSuppressedSis.Add(Si);
            }
        }
    }

    /* Chain bridges: first non-suppressed si of each body. The walk keeps the
     * incoming exec chain flowing across the suppressed init-run into these,
     * so the emitter's splice can move that wire onto Macro Exec. Search the
     * WHOLE cluster window: in the pre-body-gate layout (GM) every statement
     * up to CondLast is scaffold and the first live one sits behind the
     * inner push. */
    for (const FDetectedLoop& Loop : Builder.DetectedLoops)
    {
        for (const FBytecodeToken* T : Ordered)
        {
            const int32 Si = T->StatementIndex;
            if (Si < Loop.BodyFirst || Si > Loop.LatchSi) continue;
            if (!Builder.LoopSuppressedSis.Contains(Si))
            {
                Builder.LoopChainBridgeSis.Add(Si);
                break;
            }
        }
    }

    if (Builder.LoopSuppressedSis.Num() > 0)
    {
        UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("Loop reconstruction: suppressing %d scaffold statements"), Builder.LoopSuppressedSis.Num());
    }
}

void FBlueprintBytecodeImporter::EmitMacroLoopNodes(FFunctionBuilder& Builder)
{
    if (Builder.DetectedLoops.Num() == 0)
    {
        return;
    }

    /* Engine content layout changed across versions - /Engine/StandardMacros
     * was the pre-4.23 location; 5.x keeps the library under
     * EditorBlueprintResources. Macro graphs themselves moved: modern UBlueprint
     * carries them in MacroGraphs ("Set of macros implemented for this
     * class"), while older libraries used FunctionGraphs - check both. */
    static const TCHAR* MacroAssetPaths[] = {
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
        TEXT("/Engine/EditorKismetResources/StandardMacros.StandardMacros"),
    };
    auto NormalizeMacroName = [](const FString& In)
    {
        FString Out;
        for (TCHAR C : In)
        {
            if (FChar::IsAlpha(C) || FChar::IsDigit(C))
            {
                Out.AppendChar(FChar::ToLower(C));
            }
        }
        return Out;
    };
    auto FindMacroGraphIn = [NormalizeMacroName](UBlueprint* BP, const FString& Wanted, FString& AvailableOut) -> UEdGraph*
    {
        const FString NormWanted = NormalizeMacroName(Wanted);
        TArray<UEdGraph*> AllGraphs = BP->MacroGraphs;
        for (UEdGraph* G : BP->FunctionGraphs)
        {
            AllGraphs.AddUnique(G);
        }
        for (UEdGraph* G : AllGraphs)
        {
            if (!G) continue;
            AvailableOut += (AvailableOut.IsEmpty() ? TEXT("") : TEXT(", ")) + G->GetName();
            if (G->GetName() == Wanted || NormalizeMacroName(G->GetName()) == NormWanted)
            {
                return G;
            }
        }
        return nullptr;
    };
    auto ResolveMacroGraph = [&](const FString& Wanted, UEdGraph*& OutGraph, FString& LoadedNames) -> bool
    {
        for (const TCHAR* Path : MacroAssetPaths)
        {
            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, Path);
            if (!BP)
            {
                continue;
            }
            FString Available;
            if (UEdGraph* G = FindMacroGraphIn(BP, Wanted, Available))
            {
                OutGraph = G;
                return true;
            }
            if (LoadedNames.IsEmpty())
            {
                LoadedNames = FString::Printf(TEXT("%s (graphs: %s)"), Path, *Available);
            }
        }
        return false;
    };

    for (const FDetectedLoop& Loop : Builder.DetectedLoops)
    {
        const TCHAR* MacroNameT = MacroLoopTypeName(Loop.Type);
        if (FCString::Strcmp(MacroNameT, TEXT("None")) == 0)
        {
            continue;
        }
        const FString MacroName = MacroNameT;

        UEdGraph* MacroGraphPtr = nullptr;
        FString LoadedNames;
        if (!ResolveMacroGraph(MacroName, MacroGraphPtr, LoadedNames))
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning,
                TEXT("EmitMacroLoopNodes: macro graph '%s' found in no StandardMacros asset - loops stay flat. Loadable assets checked: %s"),
                *MacroName, LoadedNames.IsEmpty() ? TEXT("NONE LOADED") : *LoadedNames);
            continue;
        }

        UEdGraphNode** BodyHeadPtr = Builder.StatementAnchors.Find(Loop.BodyFirst);
        UEdGraphNode* BodyHead = (BodyHeadPtr && *BodyHeadPtr) ? *BodyHeadPtr : nullptr;
        if (!BodyHead)
        {
            /* The body head can be a suppressed no-op push (BeginPlay: latch
             * targets push@15) that never emits an anchor - fall back to the
             * nearest anchor inside the cluster window; the chain bridge made
             * sure the incoming exec wire reaches exactly that node. In the
             * pre-body-gate layout (GM) the whole [BodyFirst..CondLast] span
             * is scaffold, so search up to the latch where the real body
             * lives. */
            int32 BestSi = INDEX_NONE;
            for (const auto& Pair : Builder.StatementAnchors)
            {
                if (Pair.Key >= Loop.BodyFirst && Pair.Key <= Loop.LatchSi &&
                    (BestSi == INDEX_NONE || Pair.Key < BestSi))
                {
                    BestSi = Pair.Key;
                }
            }
            if (BestSi != INDEX_NONE)
            {
                BodyHead = Builder.StatementAnchors[BestSi];
            }
        }
        if (!BodyHead)
        {
            UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMacroLoopNodes: no emitted anchor for body head si=%d - loop skipped"), Loop.BodyFirst);
            continue;
        }

        UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Builder.Graph);
        MacroNode->CreateNewGuid();
        MacroNode->SetFlags(RF_Transactional);
        MacroNode->NodePosX = Builder.NextNodeX;
        MacroNode->NodePosY = BodyHead->NodePosY - 160;
        Builder.NextNodeY += 120;
        MacroNode->SetMacroGraph(MacroGraphPtr);
        Builder.Graph->AddNode(MacroNode, true, true);
        MacroNode->AllocateDefaultPins();

        /* --- Exec splice: whatever fed the body head now feeds Exec, and
         * LoopBody drives the body head instead. Macro instances expose their
         * input exec under the graph's own input pin name - "Exec" for the
         * StandardMacros loops, not PN_Execute("execute") - so resolve the
         * first exec input generically. --- */
        auto FirstExecPin = [](UEdGraphNode* Node, EEdGraphPinDirection Dir) -> UEdGraphPin*
        {
            for (UEdGraphPin* P : Node->Pins)
            {
                if (P && !P->bHidden && P->Direction == Dir &&
                    P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    return P;
                }
            }
            return nullptr;
        };
        UEdGraphPin* MacroExecIn = FirstExecPin(MacroNode, EGPD_Input);
        UEdGraphPin* HeadExecIn = FindPin(BodyHead, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
        /* When the body head is a pure node (e.g. Global Game Instance), it has
         * no exec pins. Scan forward through the body's collected anchors to
         * find the first impure node with an exec input. */
        if (!HeadExecIn)
        {
            for (const FBytecodeToken* T : Builder.Statements)
            {
                const int32 Si = T->StatementIndex;
                if (Si < Loop.BodyFirst || Si > Loop.BodyLast) continue;
                if (Builder.LoopSuppressedSis.Contains(Si)) continue;
                UEdGraphNode** Anchor = Builder.StatementAnchors.Find(Si);
                if (!Anchor || !*Anchor) continue;
                UEdGraphPin* Pin = FirstExecPin(*Anchor, EGPD_Input);
                if (Pin)
                {
                    HeadExecIn = Pin;
                    break;
                }
            }
        }
        if (MacroExecIn && HeadExecIn)
        {
            TArray<UEdGraphPin*> Sources = HeadExecIn->LinkedTo;
            for (UEdGraphPin* Src : Sources)
            {
                HeadExecIn->BreakLinkTo(Src);
                ConnectPins(Src, MacroExecIn);
            }
            if (Sources.Num() == 0)
            {
                /* The flat walk can reach the body with an empty exec chain:
                 * a trampoline jump-target region start before the cluster
                 * resets LastExecPin (it is not a chain bridge - it sits
                 * before BodyFirst), so nothing ever wired into the body.
                 * If the entry's then pin stayed free, drive Exec from it. */
                UEdGraphPin* EntryThen = FindPin(Builder.EntryNode, UEdGraphSchema_K2::PN_Then.ToString(), EGPD_Output);
                if (!EntryThen)
                {
                    EntryThen = FirstExecPin(Builder.EntryNode, EGPD_Output);
                }
                if (EntryThen && EntryThen->LinkedTo.Num() == 0)
                {
                    ConnectPins(EntryThen, MacroExecIn);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMacroLoopNodes: body head si=%d had no incoming exec - wired Entry.then -> Macro Exec"), Loop.BodyFirst);
                }
            }
            UEdGraphPin* LoopBodyOut = FindPin(MacroNode, TEXT("LoopBody"), EGPD_Output);
            if (!LoopBodyOut) LoopBodyOut = FindPin(MacroNode, TEXT("Loop Body"), EGPD_Output);
            if (LoopBodyOut)
            {
                ConnectPins(LoopBodyOut, HeadExecIn);
            }
        }

        /* --- Completed splice --- */
        if (UEdGraphNode** ExitPtr = Builder.StatementAnchors.Find(Loop.ExitAddr))
        {
            if (UEdGraphNode* ExitNode = *ExitPtr)
            {
                UEdGraphPin* ExitExecIn = FindPin(ExitNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
                UEdGraphPin* CompletedOut = FindPin(MacroNode, TEXT("Completed"), EGPD_Output);
                if (ExitExecIn && CompletedOut)
                {
                    /* Discard stale flat-wire sources into the exit node - the
                     * only real path to the Completed chain is the macro's own
                     * conditional exit. */
                    TArray<UEdGraphPin*> Sources = ExitExecIn->LinkedTo;
                    for (UEdGraphPin* Src : Sources)
                    {
                        ExitExecIn->BreakLinkTo(Src);
                    }
                    ConnectPins(CompletedOut, ExitExecIn);
                }
            }
        }
        else
        {
            /* Fallback: when the post-loop continuation was not collected in
             * the walk (e.g. the exit address sits beyond the path boundary),
             * wire Completed directly to the function-result node so the graph
             * compiles. */
            if (Builder.ResultNode)
            {
                UEdGraphPin* ResultExecIn = FindPin(Builder.ResultNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
                UEdGraphPin* CompletedOut = FindPin(MacroNode, TEXT("Completed"), EGPD_Output);
                if (ResultExecIn && CompletedOut)
                {
                    /* Same discard rule as the anchored exit path: the macro
                     * owns the continuation, so any flat tail chain already
                     * wired into the result (the post-walk hookup chains the
                     * last body node's then pin here when the path stops at
                     * the back edge) is scaffold residue. */
                    TArray<UEdGraphPin*> Stale = ResultExecIn->LinkedTo;
                    for (UEdGraphPin* Src : Stale)
                    {
                        ResultExecIn->BreakLinkTo(Src);
                    }
                    ConnectPins(CompletedOut, ResultExecIn);
                    UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMacroLoopNodes: Completed fallback -> FunctionResult (exit addr %d not in path)"), Loop.ExitAddr);
                }
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMacroLoopNodes: no anchor at exit addr %d for Completed chain"), Loop.ExitAddr);
            }
        }

        /* The macro owns everything downstream of the body now. The post-walk
         * result hookup chains Builder.LastExecPin (= last body node's then
         * pin) into the function-result node AFTER this splice runs - drop it
         * here or that hookup re-adds the exact flat tail wire we removed. */
        Builder.LastExecPin = nullptr;

        /* --- Break splice: rewire each break-site set node's incoming exec
         * onto the instance Break pin, then drop the now-redundant set node
         * (the macro owns the flag). --- */
        UEdGraphPin* BreakIn = FindPin(MacroNode, TEXT("Break"), EGPD_Input);
        for (int32 SiteSi : Loop.BreakSites)
        {
            UEdGraphNode** SitePtr = Builder.StatementAnchors.Find(SiteSi);
            if (!SitePtr || !*SitePtr || !BreakIn)
            {
                continue;
            }
            UEdGraphNode* SiteNode = *SitePtr;
            UEdGraphPin* SiteExecIn = FindPin(SiteNode, UEdGraphSchema_K2::PN_Execute.ToString(), EGPD_Input);
            if (SiteExecIn)
            {
                TArray<UEdGraphPin*> Sources = SiteExecIn->LinkedTo;
                for (UEdGraphPin* Src : Sources)
                {
                    SiteExecIn->BreakLinkTo(Src);
                    ConnectPins(Src, BreakIn);
                }
            }
            Builder.Graph->RemoveNode(SiteNode);
            Builder.StatementAnchors.Remove(SiteSi);
            Builder.VisitedAnchors.Remove(SiteSi);
        }

        /* --- Array input --- */
        UEdGraphPin* ArrayIn = FindPin(MacroNode, TEXT("Array"), EGPD_Input);
        if (ArrayIn && Loop.ArrayExpr.IsValid())
        {
            FPinValue ArrVal = ResolveExpression(Builder, Loop.ArrayExpr);
            if (ArrVal.Pin)
            {
                ConnectPins(ArrVal.Pin, ArrayIn);
            }
            else
            {
                UE_LOG(LogBlueprintBytecodeImporter, Warning, TEXT("EmitMacroLoopNodes: array expression unresolved for loop body=%d"), Loop.BodyFirst);
            }
        }

        /* --- Element / Array Index consumer repointing: any VariableGet of a
         * macro-internal exposed temp is replaced by the instance output so
         * values flow through the macro instead of stale frame locals. --- */
        auto RepointTempReads = [&](const FString& LocalName, const TCHAR* OutPinName)
        {
            if (LocalName.IsEmpty()) return;
            UEdGraphPin* MacroOut = FindPin(MacroNode, OutPinName, EGPD_Output);
            if (!MacroOut) return;
            TArray<UEdGraphNode*> AllNodes;
            Builder.Graph->GetNodesOfClass(AllNodes);
            for (UEdGraphNode* Node : AllNodes)
            {
                UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node);
                if (!VarGet || !VarGet->GetVarName().ToString().Equals(LocalName, ESearchCase::IgnoreCase))
                {
                    continue;
                }
                for (UEdGraphPin* Pin : VarGet->Pins)
                {
                    if (Pin->Direction != EGPD_Output || Pin->PinType.IsContainer() != MacroOut->PinType.IsContainer())
                    {
                        continue;
                    }
                    TArray<UEdGraphPin*> Consumers = Pin->LinkedTo;
                    for (UEdGraphPin* Consumer : Consumers)
                    {
                        Consumer->BreakLinkTo(Pin);
                        ConnectPins(MacroOut, Consumer);
                    }
                }
                Builder.Graph->RemoveNode(VarGet);
            }
        };
        RepointTempReads(Loop.ElementLocal, TEXT("Array Element"));
        RepointTempReads(Loop.IndexLocal, TEXT("Array Index"));

        /* --- Element splice: StandardMacros expose the current item as an
         * instance output, but compiled bodies re-read it explicitly as
         * GetArrayItem(ArrayExpr, IndexTemp). After the index repointing
         * above, such getters carry THIS instance's Array Index output on
         * their dimension pin - repoint their consumers onto Array Element
         * and drop the getter. Array identity compares the variable behind
         * the getter's array source with the variable feeding the instance
         * input so sibling arrays of the same struct type don't match. --- */
        UEdGraphPin* ElemOut = FindPin(MacroNode, TEXT("Array Element"), EGPD_Output);
        UEdGraphPin* IdxOut = FindPin(MacroNode, TEXT("Array Index"), EGPD_Output);
        if (ElemOut && IdxOut && ArrayIn)
        {
            auto SourceVariableName = [](UEdGraphPin* Pin) -> FString
            {
                for (UEdGraphPin* Src : Pin->LinkedTo)
                {
                    if (const UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Src->GetOwningNode()))
                    {
                        return VarGet->GetVarName().ToString();
                    }
                }
                return FString();
            };
            const FString IteratedVar = SourceVariableName(ArrayIn);
            TArray<UEdGraphNode*> AllNodes;
            Builder.Graph->GetNodesOfClass(AllNodes);
            for (UEdGraphNode* Node : AllNodes)
            {
                UK2Node_GetArrayItem* Getter = Cast<UK2Node_GetArrayItem>(Node);
                if (!Getter)
                {
                    continue;
                }
                UEdGraphPin* DimIn = FindPin(Getter, TEXT("Dimension 1"), EGPD_Input);
                UEdGraphPin* ArrIn = FindPin(Getter, TEXT("Array"), EGPD_Input);
                if (!DimIn || !ArrIn || !DimIn->LinkedTo.Contains(IdxOut))
                {
                    continue;
                }
                if (!IteratedVar.IsEmpty() &&
                    !SourceVariableName(ArrIn).Equals(IteratedVar, ESearchCase::IgnoreCase))
                {
                    continue;
                }
                for (UEdGraphPin* OutPin : Getter->Pins)
                {
                    if (!OutPin || OutPin->Direction != EGPD_Output)
                    {
                        continue;
                    }
                    TArray<UEdGraphPin*> Consumers = OutPin->LinkedTo;
                    for (UEdGraphPin* Consumer : Consumers)
                    {
                        Consumer->BreakLinkTo(OutPin);
                        ConnectPins(ElemOut, Consumer);
                    }
                }
                Builder.Graph->RemoveNode(Getter);
                UE_LOG(LogBlueprintBytecodeImporter, Log, TEXT("EmitMacroLoopNodes: repointed lowered element getter onto Array Element (loop body=%d)"), Loop.BodyFirst);
            }
        }

        UE_LOG(LogBlueprintBytecodeImporter, Log,
            TEXT("Emitted K2Node_MacroInstance '%s': body=%d exit=%d breaks=%d array=%d"),
            *MacroName, Loop.BodyFirst, Loop.ExitAddr, Loop.BreakSites.Num(),
            Loop.ArrayExpr.IsValid() ? 1 : 0);
    }

    /* --- Dead-node sweep: element/index repointing and getter removals leave
     * orphaned pure producers behind (e.g. a VariableGet whose only consumer
     * was a removed lowered Array_Get, or a re-emitted pure Global Game
     * Instance whose result nobody reads). Remove pure nodes whose visible
     * output pins all have zero links, iterating until stable so literals
     * feeding only removed calls go too. --- */
    for (int32 Sweep = 0; Sweep < 16; ++Sweep)
    {
        bool bRemovedAny = false;
        /* Pins registered as frame-temp producers are still live even with no
         * graph links yet - downstream reads resolve through the producer map
         * (e.g. a pure call feeding only another temp). Never sweep those. */
        TSet<UEdGraphPin*> RegisteredProducerPins;
        for (const auto& KV : Builder.ProducerPins)
        {
            if (KV.Value)
            {
                RegisteredProducerPins.Add(KV.Value);
            }
        }
        TArray<UEdGraphNode*> AllNodes;
        Builder.Graph->GetNodesOfClass(AllNodes);
        for (UEdGraphNode* Node : AllNodes)
        {
            const bool bPureProducer = Node->IsA<UK2Node_VariableGet>()
                || Node->IsA<UK2Node_Literal>()
                || (Cast<UK2Node_CallFunction>(Node) && Cast<UK2Node_CallFunction>(Node)->IsNodePure());
            if (!bPureProducer)
            {
                continue;
            }
            bool bIsRegisteredProducer = false;
            bool bHasVisibleOutput = false;
            bool bAnyOutputLinked = false;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->bHidden)
                {
                    continue;
                }
                bHasVisibleOutput = true;
                if (RegisteredProducerPins.Contains(Pin))
                {
                    bIsRegisteredProducer = true;
                    break;
                }
                if (Pin->LinkedTo.Num() > 0)
                {
                    bAnyOutputLinked = true;
                }
            }
            if (bIsRegisteredProducer)
            {
                continue;
            }
            if (bHasVisibleOutput && !bAnyOutputLinked)
            {
                Builder.Graph->RemoveNode(Node);
                bRemovedAny = true;
            }
        }
        if (!bRemovedAny)
        {
            break;
        }
    }
}
