/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/ControlRigImporter.h"

#if ENGINE_UE5
#include "ControlRigDeveloper/Public/ControlRigBlueprintLegacy.h"
#include "ControlRig/Public/ControlRigBlueprintGeneratedClass.h"
#include "ControlRig/Public/ControlRig.h"
#include "ControlRig/Public/Rigs/RigHierarchy.h"
#include "ControlRig/Public/Rigs/RigHierarchyController.h"
#include "ControlRig/Public/Rigs/RigHierarchyElements.h"
#include "ControlRig/Public/Rigs/RigHierarchyDefines.h"
#include "RigVMDeveloper/Public/RigVMModel/RigVMClient.h"
#include "RigVMDeveloper/Public/RigVMModel/RigVMController.h"
#include "RigVMDeveloper/Public/RigVMModel/RigVMGraph.h"
#include "RigVMDeveloper/Public/RigVMModel/RigVMLink.h"
#include "RigVMDeveloper/Public/RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMDeveloper/Public/RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMDeveloper/Public/RigVMModel/Nodes/RigVMUnitNode.h"
#include "ControlRig/Public/ControlRigGizmoLibrary.h"
#include "RigVM/Public/RigVMTypeUtils.h"
#include "RigVM/Public/RigVMCore/RigVM.h"
#include "RigVM/Public/RigVMCore/RigVMByteCode.h"
#include "RigVM/Public/RigVMCore/RigVMMemoryStorage.h"
#include "RigVM/Public/RigVMCore/RigVMMemoryStorageStruct.h"
#include "RigVM/Public/RigVMCore/RigVMPropertyPath.h"
#include "RigVM/Public/RigVMCore/RigVMMemoryCommon.h"
#include "RigVM/Public/RigVMTypeUtils.h"
#include "Templates/Function.h"
#endif

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"

#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Utilities/SehHelpers.h"

#if ENGINE_UE5
#endif

UControlRigBlueprint* IControlRigImporter::CreateControlRigBlueprint(UClass* ParentClass) {
	const EBlueprintType BlueprintType = GetBlueprintType(ParentClass);

#if ENGINE_UE5
	if (UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetPackage(), FName(*GetAssetName()), BlueprintType, UControlRigBlueprint::StaticClass(), UControlRigBlueprintGeneratedClass::StaticClass())) {
		return Cast<UControlRigBlueprint>(CreateAsset(Blueprint));
	}
#endif

	return nullptr;
}

UObject* IControlRigImporter::CreateAsset(UObject* CreatedAsset) {
#if ENGINE_UE5
	if (CreatedAsset == nullptr) {
		if (ControlRigBlueprint == nullptr) {
			const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
			ControlRigBlueprint = CreateControlRigBlueprint(LoadShellParentClass(SuperStruct, UControlRig::StaticClass()));
		}

		if (ControlRigBlueprint != nullptr) {
			return IImporter::CreateAsset(ControlRigBlueprint->GeneratedClass);
		}

		return nullptr;
	}
#endif

	return IImporter::CreateAsset(CreatedAsset);
}

bool IControlRigImporter::Import() {
#if ENGINE_UE5
	ControlRigBlueprint = GetSelectedAsset<UControlRigBlueprint>(true);

	if (!ControlRigBlueprint && GetPackage()) {
		UBlueprint* ExistingBlueprint = FindObject<UBlueprint>(GetPackage(), *GetAssetName());

		if (ExistingBlueprint) {
			ControlRigBlueprint = Cast<UControlRigBlueprint>(ExistingBlueprint);

			if (!ControlRigBlueprint) {
				AppendNotification(
					FText::FromString("Asset Name Already Taken"),
					FText::FromString(FString::Printf(TEXT("'%s' already exists and is not a Control Rig Blueprint. Rename or delete it before reflecting."), *GetAssetName())),
					3.0f,
					SNotificationItem::CS_Fail,
					true,
					350.0f
				);

				return false;
			}
		}
	}

	if (!ControlRigBlueprint) {
		const TSharedPtr<FJsonObject> SuperStruct = GetAssetData()->GetObjectField(TEXT("SuperStruct"));
		UClass* ParentClass = LoadClass(SuperStruct);

		ControlRigBlueprint = CreateControlRigBlueprint(ParentClass);
	}

	if (!ControlRigBlueprint) return false;

	UControlRigBlueprintGeneratedClass* GeneratedClass = Cast<UControlRigBlueprintGeneratedClass>(ControlRigBlueprint->GeneratedClass);
	if (!GeneratedClass) return false;

	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	if (ClassDefaultObjectExport->IsJsonInvalid()) return false;

	ClassDefaultObjectExport->Object = GeneratedClass;

	if (ConstructVariables() > 0) {
		CompileBlueprintSafe(ControlRigBlueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		GeneratedClass = Cast<UControlRigBlueprintGeneratedClass>(ControlRigBlueprint->GeneratedClass);
		if (!GeneratedClass) return false;

		ClassDefaultObjectExport->Object = GeneratedClass;
	}

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

	DeserializeHierarchyAndVM(ControlRigBlueprint);

	const bool bResult = OnAssetCreation(ControlRigBlueprint);
	DeserializeVM(ControlRigBlueprint);

	DeserializeGraph(ControlRigBlueprint);

	/* Load preview skeletal mesh from class export */
	{
		FUObjectExportContainer* Container = GetContainer();
		if (Container) {
			FString AssetPathName;
			for (FUObjectExport* Export : Container->Exports) {
				if (!Export || !Export->IsJsonValid()) continue;
				const TSharedPtr<FJsonObject>& Json = Export->JsonObject;
				if (!Json.IsValid()) continue;

				const TSharedPtr<FJsonObject>* PropsObj = nullptr;
				if (Json->TryGetObjectField(TEXT("Properties"), PropsObj) && (*PropsObj).IsValid()) {
					const TSharedPtr<FJsonObject>* MeshObj = nullptr;
					if ((*PropsObj)->TryGetObjectField(TEXT("PreviewSkeletalMesh"), MeshObj) && (*MeshObj).IsValid()) {
						(*MeshObj)->TryGetStringField(TEXT("AssetPathName"), AssetPathName);
						break;
					}
				}
			}

			if (!AssetPathName.IsEmpty()) {
				USkeletalMesh* PreviewMesh = LoadObject<USkeletalMesh>(nullptr, *AssetPathName);
				if (PreviewMesh) {
					ControlRigBlueprint->SetPreviewMesh(PreviewMesh, false);
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Set PreviewSkeletalMesh to '%s'"), *AssetPathName);
				} else {
					UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Failed to load PreviewSkeletalMesh '%s'"), *AssetPathName);
				}
			}
		}
	}

	return bResult;
#else
	return false;
#endif
}

int32 IControlRigImporter::ConstructVariables() {
	const TArray<TSharedPtr<FJsonValue>>* ChildProperties;

	if (!GetAssetExport()->TryGetArrayField(TEXT("ChildProperties"), ChildProperties)) {
		return 0;
	}

	return FBlueprintVariables::Construct(ControlRigBlueprint, *ChildProperties);
}

void IControlRigImporter::DeserializeHierarchyAndVM(UControlRigBlueprint* InControlRigBlueprint) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) return;

	DeserializeHierarchy(InControlRigBlueprint);

	DeserializeVM(InControlRigBlueprint);
#endif
}

void IControlRigImporter::DeserializeHierarchy(UControlRigBlueprint* InControlRigBlueprint) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) return;

	FUObjectExportContainer* Container = GetContainer();
	if (!Container) return;

	FUObjectExport* HierarchyExport = Container->GetExportStartingWith(TEXT("Name"), TEXT("DynamicHierarchy"));
	if (!HierarchyExport || !HierarchyExport->IsJsonValid()) return;

	const TSharedPtr<FJsonObject>& HierJson = HierarchyExport->JsonObject;
	if (!HierJson.IsValid()) return;

	const TArray<TSharedPtr<FJsonValue>>* ElementsArray = nullptr;
	if (!HierJson->TryGetArrayField(TEXT("Elements"), ElementsArray) || ElementsArray->Num() == 0) return;

	URigHierarchy* Hierarchy = NewObject<URigHierarchy>(InControlRigBlueprint, TEXT("DynamicHierarchy"));
	if (!Hierarchy) return;

	URigHierarchyController* Controller = Hierarchy->GetController(true);
	if (!Controller) return;

	TMap<FString, FRigElementKey> CreatedElements;

	for (int32 i = 0; i < ElementsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& ElemJson = (*ElementsArray)[i]->AsObject();
		if (!ElemJson.IsValid()) continue;

		/* JSON elements use LoadedKey {Type, Name} for the element identity,
		 * and ParentKey {Type, Name} for the parent reference. */
		const TSharedPtr<FJsonObject>* LoadedKeyObj = nullptr;
		if (!ElemJson->TryGetObjectField(TEXT("LoadedKey"), LoadedKeyObj) || !(*LoadedKeyObj).IsValid()) continue;

		FString ElemName;
		if (!(*LoadedKeyObj)->TryGetStringField(TEXT("Name"), ElemName)) continue;

		int32 ElemType = 0;
		(*LoadedKeyObj)->TryGetNumberField(TEXT("Type"), ElemType);

		ERigElementType RigType = (ERigElementType)ElemType;

		FString ParentName;
		const TSharedPtr<FJsonObject>* ParentKeyObj = nullptr;
		if (ElemJson->TryGetObjectField(TEXT("ParentKey"), ParentKeyObj) && (*ParentKeyObj).IsValid()) {
			(*ParentKeyObj)->TryGetStringField(TEXT("Name"), ParentName);
		}

		FRigElementKey ParentKey;
		if (!ParentName.IsEmpty() && ParentName != TEXT("None")) {
			for (const auto& Pair : CreatedElements) {
				if (Pair.Value.Name == FName(*ParentName)) {
					ParentKey = Pair.Value;
					break;
				}
			}
		}

		FTransform Transform = FTransform::Identity;
		FRigElementKey NewKey;

		if (RigType == ERigElementType::Bone) {
			NewKey = Controller->AddBone(FName(*ElemName), ParentKey, Transform, true, ERigBoneType::User, false, false);
		} else if (RigType == ERigElementType::Control) {
			FRigControlSettings Settings;
			NewKey = Controller->AddControl(FName(*ElemName), ParentKey, Settings, FRigControlValue(), Transform, Transform, false, false);
		} else if (RigType == ERigElementType::Null) {
			NewKey = Controller->AddNull(FName(*ElemName), ParentKey, Transform, true, false, false);
		}

		if (NewKey.IsValid()) {
			CreatedElements.Add(ElemName, NewKey);
		}
	}

	InControlRigBlueprint->Hierarchy = Hierarchy;

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Hierarchy deserialized with %d elements for '%s'"),
		CreatedElements.Num(), *InControlRigBlueprint->GetName());
#endif
}

void IControlRigImporter::DeserializeVM(UControlRigBlueprint* InControlRigBlueprint) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) return;

	FUObjectExportContainer* Container = GetContainer();
	if (!Container) return;

	const TSharedPtr<FJsonObject>* VMObj = nullptr;
	FString VMSource;

	for (FUObjectExport* Export : Container->Exports) {
		if (!Export || !Export->IsJsonValid()) continue;
		const TSharedPtr<FJsonObject>& Json = Export->JsonObject;

		if (Json->HasField(TEXT("VM"))) {
			const TSharedPtr<FJsonObject>* VMField = nullptr;
			if (Json->TryGetObjectField(TEXT("VM"), VMField) && (*VMField)->HasField(TEXT("ByteCodeStorage"))) {
				VMObj = VMField;
				VMSource = TEXT("inline VM on class export");
				break;
			}
		}

		if (Json->HasField(TEXT("FunctionNamesStorage"))) {
			VMObj = &Json;
			VMSource = TEXT("separate RigVM export");
			break;
		}
	}

	if (!VMObj) return;

	const TSharedPtr<FJsonObject>& VMJson = *VMObj;

	const TSharedPtr<FJsonObject>* ByteCodeStorageObj = nullptr;
	VMJson->TryGetObjectField(TEXT("ByteCodeStorage"), ByteCodeStorageObj);
	if (!ByteCodeStorageObj) return;

	const TArray<TSharedPtr<FJsonValue>>* InstructionsArray = nullptr;
	(*ByteCodeStorageObj)->TryGetArrayField(TEXT("Instructions"), InstructionsArray);
	if (!InstructionsArray) return;

	TArray<uint8> ByteCode;
	if (ByteCodeStorageObj) {
		FString ByteCodeBase64;
		if ((*ByteCodeStorageObj)->TryGetStringField(TEXT("ByteCode"), ByteCodeBase64)) {
			FBase64::Decode(ByteCodeBase64, ByteCode);
		}
	}

	TArray<FString> FunctionNames;
	const TArray<TSharedPtr<FJsonValue>>* FunctionNamesArray = nullptr;
	if (VMJson->TryGetArrayField(TEXT("FunctionNamesStorage"), FunctionNamesArray)) {
		for (const TSharedPtr<FJsonValue>& FnVal : *FunctionNamesArray) {
			FString FnName;
			if (FnVal->TryGetString(FnName)) {
				FunctionNames.Add(MoveTemp(FnName));
			}
		}
	}

	URigVM* VM = nullptr;
	if (InControlRigBlueprint->GeneratedClass) {
		UControlRigBlueprintGeneratedClass* GenClass = Cast<UControlRigBlueprintGeneratedClass>(InControlRigBlueprint->GeneratedClass);
		if (GenClass && GenClass->GetDefaultObject()) {
			UControlRig* CDO = Cast<UControlRig>(GenClass->GetDefaultObject());
			if (CDO) {
				VM = CDO->GetVM();
			}
		}
	}

	if (!VM) return;

	if (ByteCode.Num() > 0) {
		FMemoryReader Ar(ByteCode, true);
		VM->GetByteCode().Load(Ar);
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: VM deserialized for '%s'"), *InControlRigBlueprint->GetName());
#endif
}

static FString ConvertJsonValueToString(const TSharedPtr<FJsonValue>& Val) {
	if (!Val.IsValid()) return FString();

	EJson Type = Val->Type;

	if (Type == EJson::Boolean) {
		return Val->AsBool() ? TEXT("true") : TEXT("false");
	}
	if (Type == EJson::Number) {
		double D = Val->AsNumber();
		int64 I = (int64)D;
		if ((double)I == D) {
			return FString::FromInt(I);
		}
		return FString::SanitizeFloat(D);
	}
	if (Type == EJson::String) {
		return Val->AsString();
	}
	if (Type == EJson::Array) {
		const TArray<TSharedPtr<FJsonValue>>& Arr = Val->AsArray();
		FString Result = TEXT("(");
		for (int32 j = 0; j < Arr.Num(); ++j) {
			if (j > 0) Result += TEXT(",");
			Result += ConvertJsonValueToString(Arr[j]);
		}
		Result += TEXT(")");
		return Result;
	}
	if (Type == EJson::Object) {
		const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
		if (Obj.IsValid()) {
			bool bHasType = Obj->HasField(TEXT("Type"));
			bool bHasName = Obj->HasField(TEXT("Name"));
			if (bHasType && bHasName) {
				int32 ElemType = 0;
				Obj->TryGetNumberField(TEXT("Type"), ElemType);
				FString ElemName;
				Obj->TryGetStringField(TEXT("Name"), ElemName);
				return FString::Printf(TEXT("(Type=%d,Name=\"%s\")"), ElemType, *ElemName);
			}
			FString Result = TEXT("{");
			bool bFirst = true;
			for (const auto& Pair : Obj->Values) {
				if (!bFirst) Result += TEXT(",");
				bFirst = false;
				Result += FString::Printf(TEXT("\"%s\":%s"), *Pair.Key, *ConvertJsonValueToString(Pair.Value));
			}
			Result += TEXT("}");
			return Result;
		}
	}
	return FString();
}

void IControlRigImporter::DeserializeLiteralMemory(UControlRigBlueprint* InControlRigBlueprint, TMap<int32, FString>& OutRegisterToValue) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) return;

	FUObjectExportContainer* Container = GetContainer();
	if (!Container) return;

	FUObjectExport* VMExport = Container->GetExportStartingWith(TEXT("Type"), TEXT("RigVM"));
	if (!VMExport || !VMExport->IsJsonValid()) return;

	const TSharedPtr<FJsonObject>& VMJson = VMExport->JsonObject;
	if (!VMJson.IsValid()) return;

	const TSharedPtr<FJsonObject>* LiteralMemObj = nullptr;
	if (!VMJson->TryGetObjectField(TEXT("LiteralMemoryStorage"), LiteralMemObj)) return;

	const TArray<TSharedPtr<FJsonValue>>* PropertyDescsArray = nullptr;
	if (!(*LiteralMemObj)->TryGetArrayField(TEXT("PropertyDescs"), PropertyDescsArray) || PropertyDescsArray->Num() == 0) return;

	const TSharedPtr<FJsonObject>* PropertyValuesObj = nullptr;
	if (!(*LiteralMemObj)->TryGetObjectField(TEXT("PropertyValues"), PropertyValuesObj)) return;

	for (int32 i = 0; i < PropertyDescsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& Desc = (*PropertyDescsArray)[i]->AsObject();
		if (!Desc.IsValid()) continue;

		FString PropName;
		if (!Desc->TryGetStringField(TEXT("Name"), PropName)) continue;

		TSharedPtr<FJsonValue> FoundValue = (*PropertyValuesObj)->TryGetField(PropName);
		if (!FoundValue.IsValid()) continue;

		FString DefaultValue = ConvertJsonValueToString(FoundValue);
		if (!DefaultValue.IsEmpty()) {
			OutRegisterToValue.Add(i, MoveTemp(DefaultValue));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: LiteralMemory extracted %d values from JSON PropertyValues"), OutRegisterToValue.Num());
#endif
}

void IControlRigImporter::DeserializeWorkMemory(UControlRigBlueprint* InControlRigBlueprint, TMap<int32, FString>& OutRegisterToValue) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) return;

	FUObjectExportContainer* Container = GetContainer();
	if (!Container) return;

	FUObjectExport* VMExport = Container->GetExportStartingWith(TEXT("Type"), TEXT("RigVM"));
	if (!VMExport || !VMExport->IsJsonValid()) return;

	const TSharedPtr<FJsonObject>& VMJson = VMExport->JsonObject;
	if (!VMJson.IsValid()) return;

	const TSharedPtr<FJsonObject>* WorkMemObj = nullptr;
	if (!VMJson->TryGetObjectField(TEXT("DefaultWorkMemoryStorage"), WorkMemObj)) return;

	const TArray<TSharedPtr<FJsonValue>>* PropertyDescsArray = nullptr;
	if (!(*WorkMemObj)->TryGetArrayField(TEXT("PropertyDescs"), PropertyDescsArray) || PropertyDescsArray->Num() == 0) return;

	const TSharedPtr<FJsonObject>* PropertyValuesObj = nullptr;
	if (!(*WorkMemObj)->TryGetObjectField(TEXT("PropertyValues"), PropertyValuesObj)) return;

	for (int32 i = 0; i < PropertyDescsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& Desc = (*PropertyDescsArray)[i]->AsObject();
		if (!Desc.IsValid()) continue;

		FString PropName;
		if (!Desc->TryGetStringField(TEXT("Name"), PropName)) continue;

		TSharedPtr<FJsonValue> FoundValue = (*PropertyValuesObj)->TryGetField(PropName);
		if (!FoundValue.IsValid()) continue;

		FString DefaultValue = ConvertJsonValueToString(FoundValue);
		if (!DefaultValue.IsEmpty()) {
			OutRegisterToValue.Add(i, MoveTemp(DefaultValue));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: WorkMemory extracted %d values from JSON PropertyValues"), OutRegisterToValue.Num());
#endif
}

void IControlRigImporter::DeserializeGraph(UControlRigBlueprint* InControlRigBlueprint) {
#if ENGINE_UE5
	if (!InControlRigBlueprint) { UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: null blueprint")); return; }

	FUObjectExportContainer* Container = GetContainer();
	if (!Container) { UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: null container")); return; }

	const TSharedPtr<FJsonObject>* VMObj = nullptr;
	FString VMSource;

	for (FUObjectExport* Export : Container->Exports) {
		if (!Export || !Export->IsJsonValid()) continue;
		const TSharedPtr<FJsonObject>& Json = Export->JsonObject;

		if (Json->HasField(TEXT("VM"))) {
			const TSharedPtr<FJsonObject>* VMField = nullptr;
			if (Json->TryGetObjectField(TEXT("VM"), VMField) && (*VMField)->HasField(TEXT("ByteCodeStorage"))) {
				VMObj = VMField;
				VMSource = TEXT("inline VM on class export");
				break;
			}
		}

		if (Json->HasField(TEXT("FunctionNamesStorage"))) {
			VMObj = &Json;
			VMSource = TEXT("separate RigVM export");
			break;
		}
	}

	if (!VMObj) { UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: no VM data found in any export")); return; }
	UE_LOG(LogTemp, Log, TEXT("DeserializeGraph: using %s"), *VMSource);

	const TSharedPtr<FJsonObject>& VMJson = *VMObj;

	const TArray<TSharedPtr<FJsonValue>>* FunctionNamesArray = nullptr;
	if (!VMJson->TryGetArrayField(TEXT("FunctionNamesStorage"), FunctionNamesArray) || FunctionNamesArray->Num() == 0) {
		UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: no FunctionNamesStorage"));
		return;
	}

	TArray<FString> FunctionNames;
	for (const TSharedPtr<FJsonValue>& FnVal : *FunctionNamesArray) {
		FString FnName;
		if (FnVal->TryGetString(FnName)) {
			FunctionNames.Add(MoveTemp(FnName));
		}
	}

	struct FMemoryProperty {
		FString PropertyName;
		FString NodeName;
		FString PinName;
	};
	TArray<FMemoryProperty> LiteralProperties;
	TArray<FMemoryProperty> WorkProperties;

	auto ParseMemoryStorage = [](const TSharedPtr<FJsonObject>* VMJsonObj, const TCHAR* FieldName, TArray<FMemoryProperty>& OutProperties) {
		const TSharedPtr<FJsonObject>* StorageObj = nullptr;
		if (!(*VMJsonObj)->TryGetObjectField(FieldName, StorageObj)) return;
		const TArray<TSharedPtr<FJsonValue>>* Descs = nullptr;
		if (!(*StorageObj)->TryGetArrayField(TEXT("PropertyDescs"), Descs)) return;

		for (int32 i = 0; i < Descs->Num(); ++i) {
			const TSharedPtr<FJsonObject>& Desc = (*Descs)[i]->AsObject();
			if (!Desc.IsValid()) continue;
			FString RawName;
			if (!Desc->TryGetStringField(TEXT("Name"), RawName)) continue;

			FMemoryProperty Prop;
			Prop.PropertyName = RawName;

			FString Remainder;
			int32 TripleUnderscore = RawName.Find(TEXT("___"));
			if (TripleUnderscore != INDEX_NONE) {
				Remainder = RawName.Mid(TripleUnderscore + 3);
			} else {
				Remainder = RawName;
			}

			FString NodePin = Remainder;
			NodePin.RemoveFromEnd(TEXT("__Const"));

			Prop.PinName = NodePin;
			Prop.NodeName = TEXT("");

			OutProperties.Add(MoveTemp(Prop));
		}
	};

	ParseMemoryStorage(&VMJson, TEXT("LiteralMemoryStorage"), LiteralProperties);
	ParseMemoryStorage(&VMJson, TEXT("DefaultWorkMemoryStorage"), WorkProperties);

	struct FPropertyPathDesc {
		int32 PropertyIndex = INDEX_NONE;
		FString SegmentPath;
		FString HeadCPPType;
	};
	TArray<FPropertyPathDesc> PropertyPathDescs;

	{
		const TSharedPtr<FJsonObject>* WorkMemObj = nullptr;
		if (VMJson->TryGetObjectField(TEXT("DefaultWorkMemoryStorage"), WorkMemObj)) {
			const TArray<TSharedPtr<FJsonValue>>* PropPathDescsArr = nullptr;
			if ((*WorkMemObj)->TryGetArrayField(TEXT("PropertyPathDescriptions"), PropPathDescsArr)) {
				for (const TSharedPtr<FJsonValue>& Val : *PropPathDescsArr) {
					TSharedPtr<FJsonObject> Obj = Val->AsObject();
					if (!Obj.IsValid()) continue;
					FPropertyPathDesc Desc;
					Desc.PropertyIndex = static_cast<int32>(Obj->GetNumberField(TEXT("PropertyIndex")));
					Desc.SegmentPath = Obj->GetStringField(TEXT("SegmentPath"));
					Desc.HeadCPPType = Obj->GetStringField(TEXT("HeadCPPType"));
					PropertyPathDescs.Add(MoveTemp(Desc));
				}
			}
		}
	}

	// A bytecode RegisterOffset is a global index into the PropertyPathDescriptions
	// array, exactly the value returned by FRigVMCompilerWorkData::FindOrAddPropertyPath
	// (deduped by HeadCPPType + SegmentPath within the memory type). Replicate that
	// semantics: offset -> SegmentPath, indexed directly.
	TArray<FString> OffsetToSegmentPath;
	for (const FPropertyPathDesc& Desc : PropertyPathDescs) {
		OffsetToSegmentPath.Add(Desc.SegmentPath);
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Built OffsetToSegmentPath array with %d entries from %d PropertyPathDescriptions"),
		OffsetToSegmentPath.Num(), PropertyPathDescs.Num());
	for (int32 i = 0; i < OffsetToSegmentPath.Num(); ++i) {
		UE_LOG(LogTemp, Log, TEXT("  offset %d -> '%s'"), i, *OffsetToSegmentPath[i]);
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: LiteralMemory has %d properties, WorkMemory has %d properties"),
		LiteralProperties.Num(), WorkProperties.Num());
	for (int32 i = 0; i < LiteralProperties.Num(); ++i) {
		UE_LOG(LogTemp, Log, TEXT("  Literal[%d]: '%s'"), i, *LiteralProperties[i].PropertyName);
	}
	for (int32 i = 0; i < WorkProperties.Num(); ++i) {
		UE_LOG(LogTemp, Log, TEXT("  Work[%d]: '%s'"), i, *WorkProperties[i].PropertyName);
	}

	TMap<int32, FString> LiteralRegisterValues;
	DeserializeLiteralMemory(InControlRigBlueprint, LiteralRegisterValues);

	TMap<int32, FString> WorkRegisterValues;
	DeserializeWorkMemory(InControlRigBlueprint, WorkRegisterValues);

	struct FExternalVariable {
		FString Name;
		FString CPPType;
		FString DefaultValue;
	};
	TArray<FExternalVariable> ExternalVariables;

	if (Container) {
		const TArray<TSharedPtr<FJsonValue>>* ChildPropsArr = nullptr;

		for (FUObjectExport* Export : Container->Exports) {
			if (!Export || !Export->IsJsonValid()) continue;
			const TSharedPtr<FJsonObject>& Json = Export->JsonObject;
			if (Json->TryGetArrayField(TEXT("ChildProperties"), ChildPropsArr) && ChildPropsArr->Num() > 0) {
				break;
			}
			ChildPropsArr = nullptr;
		}

		if (ChildPropsArr) {
			for (int32 i = 0; i < ChildPropsArr->Num(); ++i) {
				const TSharedPtr<FJsonObject>& Prop = (*ChildPropsArr)[i]->AsObject();
				if (!Prop.IsValid()) continue;

				FString PropName, PropType;
				Prop->TryGetStringField(TEXT("Name"), PropName);
				Prop->TryGetStringField(TEXT("Type"), PropType);

				FExternalVariable Var;
				Var.Name = PropName;

				if (PropType == TEXT("DoubleProperty")) Var.CPPType = TEXT("double");
				else if (PropType == TEXT("FloatProperty")) Var.CPPType = TEXT("float");
				else if (PropType == TEXT("BoolProperty")) Var.CPPType = TEXT("bool");
				else if (PropType == TEXT("IntProperty")) Var.CPPType = TEXT("int32");
				else if (PropType == TEXT("Int64Property")) Var.CPPType = TEXT("int64");
				else if (PropType == TEXT("StructProperty")) {
					const TSharedPtr<FJsonObject>* StructRef = nullptr;
					if (Prop->TryGetObjectField(TEXT("Struct"), StructRef) && (*StructRef).IsValid()) {
						FString ObjName = (*StructRef)->GetStringField(TEXT("ObjectName"));
						int32 Tick1 = ObjName.Find(TEXT("'"));
						if (Tick1 != INDEX_NONE) {
							int32 Tick2 = ObjName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
							if (Tick2 > Tick1) Var.CPPType = ObjName.Mid(Tick1 + 1, Tick2 - Tick1 - 1);
						}
					}
				}
				else {
					Var.CPPType = TEXT("FString");
				}

				ExternalVariables.Add(MoveTemp(Var));
			}
		}
	}

	TMap<int32, URigVMVariableNode*> ExternalVarNodes;

	const TSharedPtr<FJsonObject>* ByteCodeStorageObj = nullptr;
	VMJson->TryGetObjectField(TEXT("ByteCodeStorage"), ByteCodeStorageObj);

	TMap<int32, FString> EntryPointNames;

	const TArray<TSharedPtr<FJsonValue>>* InstructionsArray = nullptr;
	if (ByteCodeStorageObj) {
		(*ByteCodeStorageObj)->TryGetArrayField(TEXT("Instructions"), InstructionsArray);
	}

	if (ByteCodeStorageObj) {
		const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
		if ((*ByteCodeStorageObj)->TryGetArrayField(TEXT("Entries"), EntriesArray)) {

			for (const TSharedPtr<FJsonValue>& EntryVal : *EntriesArray) {
				FString EntryStr;
				if (!EntryVal->TryGetString(EntryStr)) continue;

				FString EntryName;
				int32 NameStart = EntryStr.Find(TEXT("Name=\""));
				if (NameStart == INDEX_NONE) continue;
				NameStart += 6;
				int32 NameEnd = EntryStr.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
				if (NameEnd == INDEX_NONE) continue;
				EntryName = EntryStr.Mid(NameStart, NameEnd - NameStart);

				int32 InstIdx = 0;
				int32 InstStart = EntryStr.Find(TEXT("InstructionIndex="));
				if (InstStart != INDEX_NONE) {
					InstIdx = FCString::Atoi(*EntryStr.Mid(InstStart + 17));
				}

				if (InstructionsArray && InstIdx < InstructionsArray->Num()) {
					const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[InstIdx]->AsObject();
					if (InstrObj.IsValid()) {
						int32 FuncIdx = INDEX_NONE;
						InstrObj->TryGetNumberField(TEXT("FunctionIndex"), FuncIdx);
						if (FuncIdx != INDEX_NONE) {
							EntryPointNames.Add(FuncIdx, MoveTemp(EntryName));
						}
					}
				}
			}
		}
	}

	struct FBranchInfo {
		FString Label;
		int32 InstructionIndex = INDEX_NONE;
		int32 FirstInstruction = INDEX_NONE;
		int32 LastInstruction = INDEX_NONE;
	};
	TArray<FBranchInfo> BranchInfos;

	if (ByteCodeStorageObj) {
		const TArray<TSharedPtr<FJsonValue>>* BranchInfosArray = nullptr;
		if ((*ByteCodeStorageObj)->TryGetArrayField(TEXT("BranchInfos"), BranchInfosArray)) {
			for (const TSharedPtr<FJsonValue>& Val : *BranchInfosArray) {
				const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
				if (!Obj.IsValid()) continue;
				FBranchInfo BI;
				BI.Label = Obj->GetStringField(TEXT("Label"));
				BI.InstructionIndex = static_cast<int32>(Obj->GetNumberField(TEXT("InstructionIndex")));
				BI.FirstInstruction = static_cast<int32>(Obj->GetNumberField(TEXT("FirstInstruction")));
				BI.LastInstruction = static_cast<int32>(Obj->GetNumberField(TEXT("LastInstruction")));
				BranchInfos.Add(MoveTemp(BI));
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Parsed %d BranchInfos"), BranchInfos.Num());
	for (const FBranchInfo& BI : BranchInfos) {
		UE_LOG(LogTemp, Log, TEXT("  Branch '%s' InstrIdx=%d Range=[%d,%d]"),
			*BI.Label, BI.InstructionIndex, BI.FirstInstruction, BI.LastInstruction);
	}

	URigVMBlueprint* RigBlueprint = Cast<URigVMBlueprint>(InControlRigBlueprint);
	if (!RigBlueprint) return;

	FRigVMClient* Client = RigBlueprint->GetRigVMClient();
	if (!Client) return;

	const TArray<TObjectPtr<URigVMGraph>>& Models = Client->GetModels();
	if (Models.Num() == 0) { UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: no existing graphs")); return; }
	URigVMGraph* Graph = Models[0];

	URigVMController* Controller = Client->GetOrCreateController(Graph);
	if (!Controller) return;

	if (!InstructionsArray || InstructionsArray->Num() == 0) {
		UE_LOG(LogTemp, Warning, TEXT("DeserializeGraph: no Instructions"));
		return;
	}

	struct FCreatedNodeInfo {
		URigVMNode* Node = nullptr;
		int32 InstructionIndex = INDEX_NONE;
		int32 FunctionIndex = INDEX_NONE;
		FString FullName;
	};
	TArray<FCreatedNodeInfo> CreatedNodes;

	float PosX = 0.0f;
	float PosY = 0.0f;
	int32 NodesCreated = 0;
	URigVMNode* PrevExecNode = nullptr;

	for (int32 i = 0; i < InstructionsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[i]->AsObject();
		if (!InstrObj.IsValid()) continue;

		int32 OpCode = 0;
		InstrObj->TryGetNumberField(TEXT("OpCode"), OpCode);
		if (OpCode != 101) continue;

		int32 FunctionIndex = 0;
		InstrObj->TryGetNumberField(TEXT("FunctionIndex"), FunctionIndex);
		if (FunctionIndex < 0 || FunctionIndex >= FunctionNames.Num()) continue;

		const FString& FullName = FunctionNames[FunctionIndex];

		FString TypeName;
		int32 DoubleColon = FullName.Find(TEXT("::"));
		if (DoubleColon != INDEX_NONE) {
			TypeName = FullName.Left(DoubleColon);
		} else {
			TypeName = FullName;
		}

		bool bIsEntry = (TypeName == TEXT("FRigUnit_BeginExecution") || TypeName == TEXT("FRigUnit_InverseExecution") || TypeName == TEXT("FRigUnit_PrepareForExecution"));
		bool bIsBranch = TypeName.Contains(TEXT("ControlFlowBranch"));
		if (bIsEntry) {
			PrevExecNode = nullptr;
		}

		URigVMNode* NewNode = nullptr;

		if (TypeName.StartsWith(TEXT("DISPATCH_"))) {
			FString TemplateNotation = TypeName;
			FString TypeArgs;
			if (DoubleColon != INDEX_NONE) {
				TypeArgs = FullName.Mid(DoubleColon + 2);
				TemplateNotation += TEXT("(") + TypeArgs + TEXT(")");
			}
			NewNode = Controller->AddTemplateNode(
				FName(*TemplateNotation),
				FVector2D(PosX, PosY),
				FString(),
				false,
				false
			);

			/* Resolve wildcard pins using type args from the notation string.
			   AddTemplateNode calls FullyResolve with empty Types, so permutation
			   is wrong. Parse type args and resolve each wildcard pin immediately. */
			if (NewNode && !TypeArgs.IsEmpty()) {
				URigVMTemplateNode* TNode = Cast<URigVMTemplateNode>(NewNode);
				if (TNode && TNode->HasWildCardPin()) {
					/* Parse "Name:Type,Name:Type,..." from the notation args */
					TMap<FString, FString> ArgMap;
					FString Remaining = TypeArgs;
					while (!Remaining.IsEmpty()) {
						int32 CommaIdx = Remaining.Find(TEXT(","));
						FString Pair = (CommaIdx != INDEX_NONE) ? Remaining.Left(CommaIdx) : Remaining;
						if (CommaIdx != INDEX_NONE) Remaining = Remaining.Mid(CommaIdx + 1);
						else Remaining.Empty();

						int32 ColonIdx = Pair.Find(TEXT(":"));
						if (ColonIdx != INDEX_NONE) {
							ArgMap.Add(Pair.Left(ColonIdx), Pair.Mid(ColonIdx + 1));
						}
					}

					/* Resolve each wildcard input pin using the parsed type */
					for (URigVMPin* Pin : TNode->GetPins()) {
						if (!Pin || !Pin->IsWildCard()) continue;
						if (Pin->GetDirection() != ERigVMPinDirection::Input) continue;

						const FString* FoundType = ArgMap.Find(Pin->GetName());
						if (FoundType) {
							if (Controller->ResolveWildCardPin(Pin->GetPinPath(), *FoundType, NAME_None, false, false)) {
								UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Resolved wildcard pin '%s' on '%s' to '%s'"),
									*Pin->GetName(), *NewNode->GetName(), **FoundType);
							} else {
								UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Failed to resolve wildcard pin '%s' on '%s' to '%s'"),
									*Pin->GetName(), *NewNode->GetName(), **FoundType);
							}
						}
					}
				}
			}
		}
		else {
			FString CPPTypeName = TypeName;
			if (CPPTypeName.StartsWith(TEXT("F"))) {
				CPPTypeName.RemoveAt(0);
			}

			FString MethodName = TEXT("Execute");
			if (DoubleColon != INDEX_NONE) {
				MethodName = FullName.Mid(DoubleColon + 2);
				int32 ColonIdx = MethodName.Find(TEXT(":"));
				if (ColonIdx != INDEX_NONE) {
					MethodName = MethodName.Left(ColonIdx);
				}
			}

			UScriptStruct* ScriptStruct = FindFirstObject<UScriptStruct>(*CPPTypeName, EFindFirstObjectOptions::NativeFirst);

			// For entry point nodes (Forwards Solve, Construction Event, etc.), find the existing
			// pre-created node rather than trying to create a duplicate.
			if (bIsEntry && ScriptStruct) {
				const FName EventName = ScriptStruct->GetFName();
				for (const TObjectPtr<URigVMNode>& Node : Graph->GetNodes()) {
					if (Node && Node->IsEvent()) {
						URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node);
						if (UnitNode && UnitNode->GetScriptStruct() == ScriptStruct) {
							NewNode = Node;
							UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Reusing existing entry node '%s' for '%s'"), *Node->GetName(), *FullName);
							break;
						}
					}
				}
			}

			if (!NewNode && ScriptStruct) {
				NewNode = Controller->AddUnitNode(
					ScriptStruct,
					FName(*MethodName),
					FVector2D(PosX, PosY),
					FString(),
					false,
					false
				);
			}

			if (!NewNode) {
				static const TCHAR* Modules[] = { TEXT("ControlRig"), TEXT("RigVM") };
				for (const TCHAR* Mod : Modules) {
					NewNode = Controller->AddUnitNodeFromStructPath(
						FString::Printf(TEXT("/Script/%s.%s"), Mod, *CPPTypeName),
						FName(*MethodName),
						FVector2D(PosX, PosY),
						FString(),
						false,
						false
					);
					if (NewNode) break;
				}
			}
		}

		if (NewNode) {
			TArray<URigVMPin*> NewPins = NewNode->GetPins();
			URigVMPin* NewExecInputPin = nullptr;
			URigVMPin* NewExecOutputPin = nullptr;

			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Node '%s' pins:"), *NewNode->GetName());
			for (URigVMPin* Pin : NewPins) {
				if (!Pin) continue;
				UE_LOG(LogTemp, Log, TEXT("  Pin '%s' CPPType='%s' Dir=%d"), *Pin->GetName(), *Pin->GetCPPType(), (int32)Pin->GetDirection());
				if (Pin->GetCPPType().Contains(TEXT("Execute"))) {
					ERigVMPinDirection Dir = Pin->GetDirection();
					if ((Dir == ERigVMPinDirection::Input || Dir == ERigVMPinDirection::IO) && !NewExecInputPin) {
						NewExecInputPin = Pin;
					}
					if ((Dir == ERigVMPinDirection::Output || Dir == ERigVMPinDirection::IO) && !NewExecOutputPin) {
						NewExecOutputPin = Pin;
					}
				}
			}

			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: PrevExecNode=%s NewExecInputPin=%s NewExecOutputPin=%s"),
				PrevExecNode ? *PrevExecNode->GetName() : TEXT("null"),
				NewExecInputPin ? *NewExecInputPin->GetName() : TEXT("null"),
				NewExecOutputPin ? *NewExecOutputPin->GetName() : TEXT("null"));

			if (PrevExecNode && NewExecInputPin) {
				TArray<URigVMPin*> PrevPins = PrevExecNode->GetPins();
				URigVMPin* PrevExecPin = nullptr;

				URigVMPin* CompletedPin = nullptr;
				URigVMPin* FirstOutputExecPin = nullptr;
				for (URigVMPin* Pin : PrevPins) {
					if (Pin && Pin->GetCPPType().Contains(TEXT("Execute")) && Pin->GetDirection() != ERigVMPinDirection::Input) {
						if (!FirstOutputExecPin) FirstOutputExecPin = Pin;
						if (Pin->GetName() == TEXT("Completed")) {
							CompletedPin = Pin;
							break;
						}
					}
				}
				PrevExecPin = CompletedPin ? CompletedPin : FirstOutputExecPin;
				if (PrevExecPin && PrevExecPin != NewExecInputPin) {
					FString FailureReason;
					if (Controller->AddLink(PrevExecPin, NewExecInputPin, false, ERigVMPinDirection::Invalid, false, false, &FailureReason)) {
						UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Linked exec %s -> %s"), *PrevExecNode->GetNodePath(), *NewNode->GetNodePath());
					} else {
						UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Exec link failed %s -> %s: %s"), *PrevExecNode->GetNodePath(), *NewNode->GetNodePath(), *FailureReason);
					}
				}
			}

			if ((NewExecOutputPin || NewExecInputPin) && !bIsBranch) {
				PrevExecNode = NewNode;
			} else if (bIsBranch) {
				PrevExecNode = nullptr;
			}

			FCreatedNodeInfo Info;
			Info.Node = NewNode;
			Info.InstructionIndex = i;
			Info.FunctionIndex = FunctionIndex;
			Info.FullName = FullName;
			CreatedNodes.Add(MoveTemp(Info));

			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Created node '%s' for '%s'"), *FullName, *InControlRigBlueprint->GetName());
			NodesCreated++;
		} else {
			UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Failed to create node for '%s'"), *FullName);
		}

		PosX += 300.0f;
		if ((NodesCreated) % 5 == 0) {
			PosX = 0.0f;
			PosY += 300.0f;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 1 complete -- %d nodes created for '%s'"), NodesCreated, *InControlRigBlueprint->GetName());

	/* Phase 1.1: Fix Branch exec routing using BranchInfos */
	if (BranchInfos.Num() > 0) {
		int32 BranchFixes = 0;

		for (const FCreatedNodeInfo& Info : CreatedNodes) {
			URigVMNode* Node = Info.Node;
			if (!Node) continue;

			TArray<URigVMPin*> Pins = Node->GetPins();
			URigVMPin* BranchTruePin = nullptr;
			URigVMPin* BranchFalsePin = nullptr;
			URigVMPin* BranchCompletedPin = nullptr;
			URigVMPin* BranchExecutePin = nullptr;

			for (URigVMPin* Pin : Pins) {
				if (!Pin || !Pin->GetCPPType().Contains(TEXT("Execute"))) continue;
				if (Pin->GetDirection() == ERigVMPinDirection::Input || Pin->GetDirection() == ERigVMPinDirection::IO) {
					if (Pin->GetName() == TEXT("ExecuteContext") || Pin->GetName() == TEXT("Execute")) {
						BranchExecutePin = Pin;
					}
				}
				if (Pin->GetName() == TEXT("True")) BranchTruePin = Pin;
				else if (Pin->GetName() == TEXT("False")) BranchFalsePin = Pin;
				else if (Pin->GetName() == TEXT("Completed")) BranchCompletedPin = Pin;
			}

			if (!BranchTruePin || !BranchFalsePin || !BranchCompletedPin) continue;

			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Found Branch node '%s' -- fixing exec routing"), *Node->GetName());

			auto FindAllExecNodesInRange = [&](int32 FirstIdx, int32 LastIdx) -> TArray<URigVMNode*> {
				TArray<URigVMNode*> Result;
				TArray<int32> Indices;
				for (const FCreatedNodeInfo& CI : CreatedNodes) {
					if (CI.InstructionIndex < FirstIdx || CI.InstructionIndex > LastIdx) continue;
					URigVMNode* N = CI.Node;
					if (!N) continue;
					bool bHasExecPin = false;
					for (URigVMPin* P : N->GetPins()) {
						if (P && P->GetCPPType().Contains(TEXT("Execute"))) {
							bHasExecPin = true;
							break;
						}
					}
					if (!bHasExecPin) continue;
					Result.Add(N);
					Indices.Add(CI.InstructionIndex);
				}
				return Result;
			};

			auto FindFirstExecNodeInRange = [&](int32 FirstIdx, int32 LastIdx) -> URigVMNode* {
				URigVMNode* BestNode = nullptr;
				int32 BestDist = MAX_int32;
				for (const FCreatedNodeInfo& CI : CreatedNodes) {
					if (CI.InstructionIndex < FirstIdx || CI.InstructionIndex > LastIdx) continue;
					URigVMNode* N = CI.Node;
					if (!N) continue;
					bool bHasExecPin = false;
					for (URigVMPin* P : N->GetPins()) {
						if (P && P->GetCPPType().Contains(TEXT("Execute"))) {
							bHasExecPin = true;
							break;
						}
					}
					if (!bHasExecPin) continue;
					int32 Dist = CI.InstructionIndex - FirstIdx;
					if (Dist < BestDist) {
						BestDist = Dist;
						BestNode = N;
					}
				}
				return BestNode;
			};

			auto FindBranchInfo = [&](const FString& Label) -> const FBranchInfo* {
				for (const FBranchInfo& BI : BranchInfos) {
					if (BI.Label == Label) return &BI;
				}
				return nullptr;
			};

			const FBranchInfo* TrueBI = FindBranchInfo(TEXT("True"));
			const FBranchInfo* FalseBI = FindBranchInfo(TEXT("False"));
			const FBranchInfo* CompletedBI = FindBranchInfo(TEXT("Completed"));

			TArray<URigVMNode*> CompletedExecNodes;
			if (CompletedBI) {
				CompletedExecNodes = FindAllExecNodesInRange(CompletedBI->FirstInstruction, CompletedBI->LastInstruction);
			}

			TSet<URigVMPin*> ConnectedExecInputs;
			int32 CompletedFallbackIdx = 0;

			auto GetExecInput = [](URigVMNode* Node) -> URigVMPin* {
				if (!Node) return nullptr;
				for (URigVMPin* P : Node->GetPins()) {
					if (P && P->GetCPPType().Contains(TEXT("Execute")) &&
						(P->GetDirection() == ERigVMPinDirection::Input || P->GetDirection() == ERigVMPinDirection::IO)) {
						return P;
					}
				}
				return nullptr;
			};

			auto LinkBranchOutput = [&](URigVMPin* OutputPin, const FBranchInfo* BI, const TCHAR* Label) {
				if (!BI || !OutputPin) return;
				URigVMNode* TargetNode = FindFirstExecNodeInRange(BI->FirstInstruction, BI->LastInstruction);

				if (!TargetNode && CompletedExecNodes.Num() > 0) {
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Branch %s path has no exec nodes (range [%d,%d]), falling back to Completed chain"),
						Label, BI->FirstInstruction, BI->LastInstruction);
					while (CompletedFallbackIdx < CompletedExecNodes.Num()) {
						URigVMPin* CandidateInput = GetExecInput(CompletedExecNodes[CompletedFallbackIdx]);
						if (CandidateInput && !ConnectedExecInputs.Contains(CandidateInput)) {
							TargetNode = CompletedExecNodes[CompletedFallbackIdx];
							CompletedFallbackIdx++;
							break;
						}
						CompletedFallbackIdx++;
					}
				}

				if (!TargetNode) return;

				URigVMPin* TargetExecInput = GetExecInput(TargetNode);
				if (!TargetExecInput) return;

				if (ConnectedExecInputs.Contains(TargetExecInput)) return;

				TArray<URigVMLink*> SourceLinks = TargetExecInput->GetSourceLinks();
				for (URigVMLink* Link : SourceLinks) {
					if (Link) {
						URigVMPin* SrcPin = Link->GetSourcePin();
						if (SrcPin) {
							Controller->BreakLink(SrcPin, TargetExecInput);
						}
					}
				}

				FString FailureReason;
				if (Controller->AddLink(OutputPin, TargetExecInput, false, ERigVMPinDirection::Invalid, false, false, &FailureReason)) {
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Linked Branch %s -> %s"), Label, *TargetNode->GetName());
					ConnectedExecInputs.Add(TargetExecInput);
					BranchFixes++;
				} else {
					UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Branch %s link failed -> %s: %s"), Label, *TargetNode->GetName(), *FailureReason);
				}
			};

			LinkBranchOutput(BranchTruePin, TrueBI, TEXT("True"));
			LinkBranchOutput(BranchFalsePin, FalseBI, TEXT("False"));
			LinkBranchOutput(BranchCompletedPin, CompletedBI, TEXT("Completed"));
		}

		UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 1.1 complete -- %d Branch links created"), BranchFixes);
	}

	/* Phase 1.5: Set pin defaults from LiteralMemory constants */
	{
		auto GetNonExecPinsLocal = [](URigVMNode* Node) -> TArray<URigVMPin*> {
			TArray<URigVMPin*> Result;
			if (!Node) return Result;
			for (URigVMPin* Pin : Node->GetPins()) {
				if (Pin && !Pin->GetCPPType().Contains(TEXT("Execute"))) {
					Result.Add(Pin);
				}
			}
			return Result;
		};

		int32 LiteralDefaultsSet = 0;
		for (int32 i = 0; i < InstructionsArray->Num(); ++i) {
			const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[i]->AsObject();
			if (!InstrObj.IsValid()) continue;

			int32 OpCode = 0;
			InstrObj->TryGetNumberField(TEXT("OpCode"), OpCode);
			if (OpCode != 101) continue;

			URigVMNode* Node = nullptr;
			for (const FCreatedNodeInfo& CI : CreatedNodes) {
				if (CI.InstructionIndex == i) {
					Node = CI.Node;
					break;
				}
			}
			if (!Node) continue;

			TArray<URigVMPin*> DataPins = GetNonExecPinsLocal(Node);

			const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
			InstrObj->TryGetArrayField(TEXT("Arguments"), ArgsArray);
			if (!ArgsArray) continue;

			for (int32 a = 0; a < ArgsArray->Num(); ++a) {
				if (a >= DataPins.Num()) break;

				const TSharedPtr<FJsonObject>& ArgObj = (*ArgsArray)[a]->AsObject();
				if (!ArgObj.IsValid()) continue;

				int32 MemType = 0, RegIdx = 0;
				ArgObj->TryGetNumberField(TEXT("MemoryType"), MemType);
				ArgObj->TryGetNumberField(TEXT("RegisterIndex"), RegIdx);

				if (MemType != 1) continue;

				const FString* ValuePtr = LiteralRegisterValues.Find(RegIdx);
				if (!ValuePtr || ValuePtr->IsEmpty()) continue;

				const FString& DefaultValue = *ValuePtr;
				URigVMPin* DestPin = DataPins[a];
				if (!DestPin) continue;

				if (DestPin->GetCPPType() == TEXT("FRigElementKey")) {
					TArray<URigVMPin*> SubPins = DestPin->GetSubPins();
					for (URigVMPin* SubPin : SubPins) {
						if (SubPin && SubPin->GetName() == TEXT("Type")) {
							int32 TypeStart = DefaultValue.Find(TEXT("Type="));
							if (TypeStart != INDEX_NONE) {
								TypeStart += 5;
								int32 TypeEnd = DefaultValue.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, TypeStart);
								if (TypeEnd != INDEX_NONE) {
									FString TypeStr = DefaultValue.Mid(TypeStart, TypeEnd - TypeStart);
									FString PinPath = SubPin->GetPinPath();
									Controller->SetPinDefaultValue(PinPath, TypeStr, false, false);
								}
							}
						} else if (SubPin && SubPin->GetName() == TEXT("Name")) {
							int32 NameStart = DefaultValue.Find(TEXT("Name=\""));
							if (NameStart != INDEX_NONE) {
								NameStart += 6;
								int32 NameEnd = DefaultValue.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
								if (NameEnd != INDEX_NONE) {
									FString NameStr = DefaultValue.Mid(NameStart, NameEnd - NameStart);
									FString PinPath = SubPin->GetPinPath();
									Controller->SetPinDefaultValue(PinPath, NameStr, false, false);
								}
							}
						}
					}
				} else {
					FString PinPath = DestPin->GetPinPath();
					Controller->SetPinDefaultValue(PinPath, DefaultValue, false, false);
				}

				UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Set literal default %s.%s = '%s' (Reg[%d])"),
					*Node->GetName(), *DestPin->GetName(), *DefaultValue, RegIdx);
				LiteralDefaultsSet++;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 1.5 complete -- %d literal defaults set"), LiteralDefaultsSet);
	}

	/* Phase 1.6: Set pin defaults from WorkMemory initial values */
	{
		auto GetNonExecPinsLocal = [](URigVMNode* Node) -> TArray<URigVMPin*> {
			TArray<URigVMPin*> Result;
			if (!Node) return Result;
			for (URigVMPin* Pin : Node->GetPins()) {
				if (Pin && !Pin->GetCPPType().Contains(TEXT("Execute"))) {
					Result.Add(Pin);
				}
			}
			return Result;
		};

		int32 WorkDefaultsSet = 0;
		for (int32 i = 0; i < InstructionsArray->Num(); ++i) {
			const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[i]->AsObject();
			if (!InstrObj.IsValid()) continue;

			int32 OpCode = 0;
			InstrObj->TryGetNumberField(TEXT("OpCode"), OpCode);
			if (OpCode != 101) continue;

			URigVMNode* Node = nullptr;
			for (const FCreatedNodeInfo& CI : CreatedNodes) {
				if (CI.InstructionIndex == i) {
					Node = CI.Node;
					break;
				}
			}
			if (!Node) continue;

			TArray<URigVMPin*> DataPins = GetNonExecPinsLocal(Node);

			const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
			InstrObj->TryGetArrayField(TEXT("Arguments"), ArgsArray);
			if (!ArgsArray) continue;

			for (int32 a = 0; a < ArgsArray->Num(); ++a) {
				if (a >= DataPins.Num()) break;

				const TSharedPtr<FJsonObject>& ArgObj = (*ArgsArray)[a]->AsObject();
				if (!ArgObj.IsValid()) continue;

				int32 MemType = 0, RegIdx = 0;
				ArgObj->TryGetNumberField(TEXT("MemoryType"), MemType);
				ArgObj->TryGetNumberField(TEXT("RegisterIndex"), RegIdx);

				if (MemType != 0) continue;

				const FString* ValuePtr = WorkRegisterValues.Find(RegIdx);
				if (!ValuePtr || ValuePtr->IsEmpty()) continue;

				const FString& DefaultValue = *ValuePtr;
				URigVMPin* DestPin = DataPins[a];
				if (!DestPin) continue;

				if (DestPin->GetCPPType() == TEXT("FRigElementKey")) {
					TArray<URigVMPin*> SubPins = DestPin->GetSubPins();
					for (URigVMPin* SubPin : SubPins) {
						if (SubPin && SubPin->GetName() == TEXT("Type")) {
							int32 TypeStart = DefaultValue.Find(TEXT("Type="));
							if (TypeStart != INDEX_NONE) {
								TypeStart += 5;
								int32 TypeEnd = DefaultValue.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, TypeStart);
								if (TypeEnd != INDEX_NONE) {
									FString TypeStr = DefaultValue.Mid(TypeStart, TypeEnd - TypeStart);
									FString PinPath = SubPin->GetPinPath();
									Controller->SetPinDefaultValue(PinPath, TypeStr, false, false);
								}
							}
						} else if (SubPin && SubPin->GetName() == TEXT("Name")) {
							int32 NameStart = DefaultValue.Find(TEXT("Name=\""));
							if (NameStart != INDEX_NONE) {
								NameStart += 6;
								int32 NameEnd = DefaultValue.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameStart);
								if (NameEnd != INDEX_NONE) {
									FString NameStr = DefaultValue.Mid(NameStart, NameEnd - NameStart);
									FString PinPath = SubPin->GetPinPath();
									Controller->SetPinDefaultValue(PinPath, NameStr, false, false);
								}
							}
						}
					}
				} else {
					FString PinPath = DestPin->GetPinPath();
					Controller->SetPinDefaultValue(PinPath, DefaultValue, false, false);
				}

				UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Set work default %s.%s = '%s' (Reg[%d])"),
					*Node->GetName(), *DestPin->GetName(), *DefaultValue, RegIdx);
				WorkDefaultsSet++;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 1.6 complete -- %d work defaults set"), WorkDefaultsSet);
	}

	/* Phase 2: Build register writer map */
	struct FSourcePin {
		URigVMNode* Node = nullptr;
		URigVMPin* Pin = nullptr;
		FString SrcSubPinPath;
		FString DstSubPinPath;
	};
	TArray<FSourcePin> WorkRegisterSources;
	WorkRegisterSources.SetNum(WorkProperties.Num());
	TArray<TArray<FSourcePin>> WorkRegisterSubPinSources;
	WorkRegisterSubPinSources.SetNum(WorkProperties.Num());

	auto GetNonExecPins = [](URigVMNode* Node) -> TArray<URigVMPin*> {
		TArray<URigVMPin*> Result;
		if (!Node) return Result;
		for (URigVMPin* Pin : Node->GetPins()) {
			if (Pin && !Pin->GetCPPType().Contains(TEXT("Execute"))) {
				if (Pin->IsArray()) {
					TArray<URigVMPin*> SubPins = Pin->GetSubPins();
					if (SubPins.Num() > 0) {
						for (URigVMPin* Sub : SubPins) {
							if (Sub) Result.Add(Sub);
						}
					} else {
						Result.Add(Pin);
					}
				} else {
					Result.Add(Pin);
				}
			}
		}
		return Result;
	};

	for (int32 i = 0; i < InstructionsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[i]->AsObject();
		if (!InstrObj.IsValid()) continue;

		int32 OpCode = 0;
		InstrObj->TryGetNumberField(TEXT("OpCode"), OpCode);
		if (OpCode != 101) continue;

		URigVMNode* Node = nullptr;
		for (const FCreatedNodeInfo& CI : CreatedNodes) {
			if (CI.InstructionIndex == i) {
				Node = CI.Node;
				break;
			}
		}
		if (!Node) continue;

		TArray<URigVMPin*> DataPins = GetNonExecPins(Node);

		const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
		InstrObj->TryGetArrayField(TEXT("Arguments"), ArgsArray);
		if (!ArgsArray) continue;

		for (int32 a = 0; a < ArgsArray->Num(); ++a) {
			const TSharedPtr<FJsonObject>& ArgObj = (*ArgsArray)[a]->AsObject();
			if (!ArgObj.IsValid()) continue;

			int32 MemType = 0, RegIdx = 0;
			ArgObj->TryGetNumberField(TEXT("MemoryType"), MemType);
			ArgObj->TryGetNumberField(TEXT("RegisterIndex"), RegIdx);
			if (MemType != 0) continue;
			if (RegIdx < 0 || RegIdx >= WorkRegisterSources.Num()) continue;

			URigVMPin* Pin = nullptr;
			if (a < DataPins.Num()) {
				Pin = DataPins[a];
			} else if (DataPins.Num() > 0) {
				// Bytecode has more args than data pins: this is the output side of an IO pin.
				// Find the IO pin that needs its writer registered.
				for (int32 p = DataPins.Num() - 1; p >= 0; --p) {
					URigVMPin* P = DataPins[p];
					if (P && P->GetDirection() == ERigVMPinDirection::IO) {
						Pin = P;
						break;
					}
				}
			}

			if (Pin && (Pin->GetDirection() == ERigVMPinDirection::Output || Pin->GetDirection() == ERigVMPinDirection::IO)) {
				WorkRegisterSources[RegIdx].Node = Node;
				WorkRegisterSources[RegIdx].Pin = Pin;
			}
		}
	}

	/* Phase 2.5: Propagate register sources through Copy(68) instructions */
	int32 CopiesPropagated = 0;
	int32 OffsetCopiesResolved = 0;

	// Track external (MemType=2) bridge copies for cross-branch data flow.
	// The compiler inserts work→external→work copies to pass data between exec branches.
	TMap<int32, FSourcePin> ExtBridgeSources;

	for (int32 i = 0; i < InstructionsArray->Num(); ++i) {
		const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[i]->AsObject();
		if (!InstrObj.IsValid()) continue;

		int32 OpCode = 0;
		InstrObj->TryGetNumberField(TEXT("OpCode"), OpCode);
		if (OpCode != 68) continue;

		const TSharedPtr<FJsonObject>* SourceObj = nullptr;
		const TSharedPtr<FJsonObject>* TargetObj = nullptr;
		if (!InstrObj->TryGetObjectField(TEXT("Source"), SourceObj)) continue;
		if (!InstrObj->TryGetObjectField(TEXT("Target"), TargetObj)) continue;

		int32 SrcMemType = 0, SrcRegIdx = 0, SrcOffset = 65535;
		int32 TgtMemType = 0, TgtRegIdx = 0, TgtOffset = 65535;
		(*SourceObj)->TryGetNumberField(TEXT("MemoryType"), SrcMemType);
		(*SourceObj)->TryGetNumberField(TEXT("RegisterIndex"), SrcRegIdx);
		(*SourceObj)->TryGetNumberField(TEXT("RegisterOffset"), SrcOffset);
		(*TargetObj)->TryGetNumberField(TEXT("MemoryType"), TgtMemType);
		(*TargetObj)->TryGetNumberField(TEXT("RegisterIndex"), TgtRegIdx);
		(*TargetObj)->TryGetNumberField(TEXT("RegisterOffset"), TgtOffset);

		// Handle work→external bridge: record the source for later propagation
		if (SrcMemType == 0 && TgtMemType == 2) {
			if (SrcRegIdx >= 0 && SrcRegIdx < WorkRegisterSources.Num() && TgtRegIdx >= 0) {
				const FSourcePin& Src = WorkRegisterSources[SrcRegIdx];
				if (Src.Node && Src.Pin) {
					ExtBridgeSources.Add(TgtRegIdx, Src);
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Bridge source recorded WorkReg[%d] -> ExtReg[%d] (source: %s.%s)"),
						SrcRegIdx, TgtRegIdx, *Src.Node->GetName(), *Src.Pin->GetName());
				}
			}
			continue;
		}

		// Handle external→work bridge: propagate the source through
		if (SrcMemType == 2 && TgtMemType == 0) {
			if (TgtRegIdx >= 0 && TgtRegIdx < WorkRegisterSources.Num()) {
				const FSourcePin* BridgeSrc = ExtBridgeSources.Find(SrcRegIdx);
				if (BridgeSrc && BridgeSrc->Node && BridgeSrc->Pin) {
					WorkRegisterSources[TgtRegIdx] = *BridgeSrc;
					CopiesPropagated++;
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Bridge propagated ExtReg[%d] -> WorkReg[%d] (source: %s.%s)"),
						SrcRegIdx, TgtRegIdx, *BridgeSrc->Node->GetName(), *BridgeSrc->Pin->GetName());
				}
			}
			continue;
		}

		// Skip all non-work→work copies (literal→work, literal→literal, external→external, etc.)
		if (SrcMemType != 0 || TgtMemType != 0) continue;
		if (SrcRegIdx < 0 || SrcRegIdx >= WorkRegisterSources.Num()) continue;
		if (TgtRegIdx < 0 || TgtRegIdx >= WorkRegisterSources.Num()) continue;

		const FSourcePin& Src = WorkRegisterSources[SrcRegIdx];

		// Case A: Source has a full writer — propagate normally
		if (Src.Node && Src.Pin) {
			if (SrcOffset == 65535 && TgtOffset == 65535) {
				WorkRegisterSources[TgtRegIdx] = Src;
				CopiesPropagated++;

				UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Copy propagated WorkReg[%d] -> WorkReg[%d] (source: %s.%s)"),
					SrcRegIdx, TgtRegIdx, *Src.Node->GetName(), *Src.Pin->GetName());
			} else {
				FString SrcSubPinName;
				FString TgtSubPinName;

				if (SrcOffset != 65535 && OffsetToSegmentPath.IsValidIndex(SrcOffset)) {
					SrcSubPinName = OffsetToSegmentPath[SrcOffset];
				}
				if (TgtOffset != 65535 && OffsetToSegmentPath.IsValidIndex(TgtOffset)) {
					TgtSubPinName = OffsetToSegmentPath[TgtOffset];
				}

				if (!SrcSubPinName.IsEmpty() || !TgtSubPinName.IsEmpty()) {
					FSourcePin SubSource;
					SubSource.Node = Src.Node;
					SubSource.Pin = Src.Pin;
					SubSource.SrcSubPinPath = SrcSubPinName;
					SubSource.DstSubPinPath = TgtSubPinName;
					WorkRegisterSubPinSources[TgtRegIdx].Add(MoveTemp(SubSource));
					OffsetCopiesResolved++;

					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Offset copy WorkReg[%d](off=%d=%s) -> WorkReg[%d](off=%d=%s) resolved src-sub '%s' dst-sub '%s' on %s.%s"),
						SrcRegIdx, SrcOffset, *SrcSubPinName, TgtRegIdx, TgtOffset, *TgtSubPinName,
						*SubSource.SrcSubPinPath, *SubSource.DstSubPinPath,
						*Src.Node->GetName(), *Src.Pin->GetName());
				} else {
					UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Offset copy WorkReg[%d](off=%d) -> WorkReg[%d](off=%d) no PropertyPathDescription found"),
						SrcRegIdx, SrcOffset, TgtRegIdx, TgtOffset);
				}
			}
		}
		// Case B: Source has NO full writer but HAS sub-pin sources (implicit MakeStruct pattern).
		// The bytecodes build a struct from sub-register copies into WorkReg[N] (Make_Elements__IO),
		// then copy the full struct to WorkReg[M] (Make_Struct). Propagate the sub-pin sources.
		else if (WorkRegisterSubPinSources[SrcRegIdx].Num() > 0 && SrcOffset == 65535 && TgtOffset == 65535) {
			for (const FSourcePin& SubSrc : WorkRegisterSubPinSources[SrcRegIdx]) {
				FSourcePin Propagated = SubSrc;
				WorkRegisterSubPinSources[TgtRegIdx].Add(Propagated);
			}
			CopiesPropagated++;

			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Sub-pin sources propagated WorkReg[%d] -> WorkReg[%d] (MakeStruct implicit, %d sub-pins)"),
				SrcRegIdx, TgtRegIdx, WorkRegisterSubPinSources[SrcRegIdx].Num());
		}
		// Case C: Source has nothing — skip
		else {
			continue;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 2.5 complete -- %d full copies propagated, %d offset copies resolved"), CopiesPropagated, OffsetCopiesResolved);

	for (int32 RegIdx = 0; RegIdx < WorkRegisterSources.Num(); ++RegIdx) {
		const FSourcePin& Src = WorkRegisterSources[RegIdx];
		if (Src.Node && Src.Pin) {
			UE_LOG(LogTemp, Log, TEXT("  WorkReg[%d] writer: %s.%s"), RegIdx,
				*Src.Node->GetName(), *Src.Pin->GetName());
		} else {
			UE_LOG(LogTemp, Log, TEXT("  WorkReg[%d] writer: (none)"), RegIdx);
		}
		for (const FSourcePin& SubSrc : WorkRegisterSubPinSources[RegIdx]) {
			UE_LOG(LogTemp, Log, TEXT("  WorkReg[%d] sub-pin source: %s.%s src-sub='%s' dst-sub='%s'"), RegIdx,
				*SubSrc.Node->GetName(), *SubSrc.Pin->GetName(), *SubSrc.SrcSubPinPath, *SubSrc.DstSubPinPath);
		}
	}

	/* Phase 2.6: Propagate sub-pin sources from _Elements__IO to _Struct registers.
	 * The RigVM compiler builds structs via offset copies into _Elements__IO registers,
	 * then reads the assembled struct from the corresponding _Struct register. There is
	 * no explicit copy between them — the connection is implicit. We detect the pattern
	 * by name and copy the sub-pin sources. */
	int32 MakeStructPropagated = 0;
	for (int32 RegIdx = 0; RegIdx < WorkProperties.Num(); ++RegIdx) {
		if (WorkRegisterSources[RegIdx].Node) continue;
		if (WorkRegisterSubPinSources[RegIdx].Num() > 0) continue;

		FString PropName = WorkProperties[RegIdx].PropertyName;
		int32 StructIdx = PropName.Find(TEXT("_Struct"));
		if (StructIdx == INDEX_NONE) continue;

		FString ElementsName = PropName.Left(StructIdx) + TEXT("_Elements__IO");

		for (int32 SrcRegIdx = 0; SrcRegIdx < WorkProperties.Num(); ++SrcRegIdx) {
			if (WorkProperties[SrcRegIdx].PropertyName == ElementsName) {
				if (WorkRegisterSubPinSources[SrcRegIdx].Num() == 0) continue;
				for (const FSourcePin& SubSrc : WorkRegisterSubPinSources[SrcRegIdx]) {
					WorkRegisterSubPinSources[RegIdx].Add(SubSrc);
				}
				MakeStructPropagated++;
				UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: MakeStruct implicit WorkReg[%d] '%s' <- WorkReg[%d] '%s' (%d sub-pins)"),
					RegIdx, *PropName, SrcRegIdx, *ElementsName, WorkRegisterSubPinSources[SrcRegIdx].Num());
				break;
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 2.6 complete -- %d MakeStruct implicit propagations"), MakeStructPropagated);

	/* Phase 3: Create data pin links */
	int32 DataLinksCreated = 0;

	auto TryLinkPins = [&](URigVMNode* SrcNode, URigVMPin* SrcPin, URigVMNode* DstNode, URigVMPin* DstPin) -> bool {
		if (!SrcPin || !DstPin) return false;
		if (SrcNode == DstNode && SrcPin == DstPin) return false;

		FString FailureReason;
		if (Controller->AddLink(SrcPin, DstPin, false, ERigVMPinDirection::Invalid, false, false, &FailureReason)) {
			UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Linked data %s.%s -> %s.%s"),
				*SrcNode->GetName(), *SrcPin->GetName(),
				*DstNode->GetName(), *DstPin->GetName());
			DataLinksCreated++;
			return true;
		}
		UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Data link failed %s.%s -> %s.%s: %s"),
			*SrcNode->GetName(), *SrcPin->GetName(),
			*DstNode->GetName(), *DstPin->GetName(), *FailureReason);
		return false;
	};

	for (const FCreatedNodeInfo& Info : CreatedNodes) {
		URigVMNode* Node = Info.Node;
		const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[Info.InstructionIndex]->AsObject();
		if (!InstrObj.IsValid()) continue;

		TArray<URigVMPin*> DataPins = GetNonExecPins(Node);

		const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
		InstrObj->TryGetArrayField(TEXT("Arguments"), ArgsArray);
		if (!ArgsArray) continue;

		for (int32 a = 0; a < ArgsArray->Num(); ++a) {
			if (a >= DataPins.Num()) break;

			const TSharedPtr<FJsonObject>& ArgObj = (*ArgsArray)[a]->AsObject();
			if (!ArgObj.IsValid()) continue;

			URigVMPin* DestPin = DataPins[a];
			if (!DestPin) continue;
			ERigVMPinDirection DestDir = DestPin->GetDirection();
			if (DestDir != ERigVMPinDirection::Input) continue;

			int32 MemType = 0, RegIdx = 0;
			ArgObj->TryGetNumberField(TEXT("MemoryType"), MemType);
			ArgObj->TryGetNumberField(TEXT("RegisterIndex"), RegIdx);

			if (MemType != 0) continue;
			if (RegIdx < 0 || RegIdx >= WorkRegisterSources.Num()) continue;

			const TArray<FSourcePin>& SubSources = WorkRegisterSubPinSources[RegIdx];
			if (SubSources.Num() > 0) {
				for (const FSourcePin& SubSrc : SubSources) {
					if (!SubSrc.Pin || !SubSrc.Node) continue;

					URigVMPin* SrcPin = SubSrc.Pin;
					if (!SubSrc.SrcSubPinPath.IsEmpty()) {
						SrcPin = SubSrc.Pin->FindSubPin(SubSrc.SrcSubPinPath);
						if (!SrcPin) {
							UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Could not find source sub-pin '%s' on %s.%s"),
								*SubSrc.SrcSubPinPath, *SubSrc.Node->GetName(), *SubSrc.Pin->GetName());
							continue;
						}
					}

					URigVMPin* DstPin = DestPin;
					if (!SubSrc.DstSubPinPath.IsEmpty()) {
						DstPin = DestPin->FindSubPin(SubSrc.DstSubPinPath);
						if (!DstPin) {
							UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Could not find dest sub-pin '%s' on %s.%s"),
								*SubSrc.DstSubPinPath, *Node->GetName(), *DestPin->GetName());
							continue;
						}
					}

					TryLinkPins(SubSrc.Node, SrcPin, Node, DstPin);
				}
			} else {
				const FSourcePin& Src = WorkRegisterSources[RegIdx];
				if (!Src.Pin || !Src.Node) continue;
				TryLinkPins(Src.Node, Src.Pin, Node, DestPin);
			}
		}
	}

	/* Phase 4: Create variable getter nodes for External (ChildProperties) and link to input pins */
	int32 ExtLinksCreated = 0;
	float ExtPosY = PosY + 300.0f;

	for (const FCreatedNodeInfo& Info : CreatedNodes) {
		URigVMNode* Node = Info.Node;
		const TSharedPtr<FJsonObject>& InstrObj = (*InstructionsArray)[Info.InstructionIndex]->AsObject();
		if (!InstrObj.IsValid()) continue;

		TArray<URigVMPin*> DataPins = GetNonExecPins(Node);

		const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
		InstrObj->TryGetArrayField(TEXT("Arguments"), ArgsArray);
		if (!ArgsArray) continue;

		for (int32 a = 0; a < ArgsArray->Num(); ++a) {
			if (a >= DataPins.Num()) break;

			const TSharedPtr<FJsonObject>& ArgObj = (*ArgsArray)[a]->AsObject();
			if (!ArgObj.IsValid()) continue;

			URigVMPin* DestPin = DataPins[a];
			if (!DestPin) continue;
			if (DestPin->GetDirection() != ERigVMPinDirection::Input) continue;

			int32 MemType = 0, RegIdx = 0;
			ArgObj->TryGetNumberField(TEXT("MemoryType"), MemType);
			ArgObj->TryGetNumberField(TEXT("RegisterIndex"), RegIdx);

			if (MemType != 2) continue;
			if (RegIdx < 0 || RegIdx >= ExternalVariables.Num()) continue;

			const FExternalVariable& Var = ExternalVariables[RegIdx];

			URigVMVariableNode* VarNode = nullptr;
			if (URigVMVariableNode** Found = ExternalVarNodes.Find(RegIdx)) {
				VarNode = *Found;
			}

			if (!VarNode) {
				UStruct* CPPTypeObject = nullptr;
				if (Var.CPPType != TEXT("double") && Var.CPPType != TEXT("float") && Var.CPPType != TEXT("bool") && Var.CPPType != TEXT("int32") && Var.CPPType != TEXT("int64")) {
					CPPTypeObject = FindFirstObject<UStruct>(*Var.CPPType, EFindFirstObjectOptions::NativeFirst);
				}

				VarNode = Controller->AddVariableNode(
					FName(*Var.Name),
					Var.CPPType,
					CPPTypeObject,
					true,
					Var.DefaultValue,
					FVector2D(0.0f, ExtPosY),
					FString(),
					false,
					false
				);

				if (VarNode) {
					ExternalVarNodes.Add(RegIdx, VarNode);
					ExtPosY += 200.0f;
					UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Created variable getter '%s' (type='%s')"), *Var.Name, *Var.CPPType);
				} else {
					UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Failed to create variable getter '%s'"), *Var.Name);
					continue;
				}
			}

			URigVMPin* ValuePin = nullptr;
			for (URigVMPin* Pin : VarNode->GetPins()) {
				if (Pin && Pin->GetName() == TEXT("Value")) {
					ValuePin = Pin;
					break;
				}
			}
			if (!ValuePin) continue;

			FString FailureReason;
			if (Controller->AddLink(ValuePin, DestPin, false, ERigVMPinDirection::Invalid, false, false, &FailureReason)) {
				UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Linked var '%s' -> %s.%s"),
					*Var.Name, *Node->GetName(), *DestPin->GetName());
				ExtLinksCreated++;
			} else {
				UE_LOG(LogTemp, Warning, TEXT("ControlRig Importer: Var link failed '%s' -> %s.%s: %s"),
					*Var.Name, *Node->GetName(), *DestPin->GetName(), *FailureReason);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Graph created with %d nodes, %d data links, %d var links for '%s'"), NodesCreated, DataLinksCreated, ExtLinksCreated, *InControlRigBlueprint->GetName());

	/* Phase 5: Resolve any remaining dispatch template nodes with wildcard pins */
	int32 TemplatesResolved = 0;
	int32 WildcardsResolved = 0;
	for (const FCreatedNodeInfo& Info : CreatedNodes) {
		URigVMTemplateNode* TNode = Cast<URigVMTemplateNode>(Info.Node);
		if (!TNode) continue;
		if (!TNode->HasWildCardPin()) continue;

		const FRigVMTemplate* Templ = TNode->GetTemplate();
		if (!Templ) continue;
		if (!Templ->GetDispatchFactory()) continue;

		bool bResolvedSomething = false;

		TArray<FString> WildcardPinNames;
		for (URigVMPin* Pin : TNode->GetPins()) {
			if (Pin && Pin->IsWildCard() && Pin->GetDirection() == ERigVMPinDirection::Input) {
				WildcardPinNames.Add(Pin->GetName());
			}
		}

		for (const FString& PinName : WildcardPinNames) {
			URigVMPin* Pin = TNode->FindPin(PinName);
			if (!Pin || !Pin->IsWildCard()) continue;

			TArray<URigVMLink*> SourceLinks = Pin->GetSourceLinks();
			if (SourceLinks.Num() == 0) continue;

			URigVMPin* SourcePin = SourceLinks[0]->GetSourcePin();
			if (!SourcePin) continue;

			TRigVMTypeIndex SourceType = SourcePin->GetTypeIndex();

			if (Controller->ResolveWildCardPin(Pin, SourceType, false, false)) {
				WildcardsResolved++;
				bResolvedSomething = true;
				break;
			}
		}

		TemplatesResolved++;
	}
	UE_LOG(LogTemp, Log, TEXT("ControlRig Importer: Phase 5 complete -- %d dispatch nodes, %d wildcards resolved"), TemplatesResolved, WildcardsResolved);
#endif
}
