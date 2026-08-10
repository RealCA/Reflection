/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/AnimationBaseImporter.h"

#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"
#include "Modules/Cloud/Tools/AnimationData.h"

#if ENGINE_UE5
#include "Animation/AnimData/IAnimationDataController.h"
#if ENGINE_MINOR_VERSION >= 4
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#include "AnimDataController.h"
#endif

UObject* IAnimationBaseImporter::CreateAsset(UObject* CreatedAsset) {
	if (GetAssetClass() && GetAssetClass()->IsChildOf<UAnimMontage>()) {
		return IImporter::CreateAsset(NewObject<UAnimMontage>(GetPackage(), GetAssetClass(), *GetAssetName(), RF_Public | RF_Standalone));
	}

	return IImporter::CreateAsset(NewObject<UAnimSequence>(GetPackage(), UAnimSequence::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IAnimationBaseImporter::Import() {
	CreateAsset(nullptr);

	return ReadAnimationData(this, false, this);
}
