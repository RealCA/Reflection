/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Framework/Commands/InputChord.h"
#include "K2Node_Event.h"
#include "ReflectionK2Node_InputDebugKey.generated.h"

class UDynamicBlueprintBinding;

/* Event node for an imported "Input Debug Key" binding. The engine's dedicated
 * editable node for this (UK2Node_InputDebugKey) is private to the EnhancedInput
 * plugin, so we re-create its binding-facing surface here: the chord/event
 * properties are filled from the imported JSON, and GetDynamicBindingClass/
 * RegisterDynamicBinding make KismetCompiler's BuildDynamicBindingObjects emit a
 * UInputDebugKeyDelegateBinding for this node during compile. */
UCLASS()
class REFLECTION_API UReflectionK2Node_InputDebugKey : public UK2Node_Event
{
	GENERATED_BODY()

public:
	/* The chord (key + modifiers) that triggers this event. */
	UPROPERTY(EditAnywhere, Category = Key)
	FInputChord InputChord;

	/* Pressed / Released / Repeat / DoubleClick. */
	UPROPERTY()
	TEnumAsByte<EInputEvent> InputKeyEvent;

	/* Whether the event fires while the game is paused. */
	UPROPERTY()
	bool bExecuteWhenPaused = false;

	/* UK2Node interface */
	virtual UClass* GetDynamicBindingClass() const override;
	virtual void RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const override;
};