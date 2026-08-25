/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/ToolBase.h"

/* Writes the verification dataset for the bytecode importer, for the Blueprint
 * selected in the Content Browser, into Saved/BlueprintDebug/<AssetName>/:
 *
 *  - <GraphName>.t3d per graph: the standard editor text export of every node
 *    (same format as a graph Copy), so imported wiring/defaults can be diffed
 *    against the bytecode JSON ground truth directly;
 *  - <AssetName>_report.json: import diagnostics (unresolved wires, producer
 *    conflicts, missing out-temps, suppressed temps), the bytecode statement
 *    index -> emitted node cross-reference per graph, and every non-exec pin
 *    that ended up unwired with no default value. */
class TToolDumpBlueprintDebug : public TToolBase {
public:
	void Execute();
};
