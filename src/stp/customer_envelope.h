#pragma once
#include "part_placement.h"
#include <QColor>
#include <QJsonObject>
#include "color_edits.h"

namespace Stp {
struct EnvelopeProtection {
    // Exact world-space tool volume; this shape is never exported as a part.
    TopoDS_Shape shape;
    QString label;
    bool allowOriginalCut = false;
};
struct EnvelopePiece {
    TopoDS_Shape shape;
    QColor color;
    int sourceOccurrence = -1; // Session-only; not written to STEP.
};
struct EnvelopeOptions {
    bool fillClosedCavities = true;
    bool repairInputs = false;
    QColor color = QColor("#98afc7");
    std::vector<int> preservedOccurrences;
    ColorEdits preservedColors;
    std::vector<EnvelopeProtection> protections;
};
struct EnvelopeInputIssue {
    int occurrence = -1, solidCount = 0;
    QString name, path, repairMethod;
    bool valid = false, repaired = false;
    double originalVolume = 0, preparedVolume = 0, boundsChange = 0;
    QStringList before, after;
};
struct EnvelopeInspection {
    std::vector<EnvelopeInputIssue> issues;
    std::vector<EnvelopePiece> originalPieces, preparedPieces;
    int invalidCount() const;
    QString text() const;
    QJsonObject report() const;
};
EnvelopeInspection inspectEnvelopeInputs(const CadDocument&, const PlacementEdits&,
    const std::vector<int>& selected, bool repair, std::atomic_bool&, const ProgressCallback& = {});
// Local origin is the box minimum corner or cylinder bottom centre; dimensions in mm.
TopoDS_Shape envelopeProtectionPrimitive(bool cylinder, double x, double y, double z,
    const gp_Trsf& placement);
// Candidates only. Callers must obtain explicit user confirmation before applying them.
std::map<int, QString> envelopePreservationCandidates(const CadDocument&, const QStringList& keywords);
struct EnvelopeResult {
    TopoDS_Shape original, shape;
    std::vector<EnvelopePiece> pieces, originalPieces;
    int preservedOccurrences = 0, processedOccurrences = 0;
    double overlapVolume = 0, removedVolume = 0, removedOriginalVolume = 0;
    std::array<double,6> originalBounds{};
    int inputOccurrences = 0, inputSolids = 0, outputSolids = 0, filledCavities = 0;
    double inputVolume = 0, outputVolume = 0;
    std::array<double,6> bounds{};
    QStringList warnings;
    QJsonObject inputInspection;
    QJsonObject report() const;
};
// Empty selection is empty, never implicitly the whole assembly. Group IDs expand to leaves.
std::vector<int> envelopeScope(const CadDocument&, const std::vector<int>& selection);
EnvelopeResult buildCustomerEnvelope(const CadDocument&, const PlacementEdits&,
    const std::vector<int>& selected, const EnvelopeOptions&, std::atomic_bool&,
    const ProgressCallback& = {}, EnvelopeInspection* inspection = nullptr);
std::shared_ptr<CadDocument> envelopeDocument(const TopoDS_Shape&, const QColor&);
std::shared_ptr<CadDocument> envelopeDocument(const EnvelopeResult&, const QColor&);
// Fresh metadata-free XDE; atomic write only after STEP read-back validation.
void exportCustomerEnvelope(const EnvelopeResult&, const EnvelopeOptions&, const QString& path,
    const QString& sourcePath, std::atomic_bool&, const ProgressCallback& = {});
} // namespace Stp
