/* Copyright Reflection Contributors 2024-2026 */

/* TODO: Rewrite */

#include "Importers/Types/UserDefined/UserDefinedStructImporter.h"

#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Utilities/JsonHelpers.h"
#include "Internationalization/Regex.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

static const FRegexPattern PropertyNameRegexPattern(TEXT(R"((.*)_(\d+)_([0-9A-Z]+))"));

static const TMap<FString, const FName> PropertyCategoryMap = {
    {TEXT("BoolProperty"), TEXT("bool")},
    {TEXT("ByteProperty"), TEXT("byte")},
    {TEXT("IntProperty"), TEXT("int")},
    {TEXT("Int64Property"), TEXT("int64")},
    {TEXT("FloatProperty"), TEXT("real")},
    {TEXT("DoubleProperty"), TEXT("real")},
    {TEXT("StrProperty"), TEXT("string")},
    {TEXT("TextProperty"), TEXT("text")},
    {TEXT("NameProperty"), TEXT("name")},
    {TEXT("ClassProperty"), TEXT("class")},
    {TEXT("SoftClassProperty"), TEXT("softclass")},
    {TEXT("ObjectProperty"), TEXT("object")},
    {TEXT("SoftObjectProperty"), TEXT("softobject")},
    {TEXT("EnumProperty"), TEXT("byte")},
    {TEXT("StructProperty"), TEXT("struct")},
};

static const TMap<FString, EPinContainerType> ContainerTypeMap = {
    {TEXT("ArrayProperty"), EPinContainerType::Array},
    {TEXT("MapProperty"), EPinContainerType::Map},
    {TEXT("SetProperty"), EPinContainerType::Set},
};

/* GC does not trace the UUserDefinedStruct default instance (it lives in a raw
 * FStructOnScope buffer, see UUserDefinedStructEditorData::RecreateDefaultInstance),
 * so a hard object reference stored in a default value is only safe while the
 * target is rooted by something else. Defaults that point at transient or
 * never-saved objects survive until the next full GC sweep and crash it with
 * "Invalid object in GC" (or an access violation reading ~0x8000000000 on the
 * async GC worker). Null such references before they are exported to text.
 *
 * The usual offender is the cooked export itself: the game session's default held
 * a raw pointer to a reinstanced UserDefinedStruct (/Engine/Transient.
 * STRUCT_REINST_*) and FModel recorded that transient path verbatim. Re-resolving
 * it at import time yields another transient reinstate that nothing roots, so the
 * reference must be dropped. Only native (/Script) and RF_Standalone objects are
 * guaranteed to outlive the untraced buffer. */
static bool IsSafeDefaultObjectReference(const UObject* Object) {
	if (Object == nullptr) return true;

	/* Saved/loaded assets are rooted by the object registry and keep the pointer valid */
	if (Object->HasAnyFlags(RF_Standalone)) return true;

	const UPackage* Package = Object->GetPackage();
	if (Package == nullptr) return false;

	/* Native classes/structs live in /Script packages and are always rooted */
	if (Package->GetName().StartsWith(TEXT("/Script/"))) return true;

	return false;
}

static void SanitizeUnsafeObjectReferences(const UStruct* Struct, uint8* StructMemory);

static void SanitizeObjectValue(const FProperty* Property, uint8* Memory) {
	if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property)) {
		UObject* Referenced = ObjectProperty->GetObjectPropertyValue(Memory);
		if (!IsSafeDefaultObjectReference(Referenced)) {
			ObjectProperty->SetObjectPropertyValue(Memory, nullptr);
		}
		return;
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property)) {
		FScriptArrayHelper Helper(ArrayProperty, Memory);
		for (int32 Index = 0; Index < Helper.Num(); ++Index) {
			SanitizeObjectValue(ArrayProperty->Inner, Helper.GetRawPtr(Index));
		}
		return;
	}
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property)) {
		FScriptMapHelper Helper(MapProperty, Memory);
		for (int32 Index = 0; Index < Helper.Num(); ++Index) {
			SanitizeObjectValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index));
			SanitizeObjectValue(MapProperty->ValueProp, Helper.GetValuePtr(Index));
		}
		return;
	}
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property)) {
		FScriptSetHelper Helper(SetProperty, Memory);
		for (int32 Index = 0; Index < Helper.Num(); ++Index) {
			SanitizeObjectValue(SetProperty->ElementProp, Helper.GetElementPtr(Index));
		}
		return;
	}
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property)) {
		SanitizeUnsafeObjectReferences(StructProperty->Struct, Memory);
		return;
	}
}

static void SanitizeUnsafeObjectReferences(const UStruct* Struct, uint8* StructMemory) {
	for (TFieldIterator<FProperty> It(Struct); It; ++It) {
		FProperty* Property = *It;
		const int32 ArrayDim = Property->ArrayDim;
		for (int32 DimIndex = 0; DimIndex < ArrayDim; ++DimIndex) {
			uint8* Memory = Property->ContainerPtrToValuePtr<uint8>(StructMemory, DimIndex);
			SanitizeObjectValue(Property, Memory);
		}
	}
}

UObject* IUserDefinedStructImporter::CreateAsset(UObject* CreatedAsset) {
    UPackage* Package = GetPackage();
    const FString AssetName = GetAssetName();

    /* Re-importing over an existing UDS cannot reuse the object: NewObject returns
     * the stale struct with its old compiled ChildProperties/layout while the
     * importer replaces EditorData, so the first AddVariable -> OnStructureChanged
     * recompiles that stale layout. Members that were saved against reinstanced
     * structs (/Engine/Transient.STRUCT_REINST_*) resolve to empty, zero-sized
     * types on load, and reinstancing such a layout asserts "ElementSize > 0" in
     * FScriptArrayHelper. Move the old object aside so a fresh, internally
     * consistent struct is compiled from the JSON. */
    if (UUserDefinedStruct* Existing = FindObject<UUserDefinedStruct>(Package, *AssetName)) {
    //crash UUserDefinedStruct* Existing = FindObject<UUserDefinedStruct>(Package, *AssetName);

    //crash if (Existing != nullptr) {
        const FString TrashName = FString::Printf(TEXT("%s_TRASH_%s"), *AssetName, *FGuid::NewGuid().ToString());
        Existing->Rename(*TrashName, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty | REN_NonTransactional);
    }

    return IImporter::CreateAsset(FStructureEditorUtils::CreateUserDefinedStruct(Package, *AssetName, RF_Standalone | RF_Public | RF_Transactional));
    //crash UUserDefinedStruct* NewStruct = FStructureEditorUtils::CreateUserDefinedStruct(Package, *AssetName, RF_Standalone | RF_Public | RF_Transactional);

    /* Dependents resolve a same-batch reference through FAssetEntry::CreatedObject (see
     * IImporter::LoadExport) - never by name - so the shell CreateShells built stays the one
     * they point at even after this import replaces it. Point the registry at the fresh struct
     * this import actually fills in, or every dependent keeps a reference to the renamed trash
     * shell and shows up as "Struct unknown (deleted?)". */
    /*crash if (FAssetEntry* PlannedEntry = FAssetDependencyRegistry::Get().FindByPackagePath(Package->GetName())) {
        if (PlannedEntry->CreatedObject == Existing) {
            PlannedEntry->CreatedObject = NewStruct;
        }
    }

    return IImporter::CreateAsset(NewStruct);*/
}

bool IUserDefinedStructImporter::Import() {
    UUserDefinedStruct* UserDefinedStruct = Create<UUserDefinedStruct>();

    DefaultProperties = GetAssetData()->GetObjectField(TEXT("DefaultProperties"));
    GetObjectSerializer()->DeserializeObjectProperties(KeepPropertiesShared(GetAssetData(),
    {
        "Guid",
        "StructFlags"
    }), UserDefinedStruct);

    /* Struct Metadata [Editor Only Data] */
    CookedStructMetaData = GetContainer()->FindByType(FString("StructCookedMetaData"));
    
    if (CookedStructMetaData && CookedStructMetaData->Has("StructMetaData")) {
        TArray<FUObjectJsonValueExport> ObjectMetaData = CookedStructMetaData->GetObject("StructMetaData").GetArray("ObjectMetaData");

        for (FUObjectJsonValueExport& ObjectMetadataValue : ObjectMetaData) {
            FString MetadataKey = ObjectMetadataValue.GetString("Key");
            FString MetadataValue = ObjectMetadataValue.GetString("Value");

            UserDefinedStruct->SetMetaData(FName(*MetadataKey), *MetadataValue);

            /* Tooltip is a part of EditorData */
            if (MetadataKey == TEXT("Tooltip")) {
                FStructureEditorUtils::ChangeTooltip(UserDefinedStruct, MetadataValue);
            }
        }
    }
    
    /* Remove default variable */
    FStructureEditorUtils::GetVarDesc(UserDefinedStruct).Pop();

    const TArray<TSharedPtr<FJsonValue>> ChildProperties = GetAssetData()->GetArrayField(TEXT("ChildProperties"));
    
    for (const auto& Property : ChildProperties) {
        const TSharedPtr<FJsonObject>& PropertyObject = Property->AsObject();
        
        ImportPropertyIntoStruct(UserDefinedStruct, PropertyObject);
    }

    /* Rebuild the default instance from the (sanitized) default text so no raw
     * object pointer from an intermediate recompile survives into GC. */
    if (UUserDefinedStructEditorData* EditorData = Cast<UUserDefinedStructEditorData>(UserDefinedStruct->EditorData)) {
        EditorData->RecreateDefaultInstance();
    }

    /* Handle edit changes, and add it to the content browser */
    return OnAssetCreation(UserDefinedStruct);
}

void IUserDefinedStructImporter::ImportPropertyIntoStruct(UUserDefinedStruct* UserDefinedStruct, const TSharedPtr<FJsonObject> &PropertyJsonObject) {
    const FString Name = PropertyJsonObject->GetStringField(TEXT("Name"));
    const FString Type = PropertyJsonObject->GetStringField(TEXT("Type"));

    FString FieldDisplayName = Name;
    FGuid FieldGuid;

    FRegexMatcher RegexMatcher(PropertyNameRegexPattern, Name);
    
    if (RegexMatcher.FindNext()) {
        /* Import properties keeping GUID if present */
        FieldDisplayName = RegexMatcher.GetCaptureGroup(1);
        FieldGuid = StringToGuid(RegexMatcher.GetCaptureGroup(3));
    } else {
        CastChecked<UUserDefinedStructEditorData>(UserDefinedStruct->EditorData)->GenerateUniqueNameIdForMemberVariable();
        FieldGuid = FGuid::NewGuid();
    }

    FStructVariableDescription Variable; {
        Variable.VarName = *Name;
        Variable.FriendlyName = FieldDisplayName;
        Variable.VarGuid = FieldGuid;

        Variable.SetPinType(ResolvePropertyPinType(PropertyJsonObject));
    }

    FStructureEditorUtils::GetVarDesc(UserDefinedStruct).Add(Variable);
    FStructureEditorUtils::OnStructureChanged(UserDefinedStruct, FStructureEditorUtils::EStructureEditorChangeInfo::AddedVariable);

    const TSharedPtr<FJsonValue>* FoundDefault = DefaultProperties->Values.Find(Name);
    if (FoundDefault == nullptr) {
        return;
    }
    const TSharedPtr<FJsonValue>& PropertyJsonValue = *FoundDefault;

    FProperty* Property = FindFProperty<FProperty>(UserDefinedStruct, *Name);

    if (Property == nullptr) {
        return;
    }

    /* DefaultProperties ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
    FStructOnScope StructScope(UserDefinedStruct);
    uint8* InstanceMemory = StructScope.GetStructMemory();

    /* Get Property Value and deserialize the values */
    void* PropertyValue = Property->ContainerPtrToValuePtr<void>(InstanceMemory);
    GetPropertySerializer()->DeserializePropertyValue(Property, PropertyJsonValue.ToSharedRef(), PropertyValue);

    /* Null any default object references that GC cannot keep alive before the
     * value is exported to the struct's DefaultValue text (see the comment on
     * IsSafeDefaultObjectReference). */
    SanitizeUnsafeObjectReferences(UserDefinedStruct, InstanceMemory);

    /* Get the default value as a string */
    FString DefaultValue;
#if ENGINE_UE5
    Property->ExportTextItem_Direct(DefaultValue, PropertyValue, nullptr, UserDefinedStruct, 0);
#else
    Property->ExportText_Direct(DefaultValue, PropertyValue, nullptr, UserDefinedStruct, 0);
#endif

    /* Update the variable */
    FStructureEditorUtils::ChangeVariableDefaultValue(UserDefinedStruct, Variable.VarGuid, DefaultValue);

    /* Editor Only Data */
    if (CookedStructMetaData && CookedStructMetaData->Has("StructMetaData")) {
        TArray<FUObjectJsonValueExport> PropertiesMetaData = CookedStructMetaData->GetObject("StructMetaData").GetArray("PropertiesMetaData");

        for (const FUObjectJsonValueExport& Value : PropertiesMetaData) {
            /* Find a matching key */
            if (Value.GetString("Key") == Name) {
                TArray<FUObjectJsonValueExport> FieldMetaData = Value.GetObject("Value").GetArray("FieldMetaData");

                for (FUObjectJsonValueExport& FieldValue : FieldMetaData) {
                    FString MetadataKey = FieldValue.GetString("Key");
                    FString MetadataValue = FieldValue.GetString("Value");

                    Property->SetMetaData(FName(*MetadataKey), *MetadataValue);

                    if (MetadataKey == TEXT("Tooltip")) {
                        FStructureEditorUtils::ChangeVariableTooltip(UserDefinedStruct, Variable.VarGuid, MetadataValue);
                    }

                    if (MetadataKey == TEXT("DisplayName")) {
                        Variable.FriendlyName = MetadataValue;
                    }
                }
            }
        }
    }
}

FEdGraphPinType IUserDefinedStructImporter::ResolvePropertyPinType(const TSharedPtr<FJsonObject> &PropertyJsonObject) {
    const FString Type = PropertyJsonObject->GetStringField(TEXT("Type"));

    /* Special handling for containers */
    if (const EPinContainerType* ContainerType = ContainerTypeMap.Find(Type)) {
        if (*ContainerType == EPinContainerType::Map) {
            TSharedPtr<FJsonObject> KeyPropObject = PropertyJsonObject->GetObjectField(TEXT("KeyProp"));
            
            FEdGraphPinType ResolvedType = ResolvePropertyPinType(KeyPropObject);
            ResolvedType.ContainerType = *ContainerType;

            TSharedPtr<FJsonObject> ValuePropObject = PropertyJsonObject->GetObjectField(TEXT("ValueProp"));
            FEdGraphPinType ResolvedTerminalType = ResolvePropertyPinType(ValuePropObject);
            
            ResolvedType.PinValueType.TerminalCategory = ResolvedTerminalType.PinCategory;
            ResolvedType.PinValueType.TerminalSubCategory = ResolvedTerminalType.PinSubCategory;
            ResolvedType.PinValueType.TerminalSubCategoryObject = ResolvedTerminalType.PinSubCategoryObject;

            return ResolvedType;
        }

        if (*ContainerType == EPinContainerType::Set) {
            TSharedPtr<FJsonObject> ElementPropObject = PropertyJsonObject->GetObjectField(TEXT("ElementProp"));
            FEdGraphPinType ResolvedType = ResolvePropertyPinType(ElementPropObject);
            
            ResolvedType.ContainerType = *ContainerType;
            
            return ResolvedType;
        }

        if (*ContainerType == EPinContainerType::Array) {
            TSharedPtr<FJsonObject> InnerTypeObject = PropertyJsonObject->GetObjectField(TEXT("Inner"));
            FEdGraphPinType ResolvedType = ResolvePropertyPinType(InnerTypeObject);
            
            ResolvedType.ContainerType = *ContainerType;
            
            return ResolvedType;
        }
    }

    FEdGraphPinType ResolvedType = FEdGraphPinType(NAME_None, NAME_None, nullptr, EPinContainerType::None,false, FEdGraphTerminalType());

    /* Find main type from our PropertyCategoryMap */

    if (const FName* TypeCategory = PropertyCategoryMap.Find(Type)) {
        ResolvedType.PinCategory = *TypeCategory;
    } else {
        UE_LOG(LogReflection, Warning, TEXT("Type '%s' not found in PropertyCategoryMap, defaulting to 'Byte'"), *Type);
        ResolvedType.PinCategory = TEXT("byte");
    }

    /* Special handling for some types */
    if (Type == "DoubleProperty") {
        ResolvedType.PinSubCategory = TEXT("double");
    } else if (Type == "FloatProperty") {
        ResolvedType.PinSubCategory = TEXT("float");
    } else if (Type == "EnumProperty" || Type == "ByteProperty") {
        ResolvedType.PinSubCategoryObject = LoadObjectFromJsonReference(PropertyJsonObject, TEXT("Enum"));
    } else if (Type == "StructProperty") {
        ResolvedType.PinSubCategoryObject = LoadObjectFromJsonReference(PropertyJsonObject, TEXT("Struct"));
    } else if (Type == "ClassProperty" || Type == "SoftClassProperty") {
        ResolvedType.PinSubCategoryObject = LoadObjectFromJsonReference(PropertyJsonObject, TEXT("MetaClass"));
    } else if (Type == "ObjectProperty" || Type == "SoftObjectProperty") {
        ResolvedType.PinSubCategoryObject = LoadObjectFromJsonReference(PropertyJsonObject, TEXT("PropertyClass"));
    }

    return ResolvedType;
}

UObject* IUserDefinedStructImporter::LoadObjectFromJsonReference(const TSharedPtr<FJsonObject> &ParentJsonObject, const FString &ReferenceKey) {
    const TSharedPtr<FJsonObject> ReferenceObject = ParentJsonObject->GetObjectField(ReferenceKey);
    
    if (!ReferenceObject) {
        UE_LOG(LogReflection, Error, TEXT("Failed to load Object from property %s: property not found"), *ReferenceKey);
        return nullptr;
    }

    TObjectPtr<UObject> LoadedObject;
    LoadExport<UObject>(&ReferenceObject, LoadedObject);
    
    return LoadedObject;
}
