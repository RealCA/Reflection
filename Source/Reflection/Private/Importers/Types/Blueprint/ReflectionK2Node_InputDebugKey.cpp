/* Copyright Reflection Contributors 2024-2026 */

#include "ReflectionK2Node_InputDebugKey.h"
#include "InputDebugKeyDelegateBinding.h"

UClass* UReflectionK2Node_InputDebugKey::GetDynamicBindingClass() const
{
	return UInputDebugKeyDelegateBinding::StaticClass();
}

void UReflectionK2Node_InputDebugKey::RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const
{
	UInputDebugKeyDelegateBinding* KeyBindings = CastChecked<UInputDebugKeyDelegateBinding>(BindingObject);

	FBlueprintInputDebugKeyDelegateBinding Binding;
	Binding.InputChord = InputChord;
	Binding.InputKeyEvent = InputKeyEvent;
	Binding.FunctionNameToBind = CustomFunctionName;
	Binding.bExecuteWhenPaused = bExecuteWhenPaused;
	KeyBindings->InputDebugKeyDelegateBindings.Add(Binding);
}