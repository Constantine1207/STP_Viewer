#pragma once

#include <AIS_InteractiveObject.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <QString>
#include <QStringList>
#include <vector>

namespace Stp {

struct CadDocument;

enum class PmiKind { Dimension, Datum, GeometricTolerance, Graphical, Legacy };
enum class PmiStatus {
    SourceGraphics,
    SemanticFallback,
    SemanticOnly,
    MissingAssociation,
    Unsupported,
    Failed
};
enum class PmiFallback { None, Linear, Angular, Radius, Diameter, DatumText };
enum class PmiCoordinateSpace {
    // Exactly as imported by XDE. Do not duplicate or multiply by worldTransform:
    // XDE does not provide a reliable per-PMI representation-context transform.
    ImportedCoordinates
};

// Pure document data. Building this catalog never creates an AIS object.
struct PmiRecord {
    QString id;
    PmiKind kind = PmiKind::Graphical;
    PmiStatus status = PmiStatus::SemanticOnly;
    TDF_Label sourceLabel;
    QString sourceLabelEntry;
    // Empty means the importer did not preserve a reliable original STEP entity ID.
    QStringList sourceEntityIds;
    QString name;
    QString semanticText;
    QString semanticDetails; // OCCT's full object dump, including untranslated enum values.
    QString reason;
    QStringList ownerPartIds;
    QStringList ownerOccurrenceIds;
    std::vector<TDF_Label> referencedLabels;
    bool crossPart = false;
    bool associationComplete = false;
    bool safeForPartExport = false;
    PmiCoordinateSpace coordinateSpace = PmiCoordinateSpace::ImportedCoordinates;
    bool occurrencePlacementVerified = false;
    TopoDS_Shape graphicalPresentation;
    PmiFallback fallback = PmiFallback::None;
    gp_Ax2 annotationPlane;
    gp_Pnt textPosition;
    gp_Pnt point1;
    gp_Pnt point2;
    gp_Pnt angleCenter;
    gp_Circ circle;
    double value = 0.0;
    QString valueText;
    QString datumText;
};

struct PmiCatalog {
    std::vector<PmiRecord> records;
    QStringList diagnostics;
    // OCCT XDE alone cannot prove that all source STEP annotation entities survived.
    bool sourceCompletenessVerified = false;
};

PmiCatalog buildPmiCatalog(const CadDocument& document);
QString pmiKindText(PmiKind kind);
QString pmiStatusText(PmiStatus status);

// GUI thread only. World/explosion transforms are applied by the caller, never to XDE.
// A null result has an explicit error. A record with no supported presentation also
// returns null: its semantic information remains available in the catalog/table.
Handle(AIS_InteractiveObject) createPmiPresentation(const PmiRecord& record,
                                                  QString* error = nullptr);

// Only single-part, fully resolved associations with verified definition-local
// placement can be preserved in a part export.
// The exporter must still copy the label graph and verify the result after writing.
bool pmiCanExportWithPart(const PmiRecord& record, const QString& partId);

// Target already contains an exact XCAFDoc_Editor::Extract of this definition.
// Copies supported same-part semantic labels and remaps every shape/datum link.
// Each rejected record produces an explicit export diagnostic.
void copyIndependentPmi(const CadDocument& document, int partIndex,
                        const Handle(TDocStd_Document)& target, QStringList& diagnostics);

} // namespace Stp
