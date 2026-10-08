#include "stp/cad_document.h"
#include "stp/pmi.h"
#include "stp/part_export.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <DESTEP_Parameters.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_Editor.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
const Stp::PmiRecord& findRecord(const Stp::PmiCatalog& catalog, const TDF_Label& source)
{
    const auto found = std::find_if(catalog.records.begin(), catalog.records.end(),
                                  [&](const auto& record) { return record.sourceLabel == source; });
    if (found == catalog.records.end()) throw std::runtime_error("PMI record was silently omitted");
    return *found;
}
Handle(TDocStd_Document) newDocument()
{
    Handle(TDocStd_Document) result = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(result->Main());
    XCAFDoc_DocumentTool::SetLengthUnit(result, 0.001);
    return result;
}
TDF_Label addDimension(const Handle(XCAFDoc_DimTolTool)& tool, const TDF_Label& owner,
                       double value, bool graphical = false, bool modifier = false)
{
    const auto label = tool->AddDimension();
    Handle(XCAFDimTolObjects_DimensionObject) object = new XCAFDimTolObjects_DimensionObject;
    object->SetType(XCAFDimTolObjects_DimensionType_Location_LinearDistance);
    object->SetValue(value);
    object->SetPoint(gp_Pnt(0, 0, 0)); object->SetPoint2(gp_Pnt(10, 0, 0));
    object->SetPlane(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)));
    object->SetPointTextAttach(gp_Pnt(5, -5, 0));
    if (graphical)
        object->SetPresentation(BRepBuilderAPI_MakeEdge(gp_Pnt(0, -3, 0), gp_Pnt(10, -3, 0)),
                                new TCollection_HAsciiString("source-graphic"));
    if (modifier) object->AddModifier(XCAFDimTolObjects_DimensionModif_StatisticalTolerance);
    XCAFDoc_Dimension::Set(label)->SetObject(object);
    if (!owner.IsNull()) tool->SetDimension(owner, label);
    return label;
}

Stp::CadDocument roundTripFixture()
{
    Stp::CadDocument document; document.xde = newDocument();
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document.xde->Main());
    const auto tool = XCAFDoc_DocumentTool::DimTolTool(document.xde->Main());
    const auto box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    const auto part = shapes->AddShape(box, false);
    TDataStd_Name::Set(part, "PMI test block");
    TDF_Label first, second;
    for (TopExp_Explorer explorer(box, TopAbs_VERTEX); explorer.More(); explorer.Next()) {
        const auto point = BRep_Tool::Pnt(TopoDS::Vertex(explorer.Current()));
        if (point.Distance(gp_Pnt(0, 0, 0)) < 1e-9) first = shapes->AddSubShape(part, explorer.Current());
        if (point.Distance(gp_Pnt(10, 0, 0)) < 1e-9) second = shapes->AddSubShape(part, explorer.Current());
    }
    check(!first.IsNull() && !second.IsNull(), "Fixture requires two explicit reference vertices");
    // This fixture is authored here, not extracted from any third-party CAD file.
    // The strokes carry the source '10', extension lines and arrowheads, so the
    // actual STEP reader must preserve the presentation, not only a nominal value.
    TopoDS_Compound graphic; BRep_Builder builder; builder.MakeCompound(graphic);
    const auto line = [&](double x1, double y1, double x2, double y2) {
        builder.Add(graphic, BRepBuilderAPI_MakeEdge(gp_Pnt(x1, y1, 0), gp_Pnt(x2, y2, 0)));
    };
    line(0, 0, 0, -4); line(10, 0, 10, -4); line(0, -3, 10, -3);
    line(0, -3, 1, -2.6); line(0, -3, 1, -3.4);
    line(10, -3, 9, -2.6); line(10, -3, 9, -3.4);
    line(4, -4.5, 4, -6.5);
    line(5, -4.5, 6, -4.5); line(6, -4.5, 6, -6.5);
    line(6, -6.5, 5, -6.5); line(5, -6.5, 5, -4.5);
    for (bool graphical : {true, false}) {
        const auto label = addDimension(tool, {}, 10);
        Handle(XCAFDoc_Dimension) attribute;
        check(label.FindAttribute(XCAFDoc_Dimension::GetID(), attribute), "Fixture dimension attribute missing");
        auto object = attribute->GetObject();
        object->SetSemanticName(new TCollection_HAsciiString(graphical ? "Width 10 source graphic" : "Width 10 semantic only"));
        if (graphical) object->SetPresentation(graphic, new TCollection_HAsciiString("Width 10 strokes"));
        attribute->SetObject(object);
        tool->SetDimension(first, second, label);
    }
    Stp::rebuildIndex(document);
    return document;
}

void writeFixture(const Stp::CadDocument& document, const QString& path)
{
    STEPCAFControl_Writer writer; writer.SetDimTolMode(true); writer.SetNameMode(true);
    DESTEP_Parameters parameters; parameters.WriteSchema = DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
    parameters.WriteUnit = UnitsMethods_LengthUnit_Millimeter;
    check(writer.Transfer(document.xde, parameters), "AP242 fixture transfer failed");
    check(writer.Write(path.toUtf8().constData()) == IFSelect_RetDone, "AP242 fixture writing failed");
}

void checkReadBack(const Stp::CadDocument& document)
{
    const auto catalog = Stp::buildPmiCatalog(document);
    check(document.parts.size() == 1, "PMI fixture must retain its solid");
    check(catalog.records.size() == 2, "AP242 reader must retain both dimensions");
    int graphical = 0, semantic = 0;
    for (const auto& record : catalog.records) {
        check(record.kind == Stp::PmiKind::Dimension, "Read-back must preserve PMI dimension kind");
        check(record.associationComplete && record.ownerPartIds.size() == 1, "Read-back must retain dimension-to-geometry links");
        check(record.occurrencePlacementVerified, "Single identity part placement must be verified");
        Handle(XCAFDoc_Dimension) attribute;
        check(record.sourceLabel.FindAttribute(XCAFDoc_Dimension::GetID(), attribute), "Read-back dimension attribute missing");
        check(std::abs(attribute->GetObject()->GetValue() - 10) < 1e-9, "Read-back must preserve source nominal value");
        if (record.status == Stp::PmiStatus::SourceGraphics) {
            ++graphical;
            int edges = 0;
            for (TopExp_Explorer explorer(record.graphicalPresentation, TopAbs_EDGE); explorer.More(); explorer.Next()) ++edges;
            check(edges >= 12, "Source graphic dimension strokes must survive STEP serialization");
        } else {
            ++semantic;
            check(!record.reason.isEmpty(), "PMI without an imported layout must retain a diagnostic");
        }
    }
    check(graphical == 1 && semantic == 1, "AP242 reader must distinguish source graphics and semantic-only PMI");
}

void checkBasicSemanticPresentations()
{
    Stp::CadDocument document; document.xde = newDocument();
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(document.xde->Main());
    const auto tool = XCAFDoc_DocumentTool::DimTolTool(document.xde->Main());
    const gp_Ax2 plane(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    const TopoDS_Edge firstEdge = BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    const TopoDS_Edge secondEdge = BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0));
    const TopoDS_Edge circle = BRepBuilderAPI_MakeEdge(gp_Circ(plane, 5));
    TopoDS_Compound compound; BRep_Builder builder; builder.MakeCompound(compound);
    builder.Add(compound, firstEdge); builder.Add(compound, secondEdge); builder.Add(compound, circle);
    const auto part = shapes->AddShape(compound, false);
    const auto first = shapes->AddSubShape(part, firstEdge);
    const auto second = shapes->AddSubShape(part, secondEdge);
    const auto circleLabel = shapes->AddSubShape(part, circle);
    const auto dimension = [&](XCAFDimTolObjects_DimensionType type, double value,
                               const TDF_Label& a, const TDF_Label& b = {}) {
        const auto label = tool->AddDimension();
        Handle(XCAFDimTolObjects_DimensionObject) object = new XCAFDimTolObjects_DimensionObject;
        object->SetType(type); object->SetValue(value); object->SetPlane(plane);
        object->SetPointTextAttach(gp_Pnt(7, 7, 0));
        XCAFDoc_Dimension::Set(label)->SetObject(object);
        if (b.IsNull()) tool->SetDimension(a, label); else tool->SetDimension(a, b, label);
        return label;
    };
    const auto angle = dimension(XCAFDimTolObjects_DimensionType_Size_Angular, 90, first, second);
    const auto wrongAngle = dimension(XCAFDimTolObjects_DimensionType_Size_Angular, 45, first, second);
    const auto radius = dimension(XCAFDimTolObjects_DimensionType_Size_Radius, 5, circleLabel);
    const auto diameter = dimension(XCAFDimTolObjects_DimensionType_Size_Diameter, 10, circleLabel);
    const auto wrongDiameter = dimension(XCAFDimTolObjects_DimensionType_Size_Diameter, 9, circleLabel);
    const auto datum = tool->AddDatum();
    Handle(XCAFDimTolObjects_DatumObject) datumObject = new XCAFDimTolObjects_DatumObject;
    datumObject->SetName(new TCollection_HAsciiString("A")); datumObject->SetPlane(plane);
    datumObject->SetPoint(gp_Pnt(0, 0, 0)); datumObject->SetPointTextAttach(gp_Pnt(-3, -3, 0));
    XCAFDoc_Datum::Set(datum)->SetObject(datumObject);
    TDF_LabelSequence datumReferences; datumReferences.Append(first); tool->SetDatum(datumReferences, datum);
    Stp::rebuildIndex(document);
    const auto catalog = Stp::buildPmiCatalog(document);
    for (const auto& expectation : std::vector<std::pair<TDF_Label, Stp::PmiFallback>> {
             {angle, Stp::PmiFallback::Angular}, {radius, Stp::PmiFallback::Radius},
             {diameter, Stp::PmiFallback::Diameter}, {datum, Stp::PmiFallback::DatumText}}) {
        const auto& record = findRecord(catalog, expectation.first);
        check(record.fallback == expectation.second, "Complete angular/circular/datum semantics must retain their supported presentation");
        QString error;
        check(!Stp::createPmiPresentation(record, &error).IsNull() && error.isEmpty(), "Supported semantic annotation must produce a valid presentation");
    }
    check(findRecord(catalog, angle).valueText.contains(QChar(0x00b0)), "Angle label must retain degree units");
    check(findRecord(catalog, wrongAngle).fallback == Stp::PmiFallback::None, "A mismatched source angle must not be replaced by measured geometry");
    check(findRecord(catalog, wrongDiameter).fallback == Stp::PmiFallback::None, "A mismatched source diameter must not be replaced by measured geometry");
}

QJsonObject inspectPmi(const Stp::CadDocument& document)
{
    const auto catalog = Stp::buildPmiCatalog(document);
    QJsonArray records;
    for (const auto& record : catalog.records) {
        QJsonObject item {
            {"id", record.id}, {"name", record.name}, {"kind", Stp::pmiKindText(record.kind)},
            {"status", Stp::pmiStatusText(record.status)}, {"semanticText", record.semanticText},
            {"reason", record.reason}, {"associationComplete", record.associationComplete},
            {"placementVerified", record.occurrencePlacementVerified}, {"safeForPartExport", record.safeForPartExport},
            {"ownerParts", QJsonArray::fromStringList(record.ownerPartIds)},
            {"ownerOccurrences", QJsonArray::fromStringList(record.ownerOccurrenceIds)}
        };
        if (record.status == Stp::PmiStatus::SourceGraphics || record.status == Stp::PmiStatus::SemanticFallback) {
            QString error;
            item["presentationCreated"] = !Stp::createPmiPresentation(record, &error).IsNull();
            item["presentationError"] = error;
        }
        records.append(item);
    }
    return {{"path", document.sourcePath}, {"parts", int(document.parts.size())},
            {"occurrences", int(document.occurrences.size())}, {"pmiCount", int(catalog.records.size())},
            {"sourceCompletenessVerified", catalog.sourceCompletenessVerified},
            {"diagnostics", QJsonArray::fromStringList(catalog.diagnostics)}, {"records", records}};
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--generate-sample")) {
            writeFixture(roundTripFixture(), QString::fromLocal8Bit(argv[2]));
            return 0;
        }
        if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--inspect")) {
            std::atomic_bool cancel{false};
            const auto document = Stp::loadStep(QString::fromLocal8Bit(argv[2]), {}, cancel);
            auto report = inspectPmi(*document);
            QTemporaryDir directory;
            check(directory.isValid(), "Cannot create inspection export directory");
            const auto split = Stp::exportParts(*document, {}, directory.path(), {}, cancel);
            QJsonArray exported;
            for (const auto& path : split.files) exported.append(inspectPmi(*Stp::loadStep(path, {}, cancel)));
            report["exportedParts"] = exported;
            report["exportDiagnostics"] = QJsonArray::fromStringList(split.diagnostics);
            QFile output(QString::fromLocal8Bit(argv[3]));
            check(output.open(QIODevice::WriteOnly), "Cannot write PMI inspection report");
            check(output.write(QJsonDocument(report).toJson()) >= 0, "Cannot save PMI inspection report");
            return 0;
        }
        Stp::CadDocument source;
        source.xde = newDocument();
        const auto shapes = XCAFDoc_DocumentTool::ShapeTool(source.xde->Main());
        const TDF_Label partA = shapes->AddShape(BRepPrimAPI_MakeBox(10, 10, 10), false);
        const TDF_Label partB = shapes->AddShape(BRepPrimAPI_MakeBox(gp_Pnt(20, 0, 0), 5, 5, 5), false);
        TDataStd_Name::Set(partA, "PartA"); TDataStd_Name::Set(partB, "PartB");
        Stp::rebuildIndex(source);
        const auto tool = XCAFDoc_DocumentTool::DimTolTool(source.xde->Main());
        const TDF_Label graphical = addDimension(tool, partA, 10, true);
        const TDF_Label semantic = addDimension(tool, partA, 10);
        const TDF_Label unsupported = addDimension(tool, partA, 10, false, true);
        const TDF_Label mismatch = addDimension(tool, partA, 11);
        const TDF_Label orphan = addDimension(tool, {}, 10);
        const TDF_Label crossPart = addDimension(tool, {}, 10);
        tool->SetDimension(partA, partB, crossPart);

        const TDF_Label datum = tool->AddDatum();
        Handle(XCAFDimTolObjects_DatumObject) datumObject = new XCAFDimTolObjects_DatumObject;
        datumObject->SetName(new TCollection_HAsciiString("A"));
        XCAFDoc_Datum::Set(datum)->SetObject(datumObject);
        TDF_LabelSequence datumShapes; datumShapes.Append(partA); tool->SetDatum(datumShapes, datum);
        const TDF_Label tolerance = tool->AddGeomTolerance();
        Handle(XCAFDimTolObjects_GeomToleranceObject) tolObject = new XCAFDimTolObjects_GeomToleranceObject;
        tolObject->SetType(XCAFDimTolObjects_GeomToleranceType_Position); tolObject->SetValue(0.1);
        XCAFDoc_GeomTolerance::Set(tolerance)->SetObject(tolObject);
        tool->SetGeomTolerance(partA, tolerance); tool->SetDatumToGeomTol(datum, tolerance);

        const auto catalog = Stp::buildPmiCatalog(source);
        check(catalog.records.size() == 8, "Catalog must include graphical, semantic, unsupported and orphan annotations");
        check(!catalog.sourceCompletenessVerified && !catalog.diagnostics.isEmpty(), "XDE-only source completeness must remain unknown");
        const auto& graphicsRecord = findRecord(catalog, graphical);
        check(graphicsRecord.status == Stp::PmiStatus::SourceGraphics, "Imported graphics should take precedence");
        check(!graphicsRecord.graphicalPresentation.IsNull(), "Graphical shape must be retained");
        check(graphicsRecord.sourceEntityIds.isEmpty(), "Do not invent a STEP entity ID from an XDE label");
        check(graphicsRecord.occurrencePlacementVerified, "Unique identity instance can follow explosion without a guessed transform");
        auto translated = source;
        for (auto& occurrence : translated.occurrences)
            if (occurrence.definitionLabel == partA) occurrence.worldTransform.SetTranslation(gp_Vec(20, 0, 0));
        check(!findRecord(Stp::buildPmiCatalog(translated), graphical).occurrencePlacementVerified,
              "Non-identity instance must not claim a verified PMI transform");
        check(!findRecord(Stp::buildPmiCatalog(translated), graphical).safeForPartExport,
              "Unverified assembly coordinates must not be copied into a part-local export");
        auto repeated = source;
        auto duplicate = *std::find_if(source.occurrences.begin(), source.occurrences.end(),
                                      [&](const auto& occurrence) { return occurrence.definitionLabel == partA; });
        duplicate.id += QStringLiteral("-repeated"); repeated.occurrences.push_back(duplicate);
        check(!findRecord(Stp::buildPmiCatalog(repeated), graphical).occurrencePlacementVerified,
              "Repeated instances must not claim a unique PMI transform");
        check(findRecord(catalog, semantic).fallback == Stp::PmiFallback::Linear, "Complete simple linear dimension should render");
        check(findRecord(catalog, unsupported).fallback == Stp::PmiFallback::None, "Modifiers must never be silently dropped by fallback");
        check(findRecord(catalog, unsupported).semanticText.contains(QStringLiteral("修饰符")), "Modifier remains visible in semantic table");
        check(!findRecord(catalog, unsupported).semanticDetails.isEmpty(), "Raw semantic data must remain inspectable");
        check(findRecord(catalog, mismatch).fallback == Stp::PmiFallback::None, "Geometric measure must not overwrite source nominal dimension");
        check(findRecord(catalog, orphan).status == Stp::PmiStatus::MissingAssociation, "Missing association is explicitly reported");
        check(findRecord(catalog, crossPart).crossPart && !findRecord(catalog, crossPart).safeForPartExport, "Cross-part annotations cannot be copied as single-part PMI");
        QString error;
        check(!Stp::createPmiPresentation(graphicsRecord, &error).IsNull() && error.isEmpty(), "Imported graphical presentation should create an AIS object");
        check(!Stp::createPmiPresentation(findRecord(catalog, semantic), &error).IsNull(), "Valid linear semantic presentation should create an AIS object");
        check(Stp::createPmiPresentation(findRecord(catalog, unsupported), &error).IsNull() && !error.isEmpty(), "Unsupported presentation must report why");
        bool workerRejected = false;
        std::thread worker([&] { QString workerError; workerRejected = Stp::createPmiPresentation(graphicsRecord, &workerError).IsNull() && !workerError.isEmpty(); });
        worker.join(); check(workerRejected, "AIS creation must reject non-GUI thread");

        const auto partIt = std::find_if(source.parts.begin(), source.parts.end(), [&](const auto& part) { return part.label == partA; });
        check(partIt != source.parts.end(), "Part index must be available");
        const int partIndex = static_cast<int>(std::distance(source.parts.begin(), partIt));
        Handle(TDocStd_Document) target = newDocument();
        check(XCAFDoc_Editor::Extract(partA, target->Main()), "Exact part extraction failed");
        QStringList diagnostics;
        Stp::copyIndependentPmi(source, partIndex, target, diagnostics);
        Stp::CadDocument exported; exported.xde = target; Stp::rebuildIndex(exported);
        const auto exportedCatalog = Stp::buildPmiCatalog(exported);
        check(exportedCatalog.records.size() == 6, "Independent same-part annotations including modifiers and datum links must copy");
        const auto copiedTool = XCAFDoc_DocumentTool::DimTolTool(target->Main());
        TDF_LabelSequence copiedTolerances; copiedTool->GetGeomToleranceLabels(copiedTolerances);
        check(copiedTolerances.Length() == 1, "Tolerance should survive part extraction");
        TDF_LabelSequence linkedDatums; copiedTool->GetDatumOfTolerLabels(copiedTolerances.First(), linkedDatums);
        check(linkedDatums.Length() == 1 && linkedDatums.First().Data() == target->GetData(), "Tolerance must refer to target document datum, never source label");
        check(diagnostics.join('\n').contains(QStringLiteral("跨越多个零件")), "Cross-part export omission must be explicit");
        check(diagnostics.join('\n').contains(QStringLiteral("关联不完整")), "Orphan export omission must be explicit");
        Handle(TDocStd_Document) translatedTarget = newDocument();
        check(XCAFDoc_Editor::Extract(partA, translatedTarget->Main()), "Translated part extraction failed");
        QStringList translatedDiagnostics;
        Stp::copyIndependentPmi(translated, partIndex, translatedTarget, translatedDiagnostics);
        Stp::CadDocument translatedExport; translatedExport.xde = translatedTarget; Stp::rebuildIndex(translatedExport);
        check(Stp::buildPmiCatalog(translatedExport).records.empty(), "Do not copy annotations with unverified local coordinates");
        check(translatedDiagnostics.join('\n').contains(QStringLiteral("坐标映射未验证")), "Coordinate export omission must be explicit");
        check(Stp::buildPmiCatalog(source).records.size() == 8, "Export must not mutate the source PMI catalog");
        QTemporaryDir temporary;
        check(temporary.isValid(), "Cannot create PMI test directory");
        const auto fixturePath = QDir(temporary.path()).filePath(QStringLiteral("pmi-basic.step"));
        writeFixture(roundTripFixture(), fixturePath);
        std::atomic_bool cancel{false};
        const auto loaded = Stp::loadStep(fixturePath, {}, cancel);
        checkReadBack(*loaded);
        const auto split = Stp::exportParts(*loaded, {}, temporary.path(), {}, cancel);
        check(split.files.size() == 1, "Independent PMI fixture must export exactly one part");
        checkReadBack(*Stp::loadStep(split.files.front(), {}, cancel));
        checkBasicSemanticPresentations();
        std::cout << "STP PMI tests passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "STP PMI test failure: " << failure.what() << '\n'; return 1;
    }
}
