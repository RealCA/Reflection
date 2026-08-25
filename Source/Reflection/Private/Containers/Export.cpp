/* Copyright Reflection Contributors 2024-2026 */

#include "Containers/Export.h"
#include "Engine/EngineUtilities.h"
#include "Settings/ReflectionSettings.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"

FString ReadPathFromObject(const FUObjectJsonValueExport& PackageIndex) {
	FString ObjectType, ObjectName, ObjectPath, Outer;
	PackageIndex.GetString("ObjectName").Split("'", &ObjectType, &ObjectName);

	ObjectPath = PackageIndex.GetString("ObjectPath");
	ObjectPath.Split(".", &ObjectPath, nullptr);

	const UReflectionSettings* Settings = GetSettings();

	if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Content"), TEXT("/Game"));
	}

	ObjectPath = ObjectPath.Replace(TEXT("Engine/Content"), TEXT("/Engine"));
	ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", nullptr, &ObjectName);
	}

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", &Outer, &ObjectName);
	}

	return ObjectPath + "." + ObjectName;
}

UClass* FUObjectExport::GetClass() {
	if (Class) return Class;
	
	FString ClassName = GetString("Class");

	if (Has("Template")) {
		ClassName = ReadPathFromObject(GetObject("Template")).Replace(TEXT("Default__"), TEXT(""));
	}

	if (ClassName.Contains("'")) {
		ClassName.Split("'", nullptr, &ClassName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		ClassName.Split("'", &ClassName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}

	UClass* OutClass = FindClassByType(ClassName);
	if (!OutClass) {
		OutClass = FindClassByType(GetType().ToString());
	}

	if (Has("Next") || Has("SuperStruct") || Has("Super")) {
		/* Parent refs appear at TWO levels and TWO spellings depending on the
		 * exporter: top-level "SuperStruct" (BP_WorldPawn: Class'Character'),
		 * top-level "Super" (BP_Stockpile: BP_ManagerialGame_C), or inside
		 * Properties (the old GetSuperStructJsonObject path). Reading only
		 * Properties.SuperStruct left BP_WorldPawn_C with a NULL parent and
		 * faulted every compile that walked a member typed by it (08.24/25).
		 * Resolve top-level first, then the Properties fallback, and build the
		 * full <package>.<ClassName> path like ResolveObjectConstValue does. */
		TSharedPtr<FJsonObject> SuperJson;
		if (JsonObject->HasField(TEXT("SuperStruct"))) SuperJson = JsonObject->GetObjectField(TEXT("SuperStruct"));
		else if (JsonObject->HasField(TEXT("Super"))) SuperJson = JsonObject->GetObjectField(TEXT("Super"));
		else SuperJson = GetSuperStructJsonObject(GetProperties());

		if (SuperJson.IsValid()) {
			FString SuperName = SuperJson->GetStringField(TEXT("ObjectName"));
			FString SuperPath = SuperJson->GetStringField(TEXT("ObjectPath"));

			FString ShortName = StripObjectOuter(SuperName);
			int32 Dot;
			if (SuperPath.FindChar(TEXT('.'), Dot)) SuperPath = SuperPath.Left(Dot);

			FString FullPath = SuperPath + TEXT(".") + ShortName;
			OutClass = FindObject<UClass>(nullptr, *FullPath);
			if (!OutClass) OutClass = LoadClass<UClass>(nullptr, *FullPath);
		}
	}

	if (!OutClass) return nullptr;

	Class = OutClass;
	return Class;
}
