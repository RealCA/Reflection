/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UBlueprintGeneratedClass;
class FUObjectExportContainer;

/* Rebuilds the functions a blueprint generated class exports.
 *
 * The exported file holds no graphs, only the compiled result: each Function export carries a
 * ChildProperties list (the function's parameters and locals) and a ScriptBytecode stream. The
 * importer creates the UFunctions on the generated class and re-encodes the bytecode into the
 * same raw-pointer form the VM executes (pointers are resolved by re-looking up the property and
 * object references the exported bytecode decodes to).
 *
 * Bytecode fidelity matters: jumps are absolute byte offsets into the original stream, callers
 * pass those same offsets through the ubergraph's EntryPoint parameter, and EX_Context/EX_SwitchValue
 * skip fields are computed from the emitted size of the following expressions. Every encoded
 * function is validated against the exported offsets before it is accepted. */
struct REFLECTION_API FBlueprintFunctions {
	/* Creates a UFunction per Function export, re-encodes its ScriptBytecode and wires the
	 * generated class's function map. Returns how many functions were constructed. */
	static int32 Construct(UBlueprint* Blueprint, FUObjectExportContainer* Container);
};
