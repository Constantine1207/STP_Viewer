#include "stp/measurement.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Builder.hxx>
#include <STEPControl_Writer.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BezierCurve.hxx>
#include <Precision.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace Stp;
const double pi = std::acos(-1.);
void check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}
bool approximatelyEqual(double a, double b, double tolerance = 1.e-6) {
    return std::abs(a - b) <= tolerance * std::max({1., std::abs(a), std::abs(b)});
}
void checkPoint(const gp_Pnt& actual, const gp_Pnt& expected, const char* description, double tolerance = 1.e-6) {
    check(actual.Distance(expected) < tolerance, description);
}
MeasurementEntity entity(const TopoDS_Shape& shape, MeasurementSelection selection, int occurrence = 0) {
    MeasurementEntity result;
    result.shape = shape; result.selection = selection; result.occurrence = occurrence;
    result.occurrenceId = QString::number(occurrence); result.name = QStringLiteral("计算测试");
    return result;
}
MeasurementResult calculate(MeasurementKind kind, std::vector<MeasurementEntity> a,
                            std::vector<MeasurementEntity> b = {},
                            MeasurementCoordinates coordinates = MeasurementCoordinates::Original) {
    std::atomic_bool cancel{false};
    auto result = calculateMeasurement({kind, coordinates, std::move(a), std::move(b)}, cancel);
    if (!result.success) std::cerr << result.error.toStdString() << '\n';
    check(result.success, "expected successful measurement");
    check(!result.values.empty(), "successful measurement has no numeric results");
    for (const auto& value : result.values) check(std::isfinite(value.value), "non-finite measurement output");
    return result;
}
MeasurementResult failure(MeasurementKind kind, std::vector<MeasurementEntity> a,
                          std::vector<MeasurementEntity> b = {}) {
    std::atomic_bool cancel{false};
    const auto result = calculateMeasurement({kind, MeasurementCoordinates::Original, std::move(a), std::move(b)}, cancel);
    check(!result.success && !result.error.isEmpty() && result.values.empty(), "failure needs an explicit error and no stale values");
    return result;
}
TopoDS_Face surfaceFace(const TopoDS_Shape& shape, GeomAbs_SurfaceType type) {
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const auto face = TopoDS::Face(explorer.Current());
        if (BRepAdaptor_Surface(face).GetType() == type) return face;
    }
    throw std::runtime_error("analytic fixture face missing");
}
TopoDS_Face yzFace(double x) {
    return BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(x, 0, 0), gp_Dir(1, 0, 0)), -2, 2, -2, 2).Face();
}
TopoDS_Face holedFace() {
    BRepBuilderAPI_MakePolygon outer;
    outer.Add(gp_Pnt(0, 0, 0)); outer.Add(gp_Pnt(10, 0, 0));
    outer.Add(gp_Pnt(10, 10, 0)); outer.Add(gp_Pnt(0, 10, 0)); outer.Close();
    BRepBuilderAPI_MakePolygon inner;
    inner.Add(gp_Pnt(3, 3, 0)); inner.Add(gp_Pnt(7, 3, 0));
    inner.Add(gp_Pnt(7, 7, 0)); inner.Add(gp_Pnt(3, 7, 0)); inner.Close();
    BRepBuilderAPI_MakeFace maker(outer.Wire());
    maker.Add(TopoDS::Wire(inner.Wire().Reversed()));
    return maker.Face();
}

void distanceTests() {
    const std::vector<MeasurementEntity> left {
        entity(BRepBuilderAPI_MakeVertex(gp_Pnt(0, 0, 0)), MeasurementSelection::Vertex),
        entity(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(0, 4, 0)), MeasurementSelection::Edge),
        entity(yzFace(0), MeasurementSelection::Face),
        entity(BRepPrimAPI_MakeBox(gp_Pnt(-2, -2, -2), gp_Pnt(0, 2, 2)), MeasurementSelection::Solid)
    };
    auto right = left;
    for (auto& item : right) {
        item.occurrence = 1; item.occurrenceId = QStringLiteral("1");
        // The right-hand solid's nearest face is 10 mm from x = 0.
        item.originalTransform.SetTranslation(gp_Vec(item.selection == MeasurementSelection::Solid ? 12 : 10, 0, 0));
        item.displayTransform = item.originalTransform;
    }
    int pairs = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        for (size_t j = i; j < right.size(); ++j) {
            const auto result = calculate(MeasurementKind::Distance, {left[i]}, {right[j]});
            check(approximatelyEqual(result.values.front().value, 10), "one of the ten point/edge/face/solid pairs has wrong distance");
            check(result.anchors.size() == 2 && approximatelyEqual(result.anchors[0].Distance(result.anchors[1]), 10), "distance witnesses disagree with distance");
            const auto reverse = calculate(MeasurementKind::Distance, {right[j]}, {left[i]});
            check(approximatelyEqual(reverse.values.front().value, 10), "distance is not symmetric");
            ++pairs;
        }
    }
    check(pairs == 10, "distance pair coverage incomplete");
    auto external = entity(BRepBuilderAPI_MakeVertex(gp_Pnt(10, 10, 0)), MeasurementSelection::Vertex);
    const auto edge = calculate(MeasurementKind::Distance, {external}, {left[1]});
    check(approximatelyEqual(edge.values.front().value, std::sqrt(136.)), "edge was measured as an unbounded supporting line");
    const auto face = calculate(MeasurementKind::Distance, {external}, {left[2]});
    check(approximatelyEqual(face.values.front().value, std::sqrt(164.)), "face was measured as an unbounded supporting plane");
    const auto box = entity(BRepPrimAPI_MakeBox(10, 10, 10), MeasurementSelection::Solid);
    const auto inside = entity(BRepBuilderAPI_MakeVertex(gp_Pnt(2, 3, 4)), MeasurementSelection::Vertex);
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {inside}, {box}).values.front().value, 0), "point inside solid must have zero minimum distance");
    BRep_Builder builder; TopoDS_Compound compound; builder.MakeCompound(compound); builder.Add(compound, box.shape);
    auto compoundPart = entity(compound, MeasurementSelection::Part, 4);
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {inside}, {compoundPart}).values.front().value, 0),
          "point inside compound part must have zero minimum distance");
    auto remote = box; remote.occurrence = 5; remote.occurrenceId = QStringLiteral("5");
    remote.originalTransform.SetTranslation(gp_Vec(50, 0, 0));
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {inside}, {box, remote}).values.front().value, 0),
          "point inside one body of selected group must have zero minimum distance");
    const auto contained = entity(BRepPrimAPI_MakeBox(gp_Pnt(1, 1, 1), gp_Pnt(2, 2, 2)), MeasurementSelection::Solid, 2);
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {contained}, {box}).values.front().value, 0), "contained solid must have zero distance");
    const auto touching = entity(BRepPrimAPI_MakeBox(gp_Pnt(10, 0, 0), gp_Pnt(20, 10, 10)), MeasurementSelection::Solid, 3);
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {box}, {touching}).values.front().value, 0), "touching solids must have zero distance");
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {box}, {box}).values.front().value, 0), "coincident shapes must have zero distance");
    auto picked = left[2]; picked.selection = MeasurementSelection::PointOnGeometry;
    picked.pickedPoint = gp_Pnt(0, 2, 0);
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {picked}, {left[0]}).values.front().value, 2), "point-on-face was accidentally measured as whole face");
    check(approximatelyEqual(calculate(MeasurementKind::Distance, {right[0], right[1]}, {left[0]}).values.front().value, 10), "group minimum distance failed");
    failure(MeasurementKind::Distance, {left[0]});
}

void scalarAndCenterTests() {
    const auto line = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)), MeasurementSelection::Edge);
    const auto length = calculate(MeasurementKind::Length, {line});
    check(approximatelyEqual(length.values.front().value, 10), "linear edge length wrong");
    checkPoint(length.anchors.front(), gp_Pnt(5, 0, 0), "linear edge center wrong");
    const gp_Circ circle(gp_Ax2(gp_Pnt(3, 4, 5), gp_Dir(0, 0, 1)), 5);
    auto arc = entity(BRepBuilderAPI_MakeEdge(circle, 0, pi / 2.), MeasurementSelection::Edge);
    const auto arcLength = calculate(MeasurementKind::Length, {arc});
    check(approximatelyEqual(arcLength.values.front().value, 5 * pi / 2.), "arc length wrong");
    checkPoint(arcLength.anchors.front(), gp_Pnt(3 + 10 / pi, 4 + 10 / pi, 5), "arc linear centroid must differ from circle center");
    const auto radius = calculate(MeasurementKind::Radius, {arc});
    check(approximatelyEqual(radius.values.front().value, 5) && radius.circle.has_value(), "arc analytic radius wrong");
    checkPoint(radius.anchors.front(), circle.Location(), "arc radius center wrong");
    check(approximatelyEqual(calculate(MeasurementKind::Diameter, {arc}).values.front().value, 10), "arc diameter wrong");
    auto cylinder = entity(surfaceFace(BRepPrimAPI_MakeCylinder(5, 20), GeomAbs_Cylinder), MeasurementSelection::Face);
    check(approximatelyEqual(calculate(MeasurementKind::Radius, {cylinder}).values.front().value, 5), "cylinder radius wrong");
    check(approximatelyEqual(calculate(MeasurementKind::Diameter, {cylinder}).values.front().value, 10), "cylinder diameter wrong");
    auto sphere = entity(surfaceFace(BRepPrimAPI_MakeSphere(gp_Pnt(1, 2, 3), 7), GeomAbs_Sphere), MeasurementSelection::Face);
    const auto sphereRadius = calculate(MeasurementKind::Radius, {sphere});
    check(approximatelyEqual(sphereRadius.values.front().value, 7) && !sphereRadius.circle, "sphere radius should not invent a measured circle");
    checkPoint(sphereRadius.anchors.front(), gp_Pnt(1, 2, 3), "sphere center wrong");
    const auto planar = entity(holedFace(), MeasurementSelection::Face);
    const auto area = calculate(MeasurementKind::Area, {planar});
    check(approximatelyEqual(area.values.front().value, 84), "trimmed hole ignored by surface area");
    checkPoint(area.anchors.front(), gp_Pnt(5, 5, 0), "surface area centroid wrong");
    auto box = entity(BRepPrimAPI_MakeBox(2, 3, 4), MeasurementSelection::Solid);
    const auto volume = calculate(MeasurementKind::Volume, {box});
    check(approximatelyEqual(volume.values.front().value, 24), "solid volume wrong");
    check(volume.values.size() >= 2 && volume.values[1].label == QStringLiteral("表面积")
          && approximatelyEqual(volume.values[1].value, 52), "volume result must include true surface area");
    checkPoint(volume.anchors.front(), gp_Pnt(1, 1.5, 2), "solid volume centroid wrong");
    check(approximatelyEqual(calculate(MeasurementKind::Area, {box}).values.front().value, 52), "solid surface area wrong");
    const auto sphereVolume = calculate(MeasurementKind::Volume,
        {entity(BRepPrimAPI_MakeSphere(7), MeasurementSelection::Solid)});
    check(approximatelyEqual(sphereVolume.values.front().value, 4. / 3. * pi * 343., 1.e-5), "sphere volume must be true volume, not bounding-box volume");
    check(approximatelyEqual(calculate(MeasurementKind::Area,
        {entity(BRepPrimAPI_MakeSphere(7), MeasurementSelection::Solid)}).values.front().value, 4 * pi * 49., 1.e-5), "sphere surface area wrong");
    auto duplicate = box;
    check(approximatelyEqual(calculate(MeasurementKind::Volume, {box, duplicate}).values.front().value, 24), "same occurrence/shape was counted twice");
    duplicate.occurrence = 1; duplicate.occurrenceId = QStringLiteral("1");
    check(approximatelyEqual(calculate(MeasurementKind::Volume, {box, duplicate}).values.front().value, 48), "overlapping repeated occurrence was improperly merged");
    duplicate.originalTransform.SetTranslation(gp_Vec(10, 0, 0));
    const auto assembly = calculate(MeasurementKind::Volume, {box, duplicate});
    check(approximatelyEqual(assembly.values.front().value, 48), "assembly volume sum wrong");
    checkPoint(assembly.anchors.front(), gp_Pnt(6, 1.5, 2), "assembly volume centroid wrong");
    const auto sourceVolume = box.shape;
    check(box.shape.IsSame(sourceVolume) && box.shape.Location().IsIdentity(), "measurement mutated source topology location");
    failure(MeasurementKind::Volume, {planar});
    failure(MeasurementKind::Mass, {box});
    failure(MeasurementKind::Radius, {line});
    failure(MeasurementKind::Length, {box});
    failure(MeasurementKind::Point, {line});
}

void densityAndTransformTests() {
    auto first = entity(BRepPrimAPI_MakeBox(10, 10, 10), MeasurementSelection::Solid, 1);
    auto second = first; second.occurrence = 2; second.occurrenceId = QStringLiteral("2");
    second.originalTransform.SetTranslation(gp_Vec(20, 0, 0));
    first.densityKgM3 = 1000.; second.densityKgM3 = 3000.;
    const auto mass = calculate(MeasurementKind::Mass, {first, second});
    check(approximatelyEqual(mass.values.front().value, 0.004), "density unit conversion from mm3 to kg wrong");
    checkPoint(mass.anchors.front(), gp_Pnt(20, 5, 5), "different-density assembly center must be mass weighted");
    const auto center = calculate(MeasurementKind::CenterOfMass, {first, second});
    checkPoint(center.anchors.front(), gp_Pnt(20, 5, 5), "center of mass differs from mass measurement center");
    check(center.note.contains(QStringLiteral("质量加权")), "mass center must identify density weighting");
    const auto volumeX = std::find_if(center.values.begin(), center.values.end(), [](const auto& value) {
        return value.label == QStringLiteral("体积中心 X");
    });
    const auto massX = std::find_if(center.values.begin(), center.values.end(), [](const auto& value) {
        return value.label == QStringLiteral("质量中心 X");
    });
    check(volumeX != center.values.end() && massX != center.values.end() && approximatelyEqual(volumeX->value, 15.)
          && approximatelyEqual(massX->value, 20.), "geometric and physical centers must be distinctly reported");
    second.densityKgM3.reset();
    const auto noMassCenter = failure(MeasurementKind::CenterOfMass, {first, second});
    check(noMassCenter.error.contains(QStringLiteral("缺少有效密度")), "missing-density mass center must fail explicitly");
    failure(MeasurementKind::Mass, {first, second});
    checkPoint(calculate(MeasurementKind::VolumeCenter, {first, second}).anchors.front(), gp_Pnt(15, 5, 5), "independent volume center requires no density");
    for (double density : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()}) {
        second.densityKgM3 = density;
        checkPoint(calculate(MeasurementKind::VolumeCenter, {first, second}).anchors.front(), gp_Pnt(15, 5, 5),
                   "density must never affect or break volume center calculation");
        checkPoint(calculate(MeasurementKind::Volume, {first, second}).anchors.front(), gp_Pnt(15, 5, 5),
                   "density must never affect or break volume calculation");
        if (!std::isfinite(density) || density <= 0) {
            failure(MeasurementKind::Mass, {second}); failure(MeasurementKind::CenterOfMass, {second});
        }
    }
    failure(MeasurementKind::VolumeCenter, {entity(yzFace(0), MeasurementSelection::Face)});
    failure(MeasurementKind::CenterOfMass, {entity(yzFace(0), MeasurementSelection::Face)});
    auto point = entity(BRepBuilderAPI_MakeVertex(gp_Pnt(1, 0, 0)), MeasurementSelection::Vertex);
    point.originalTransform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), pi / 2.);
    point.originalTransform.SetTranslationPart(gp_Vec(10, 20, 30));
    point.displayTransform = point.originalTransform;
    point.displayTransform.SetTranslationPart(gp_Vec(100, 200, 300));
    checkPoint(calculate(MeasurementKind::Point, {point}).anchors.front(), gp_Pnt(10, 21, 30), "original instance coordinate transform wrong");
    checkPoint(calculate(MeasurementKind::Point, {point}, {}, MeasurementCoordinates::Displayed).anchors.front(),
               gp_Pnt(100, 201, 300), "displayed transform ignored");
    first.originalTransform = point.originalTransform;
    first.displayTransform = point.displayTransform;
    checkPoint(calculate(MeasurementKind::Volume, {first}).anchors.front(), gp_Pnt(5, 25, 35), "rotated solid centroid wrong");
    checkPoint(calculate(MeasurementKind::CenterOfMass, {first}, {}, MeasurementCoordinates::Displayed).anchors.front(),
               gp_Pnt(95, 205, 305), "displayed center of mass ignored");
    point.originalTransform.SetScale(gp_Pnt(), 2);
    failure(MeasurementKind::Point, {point});
    BRep_Builder builder; TopoDS_Solid invalid; builder.MakeSolid(invalid);
    failure(MeasurementKind::Volume, {entity(invalid, MeasurementSelection::Solid)});
    TopoDS_Compound mixed; builder.MakeCompound(mixed); builder.Add(mixed, first.shape); builder.Add(mixed, yzFace(100));
    failure(MeasurementKind::Volume, {entity(mixed, MeasurementSelection::Part)});
}

void angleTests() {
    auto x = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0)), MeasurementSelection::Edge);
    auto y = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(0, 10, 0)), MeasurementSelection::Edge);
    auto diagonal = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(10, 10, 0)), MeasurementSelection::Edge);
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {x}, {x}).values.front().value, 0), "parallel direction must return zero angle");
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {x}, {y}).values.front().value, 90), "perpendicular line angle wrong");
    const auto fortyFive = calculate(MeasurementKind::Angle, {x}, {diagonal});
    check(approximatelyEqual(fortyFive.values.front().value, 45), "45-degree line angle wrong");
    check(fortyFive.anchors.size() == 3 && fortyFive.directionA && fortyFive.directionB, "angle overlay geometry missing");
    auto reversed = x; reversed.shape.Reverse();
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {reversed}, {diagonal}).values.front().value, 45), "edge orientation changed angle");
    auto xy = entity(BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(), gp_Dir(0, 0, 1)), -10, 10, -10, 10), MeasurementSelection::Face);
    auto tilted = entity(BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(), gp_Dir(0, 1, 1)), -10, 10, -10, 10), MeasurementSelection::Face);
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {xy}, {tilted}).values.front().value, 45), "plane/plane angle wrong");
    tilted.shape.Reverse();
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {xy}, {tilted}).values.front().value, 45), "face orientation changed unsigned angle");
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {x}, {xy}).values.front().value, 0), "line lying parallel to plane must have zero angle");
    auto z = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(), gp_Pnt(0, 0, 10)), MeasurementSelection::Edge);
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {z}, {xy}).values.front().value, 90), "line normal to plane must have 90-degree angle");
    const auto normalLine = calculate(MeasurementKind::Angle, {xy}, {z});
    check(approximatelyEqual(normalLine.values.front().value, 90) && normalLine.directionA && normalLine.directionB,
          "reverse plane/line angle wrong");
    gp_Trsf rotation; rotation.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), pi / 4.);
    y.originalTransform = rotation;
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {x}, {y}).values.front().value, 45), "transformed line direction ignored");
    auto arc = entity(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(), gp_Dir(0, 0, 1)), 5), 0, pi / 2.), MeasurementSelection::Edge);
    arc.curveParameter = pi / 4.; arc.pickedPoint = gp_Pnt(5 / std::sqrt(2.), 5 / std::sqrt(2.), 0);
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {arc}, {x}).values.front().value, 45), "curve tangent angle ignored picked parameter");
    arc.direction = MeasurementDirection::FeatureAxis;
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {arc}, {z}).values.front().value, 0), "circle feature axis angle wrong");
    auto cylinder = entity(surfaceFace(BRepPrimAPI_MakeCylinder(5, 20), GeomAbs_Cylinder), MeasurementSelection::Face);
    cylinder.surfaceParameters = std::array<double, 2>{{0, 10}}; cylinder.pickedPoint = gp_Pnt(5, 0, 10);
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {cylinder}, {x}).values.front().value, 90), "cylinder local tangent-plane angle wrong");
    cylinder.direction = MeasurementDirection::FeatureAxis;
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {cylinder}, {z}).values.front().value, 0), "cylinder feature axis angle wrong");
    auto cone = entity(surfaceFace(BRepPrimAPI_MakeCone(5, 2, 20), GeomAbs_Cone), MeasurementSelection::Face);
    cone.direction = MeasurementDirection::FeatureAxis;
    check(approximatelyEqual(calculate(MeasurementKind::Angle, {cone}, {z}).values.front().value, 0), "cone feature axis angle wrong");
    arc.direction = MeasurementDirection::Geometry; arc.curveParameter.reset();
    failure(MeasurementKind::Angle, {arc}, {x});
    cylinder.direction = MeasurementDirection::Geometry; cylinder.surfaceParameters.reset();
    failure(MeasurementKind::Angle, {cylinder}, {x});
    failure(MeasurementKind::Angle, {x, y}, {z});
}

void derivedReferenceTests() {
    const auto circle = [](const gp_Pnt& center, const gp_Dir& normal, MeasurementSelection mode, int instance = 0) {
        return entity(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(center, normal), 2), .1, 1.2), mode, instance);
    };
    auto center = circle(gp_Pnt(10,20,30), gp_Dir(0,0,1), MeasurementSelection::FeatureCenter);
    QString error;
    check(refineMeasurementPick(center,gp_Pnt(12,20,30),&error), "circle-center source pick failed");
    checkPoint(calculate(MeasurementKind::Point,{center}).anchors.front(),gp_Pnt(10,20,30), "arc center must not be replaced by clicked curve point");
    center.originalTransform.SetTranslation(gp_Vec(100,0,0));
    center.displayTransform.SetTranslation(gp_Vec(200,0,0));
    checkPoint(calculate(MeasurementKind::Point,{center}).anchors.front(),gp_Pnt(110,20,30), "derived center instance transform wrong");
    checkPoint(calculate(MeasurementKind::Point,{center},{},MeasurementCoordinates::Displayed).anchors.front(),gp_Pnt(210,20,30), "derived center display transform wrong");
    auto otherCenter = circle(gp_Pnt(110,20,40),gp_Dir(0,0,1),MeasurementSelection::FeatureCenter,1);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{center},{otherCenter}).values.front().value,10), "center to center distance wrong");
    auto axis = circle(gp_Pnt(),gp_Dir(0,0,1),MeasurementSelection::FeatureAxis);
    auto parallel = circle(gp_Pnt(3,4,1000),gp_Dir(0,0,1),MeasurementSelection::FeatureAxis,1);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{parallel}).values.front().value,5), "parallel infinite axes wrong");
    auto skew = circle(gp_Pnt(0,5,20),gp_Dir(1,0,0),MeasurementSelection::FeatureAxis,1);
    auto crossing = circle(gp_Pnt(0,0,20),gp_Dir(1,0,0),MeasurementSelection::FeatureAxis,1);
    auto coaxial = circle(gp_Pnt(0,0,1000),gp_Dir(0,0,1),MeasurementSelection::FeatureAxis,1);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{skew}).values.front().value,5), "skew axes wrong");
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{crossing}).values.front().value,0), "intersecting axes must have zero distance");
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{coaxial}).values.front().value,0), "coaxial axes must have zero distance");
    auto almostParallel = circle(gp_Pnt(5,0,10),gp_Dir(1.e-8,0,1),MeasurementSelection::FeatureAxis,1);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{almostParallel}).values.front().value,0), "near-parallel intersecting axis calculation lost precision");
    auto point = entity(BRepBuilderAPI_MakeVertex(gp_Pnt(3,4,1.e6)),MeasurementSelection::Vertex,1);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{axis},{point}).values.front().value,5), "infinite axis point distance truncated to source size");
    auto farEdge = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(3,0,1.e6),gp_Pnt(3,0,2.e6)),MeasurementSelection::Edge,1);
    auto farFace = entity(yzFace(4),MeasurementSelection::Face,1); farFace.originalTransform.SetTranslation(gp_Vec(0,0,1.e6));
    auto farSolid = entity(BRepPrimAPI_MakeBox(gp_Pnt(5,-1,1.e6),gp_Pnt(7,1,1.e6+10)),MeasurementSelection::Solid,1);
    auto throughSolid = entity(BRepPrimAPI_MakeBox(gp_Pnt(-1,-1,1.e6),gp_Pnt(1,1,1.e6+10)),MeasurementSelection::Solid,1);
    for (const auto& fixture : std::vector<std::pair<MeasurementEntity,double>>{{farEdge,3},{farFace,4},{farSolid,5},{throughSolid,0}}) {
        const auto result = calculate(MeasurementKind::Distance,{axis},{fixture.first});
        check(approximatelyEqual(result.values.front().value,fixture.second), "axis to remote bounded geometry wrong");
        check(approximatelyEqual(result.anchors[0].Distance(result.anchors[1]),fixture.second), "axis witnesses disagree with distance");
        check(approximatelyEqual(calculate(MeasurementKind::Distance,{fixture.first},{axis}).values.front().value,fixture.second), "axis bounded distance is not symmetric");
    }
    check(approximatelyEqual(calculate(MeasurementKind::Angle,{axis},{skew}).values.front().value,90), "axis angle wrong");
    auto cylinder = entity(surfaceFace(BRepPrimAPI_MakeCylinder(5,20),GeomAbs_Cylinder),MeasurementSelection::FeatureAxis);
    auto cone = entity(surfaceFace(BRepPrimAPI_MakeCone(5,2,20),GeomAbs_Cone),MeasurementSelection::FeatureAxis);
    check(refineMeasurementPick(cylinder,gp_Pnt(5,0,10),&error), "cylinder derived axis pick failed");
    check(approximatelyEqual(calculate(MeasurementKind::Angle,{cylinder},{axis}).values.front().value,0), "cylinder axis angle wrong");
    check(approximatelyEqual(calculate(MeasurementKind::Angle,{cone},{axis}).values.front().value,0), "cone axis angle wrong");
    cylinder.selection = MeasurementSelection::FeatureCenter;
    check(!measurementFeatureReference(cylinder,MeasurementCoordinates::Original,&error) && !error.isEmpty(), "cylinder must not invent a section center");
    check(!refineMeasurementPick(cylinder,gp_Pnt(5,0,10),&error), "invalid center source was accepted by viewer pick refinement");
    failure(MeasurementKind::Point,{cylinder});
    auto line = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(),gp_Pnt(1,0,0)),MeasurementSelection::FeatureAxis);
    failure(MeasurementKind::Distance,{line},{point});
    failure(MeasurementKind::Point,{axis});
    failure(MeasurementKind::Length,{axis});
}

void extensionTests() {
    const auto expanded=[](MeasurementEntity value) { value.extent=MeasurementExtent::Infinite; return value; };
    const auto vertex=[](const gp_Pnt& point) { return entity(BRepBuilderAPI_MakeVertex(point),MeasurementSelection::Vertex,2); };
    const auto line=[](const gp_Pnt& a,const gp_Pnt& b) { return entity(BRepBuilderAPI_MakeEdge(a,b),MeasurementSelection::Edge); };
    const auto plane=[](const gp_Pnt& point,const gp_Dir& normal) {
        return entity(BRepBuilderAPI_MakeFace(gp_Pln(point,normal),-1,1,-1,1),MeasurementSelection::Face,1);
    };
    const auto expect=[&](const MeasurementEntity& a,const MeasurementEntity& b,double expected) {
        const auto result=calculate(MeasurementKind::Distance,{a},{b});
        check(approximatelyEqual(result.values.front().value,expected),"extended reference distance incorrect");
        check(approximatelyEqual(result.anchors[0].Distance(result.anchors[1]),expected),"extended reference witnesses disagree");
        const auto reverse=calculate(MeasurementKind::Distance,{b},{a});
        check(approximatelyEqual(reverse.values.front().value,expected),"extended reference reversal incorrect");
        if (expected==0) checkPoint(result.anchors[0],result.anchors[1],"zero distance needs common witness");
        return result;
    };
    auto x=line(gp_Pnt(),gp_Pnt(1,0,0));
    expect(x,vertex(gp_Pnt(10,3,0)),std::sqrt(90.));
    const auto beyond=expect(expanded(x),vertex(gp_Pnt(10,3,0)),3.);
    checkPoint(beyond.anchors[0],gp_Pnt(10,0,0),"infinite straight edge truncated to endpoint");
    auto xy=plane(gp_Pnt(),gp_Dir(0,0,1));
    expect(expanded(xy),vertex(gp_Pnt(10,3,5)),5.);
    auto z=line(gp_Pnt(10,0,5),gp_Pnt(10,0,6));
    expect(z,xy,std::sqrt(106.));
    expect(expanded(z),xy,9.);
    expect(z,expanded(xy),5.);
    expect(expanded(z),expanded(xy),0.);
    expect(expanded(x),expanded(line(gp_Pnt(0,5,3),gp_Pnt(0,5,4))),5.);
    expect(expanded(x),expanded(line(gp_Pnt(0,3,4),gp_Pnt(1,3,4))),5.);
    expect(expanded(x),expanded(line(gp_Pnt(10,0,0),gp_Pnt(11,0,0))),0.);
    expect(expanded(x),expanded(line(gp_Pnt(10,-1,0),gp_Pnt(10,1,0))),0.);
    expect(expanded(line(gp_Pnt(0,0,5),gp_Pnt(1,0,5))),expanded(xy),5.);
    expect(expanded(xy),expanded(plane(gp_Pnt(0,0,5),gp_Dir(0,0,-1))),5.);
    expect(expanded(xy),expanded(plane(gp_Pnt(10,20,0),gp_Dir(0,0,1))),0.);
    expect(expanded(xy),expanded(plane(gp_Pnt(3,20,5),gp_Dir(1,0,0))),0.);
    auto holed=entity(holedFace(),MeasurementSelection::Face,1);
    expect(holed,vertex(gp_Pnt(5,5,2)),std::sqrt(8.));
    expect(expanded(holed),vertex(gp_Pnt(5,5,2)),2.);
    auto throughHole=expanded(line(gp_Pnt(5,5,10),gp_Pnt(5,5,11)));
    expect(throughHole,holed,2.); // Preserve the other object's actual hole.
    expect(throughHole,expanded(holed),0.);
    auto circle=entity(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(1.e6,2.e6,5),gp_Dir(0,0,1)),2)),MeasurementSelection::Edge,2);
    expect(expanded(xy),circle,5.);
    auto axis=circle;axis.selection=MeasurementSelection::FeatureAxis;
    expect(axis,expanded(xy),0.);
    expect(expanded(xy),entity(BRepPrimAPI_MakeBox(gp_Pnt(1.e6,2.e6,10),gp_Pnt(1.e6+1,2.e6+1,12)),MeasurementSelection::Solid,2),10.);
    expect(expanded(xy),entity(BRepPrimAPI_MakeBox(gp_Pnt(1.e6,2.e6,-1),gp_Pnt(1.e6+1,2.e6+1,1)),MeasurementSelection::Solid,2),0.);
    expect(expanded(xy),entity(BRepBuilderAPI_MakeEdge(gp_Pnt(1.e6,2.e6,5),gp_Pnt(1.e6+1,2.e6,5)),MeasurementSelection::Edge,2),5.);
    auto remoteFace=entity(yzFace(1.e6),MeasurementSelection::Face,2);
    remoteFace.originalTransform.SetTranslation(gp_Vec(0,2.e6,10)); expect(expanded(xy),remoteFace,8.);
    TopoDS_Compound disconnected;BRep_Builder builder;builder.MakeCompound(disconnected);
    builder.Add(disconnected,BRepPrimAPI_MakeBox(gp_Pnt(0,0,5),gp_Pnt(1,1,6)));
    builder.Add(disconnected,BRepPrimAPI_MakeBox(gp_Pnt(0,0,-6),gp_Pnt(1,1,-5)));
    expect(expanded(xy),entity(disconnected,MeasurementSelection::Part,2),5.);
    auto transformed=expanded(x);
    transformed.originalTransform.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,0,1)),pi/2.);
    transformed.originalTransform.SetTranslationPart(gp_Vec(100,0,0));
    transformed.displayTransform=transformed.originalTransform;transformed.displayTransform.SetTranslationPart(gp_Vec(200,0,0));
    expect(transformed,vertex(gp_Pnt(110,3,0)),10.);
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{transformed},{vertex(gp_Pnt(110,3,0))},MeasurementCoordinates::Displayed).values.front().value,90.),"expanded placement applied twice or coordinate mode ignored");
    check(approximatelyEqual(calculate(MeasurementKind::Distance,{x,expanded(x)},{vertex(gp_Pnt(10,0,0))}).values.front().value,0.),"extent ignored during deduplication");
    for (double angle : {0.,pi/4.,pi/2.}) {
        auto other=line(gp_Pnt(0,3,4),gp_Pnt(std::cos(angle),3+std::sin(angle),4));
        const auto bounded=calculate(MeasurementKind::Angle,{x},{other}).values.front().value;
        for (auto pair : std::vector<std::pair<MeasurementEntity,MeasurementEntity>>{{expanded(x),other},{x,expanded(other)},{expanded(x),expanded(other)}})
            check(approximatelyEqual(calculate(MeasurementKind::Angle,{pair.first},{pair.second}).values.front().value,bounded),"extension changed analytic angle");
    }
    check(approximatelyEqual(calculate(MeasurementKind::Angle,{expanded(z)},{expanded(xy)}).values.front().value,90.),"extended line-plane angle incorrect");
    check(approximatelyEqual(calculate(MeasurementKind::Angle,{expanded(xy)},{expanded(plane(gp_Pnt(),gp_Dir(1,0,0)))}).values.front().value,90.),"extended plane-plane angle incorrect");
    gp_Trsf commonPose;commonPose.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(1,2,3)),.7);
    commonPose.SetTranslationPart(gp_Vec(50,60,70));
    auto tiltedPlane=expanded(xy),tiltedParallelLine=expanded(line(gp_Pnt(0,0,5),gp_Pnt(1,0,5)));
    tiltedPlane.originalTransform=commonPose;tiltedParallelLine.originalTransform=commonPose;
    expect(tiltedParallelLine,tiltedPlane,5.);
    auto tiltedOtherPlane=expanded(plane(gp_Pnt(0,0,5),gp_Dir(0,0,1)));tiltedOtherPlane.originalTransform=commonPose;
    expect(tiltedPlane,tiltedOtherPlane,5.);
    QString reason;
    check(!measurementReference(expanded(circle),MeasurementCoordinates::Original,&reason)&&reason.contains(QStringLiteral("曲线")),"curve extension should fail explicitly");
    failure(MeasurementKind::Distance,{expanded(circle)},{vertex(gp_Pnt())});
    auto cylinder=entity(surfaceFace(BRepPrimAPI_MakeCylinder(5,20),GeomAbs_Cylinder),MeasurementSelection::Face);
    failure(MeasurementKind::Angle,{expanded(cylinder)},{expanded(xy)});
    failure(MeasurementKind::Length,{expanded(x)});
    auto falseEdge=vertex(gp_Pnt());falseEdge.extent=MeasurementExtent::Infinite;
    failure(MeasurementKind::Distance,{falseEdge},{x});
    auto vertical=expanded(line(gp_Pnt(),gp_Pnt(0,0,1)));
    auto tinyAxis=entity(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(5,0,10),gp_Dir(1.e-13,0,1)),2)),MeasurementSelection::FeatureAxis,1);
    check(failure(MeasurementKind::Distance,{vertical},{tinyAxis}).error.contains(QStringLiteral("过远")),"near-parallel remote intersection must not report false positive distance");
    tinyAxis.shape=BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(5,0,10),gp_Dir(1.e-8,0,1)),2));
    expect(vertical,tinyAxis,0.);
    auto distantIntersection=expanded(line(gp_Pnt(0,0,5),gp_Pnt(1,0,5+1.e-13)));
    failure(MeasurementKind::Distance,{distantIntersection},{expanded(xy)});
    std::atomic_bool cancel{false};
    const auto cancelled=calculateMeasurement({MeasurementKind::Distance,MeasurementCoordinates::Original,{expanded(xy)},{circle}},cancel,
        [&](int percent,const QString&){if(percent>0)cancel=true;});
    check(!cancelled.success&&cancelled.error.contains(QStringLiteral("取消")),"expanded covering solve ignored cancellation");
}

void pickAndCancellationTests() {
    QString error;
    auto edge = entity(BRepBuilderAPI_MakeEdge(gp_Pnt(), gp_Pnt(10, 0, 0)), MeasurementSelection::PointOnGeometry);
    check(refineMeasurementPick(edge, gp_Pnt(20, 3, 0), &error) && error.isEmpty(), "bounded edge endpoint projection failed");
    checkPoint(*edge.pickedPoint, gp_Pnt(10, 0, 0), "pick projected onto unbounded edge");
    check(edge.curveParameter.has_value(), "edge endpoint parameter missing");
    checkPoint(calculate(MeasurementKind::Point, {edge}).anchors.front(), gp_Pnt(10, 0, 0), "point-on-edge coordinate wrong");
    edge.originalTransform.SetTranslation(gp_Vec(100, 0, 0));
    checkPoint(calculate(MeasurementKind::Point, {edge}).anchors.front(), gp_Pnt(110, 0, 0), "point-on-edge instance transform wrong");
    auto face = entity(holedFace(), MeasurementSelection::PointOnGeometry);
    check(refineMeasurementPick(face, gp_Pnt(5, 5, 2), &error), "trimmed-hole face pick failed");
    check(face.pickedPoint && face.surfaceParameters, "face pick coordinates/parameters missing");
    check(approximatelyEqual(face.pickedPoint->Z(), 0) && approximatelyEqual(face.pickedPoint->Distance(gp_Pnt(5, 5, 0)), 2),
          "point inside hole must snap to bounded hole edge");
    check(refineMeasurementPick(face, gp_Pnt(12, 5, 2), &error), "outer-trimmed face pick failed");
    checkPoint(*face.pickedPoint, gp_Pnt(10, 5, 0), "face pick projected beyond trim boundary");
    auto vertex = entity(BRepBuilderAPI_MakeVertex(gp_Pnt(1, 2, 3)), MeasurementSelection::Vertex);
    check(refineMeasurementPick(vertex, gp_Pnt(3, 4, 5), &error), "vertex pick failed");
    checkPoint(*vertex.pickedPoint, gp_Pnt(1, 2, 3), "vertex pick did not retain exact topology point");
    const auto savedPoint = vertex.pickedPoint;
    check(!refineMeasurementPick(vertex, gp_Pnt(std::numeric_limits<double>::quiet_NaN(), 0, 0), &error)
          && !error.isEmpty(), "invalid sample accepted");
    checkPoint(*vertex.pickedPoint, *savedPoint, "failed projection overwrote valid reference point");
    auto cylinder = entity(surfaceFace(BRepPrimAPI_MakeCylinder(5, 20), GeomAbs_Cylinder), MeasurementSelection::Face);
    check(refineMeasurementPick(cylinder, gp_Pnt(5.1, 0, 10), &error) && cylinder.surfaceParameters,
          "curved face bounded parameters missing");
    checkPoint(*cylinder.pickedPoint, gp_Pnt(5, 0, 10), "curved face exact projection wrong", 1.e-5);
    std::atomic_bool cancel{true};
    auto cancelled = calculateMeasurement({MeasurementKind::Point, MeasurementCoordinates::Original, {vertex}, {}}, cancel);
    check(!cancelled.success && cancelled.error.contains(QStringLiteral("取消")), "pre-cancelled request ran");
    cancel.store(false);
    auto during = calculateMeasurement({MeasurementKind::Area, MeasurementCoordinates::Original, {face}, {}}, cancel,
        [&](int percent, const QString&) { if (percent > 0) cancel.store(true); });
    check(!during.success && during.error.contains(QStringLiteral("取消")) && during.values.empty(), "cancelled work published a result");
    cancel.store(false);
    auto callbackFailure = calculateMeasurement({MeasurementKind::Point, MeasurementCoordinates::Original, {vertex}, {}}, cancel,
        [](int, const QString&) { throw std::runtime_error("test callback failure"); });
    check(!callbackFailure.success && !callbackFailure.error.isEmpty(), "unexpected exception escaped calculation API");
    const auto formatted = measurementResultText(calculate(MeasurementKind::Radius,
        {entity(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(), gp_Dir(0, 0, 1)), 5)), MeasurementSelection::Edge)}), 3);
    check(formatted.contains(QStringLiteral("5.000 mm")), "result text precision/unit wrong");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size()==3 && app.arguments()[1]==QStringLiteral("--generate-extension-sample")) {
            TopoDS_Compound sample;BRep_Builder builder;builder.MakeCompound(sample);
            builder.Add(sample,BRepPrimAPI_MakeCylinder(10,30));
            builder.Add(sample,BRepPrimAPI_MakeBox(gp_Pnt(50,20,60),10,10,10));
            builder.Add(sample,BRepPrimAPI_MakeSphere(gp_Pnt(80,-20,0),5));
            STEPControl_Writer writer;
            check(writer.Transfer(sample,STEPControl_AsIs)==IFSelect_RetDone,"sample transfer failed");
            check(writer.Write(app.arguments()[2].toUtf8().constData())==IFSelect_RetDone,"sample write failed");
            return 0;
        }
        distanceTests(); scalarAndCenterTests(); densityAndTransformTests(); angleTests(); derivedReferenceTests(); extensionTests(); pickAndCancellationTests();
        std::cout << "PASS: ten exact distance pairs, bounded witnesses, analytic radii, trimmed area, solid volume, "
                     "instance and density centers, transformed geometry, local tangent/normal/feature-axis angles, "
                     "independent volume center, circle centers, infinite axis/line/plane witnesses and extension combinations, precise bounded picks, cancellation and explicit failures\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n'; return 1;
    }
}
