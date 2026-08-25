/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/DumpBlueprintDebug.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/ContentBrowser.h"
#include "Importers/Types/Blueprint/BlueprintBytecodeImporter.h"

/* Filename-safe form of a graph name ("Set Camera location by index" keeps its
 * spaces; path separators and other reserved characters become '_'). */
static FString SanitizeGraphFileName(const FString& InName) {
	FString Out = InName;
	const TCHAR* Invalid = TEXT("\\/:*?\"<>|");
	for (TCHAR& Ch : Out) {
		if (FCString::Strchr(Invalid, Ch) != nullptr || Ch < 0x20) {
			Ch = TEXT('_');
		}
	}
	return Out;
}

static TSharedRef<FJsonObject> DescribeGraph(UEdGraph* Graph, const FString& FileName) {
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Graph->GetName());
	Obj->SetNumberField(TEXT("nodeCount"), (double)Graph->Nodes.Num());

	/* Non-exec pins that ended up unwired with no default value: the signature
	 * of a missing producer (entry param never wired, case temp unresolved,
	 * out-temp with no registration). Literal-set pins are intentional and
	 * excluded. */
	TArray<TSharedPtr<FJsonValue>> Unwired;
	for (UEdGraphNode* Node : Graph->Nodes) {
		if (!Node) continue;

		for (UEdGraphPin* Pin : Node->Pins) {
			if (!Pin || Pin->bHidden || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->LinkedTo.Num() > 0) continue;
			if (!Pin->DefaultValue.IsEmpty()) continue;

			const FString Entry = FString::Printf(TEXT("%s :: %s [%s %s]"),
				*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
				*Pin->PinName.ToString(),
				Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"),
				*Pin->PinType.PinCategory.ToString());
			Unwired.Add(MakeShareable(new FJsonValueString(Entry)));
		}
	}
	Obj->SetArrayField(TEXT("unwiredNoDefaultPins"), Unwired);

	Obj->SetStringField(TEXT("file"), FileName);
	return Obj;
}

void TToolDumpBlueprintDebug::Execute() {
	UBlueprint* Blueprint = GetSelectedAsset<UBlueprint>();
	if (!Blueprint) {
		return;
	}

	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintDebug"), Blueprint->GetName());
	IFileManager::Get().MakeDirectory(*Dir, /* Tree */ true);

	TArray<TSharedPtr<FJsonValue>> GraphReports;

	/* Every graph the importer touches: ubergraph pages, functions, macros
	 * and event graphs. */
	TArray<UEdGraph*> Graphs;
	for (UEdGraph* Graph : Blueprint->UbergraphPages) Graphs.Add(Graph);
	for (UEdGraph* Graph : Blueprint->FunctionGraphs) Graphs.Add(Graph);
	for (UEdGraph* Graph : Blueprint->MacroGraphs) Graphs.Add(Graph);
	for (UEdGraph* Graph : Blueprint->EventGraphs) Graphs.Add(Graph);

	int32 WrittenGraphs = 0;
	for (UEdGraph* Graph : Graphs) {
		if (!Graph) continue;

		const FString FileName = SanitizeGraphFileName(Graph->GetName()) + TEXT(".t3d");
		TSet<UObject*> NodeSet;
		for (UEdGraphNode* Node : Graph->Nodes) {
			if (Node) {
				NodeSet.Add(Node);
			}
		}

		FString TextExport;
		if (NodeSet.Num() > 0) {
			FEdGraphUtilities::ExportNodesToText(NodeSet, TextExport);
		}
		if (TextExport.IsEmpty()) {
			continue;
		}

		const FString FilePath = FPaths::Combine(Dir, FileName);
		if (FFileHelper::SaveStringToFile(TextExport, *FilePath)) {
			++WrittenGraphs;
		}

		GraphReports.Add(MakeShareable(new FJsonValueObject(DescribeGraph(Graph, FileName))));
	}

	/* Report JSON: diagnostics + bytecode si -> node cross-reference. */
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("asset"), Blueprint->GetName());
	Report->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Report->SetStringField(TEXT("generatedAt"), FDateTime::Now().ToString());

	{
		TArray<TSharedPtr<FJsonValue>> DiagLines;
		for (const FString& Line : FBlueprintBytecodeImporter::LastImportDiagnostics.Lines) {
			DiagLines.Add(MakeShareable(new FJsonValueString(Line)));
		}
		TSharedRef<FJsonObject> Diag = MakeShared<FJsonObject>();
		Diag->SetStringField(TEXT("asset"), FBlueprintBytecodeImporter::LastImportDiagnostics.AssetName);
		Diag->SetArrayField(TEXT("lines"), DiagLines);
		Report->SetObjectField(TEXT("importDiagnostics"), Diag);
	}

	{
		TSharedRef<FJsonObject> StatementMap = MakeShared<FJsonObject>();
		for (const TPair<FString, TMap<int32, FString>>& GraphSi : FBlueprintBytecodeImporter::LastImportDiagnostics.StatementNodes) {
			TSharedRef<FJsonObject> SiEntries = MakeShared<FJsonObject>();
			TArray<int32> Sis;
			GraphSi.Value.GetKeys(Sis);
			Sis.Sort();
			for (const int32 Si : Sis) {
				SiEntries->SetStringField(FString::FromInt(Si), GraphSi.Value[Si]);
			}
			StatementMap->SetObjectField(GraphSi.Key, SiEntries);
		}
		Report->SetObjectField(TEXT("statementNodeMap"), StatementMap);
	}

	Report->SetArrayField(TEXT("graphs"), GraphReports);

	FString ReportText;
	{
		const TSharedRef<FJsonObject> ReportObject = Report;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportText);
		FJsonSerializer::Serialize(ReportObject, Writer);
	}
	const FString ReportPath = FPaths::Combine(Dir, Blueprint->GetName() + TEXT("_report.json"));
	FFileHelper::SaveStringToFile(ReportText, *ReportPath);

	AppendNotification(
		FText::FromString(TEXT("Blueprint Debug Data")),
		FText::FromString(FString::Printf(TEXT("%d graph(s) -> %s"), WrittenGraphs, *Dir)),
		6.0f,
		SNotificationItem::CS_Success,
		true,
		420.0f
	);

	FPlatformProcess::ExploreFolder(*Dir);
}
