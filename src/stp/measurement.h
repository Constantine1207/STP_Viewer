#pragma once

#include "cad_document.h"
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax1.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <QMetaType>

namespace Stp {

enum class MeasurementKind { Point, Length, Radius, Diameter, Area, Volume, Mass, CenterOfMass, Distance, Angle, VolumeCenter };
enum class MeasurementSelection { Vertex, PointOnGeometry, Edge, Face, Solid, Part, FeatureCenter, FeatureAxis };
enum class MeasurementCoordinates { Original, Displayed };
enum class MeasurementDirection { Geometry, FeatureAxis };
enum class MeasurementExtent { Bounded, Infinite };

// Definition-local topology and parameters survive replacement of AIS owners.
struct MeasurementEntity {
    int occurrence = -1;
    QString occurrenceId, name;
    TopoDS_Shape shape;
    MeasurementSelection selection = MeasurementSelection::Part;
    MeasurementDirection direction = MeasurementDirection::Geometry;
    MeasurementExtent extent = MeasurementExtent::Bounded;
    std::optional<gp_Pnt> pickedPoint;
    std::optional<double> curveParameter;
    std::optional<std::array<double, 2>> surfaceParameters;
    gp_Trsf originalTransform, displayTransform;
    std::optional<double> densityKgM3;
};

struct MeasurementRequest {
    MeasurementKind kind = MeasurementKind::Distance;
    MeasurementCoordinates coordinates = MeasurementCoordinates::Original;
    std::vector<MeasurementEntity> a, b;
};

struct MeasurementValue { QString label, unit; double value = 0; };
struct MeasurementResult {
    bool success = false;
    QString error, note;
    std::vector<MeasurementValue> values;
    std::vector<gp_Pnt> anchors;
    std::optional<gp_Circ> circle;
    std::optional<gp_Dir> directionA, directionB;
};

struct MeasurementRecord {
    quint64 id = 0;
    MeasurementRequest request;
    MeasurementResult result;
    bool visible = true;
};

QString measurementKindText(MeasurementKind kind);
QString measurementCoordinatesText(MeasurementCoordinates coordinates);
QString measurementResultText(const MeasurementResult& result, int decimals = 3);
struct MeasurementFeature {
    std::optional<gp_Pnt> center;
    std::optional<gp_Ax1> axis;
};
// Resolve exact derived geometry from its retained source topology and placement.
std::optional<MeasurementFeature> measurementFeatureReference(const MeasurementEntity& entity,
    MeasurementCoordinates coordinates, QString* error = nullptr);
struct MeasurementReference {
    TopoDS_Shape bounded;
    std::optional<gp_Lin> line;
    std::optional<gp_Pln> plane;
    gp_Pnt anchor;
};
// Shared world-space reference; source topology and pick parameters stay bounded.
std::optional<MeasurementReference> measurementReference(const MeasurementEntity& entity,
    MeasurementCoordinates coordinates, QString* error = nullptr);
QString measurementReferenceText(const MeasurementEntity& entity);
MeasurementResult calculateMeasurement(const MeasurementRequest& request, std::atomic_bool& cancel,
                                      const ProgressCallback& progress = {});
// Project a detected sample onto bounded precise topology, in definition coordinates.
bool refineMeasurementPick(MeasurementEntity& entity, const gp_Pnt& localSample, QString* error = nullptr);

} // namespace Stp
Q_DECLARE_METATYPE(Stp::MeasurementEntity)
