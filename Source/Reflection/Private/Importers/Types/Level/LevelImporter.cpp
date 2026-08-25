/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Level/LevelImporter.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/Engine.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SkyLight.h"
#include "Engine/RectLight.h"
#include "Engine/Brush.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/AudioComponent.h"
#include "Components/PostProcessComponent.h"
#include "Camera/CameraComponent.h"
#include "CineCameraComponent.h"
#include "Camera/CameraActor.h"
#include "CineCameraActor.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Factories/WorldFactory.h"
#include "LevelEditor.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogLevelImporter, Log, All);

UObject* ILevelImporter::CreateAsset(UObject* CreatedAsset) {
	return CreatedAsset;
}

bool ILevelImporter::Import() {
	const FString WorldName = GetAssetName();
	UPackage* WorldPackage = GetPackage();

	UE_LOG(LogLevelImporter, Log, TEXT("=== IMPORT START: %s ==="), *WorldName);

	if (!WorldPackage) {
		UE_LOG(LogLevelImporter, Error, TEXT("ABORT: No package"));
		return false;
	}

	FUObjectExportContainer* Container = GetPropertySerializer()->ExportsContainer;
	if (!Container) {
		UE_LOG(LogLevelImporter, Error, TEXT("ABORT: No container"));
		return false;
	}

	UE_LOG(LogLevelImporter, Log, TEXT("  Container exports: %d"), Container->Exports.Num());

	/* Step 1: Classify exports */
	TArray<FUObjectExport*> ActorExports;
	TArray<FUObjectExport*> ComponentExports;

	for (FUObjectExport* Export : Container->Exports) {
		if (!Export || !Export->IsJsonValid()) continue;

		const FString ExportType = Export->GetType().ToString();
		if (ExportType == TEXT("World") || ExportType == TEXT("Level")
			|| ExportType == TEXT("WorldSettings") || ExportType == TEXT("Model")
			|| ExportType == TEXT("NavigationSystemModuleConfig")
			|| ExportType == TEXT("LevelStreamingDynamic")) {
			continue;
		}

		if (!Export->JsonObject->HasField(TEXT("Outer"))) continue;

		const TSharedPtr<FJsonObject>& Outer = Export->JsonObject->GetObjectField(TEXT("Outer"));
		if (!Outer->HasField(TEXT("ObjectName"))) continue;

		const FString OuterName = Outer->GetStringField(TEXT("ObjectName"));

		if (OuterName.StartsWith(TEXT("Level'"))) {
			ActorExports.Add(Export);
		} else if (OuterName.Contains(TEXT("."))) {
			ComponentExports.Add(Export);
		}
	}

	UE_LOG(LogLevelImporter, Log, TEXT("  Actors: %d, Components: %d"), ActorExports.Num(), ComponentExports.Num());

	/* Step 2: Tear down the old editor world (same as UEditorEngine::EditorDestroyWorld) */
	UE_LOG(LogLevelImporter, Log, TEXT("STEP 1: Tearing down old editor world..."));

	FWorldContext& Context = GEditor->GetEditorWorldContext();
	UWorld* OldWorld = Context.World();

	if (OldWorld) {
		if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor"))) {
			FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
			LevelEditor.BroadcastMapChanged(OldWorld, EMapChangeType::TearDownWorld);
		}

		OldWorld->ClearWorldComponents();
		GEditor->SelectNone(true, true);
		OldWorld->ActiveGroupActors.Empty();

		if (OldWorld->WorldType != EWorldType::EditorPreview && OldWorld->WorldType != EWorldType::Inactive) {
			OldWorld->ClearFlags(RF_Standalone | RF_Transactional);
			OldWorld->RemoveFromRoot();
			OldWorld->SetFlags(RF_Transient);
		}
	}

	/* Step 3: Create a new world via UWorldFactory (same as UEditorEngine::NewMap) */
	UE_LOG(LogLevelImporter, Log, TEXT("STEP 2: Creating UWorld via UWorldFactory..."));

	WorldPackage->SetPackageFlags(PKG_NewlyCreated);

	UWorldFactory* Factory = NewObject<UWorldFactory>();
	Factory->WorldType = EWorldType::Editor;
	Factory->bInformEngineOfWorld = true;
	Factory->bCreateWorldPartition = false;
	Factory->FeatureLevel = GMaxRHIFeatureLevel;

	UWorld* World = CastChecked<UWorld>(Factory->FactoryCreateNew(
		UWorld::StaticClass(), WorldPackage, *WorldName, RF_Public | RF_Standalone, NULL, GWarn));

	if (!World) {
		UE_LOG(LogLevelImporter, Error, TEXT("ABORT: UWorldFactory::FactoryCreateNew returned null"));
		return false;
	}

	UE_LOG(LogLevelImporter, Log, TEXT("  UWorld created: %s"), *World->GetFullName());
	UE_LOG(LogLevelImporter, Log, TEXT("  Package: %s"), *WorldPackage->GetName());

	/* Step 4: Set as the editor's current world */
	Context.SetCurrentWorld(World);
	GWorld = World;
	World->AddToRoot();
	World->UpdateWorldComponents(true, true);

	GEditor->NoteSelectionChange();
	GEngine->BroadcastLevelActorListChanged();
	FEditorDelegates::MapChange.Broadcast(MapChangeEventFlags::NewMap);

	if (FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor"))) {
		FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		LevelEditor.BroadcastMapChanged(World, EMapChangeType::NewMap);
	}

	UE_LOG(LogLevelImporter, Log, TEXT("  Editor world set, outliner updated."));

	ULevel* Level = World->PersistentLevel;
	if (!Level) {
		UE_LOG(LogLevelImporter, Error, TEXT("ABORT: PersistentLevel is null"));
		return false;
	}

	/* Step 5: Spawn actors */
	TMap<FString, AActor*> SpawnedActors;
	for (FUObjectExport* ActorExport : ActorExports) {
		AActor* Actor = SpawnActorFromExport(ActorExport, Level);
		if (Actor) {
			SpawnedActors.Add(ActorExport->GetName().ToString(), Actor);
		}
	}
	UE_LOG(LogLevelImporter, Log, TEXT("STEP 3: Spawned %d Actors"), SpawnedActors.Num());

	/* Step 6: Create components */
	int32 CompCount = 0;
	for (FUObjectExport* CompExport : ComponentExports) {
		const TSharedPtr<FJsonObject>& Outer = CompExport->JsonObject->GetObjectField(TEXT("Outer"));
		const FString OuterName = Outer->GetStringField(TEXT("ObjectName"));

		AActor* Owner = FindOwnerActor(OuterName, SpawnedActors);
		if (!Owner) {
			UE_LOG(LogLevelImporter, Warning, TEXT("  Component '%s' owner not found (Outer=%s)"),
				*CompExport->GetName().ToString(), *OuterName);
			continue;
		}

		UActorComponent* Comp = CreateComponentFromExport(CompExport, Owner);
		if (Comp) ++CompCount;
	}
	UE_LOG(LogLevelImporter, Log, TEXT("STEP 4: Created %d Components"), CompCount);

	/* Step 7: Finalize level */
	Level->SortActorList();
	Level->MarkPackageDirty();
	GEditor->RedrawLevelEditingViewports();

	UE_LOG(LogLevelImporter, Log, TEXT("STEP 5: Level '%s' loaded in editor (%d actors, %d components)"),
		*WorldName, SpawnedActors.Num(), CompCount);
	UE_LOG(LogLevelImporter, Log, TEXT("  Use File > Save Current Level As to save to Content Browser."));

	UE_LOG(LogLevelImporter, Log, TEXT("=== IMPORT END: %s ==="), *WorldName);
	return true;
}

AActor* ILevelImporter::SpawnActorFromExport(FUObjectExport* Export, ULevel* Level) {
	if (!Export || !Level) return nullptr;

	const FString ActorName = Export->GetName().ToString();
	const FString ClassName = Export->GetType().ToString();

	UClass* ActorClass = ResolveClass(ClassName);
	if (!ActorClass) {
		UE_LOG(LogLevelImporter, Warning, TEXT("  Unknown class: %s (%s)"), *ActorName, *ClassName);
		return nullptr;
	}

	FTransform SpawnTransform = FTransform::Identity;
	if (Export->JsonObject->HasField(TEXT("Properties"))) {
		const TSharedPtr<FJsonObject>& Props = Export->JsonObject->GetObjectField(TEXT("Properties"));
		if (Props.IsValid()) {
			SpawnTransform = ReadTransform(Props);
		}
	}

	FActorSpawnParameters Params;
	Params.OverrideLevel = Level;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Actor = Level->GetWorld()->SpawnActor<AActor>(ActorClass, SpawnTransform, Params);
	if (Actor) {
		Actor->SetActorLabel(ActorName);
		if (Export->JsonObject->HasField(TEXT("Properties"))) {
			ApplyActorProperties(Actor, Export->JsonObject->GetObjectField(TEXT("Properties")));
		}
	}

	return Actor;
}

UActorComponent* ILevelImporter::CreateComponentFromExport(FUObjectExport* Export, AActor* Owner) {
	if (!Export || !Owner) return nullptr;

	const FString CompName = Export->GetName().ToString();
	const FString CompType = Export->GetType().ToString();

	UClass* CompClass = ResolveComponentClass(CompType);
	if (!CompClass) return nullptr;

	UActorComponent* Existing = nullptr;
	for (UActorComponent* C : Owner->GetComponents()) {
		if (C && C->GetName() == CompName) {
			Existing = C;
			break;
		}
	}
	if (Existing) {
		if (Existing->IsA(CompClass)) {
			return Existing;
		}
		if (CompClass->IsChildOf(Existing->GetClass())) {
			Existing->DestroyComponent();
		} else {
			return nullptr;
		}
	}

	UActorComponent* Comp = NewObject<UActorComponent>(Owner, CompClass, *CompName);
	if (!Comp) return nullptr;

	if (Comp->IsA<USceneComponent>()) {
		USceneComponent* SceneComp = Cast<USceneComponent>(Comp);
		USceneComponent* Root = Owner->GetRootComponent();
		if (Root) {
			SceneComp->SetupAttachment(Root);
		} else {
			Owner->SetRootComponent(SceneComp);
		}
	}

	Comp->RegisterComponent();

	if (Export->JsonObject->HasField(TEXT("Properties"))) {
		ApplyComponentProperties(Comp, Export->JsonObject->GetObjectField(TEXT("Properties")));
	}

	return Comp;
}

AActor* ILevelImporter::FindOwnerActor(const FString& OuterName, const TMap<FString, AActor*>& Actors) {
	FString ActorName;
	int32 DotIdx = OuterName.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (DotIdx != INDEX_NONE) {
		int32 QuoteIdx = OuterName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (QuoteIdx != INDEX_NONE && QuoteIdx > DotIdx) {
			ActorName = OuterName.Mid(DotIdx + 1, QuoteIdx - DotIdx - 1);
		}
	}

	if (!ActorName.IsEmpty()) {
		AActor* const* Found = Actors.Find(ActorName);
		if (Found) return *Found;
	}

	for (const auto& Pair : Actors) {
		if (OuterName.Contains(Pair.Key)) {
			return Pair.Value;
		}
	}
	return nullptr;
}

void ILevelImporter::ApplyActorProperties(AActor* Actor, const TSharedPtr<FJsonObject>& Props) {
	if (!Actor || !Props.IsValid()) return;

	if (Props->HasField(TEXT("bReplicates"))) {
		Actor->SetReplicates(Props->GetBoolField(TEXT("bReplicates")));
	}

	if (Props->HasField(TEXT("PrimaryActorTick"))) {
		const TSharedPtr<FJsonObject>& Tick = Props->GetObjectField(TEXT("PrimaryActorTick"));
		if (Tick->HasField(TEXT("bCanEverTick"))) {
			Actor->SetActorTickEnabled(Tick->GetBoolField(TEXT("bCanEverTick")));
		}
	}

	if (Props->HasField(TEXT("bHidden"))) {
		Actor->SetActorHiddenInGame(Props->GetBoolField(TEXT("bHidden")));
	}

	if (Props->HasField(TEXT("bCanBeDamaged"))) {
		Actor->SetCanBeDamaged(Props->GetBoolField(TEXT("bCanBeDamaged")));
	}

	if (Props->HasField(TEXT("Tags"))) {
		const TArray<TSharedPtr<FJsonValue>>& Tags = Props->GetArrayField(TEXT("Tags"));
		for (const TSharedPtr<FJsonValue>& TagVal : Tags) {
			Actor->Tags.Add(FName(*TagVal->AsString()));
		}
	}
}

void ILevelImporter::ApplyComponentProperties(UActorComponent* Comp, const TSharedPtr<FJsonObject>& Props) {
	if (!Comp || !Props.IsValid()) return;

	if (Props->HasField(TEXT("ComponentTags"))) {
		const TArray<TSharedPtr<FJsonValue>>& Tags = Props->GetArrayField(TEXT("ComponentTags"));
		for (const TSharedPtr<FJsonValue>& TagVal : Tags) {
			Comp->ComponentTags.Add(FName(*TagVal->AsString()));
		}
	}

	if (Comp->IsA<USceneComponent>()) {
		USceneComponent* SC = Cast<USceneComponent>(Comp);

		if (Props->HasField(TEXT("RelativeLocation"))) {
			SC->SetRelativeLocation(ReadVector(Props->GetObjectField(TEXT("RelativeLocation"))));
		}
		if (Props->HasField(TEXT("RelativeRotation"))) {
			SC->SetRelativeRotation(ReadRotator(Props->GetObjectField(TEXT("RelativeRotation"))));
		}
		if (Props->HasField(TEXT("RelativeScale3D"))) {
			SC->SetRelativeScale3D(ReadVector(Props->GetObjectField(TEXT("RelativeScale3D"))));
		}
		if (Props->HasField(TEXT("bVisible"))) {
			SC->SetVisibility(Props->GetBoolField(TEXT("bVisible")));
		}
	}
}

UClass* ILevelImporter::ResolveClass(const FString& ClassName) {
	if (ClassName == TEXT("Actor")) return AActor::StaticClass();

	static const TMap<FString, UClass*> CommonClasses = {
		{ TEXT("StaticMeshActor"),   AStaticMeshActor::StaticClass() },
		{ TEXT("PlayerStart"),       APlayerStart::StaticClass() },
		{ TEXT("DirectionalLight"),  ADirectionalLight::StaticClass() },
		{ TEXT("PointLight"),        APointLight::StaticClass() },
		{ TEXT("RectLight"),         ARectLight::StaticClass() },
		{ TEXT("SkyLight"),          ASkyLight::StaticClass() },
		{ TEXT("Brush"),             ABrush::StaticClass() },
		{ TEXT("LevelScriptActor"),  ALevelScriptActor::StaticClass() },
		{ TEXT("CameraActor"),       ACameraActor::StaticClass() },
		{ TEXT("CineCameraActor"),   ACineCameraActor::StaticClass() },
	};

	const UClass* const* Found = CommonClasses.Find(ClassName);
	if (Found) return const_cast<UClass*>(*Found);

	UClass* Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *ClassName));
	if (Class) return Class;

	if (ClassName.EndsWith(TEXT("_C"))) {
		FString BpName = ClassName.LeftChop(2);

		// Derive the project content root from the package being imported instead
		// of hardcoding a folder (e.g. "/Game/TouchyGame/"). The level usually
		// lives at /Game/<Root>/Maps/X, so walk back to the first folder after
		// /Game and probe both /Game/<Root>/BP and /Game/<Root>.
		TArray<FString> Roots;
		if (UPackage* Pkg = GetPackage())
		{
			FString PkgName = Pkg->GetName();
			PkgName.RemoveFromStart(TEXT("/Game/"), ESearchCase::CaseSensitive);
			int32 SlashIdx = INDEX_NONE;
			if (PkgName.FindChar(TEXT('/'), SlashIdx))
			{
				Roots.Add(TEXT("/Game/") + PkgName.Left(SlashIdx));
			}
		}
		Roots.Add(TEXT("/Game"));

		for (const FString& Root : Roots)
		{
			FString Path = FString::Printf(TEXT("%s/BP/%s.%s_C"), *Root, *BpName, *BpName);
			Class = FindObject<UClass>(nullptr, *Path);
			if (Class) return Class;

			Path = FString::Printf(TEXT("%s/%s.%s_C"), *Root, *BpName, *BpName);
			Class = FindObject<UClass>(nullptr, *Path);
			if (Class) return Class;
		}

		Class = FindFirstObject<UClass>(*ClassName);
		if (Class) return Class;
	}

	return nullptr;
}

UClass* ILevelImporter::ResolveComponentClass(const FString& CompType) {
	static const TMap<FString, UClass*> KnownTypes = {
		{ TEXT("SceneComponent"),             USceneComponent::StaticClass() },
		{ TEXT("DefaultSceneRoot"),           USceneComponent::StaticClass() },
		{ TEXT("StaticMeshComponent"),        UStaticMeshComponent::StaticClass() },
		{ TEXT("SkeletalMeshComponent"),      USkeletalMeshComponent::StaticClass() },
		{ TEXT("ArrowComponent"),             UArrowComponent::StaticClass() },
		{ TEXT("BoxComponent"),               UBoxComponent::StaticClass() },
		{ TEXT("SphereComponent"),            USphereComponent::StaticClass() },
		{ TEXT("CapsuleComponent"),           UCapsuleComponent::StaticClass() },
		{ TEXT("BillboardComponent"),         UBillboardComponent::StaticClass() },
		{ TEXT("ChildActorComponent"),        UChildActorComponent::StaticClass() },
		{ TEXT("DirectionalLightComponent"),  UDirectionalLightComponent::StaticClass() },
		{ TEXT("PointLightComponent"),        UPointLightComponent::StaticClass() },
		{ TEXT("RectLightComponent"),         URectLightComponent::StaticClass() },
		{ TEXT("SkyLightComponent"),          USkyLightComponent::StaticClass() },
		{ TEXT("AudioComponent"),             UAudioComponent::StaticClass() },
		{ TEXT("PostProcessComponent"),       UPostProcessComponent::StaticClass() },
		{ TEXT("CameraComponent"),            UCameraComponent::StaticClass() },
		{ TEXT("CineCameraComponent"),        UCineCameraComponent::StaticClass() },
	};

	const UClass* const* Found = KnownTypes.Find(CompType);
	if (Found) return const_cast<UClass*>(*Found);

	UClass* CompClass = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *CompType));
	if (CompClass && CompClass->IsChildOf(UActorComponent::StaticClass())) return CompClass;

	CompClass = FindFirstObject<UClass>(*CompType);
	if (CompClass && CompClass->IsChildOf(UActorComponent::StaticClass())) return CompClass;

	return nullptr;
}

FTransform ILevelImporter::ReadTransform(const TSharedPtr<FJsonObject>& Props) {
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector(1.f);

	if (Props->HasField(TEXT("RelativeLocation"))) {
		Location = ReadVector(Props->GetObjectField(TEXT("RelativeLocation")));
	}
	if (Props->HasField(TEXT("RelativeRotation"))) {
		Rotation = ReadRotator(Props->GetObjectField(TEXT("RelativeRotation")));
	}
	if (Props->HasField(TEXT("RelativeScale3D"))) {
		Scale = ReadVector(Props->GetObjectField(TEXT("RelativeScale3D")));
	}

	return FTransform(Rotation, Location, Scale);
}

FVector ILevelImporter::ReadVector(const TSharedPtr<FJsonObject>& Json) {
	if (!Json.IsValid()) return FVector::ZeroVector;
	return FVector(
		Json->GetNumberField(TEXT("X")),
		Json->GetNumberField(TEXT("Y")),
		Json->GetNumberField(TEXT("Z"))
	);
}

FRotator ILevelImporter::ReadRotator(const TSharedPtr<FJsonObject>& Json) {
	if (!Json.IsValid()) return FRotator::ZeroRotator;
	return FRotator(
		Json->GetNumberField(TEXT("Pitch")),
		Json->GetNumberField(TEXT("Yaw")),
		Json->GetNumberField(TEXT("Roll"))
	);
}
