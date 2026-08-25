/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class ILevelImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	AActor* SpawnActorFromExport(FUObjectExport* Export, ULevel* Level);
	UActorComponent* CreateComponentFromExport(FUObjectExport* Export, AActor* Owner);
	AActor* FindOwnerActor(const FString& OuterName, const TMap<FString, AActor*>& Actors);

	void ApplyActorProperties(AActor* Actor, const TSharedPtr<FJsonObject>& Props);
	void ApplyComponentProperties(UActorComponent* Comp, const TSharedPtr<FJsonObject>& Props);

	UClass* ResolveClass(const FString& ClassName);
	UClass* ResolveComponentClass(const FString& CompType);

	FTransform ReadTransform(const TSharedPtr<FJsonObject>& Props);
	FVector ReadVector(const TSharedPtr<FJsonObject>& Json);
	FRotator ReadRotator(const TSharedPtr<FJsonObject>& Json);
};

REGISTER_IMPORTER(ILevelImporter, (TArray<FString>{
	TEXT("World"),
	TEXT("Level"),
}), TEXT("Level Assets"));
