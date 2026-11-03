#pragma once
#include "part_placement.h"
#include <QColor>
#include <QJsonObject>

namespace Stp {
struct EnvelopeOptions {
    bool fillClosedCavities = true;
    QColor color = QColor("#98afc7");
};
struct EnvelopeResult {
    TopoDS_Shape original, shape;
    int inputOccurrences = 0, inputSolids = 0, outputSolids = 0, filledCavities = 0;
    double inputVolume = 0, outputVolume = 0;
    std::array<double,6> bounds{};
    QStringList warnings;
    QJsonObject report() const;
};
// Empty selection is empty, never implicitly the whole assembly. Group IDs expand to leaves.
std::vector<int> envelopeScope(const CadDocument&, const std::vector<int>& selection);
EnvelopeResult buildCustomerEnvelope(const CadDocument&, const PlacementEdits&,
    const std::vector<int>& selected, const EnvelopeOptions&, std::atomic_bool&,
    const ProgressCallback& = {});
std::shared_ptr<CadDocument> envelopeDocument(const TopoDS_Shape&, const QColor&);
// Fresh metadata-free XDE; atomic write only after STEP read-back validation.
void exportCustomerEnvelope(const EnvelopeResult&, const EnvelopeOptions&, const QString& path,
    const QString& sourcePath, std::atomic_bool&, const ProgressCallback& = {});
} // namespace Stp
