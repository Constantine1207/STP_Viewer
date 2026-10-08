#pragma once

#include <QString>
#include <QStringList>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Stp {

using ProgressCallback = std::function<void(int, const QString&)>;

struct PartDefinition {
    QString id, name, number, material;
    TDF_Label label;
    TopoDS_Shape shape; // Exact definition geometry, independent of occurrence/view transforms.
    std::optional<double> densityKgM3, volumeMm3, areaMm2, massKg;
    // Session-only supplements for missing source fields. Never written into XDE
    // or substituted for original attributes during exact STEP part export.
    std::optional<QString> manualMaterial;
    std::optional<double> manualDensityKgM3;
    std::array<double, 3> bboxMm {{0, 0, 0}};
    bool bboxValid = false;
    bool validSolid = false;
};

struct Occurrence {
    QString id, name, path;
    int parent = -1;
    int partIndex = -1; // -1 denotes an assembly/group, never an invented part.
    TDF_Label label, definitionLabel;
    gp_Trsf localTransform, worldTransform;
    std::vector<int> children;
};

struct CadDocument {
    Handle(TDocStd_Document) xde;
    QString sourcePath;
    QString sourceUnits;
    QStringList diagnostics;
    std::vector<PartDefinition> parts;
    std::vector<Occurrence> occurrences;
    bool occurrenceScopeApplied = false; // Distinguishes full-source quantities from checked scope.
    // PMI ownership and imported-coordinate confidence use the complete source
    // assembly even when a checkbox scope omits other parts/instances.
    std::shared_ptr<const CadDocument> sourceAssemblyContext;
};

QString labelEntry(const TDF_Label& label);
QString labelName(const TDF_Label& label);
std::shared_ptr<CadDocument> loadStep(const QString& path, const ProgressCallback& progress,
                                     std::atomic_bool& cancel);
// Indexes an existing XDE document without altering its geometry or attributes.
void rebuildIndex(CadDocument& document, const ProgressCallback& progress = {},
                  std::atomic_bool* cancel = nullptr);
std::vector<int> partQuantities(const CadDocument& document);
std::vector<int> selectedPartIndices(const CadDocument& document,
                                   const std::vector<int>& selectedOccurrences);
bool materialEditable(const PartDefinition& part);
bool densityEditable(const PartDefinition& part);
QString effectiveMaterial(const PartDefinition& part);
std::optional<double> effectiveDensity(const PartDefinition& part);
std::optional<double> effectiveMass(const PartDefinition& part);
// Empty input clears the supplement. Imported fields cannot be overwritten.
// A false result leaves the previous value unchanged.
bool setManualMaterial(PartDefinition& part, const QString& value);
bool setManualDensity(PartDefinition& part, const QString& value, QString* error = nullptr);
// Keeps exactly the supplied valid leaf occurrences and their ancestors, with
// remapped vector references. Empty means empty, unlike selectedPartIndices().
// Source labels, geometry, original placements and XDE are retained unmodified.
CadDocument scopedBomDocument(const CadDocument& document,
                              const std::vector<int>& includedLeafOccurrences);

} // namespace Stp
