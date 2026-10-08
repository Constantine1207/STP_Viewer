#include "measurement.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace Stp {
namespace {
constexpr double integrationTolerance = 1.e-8;
const double pi = std::acos(-1.0);

struct MeasurementFailure { QString text; };
[[noreturn]] void fail(const QString& text) { throw MeasurementFailure{text}; }
void checkCancel(const std::atomic_bool& cancel) {
    if (cancel.load()) fail(QStringLiteral("测量已取消"));
}
bool finitePoint(const gp_Pnt& point) {
    return std::isfinite(point.X()) && std::isfinite(point.Y()) && std::isfinite(point.Z());
}
void requirePositive(double value, const QString& error) {
    if (!std::isfinite(value) || value <= 0) fail(error);
}
const gp_Trsf& transform(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    return coordinates == MeasurementCoordinates::Original ? entity.originalTransform : entity.displayTransform;
}
gp_Pnt locatedPoint(const MeasurementEntity& entity, const gp_Pnt& point, MeasurementCoordinates coordinates) {
    return point.Transformed(transform(entity, coordinates));
}
TopoDS_Shape locatedShape(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    if (entity.shape.IsNull()) fail(QStringLiteral("所选几何已经失效，请重新选择"));
    const auto& placement = transform(entity, coordinates);
    for (int row = 1; row <= 3; ++row)
        for (int column = 1; column <= 4; ++column)
            if (!std::isfinite(placement.Value(row, column))) fail(QStringLiteral("实例位置包含无效数值"));
    if (std::abs(std::abs(placement.ScaleFactor()) - 1.) > TopLoc_Location::ScalePrec())
        fail(QStringLiteral("测量实例位置必须是刚性变换"));
    return entity.shape.Moved(TopLoc_Location(placement));
}

bool sameOccurrence(const MeasurementEntity& a, const MeasurementEntity& b) {
    if (!a.occurrenceId.isEmpty() || !b.occurrenceId.isEmpty())
        return a.occurrenceId == b.occurrenceId;
    return a.occurrence == b.occurrence;
}
std::vector<MeasurementEntity> uniqueEntities(const std::vector<MeasurementEntity>& entities) {
    std::vector<MeasurementEntity> result;
    for (const auto& entity : entities) {
        if (entity.shape.IsNull()) fail(QStringLiteral("所选几何已经失效，请重新选择"));
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto& other) {
            return sameOccurrence(entity, other) && entity.shape.IsSame(other.shape) && entity.selection == other.selection
                && entity.direction == other.direction && entity.extent == other.extent
                && entity.pickedPoint.has_value() == other.pickedPoint.has_value()
                && (!entity.pickedPoint || entity.pickedPoint->Distance(*other.pickedPoint) <= Precision::Confusion());
        });
        if (!duplicate) result.push_back(entity);
    }
    return result;
}
void requireSingle(const std::vector<MeasurementEntity>& group, const QString& error) {
    if (group.size() != 1) fail(error);
}
void addCoordinates(MeasurementResult& result, const gp_Pnt& point, const QString& prefix = {}) {
    result.values.push_back({prefix + QStringLiteral("X"), QStringLiteral("mm"), point.X()});
    result.values.push_back({prefix + QStringLiteral("Y"), QStringLiteral("mm"), point.Y()});
    result.values.push_back({prefix + QStringLiteral("Z"), QStringLiteral("mm"), point.Z()});
}
MeasurementFeature featureReference(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    const auto shape = locatedShape(entity, coordinates);
    if (shape.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (curve.GetType() == GeomAbs_Circle) {
            const auto circle = curve.Circle();
            return {circle.Location(), circle.Axis()};
        }
    }
    if (entity.selection == MeasurementSelection::FeatureCenter)
        fail(QStringLiteral("圆心只适用于解析圆或圆弧边；圆柱面、圆锥面请选择轴线"));
    if (shape.ShapeType() == TopAbs_FACE) {
        BRepAdaptor_Surface surface(TopoDS::Face(shape));
        if (surface.GetType() == GeomAbs_Cylinder) return {{}, surface.Cylinder().Axis()};
        if (surface.GetType() == GeomAbs_Cone) return {{}, surface.Cone().Axis()};
    }
    fail(QStringLiteral("轴线只适用于解析圆、圆弧、圆柱面或圆锥面"));
}
gp_Pnt selectedPoint(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    if (entity.selection == MeasurementSelection::FeatureCenter)
        return *featureReference(entity, coordinates).center;
    if (entity.selection == MeasurementSelection::PointOnGeometry) {
        if (!entity.pickedPoint || !finitePoint(*entity.pickedPoint)) fail(QStringLiteral("请在精确几何上重新选择参考点"));
        return locatedPoint(entity, *entity.pickedPoint, coordinates);
    }
    const auto shape = locatedShape(entity, coordinates);
    if (shape.ShapeType() != TopAbs_VERTEX) fail(QStringLiteral("点坐标测量需要选择顶点或几何上的点"));
    return BRep_Tool::Pnt(TopoDS::Vertex(shape));
}
TopoDS_Shape distanceShape(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    if (entity.selection == MeasurementSelection::PointOnGeometry || entity.selection == MeasurementSelection::FeatureCenter)
        return BRepBuilderAPI_MakeVertex(selectedPoint(entity, coordinates)).Shape();
    return locatedShape(entity, coordinates);
}
TopoDS_Shape groupShape(const std::vector<MeasurementEntity>& entities, MeasurementCoordinates coordinates) {
    if (entities.size() == 1) return distanceShape(entities.front(), coordinates);
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const auto& entity : entities) builder.Add(compound, distanceShape(entity, coordinates));
    return compound;
}

std::optional<gp_Pnt> pointInsideSolid(const TopoDS_Shape& container, const TopoDS_Shape& other,
                                     std::atomic_bool& cancel) {
    TopTools_IndexedMapOfShape solids, vertices;
    TopExp::MapShapes(container, TopAbs_SOLID, solids);
    if (solids.IsEmpty()) return std::nullopt;
    TopExp::MapShapes(other, TopAbs_VERTEX, vertices);
    for (int i = 1; i <= solids.Extent(); ++i) {
        checkCancel(cancel);
        BRepClass3d_SolidClassifier classifier(solids(i));
        for (int j = 1; j <= vertices.Extent(); ++j) {
            checkCancel(cancel);
            const auto point = BRep_Tool::Pnt(TopoDS::Vertex(vertices(j)));
            classifier.Perform(point, Precision::Confusion());
            if (classifier.State() == TopAbs_IN) return point;
        }
    }
    return std::nullopt;
}

class DistanceProgress final : public Message_ProgressIndicator {
public:
    DistanceProgress(std::atomic_bool& cancel, const ProgressCallback& progress)
        : m_cancel(cancel), m_progress(progress) {}
protected:
    Standard_Boolean UserBreak() override { return m_cancel.load(); }
    void Show(const Message_ProgressScope&, Standard_Boolean) override {
        const int percent = 20 + int(GetPosition() * 75);
        if (m_progress && percent != m_lastPercent && !m_cancel.load()) {
            m_lastPercent = percent;
            m_progress(percent, QStringLiteral("计算精确最小距离"));
        }
    }
private:
    std::atomic_bool& m_cancel;
    ProgressCallback m_progress;
    int m_lastPercent = -1;
};

struct DistanceWitness {
    double value;
    gp_Pnt a, b;
    bool containment = false;
    bool nonUnique = false;
};
DistanceWitness boundedDistance(const TopoDS_Shape& a, const TopoDS_Shape& b,
    std::atomic_bool& cancel, const ProgressCallback& progress,
    const gp_Pnt* preferredA = nullptr, const gp_Pnt* preferredB = nullptr) {
    BRepExtrema_DistShapeShape distance;
    distance.LoadS1(a); distance.LoadS2(b);
    distance.SetDeflection(Precision::Confusion());
    Handle(Message_ProgressIndicator) indicator = new DistanceProgress(cancel, progress);
    distance.Perform(indicator->Start()); checkCancel(cancel);
    if (!distance.IsDone() || distance.NbSolution() < 1) fail(QStringLiteral("精确最小距离计算失败"));
    DistanceWitness result {distance.InnerSolution() ? 0. : distance.Value(),
        distance.PointOnShape1(1), distance.PointOnShape2(1), bool(distance.InnerSolution()), distance.NbSolution()>1};
    if (preferredA && preferredB && !distance.InnerSolution()) {
        double score=std::hypot(result.a.Distance(*preferredA),result.b.Distance(*preferredB));
        for (int i=2;i<=distance.NbSolution();++i) {
            checkCancel(cancel);
            const auto p=distance.PointOnShape1(i),q=distance.PointOnShape2(i);
            const double candidate=std::hypot(p.Distance(*preferredA),q.Distance(*preferredB));
            if (candidate<score) { score=candidate;result.a=p;result.b=q; }
        }
    }
    if (!std::isfinite(result.value) || result.value < 0) fail(QStringLiteral("最小距离包含无效数值"));
    if (result.value > Precision::Confusion()) {
        auto inside = pointInsideSolid(a, b, cancel);
        if (!inside) inside = pointInsideSolid(b, a, cancel);
        if (inside) result = {0., *inside, *inside, true};
    }
    if (result.value <= Precision::Confusion()) { result.value = 0.; result.b = result.a; }
    return result;
}
double stableDot(const gp_Vec& a, const gp_Vec& b) {
    const double products[3] {a.X()*b.X(), a.Y()*b.Y(), a.Z()*b.Z()};
    double sum = 0, correction = 0;
    for (int i = 0; i < 3; ++i) {
        const double next = sum + products[i], part = next - sum;
        correction += (sum - (next - part)) + (products[i] - part);
        correction += std::fma(a.Coord(i+1), b.Coord(i+1), -products[i]);
        sum = next;
    }
    return sum + correction;
}
double productDifference(double a, double b, double c, double d) {
    const double cd = c*d;
    return std::fma(a,b,-cd) + std::fma(-c,d,cd);
}
gp_Vec stableCross(const gp_Vec& a, const gp_Vec& b) {
    return {productDifference(a.Y(),b.Z(),a.Z(),b.Y()),
            productDifference(a.Z(),b.X(),a.X(),b.Z()),
            productDifference(a.X(),b.Y(),a.Y(),b.X())};
}
double norm(const gp_Vec& value) { return std::hypot(value.X(),value.Y(),value.Z()); }
gp_Pnt lineProjection(const gp_Lin& line, const gp_Pnt& point) {
    const gp_Vec direction(line.Direction());
    return line.Location().Translated(direction * stableDot(gp_Vec(line.Location(),point),direction));
}
gp_Pnt planeProjection(const gp_Pln& plane, const gp_Pnt& point) {
    const gp_Vec normal(plane.Axis().Direction());
    return point.Translated(normal * -stableDot(gp_Vec(plane.Location(),point),normal));
}
bool pointSelection(const MeasurementEntity& entity) {
    return entity.selection == MeasurementSelection::Vertex || entity.selection == MeasurementSelection::PointOnGeometry
        || entity.selection == MeasurementSelection::FeatureCenter;
}
MeasurementReference resolveReference(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    MeasurementReference reference;
    const auto shape = locatedShape(entity, coordinates);
    const auto sample = entity.pickedPoint && finitePoint(*entity.pickedPoint)
        ? std::optional<gp_Pnt>(locatedPoint(entity,*entity.pickedPoint,coordinates)) : std::nullopt;
    if (entity.extent == MeasurementExtent::Infinite
        && entity.selection != MeasurementSelection::Edge && entity.selection != MeasurementSelection::Face)
        fail(QStringLiteral("无限拓展仅适用于解析直线边或平面面"));
    if (entity.selection == MeasurementSelection::FeatureAxis) {
        reference.line = gp_Lin(*featureReference(entity,coordinates).axis);
        reference.anchor = lineProjection(*reference.line,sample.value_or(reference.line->Location()));
    } else if (entity.extent == MeasurementExtent::Infinite) {
        if (entity.selection == MeasurementSelection::Edge && shape.ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve curve(TopoDS::Edge(shape));
            if (curve.GetType() != GeomAbs_Line)
                fail(QStringLiteral("曲线或圆弧不支持无限拓展；仅解析直线边可拓展"));
            reference.line = curve.Line();
            reference.anchor = lineProjection(*reference.line,sample.value_or(curve.Value((curve.FirstParameter()+curve.LastParameter())/2.)));
        } else if (entity.selection == MeasurementSelection::Face && shape.ShapeType() == TopAbs_FACE) {
            BRepAdaptor_Surface surface(TopoDS::Face(shape));
            if (surface.GetType() != GeomAbs_Plane)
                fail(QStringLiteral("曲面不支持无限拓展；仅解析平面面可拓展"));
            reference.plane = surface.Plane();
            reference.anchor = planeProjection(*reference.plane,sample.value_or(surface.Value(
                (surface.FirstUParameter()+surface.LastUParameter())/2.,
                (surface.FirstVParameter()+surface.LastVParameter())/2.)));
        } else fail(QStringLiteral("无限拓展需要先选择解析直线边或平面面"));
    } else {
        reference.bounded = distanceShape(entity,coordinates);
        reference.anchor = pointSelection(entity) ? selectedPoint(entity,coordinates) : sample.value_or(gp_Pnt());
        if (!sample && !pointSelection(entity)) {
            Bnd_Box box; BRepBndLib::AddOptimal(reference.bounded,box,false,true);
            if (!box.IsVoid() && !box.IsOpen()) {
                double x0,y0,z0,x1,y1,z1; box.Get(x0,y0,z0,x1,y1,z1);
                reference.anchor.SetCoord(x0/2.+x1/2.,y0/2.+y1/2.,z0/2.+z1/2.);
            }
        }
    }
    if (!finitePoint(reference.anchor)) fail(QStringLiteral("测量参考位置无效，请重新拾取"));
    return reference;
}

double referenceTolerance(const MeasurementEntity& entity) {
    double tolerance = Precision::Confusion()*10.;
    for (TopExp_Explorer e(entity.shape,TopAbs_EDGE); e.More(); e.Next())
        tolerance = std::max(tolerance,BRep_Tool::Tolerance(TopoDS::Edge(e.Current()))*2.);
    for (TopExp_Explorer f(entity.shape,TopAbs_FACE); f.More(); f.Next())
        tolerance = std::max(tolerance,BRep_Tool::Tolerance(TopoDS::Face(f.Current()))*2.);
    return tolerance;
}
void validateWitness(DistanceWitness& witness, const MeasurementReference& a, const MeasurementReference& b, double tolerance) {
    if (!std::isfinite(witness.value) || witness.value < 0 || !finitePoint(witness.a) || !finitePoint(witness.b))
        fail(QStringLiteral("无限参考几何最近点包含无效数值"));
    double scale = 1.;
    for (const auto* point : {&witness.a,&witness.b})
        scale = std::max({scale,std::abs(point->X()),std::abs(point->Y()),std::abs(point->Z())});
    if (scale * std::numeric_limits<double>::epsilon()*2. > tolerance)
        fail(QStringLiteral("交点或最近点过远，无法可靠确定最近点"));
    const auto residual = [](const MeasurementReference& reference,const gp_Pnt& point) {
        if (reference.line) return norm(stableCross(gp_Vec(reference.line->Location(),point),gp_Vec(reference.line->Direction())));
        if (reference.plane) return std::abs(stableDot(gp_Vec(reference.plane->Location(),point),gp_Vec(reference.plane->Axis().Direction())));
        if (reference.bounded.ShapeType()==TopAbs_VERTEX) return point.Distance(reference.anchor);
        return 0.; // The exact bounded solver verifies its own support topology.
    };
    if (residual(a,witness.a)>tolerance || residual(b,witness.b)>tolerance
        || std::abs(norm(gp_Vec(witness.a,witness.b))-witness.value)>tolerance)
        fail(QStringLiteral("无限参考几何最近点数值不稳定，无法可靠测量"));
    if (witness.value<=Precision::Confusion()) {
        // Validate a common point against both references before coalescing it.
        if (residual(b,witness.a)>tolerance) fail(QStringLiteral("无法可靠确定两参考几何的交点"));
        witness.value=0.; witness.b=witness.a;
    }
}

DistanceWitness analyticDistance(const MeasurementReference& a, const MeasurementReference& b) {
    if (a.line && b.line) {
        const gp_Vec u(a.line->Direction()),v(b.line->Direction()),offset(a.anchor,b.anchor);
        const auto cross=stableCross(u,v); const double sine=norm(cross);
        if (sine==0.) {
            const auto p=a.anchor.Translated(u*(stableDot(offset,u)/2.));
            const auto q=lineProjection(*b.line,p);
            return {norm(stableCross(offset,u)),p,q,false,true};
        }
        const auto normal=cross/sine;
        const double s=stableDot(stableCross(offset,v),normal)/sine;
        const double t=stableDot(stableCross(offset,u),normal)/sine;
        return {std::abs(stableDot(offset,normal)),a.anchor.Translated(u*s),b.anchor.Translated(v*t)};
    }
    if (a.line && b.plane) {
        const gp_Vec u(a.line->Direction()),normal(b.plane->Axis().Direction());
        const double denominator=stableDot(u,normal);
        const gp_Vec offset(a.anchor,b.anchor);
        if (denominator==0.) {
            const auto p=a.anchor.Translated(u*(stableDot(offset,u)/2.));
            const auto q=planeProjection(*b.plane,p);
            return {std::abs(stableDot(offset,normal)),p,q,false,true};
        }
        const auto p=a.anchor.Translated(u*(stableDot(offset,normal)/denominator));
        return {0.,p,planeProjection(*b.plane,p)};
    }
    if (a.plane && b.line) {
        auto result=analyticDistance(b,a); std::swap(result.a,result.b); return result;
    }
    const gp_Vec n(a.plane->Axis().Direction()),m(b.plane->Axis().Direction());
    const auto cross=stableCross(n,m); const double sine=norm(cross);
    const auto middle=a.anchor.Translated(gp_Vec(a.anchor,b.anchor)/2.);
    if (sine==0.) {
        const auto p=planeProjection(*a.plane,middle),q=planeProjection(*b.plane,p);
        return {std::abs(stableDot(gp_Vec(a.anchor,b.anchor),m)),p,q,false,true};
    }
    const auto direction=cross/sine;
    const auto origin=a.anchor.Translated(stableCross(direction,n)*(stableDot(gp_Vec(a.anchor,b.anchor),m)/sine));
    const auto p=origin.Translated(direction*stableDot(gp_Vec(origin,middle),direction));
    return {0.,p,planeProjection(*b.plane,p),false,true};
}

TopoDS_Shape coveringShape(const MeasurementReference& infinite,const TopoDS_Shape& bounded) {
    Bnd_Box box; BRepBndLib::AddOptimal(bounded,box,false,true);
    if (box.IsVoid() || box.IsOpen()) fail(QStringLiteral("无限参考距离需要有效的有界对象"));
    double x0,y0,z0,x1,y1,z1; box.Get(x0,y0,z0,x1,y1,z1);
    double u0=std::numeric_limits<double>::infinity(),u1=-u0,v0=u0,v1=-u0;
    const auto origin=infinite.line ? infinite.line->Location() : infinite.plane->Location();
    const gp_Vec u(infinite.line ? infinite.line->Direction() : infinite.plane->Position().XDirection());
    const gp_Vec v(infinite.plane ? infinite.plane->Position().YDirection() : gp_Dir(0,1,0));
    for (int i=0;i<8;++i) {
        const gp_Vec offset(origin,gp_Pnt(i&1?x1:x0,i&2?y1:y0,i&4?z1:z0));
        const double pu=stableDot(offset,u),pv=stableDot(offset,v);
        u0=std::min(u0,pu);u1=std::max(u1,pu);v0=std::min(v0,pv);v1=std::max(v1,pv);
    }
    const auto enlarge=[](double& low,double& high) {
        if (!std::isfinite(low)||!std::isfinite(high)) fail(QStringLiteral("无限参考求解区间无效"));
        const double margin=std::max(Precision::Confusion()*100.,(high-low)*1.e-9);
        low=std::nextafter(low-margin,-std::numeric_limits<double>::infinity());
        high=std::nextafter(high+margin,std::numeric_limits<double>::infinity());
        if (!std::isfinite(low)||!std::isfinite(high)||low>=high) fail(QStringLiteral("无限参考求解区间超出有效范围"));
    };
    enlarge(u0,u1);
    if (infinite.line) return BRepBuilderAPI_MakeEdge(*infinite.line,u0,u1).Shape();
    enlarge(v0,v1);
    return BRepBuilderAPI_MakeFace(*infinite.plane,u0,u1,v0,v1).Shape();
}
DistanceWitness referenceDistance(const MeasurementEntity& first,const MeasurementEntity& second,
    MeasurementCoordinates coordinates,std::atomic_bool& cancel,const ProgressCallback& progress) {
    checkCancel(cancel);
    const auto a=resolveReference(first,coordinates),b=resolveReference(second,coordinates);
    const bool infiniteA=a.line||a.plane,infiniteB=b.line||b.plane;
    if (!infiniteA&&!infiniteB) return boundedDistance(a.bounded,b.bounded,cancel,progress);
    DistanceWitness result;
    if (infiniteA&&infiniteB) {
        const auto& placement=transform(first,coordinates);
        bool commonPlacement=true;
        for (int row=1;row<=3;++row) for (int column=1;column<=4;++column)
            if (placement.Value(row,column)!=transform(second,coordinates).Value(row,column)) commonPlacement=false;
        if (commonPlacement) {
            // A shared rigid placement preserves parallelism exactly. Solving
            // in that frame avoids inventing far intersections from roundoff
            // when a perpendicular line/normal pair is rotated together.
            auto localFirst=first,localSecond=second;
            localFirst.originalTransform=gp_Trsf();localFirst.displayTransform=gp_Trsf();
            localSecond.originalTransform=gp_Trsf();localSecond.displayTransform=gp_Trsf();
            const auto localA=resolveReference(localFirst,coordinates),localB=resolveReference(localSecond,coordinates);
            result=analyticDistance(localA,localB);
            validateWitness(result,localA,localB,std::max(referenceTolerance(first),referenceTolerance(second)));
            result.a.Transform(placement);result.b.Transform(placement);
        } else result=analyticDistance(a,b);
    }
    else {
        const auto& infinite=infiniteA?a:b;
        const auto& bounded=infiniteA?b:a;
        if (bounded.bounded.ShapeType()==TopAbs_VERTEX) {
            const auto p=infinite.line?lineProjection(*infinite.line,bounded.anchor):planeProjection(*infinite.plane,bounded.anchor);
            result={norm(gp_Vec(p,bounded.anchor)),p,bounded.anchor};
        } else {
            result=boundedDistance(coveringShape(infinite,bounded.bounded),bounded.bounded,cancel,progress,&infinite.anchor,&bounded.anchor);
            if (result.nonUnique && !result.containment) {
                const auto middle=infinite.anchor.Translated(gp_Vec(infinite.anchor,bounded.anchor)/2.);
                const auto preferred=infinite.line?lineProjection(*infinite.line,middle):planeProjection(*infinite.plane,middle);
                const auto candidate=boundedDistance(BRepBuilderAPI_MakeVertex(preferred),bounded.bounded,cancel,progress);
                if (std::abs(candidate.value-result.value)<=Precision::Confusion()
                    && std::hypot(candidate.a.Distance(infinite.anchor),candidate.b.Distance(bounded.anchor))
                       <std::hypot(result.a.Distance(infinite.anchor),result.b.Distance(bounded.anchor))) {
                    result.a=candidate.a;result.b=candidate.b;
                }
            }
        }
        if (!infiniteA) std::swap(result.a,result.b);
    }
    checkCancel(cancel);
    validateWitness(result,a,b,std::max(referenceTolerance(first),referenceTolerance(second)));
    return result;
}

GProp_GProps volumeProperties(const TopoDS_Shape& shape, std::atomic_bool& cancel) {
    TopTools_IndexedMapOfShape solids, allFaces, solidFaces;
    TopExp::MapShapes(shape, TopAbs_SOLID, solids);
    TopExp::MapShapes(shape, TopAbs_FACE, allFaces);
    for (int i = 1; i <= solids.Extent(); ++i) TopExp::MapShapes(solids(i), TopAbs_FACE, solidFaces);
    if (solids.IsEmpty() || allFaces.Extent() != solidFaces.Extent())
        fail(QStringLiteral("体积、质量和实体质心只适用于封闭有效实体；开放面壳请测量面积"));
    GProp_GProps total;
    for (int i = 1; i <= solids.Extent(); ++i) {
        checkCancel(cancel);
        const auto& solid = solids(i);
        if (!BRepCheck_Analyzer(solid, true).IsValid()) fail(QStringLiteral("实体几何无效，无法计算可靠的体积或质心"));
        bool hasShell = false;
        for (TopExp_Explorer shell(solid, TopAbs_SHELL); shell.More(); shell.Next()) {
            hasShell = true;
            BRepCheck_Shell checker(TopoDS::Shell(shell.Current()));
            if (checker.Closed() != BRepCheck_NoError) fail(QStringLiteral("实体存在开放边界，无法计算体积或质心"));
        }
        if (!hasShell) fail(QStringLiteral("实体不包含封闭壳，无法计算体积或质心"));
        GProp_GProps properties;
        const double error = BRepGProp::VolumeProperties(solid, properties, integrationTolerance, true, true);
        if (!std::isfinite(error) || error < 0) fail(QStringLiteral("实体体积积分失败"));
        requirePositive(properties.Mass(), QStringLiteral("实体体积不是有效正数，请检查实体方向和封闭性"));
        if (!finitePoint(properties.CentreOfMass())) fail(QStringLiteral("实体质心包含无效数值"));
        total.Add(properties);
    }
    return total;
}

struct DirectionInfo { gp_Dir direction; gp_Pnt point; bool normal = false; QString description; };
DirectionInfo measurementDirection(const MeasurementEntity& entity, MeasurementCoordinates coordinates) {
    if (entity.extent == MeasurementExtent::Infinite) {
        const auto reference=resolveReference(entity,coordinates);
        if (reference.line) return {reference.line->Direction(),reference.anchor,false,QStringLiteral("无限直线")};
        if (reference.plane) return {reference.plane->Axis().Direction(),reference.anchor,true,QStringLiteral("无限平面法线")};
    }
    if (entity.selection == MeasurementSelection::FeatureAxis) {
        const auto axis = *featureReference(entity, coordinates).axis;
        return {axis.Direction(), axis.Location(), false, QStringLiteral("无限参考轴")};
    }
    const auto shape = locatedShape(entity, coordinates);
    const auto fallbackPoint = [&](const gp_Pnt& value) {
        return entity.pickedPoint ? locatedPoint(entity, *entity.pickedPoint, coordinates) : value;
    };
    if (shape.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (entity.direction == MeasurementDirection::FeatureAxis) {
            if (curve.GetType() != GeomAbs_Circle) fail(QStringLiteral("边的特征轴只适用于解析圆或圆弧"));
            const gp_Circ circle = curve.Circle();
            return {circle.Axis().Direction(), circle.Location(), false, QStringLiteral("圆轴")};
        }
        if (curve.GetType() == GeomAbs_Line)
            return {curve.Line().Direction(), fallbackPoint(curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2.)),
                    false, QStringLiteral("直线方向")};
        if (!entity.curveParameter) fail(QStringLiteral("曲线切线角度需要先在曲线上选择参考点"));
        const double t = *entity.curveParameter;
        if (!std::isfinite(t) || t < curve.FirstParameter() - Precision::PConfusion()
            || t > curve.LastParameter() + Precision::PConfusion()) fail(QStringLiteral("曲线参考点参数无效，请重新选择"));
        gp_Pnt point; gp_Vec tangent;
        curve.D1(std::clamp(t, curve.FirstParameter(), curve.LastParameter()), point, tangent);
        if (tangent.SquareMagnitude() <= Precision::SquareConfusion()) fail(QStringLiteral("曲线在所选点处的切线无法确定"));
        return {gp_Dir(tangent), point, false, QStringLiteral("所选点切线")};
    }
    if (shape.ShapeType() == TopAbs_FACE) {
        const auto face = TopoDS::Face(shape);
        BRepAdaptor_Surface surface(face);
        if (entity.direction == MeasurementDirection::FeatureAxis) {
            if (surface.GetType() == GeomAbs_Cylinder) {
                const auto cylinder = surface.Cylinder();
                return {cylinder.Axis().Direction(), cylinder.Location(), false, QStringLiteral("圆柱轴")};
            }
            if (surface.GetType() == GeomAbs_Cone) {
                const auto cone = surface.Cone();
                return {cone.Axis().Direction(), cone.Location(), false, QStringLiteral("圆锥轴")};
            }
            fail(QStringLiteral("面的特征轴只适用于解析圆柱面或圆锥面"));
        }
        if (surface.GetType() == GeomAbs_Plane)
            return {surface.Plane().Axis().Direction(), fallbackPoint(surface.Plane().Location()), true, QStringLiteral("平面法线")};
        if (!entity.surfaceParameters) fail(QStringLiteral("曲面法线角度需要先在曲面上选择参考点"));
        const auto uv = *entity.surfaceParameters;
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) fail(QStringLiteral("曲面参考点参数无效，请重新选择"));
        BRepClass_FaceClassifier classifier(face, gp_Pnt2d(uv[0], uv[1]), std::max(BRep_Tool::Tolerance(face), Precision::Confusion()));
        if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON)
            fail(QStringLiteral("曲面参考点位于裁剪边界之外，请重新选择"));
        gp_Pnt point; gp_Vec du, dv;
        surface.D1(uv[0], uv[1], point, du, dv);
        const gp_Vec normal = du.Crossed(dv);
        if (normal.SquareMagnitude() <= Precision::SquareConfusion()) fail(QStringLiteral("曲面在所选点处的法线无法确定"));
        return {gp_Dir(normal), point, true, QStringLiteral("所选点法线")};
    }
    fail(QStringLiteral("角度需要选择边或面；点和实体没有唯一方向"));
}

void radiusMeasurement(const MeasurementEntity& entity, MeasurementCoordinates coordinates,
                       MeasurementKind kind, MeasurementResult& result) {
    const auto shape = locatedShape(entity, coordinates);
    double radius = 0;
    gp_Pnt center, reference;
    QString centerLabel = QStringLiteral("圆心 ");
    if (shape.ShapeType() == TopAbs_EDGE) {
        BRepAdaptor_Curve curve(TopoDS::Edge(shape));
        if (curve.GetType() != GeomAbs_Circle) fail(QStringLiteral("半径或直径需要解析圆、圆弧、圆柱面或球面"));
        const auto circle = curve.Circle();
        radius = circle.Radius(); center = circle.Location();
        const double parameter = entity.curveParameter.value_or(curve.FirstParameter());
        reference = curve.Value(std::clamp(parameter, curve.FirstParameter(), curve.LastParameter()));
        result.circle = circle;
        if (!curve.IsClosed()) result.note = QStringLiteral("测量圆弧所属解析圆的半径或直径");
    } else if (shape.ShapeType() == TopAbs_FACE) {
        BRepAdaptor_Surface surface(TopoDS::Face(shape));
        if (surface.GetType() == GeomAbs_Cylinder) {
            const auto cylinder = surface.Cylinder(); radius = cylinder.Radius();
            const double u = entity.surfaceParameters ? (*entity.surfaceParameters)[0] : 0.;
            const double v = entity.surfaceParameters ? (*entity.surfaceParameters)[1] :
                             (surface.FirstVParameter() + surface.LastVParameter()) / 2.;
            center = cylinder.Location().Translated(gp_Vec(cylinder.Axis().Direction()) * v);
            reference = surface.Value(u, v);
            result.circle = gp_Circ(gp_Ax2(center, cylinder.Axis().Direction(), cylinder.Position().XDirection()), radius);
            result.note = QStringLiteral("测量解析圆柱面的半径或直径");
            centerLabel = QStringLiteral("截面圆心 ");
        } else if (surface.GetType() == GeomAbs_Sphere) {
            const auto sphere = surface.Sphere(); radius = sphere.Radius(); center = sphere.Location();
            reference = entity.pickedPoint ? locatedPoint(entity, *entity.pickedPoint, coordinates) :
                        center.Translated(gp_Vec(sphere.Position().XDirection()) * radius);
            result.note = QStringLiteral("测量解析球面的半径或直径"); centerLabel = QStringLiteral("球心 ");
        } else fail(QStringLiteral("该面没有恒定解析半径；请选择圆柱面或球面"));
    } else fail(QStringLiteral("半径或直径需要选择圆边、圆弧边、圆柱面或球面"));
    requirePositive(radius, QStringLiteral("半径不是有效正数"));
    result.values.push_back({kind == MeasurementKind::Radius ? QStringLiteral("半径") : QStringLiteral("直径"),
                             QStringLiteral("mm"), radius * (kind == MeasurementKind::Diameter ? 2. : 1.)});
    addCoordinates(result, center, centerLabel);
    result.anchors = {center, reference};
}
} // namespace

QString measurementKindText(MeasurementKind kind) {
    switch (kind) {
    case MeasurementKind::Point: return QStringLiteral("点坐标");
    case MeasurementKind::Length: return QStringLiteral("长度");
    case MeasurementKind::Radius: return QStringLiteral("半径");
    case MeasurementKind::Diameter: return QStringLiteral("直径");
    case MeasurementKind::Area: return QStringLiteral("面积");
    case MeasurementKind::Volume: return QStringLiteral("体积");
    case MeasurementKind::Mass: return QStringLiteral("质量");
    case MeasurementKind::CenterOfMass: return QStringLiteral("质量中心");
    case MeasurementKind::VolumeCenter: return QStringLiteral("体积中心");
    case MeasurementKind::Distance: return QStringLiteral("最小距离");
    case MeasurementKind::Angle: return QStringLiteral("角度");
    }
    return {};
}
QString measurementCoordinatesText(MeasurementCoordinates coordinates) {
    return coordinates == MeasurementCoordinates::Original ? QStringLiteral("原始装配位置") : QStringLiteral("当前显示位置");
}
QString measurementResultText(const MeasurementResult& result, int decimals) {
    if (!result.success) return result.error;
    QStringList lines;
    const int precision = std::clamp(decimals, 0, 12);
    for (const auto& value : result.values)
        lines << QStringLiteral("%1：%2%3").arg(value.label, QString::number(value.value == 0 ? 0. : value.value, 'f', precision),
                                              value.unit.isEmpty() ? QString{} : QStringLiteral(" ") + value.unit);
    if (!result.note.isEmpty()) lines << result.note;
    return lines.join(QLatin1Char('\n'));
}

MeasurementResult calculateMeasurement(const MeasurementRequest& request, std::atomic_bool& cancel,
                                      const ProgressCallback& progress) {
    MeasurementResult result;
    try {
        checkCancel(cancel);
        if (progress) progress(0, QStringLiteral("准备精确几何测量"));
        const auto a = uniqueEntities(request.a), b = uniqueEntities(request.b);
        if (a.empty()) fail(QStringLiteral("请先选择测量对象 A"));
        for (const auto* group : {&a, &b}) for (const auto& entity : *group) {
            if (entity.extent == MeasurementExtent::Infinite) {
                if (request.kind != MeasurementKind::Distance && request.kind != MeasurementKind::Angle)
                    fail(QStringLiteral("无限拓展用于测量之间的距离或角度"));
                resolveReference(entity,request.coordinates);
            }
            if (entity.selection == MeasurementSelection::FeatureAxis && request.kind != MeasurementKind::Distance && request.kind != MeasurementKind::Angle)
                fail(QStringLiteral("无限参考轴用于距离或角度测量"));
            if (entity.selection == MeasurementSelection::FeatureCenter && request.kind != MeasurementKind::Point && request.kind != MeasurementKind::Distance)
                fail(QStringLiteral("圆心用于点坐标或距离测量"));
        }
        if (request.kind == MeasurementKind::Distance || request.kind == MeasurementKind::Angle) {
            if (b.empty()) fail(QStringLiteral("请先选择测量对象 B"));
        } else if (!b.empty()) fail(QStringLiteral("此测量只需要对象 A，请清空对象 B"));
        switch (request.kind) {
        case MeasurementKind::Point: {
            requireSingle(a, QStringLiteral("点坐标测量只允许一个点"));
            const auto point = selectedPoint(a.front(), request.coordinates);
            addCoordinates(result, point); result.anchors = {point};
            if (a.front().selection == MeasurementSelection::FeatureCenter) result.note = QStringLiteral("解析圆或圆弧的圆心；非鼠标拾取点");
            break;
        }
        case MeasurementKind::Radius:
        case MeasurementKind::Diameter:
            requireSingle(a, QStringLiteral("半径或直径测量只允许一个解析几何对象"));
            radiusMeasurement(a.front(), request.coordinates, request.kind, result); break;
        case MeasurementKind::Distance: {
            const bool hasInfinite = std::any_of(a.begin(), a.end(), [](const auto& e) { return e.selection == MeasurementSelection::FeatureAxis || e.extent == MeasurementExtent::Infinite; })
                || std::any_of(b.begin(), b.end(), [](const auto& e) { return e.selection == MeasurementSelection::FeatureAxis || e.extent == MeasurementExtent::Infinite; });
            DistanceWitness witness;
            if (hasInfinite) {
                witness.value = std::numeric_limits<double>::infinity();
                for (const auto& first : a) for (const auto& second : b) {
                    const auto candidate = referenceDistance(first, second, request.coordinates, cancel, progress);
                    if (candidate.value < witness.value) witness = candidate;
                }
            } else witness = boundedDistance(groupShape(a, request.coordinates), groupShape(b, request.coordinates), cancel, progress);
            const double value = witness.value;
            const auto pointA = witness.a, pointB = witness.b;
            const bool containment = witness.containment;
            result.values.push_back({QStringLiteral("最小距离"), QStringLiteral("mm"), value});
            result.values.push_back({QStringLiteral("ΔX"), QStringLiteral("mm"), pointB.X() - pointA.X()});
            result.values.push_back({QStringLiteral("ΔY"), QStringLiteral("mm"), pointB.Y() - pointA.Y()});
            result.values.push_back({QStringLiteral("ΔZ"), QStringLiteral("mm"), pointB.Z() - pointA.Z()});
            addCoordinates(result, pointA, QStringLiteral("A 最近点 "));
            addCoordinates(result, pointB, QStringLiteral("B 最近点 "));
            result.anchors = {pointA, pointB};
            if (containment) result.note = QStringLiteral("对象相交或存在实体包含关系，最小距离为 0");
            if (hasInfinite) result.note = QStringLiteral("无限参考几何最小距离；显示长度和大小不参与计算")
                + (containment ? QStringLiteral("；对象相交或包含，距离为 0") : QString());
            if (witness.nonUnique) result.note += QStringLiteral("；最近点不唯一，显示靠近拾取位置的代表解");
            break;
        }
        case MeasurementKind::Angle: {
            requireSingle(a, QStringLiteral("角度测量的对象 A 必须是单个边或面"));
            requireSingle(b, QStringLiteral("角度测量的对象 B 必须是单个边或面"));
            const auto first = measurementDirection(a.front(), request.coordinates);
            const auto second = measurementDirection(b.front(), request.coordinates);
            gp_Dir armA = first.direction, armB = second.direction;
            double dot = std::clamp(std::abs(armA.Dot(armB)), 0., 1.);
            const bool lineFace = first.normal != second.normal;
            const double radians = lineFace ? std::asin(dot) : std::acos(dot);
            if (lineFace) {
                const gp_Dir line = first.normal ? second.direction : first.direction;
                const gp_Dir normal = first.normal ? first.direction : second.direction;
                gp_Vec inPlane = gp_Vec(line) - gp_Vec(normal) * line.Dot(normal);
                if (inPlane.SquareMagnitude() <= Precision::SquareConfusion()) {
                    const gp_Dir basis = std::abs(normal.X()) < 0.8 ? gp_Dir(1, 0, 0) : gp_Dir(0, 1, 0);
                    inPlane = gp_Vec(normal).Crossed(gp_Vec(basis));
                }
                if (first.normal) { armA = gp_Dir(inPlane); armB = line; }
                else { armA = line; armB = gp_Dir(inPlane); }
            } else if (armA.Dot(armB) < 0) armB.Reverse();
            const gp_Pnt origin = first.point.Translated(gp_Vec(first.point, second.point) / 2.);
            result.anchors = {origin, origin.Translated(gp_Vec(armA) * 10.), origin.Translated(gp_Vec(armB) * 10.)};
            result.directionA = armA; result.directionB = armB;
            result.values.push_back({QStringLiteral("夹角"), QStringLiteral("°"), radians * 180. / pi});
            result.note = QStringLiteral("%1 / %2；%3（0–90°，方向无关）")
                          .arg(first.description, second.description, lineFace ? QStringLiteral("线面夹角") : QStringLiteral("较小夹角"));
            break;
        }
        case MeasurementKind::Length:
        case MeasurementKind::Area:
        case MeasurementKind::Volume:
        case MeasurementKind::Mass:
        case MeasurementKind::CenterOfMass:
        case MeasurementKind::VolumeCenter: {
            GProp_GProps geometry, mass;
            double totalSurfaceArea = 0;
            bool allDensity = true;
            const bool physical = request.kind == MeasurementKind::Mass || request.kind == MeasurementKind::CenterOfMass;
            for (size_t index = 0; index < a.size(); ++index) {
                checkCancel(cancel);
                const auto& entity = a[index];
                const auto shape = locatedShape(entity, request.coordinates);
                GProp_GProps properties;
                int dimension = 3;
                if (request.kind == MeasurementKind::Length) dimension = 1;
                else if (request.kind == MeasurementKind::Area) dimension = 2;
                if (dimension == 1) {
                    if (shape.ShapeType() != TopAbs_EDGE && shape.ShapeType() != TopAbs_WIRE)
                        fail(QStringLiteral("长度测量需要选择边或线框"));
                    BRepGProp::LinearProperties(shape, properties, true, false);
                } else if (dimension == 2) {
                    TopTools_IndexedMapOfShape faces; TopExp::MapShapes(shape, TopAbs_FACE, faces);
                    if (faces.IsEmpty()) fail(QStringLiteral("面积测量需要选择面、面壳或实体"));
                    const double error = BRepGProp::SurfaceProperties(shape, properties, integrationTolerance, true);
                    if (!std::isfinite(error) || error < 0) fail(QStringLiteral("面积积分失败"));
                } else {
                    properties = volumeProperties(shape, cancel);
                    if (request.kind == MeasurementKind::Volume) {
                        GProp_GProps surface;
                        const double error = BRepGProp::SurfaceProperties(shape, surface, integrationTolerance, true);
                        if (!std::isfinite(error) || error < 0) fail(QStringLiteral("表面积积分失败"));
                        requirePositive(surface.Mass(), QStringLiteral("实体表面积不是有效正数"));
                        totalSurfaceArea += surface.Mass();
                    }
                }
                requirePositive(properties.Mass(), QStringLiteral("所选对象的几何量不是有效正数"));
                if (!finitePoint(properties.CentreOfMass())) fail(QStringLiteral("几何中心包含无效数值"));
                geometry.Add(properties);
                if (physical) {
                    if (!entity.densityKgM3 || !std::isfinite(*entity.densityKgM3) || *entity.densityKgM3 <= 0)
                        allDensity = false;
                    else {
                        const double kg = properties.Mass() * (*entity.densityKgM3 * 1.e-9);
                        requirePositive(kg, QStringLiteral("实体质量超出有效数值范围，请检查密度"));
                        mass.Add(properties, *entity.densityKgM3 * 1.e-9);
                    }
                }
                if (progress) progress(10 + int((index + 1) * 80 / a.size()), QStringLiteral("计算几何属性和中心"));
            }
            checkCancel(cancel);
            const auto geometricCenter = geometry.CentreOfMass();
            gp_Pnt center = geometricCenter;
            QString centerLabel;
            if (request.kind == MeasurementKind::Length) {
                result.values.push_back({QStringLiteral("长度"), QStringLiteral("mm"), geometry.Mass()}); centerLabel = QStringLiteral("长度中心 ");
            } else if (request.kind == MeasurementKind::Area) {
                result.values.push_back({QStringLiteral("面积"), QStringLiteral("mm²"), geometry.Mass()}); centerLabel = QStringLiteral("面积中心 ");
            } else if (request.kind == MeasurementKind::Volume) {
                result.values.push_back({QStringLiteral("体积"), QStringLiteral("mm³"), geometry.Mass()});
                result.values.push_back({QStringLiteral("表面积"), QStringLiteral("mm²"), totalSurfaceArea});
                centerLabel = QStringLiteral("体积中心 ");
            } else if (physical) {
                if (!allDensity) fail(QStringLiteral("部分实体缺少有效密度，无法计算质量或质量中心；可在材料属性中补充密度，或独立测量体积中心"));
                requirePositive(mass.Mass(), QStringLiteral("质量不是有效正数"));
                center = mass.CentreOfMass();
                result.values.push_back({QStringLiteral("质量"), QStringLiteral("kg"), mass.Mass()});
                result.values.push_back({QStringLiteral("体积"), QStringLiteral("mm³"), geometry.Mass()});
                addCoordinates(result, geometricCenter, QStringLiteral("体积中心 "));
                centerLabel = QStringLiteral("质量中心 ");
                result.note = QStringLiteral("使用各实例有效密度进行质量加权；重复实例分别计入，未进行布尔合并");
            } else {
                result.values.push_back({QStringLiteral("体积"), QStringLiteral("mm³"), geometry.Mass()});
                centerLabel = QStringLiteral("体积中心 ");
                result.note = QStringLiteral("按体积加权，不使用密度；重复实例分别计入，未进行布尔合并");
            }
            if (a.size() > 1 && result.note.isEmpty()) result.note = QStringLiteral("按选中几何和实例求和；重叠区域未进行布尔合并");
            addCoordinates(result, center, centerLabel); result.anchors = {center};
            break;
        }
        }
        checkCancel(cancel);
        for (const auto& value : result.values)
            if (!std::isfinite(value.value)) fail(QStringLiteral("测量结果包含无效数值"));
        for (const auto& anchor : result.anchors)
            if (!finitePoint(anchor)) fail(QStringLiteral("测量标记位置包含无效数值"));
        result.success = true;
        if (progress) progress(100, QStringLiteral("测量完成"));
    } catch (const MeasurementFailure& failure) {
        result = {}; result.error = failure.text;
    } catch (const Standard_Failure& failure) {
        result = {}; result.error = QStringLiteral("精确几何测量失败：%1").arg(QString::fromUtf8(failure.GetMessageString()));
    } catch (const std::exception& failure) {
        result = {}; result.error = QStringLiteral("测量失败：%1").arg(QString::fromUtf8(failure.what()));
    } catch (...) {
        result = {}; result.error = QStringLiteral("精确几何测量发生未知错误");
    }
    if (cancel.load()) { result = {}; result.error = QStringLiteral("测量已取消"); }
    return result;
}

QString measurementReferenceText(const MeasurementEntity& entity) {
    if (entity.selection==MeasurementSelection::FeatureAxis) return QStringLiteral("无限轴");
    if (entity.selection==MeasurementSelection::FeatureCenter) return QStringLiteral("圆心");
    if (entity.selection==MeasurementSelection::Edge)
        return entity.extent==MeasurementExtent::Infinite?QStringLiteral("无限直线"):QStringLiteral("有界边");
    if (entity.selection==MeasurementSelection::Face)
        return entity.extent==MeasurementExtent::Infinite?QStringLiteral("无限平面"):QStringLiteral("裁剪面");
    return {};
}
std::optional<MeasurementReference> measurementReference(const MeasurementEntity& entity,
    MeasurementCoordinates coordinates,QString* error) {
    try {
        auto result=resolveReference(entity,coordinates);
        if (error) error->clear();
        return result;
    } catch (const MeasurementFailure& failure) { if (error) *error=failure.text;
    } catch (const Standard_Failure& failure) { if (error) *error=QString::fromUtf8(failure.GetMessageString());
    } catch (...) { if (error) *error=QStringLiteral("测量参考几何无效"); }
    return std::nullopt;
}

std::optional<MeasurementFeature> measurementFeatureReference(const MeasurementEntity& entity,
    MeasurementCoordinates coordinates, QString* error) {
    try {
        auto result = featureReference(entity, coordinates);
        if (error) error->clear();
        return result;
    } catch (const MeasurementFailure& failure) {
        if (error) *error = failure.text;
    } catch (const Standard_Failure& failure) {
        if (error) *error = QString::fromUtf8(failure.GetMessageString());
    } catch (...) {
        if (error) *error = QStringLiteral("解析圆心或轴线无效");
    }
    return std::nullopt;
}

bool refineMeasurementPick(MeasurementEntity& entity, const gp_Pnt& localSample, QString* error) {
    try {
        if (!finitePoint(localSample) || entity.shape.IsNull()) fail(QStringLiteral("参考点或几何无效，请重新选择"));
        const auto shape = entity.shape;
        if (entity.selection == MeasurementSelection::FeatureCenter || entity.selection == MeasurementSelection::FeatureAxis) {
            // Validate the analytic source, independently of tessellation/pick samples.
            featureReference(entity, MeasurementCoordinates::Original);
        }
        MeasurementEntity refined = entity;
        refined.curveParameter.reset(); refined.surfaceParameters.reset();
        if (shape.ShapeType() == TopAbs_VERTEX) {
            refined.pickedPoint = BRep_Tool::Pnt(TopoDS::Vertex(shape));
        } else {
            // A point inside a solid must snap to its boundary, not to the zero
            // distance containment solution of the volume itself.
            TopoDS_Shape boundary = shape;
            if (shape.ShapeType() < TopAbs_FACE) {
                TopTools_IndexedMapOfShape faces; TopExp::MapShapes(shape, TopAbs_FACE, faces);
                if (!faces.IsEmpty()) {
                    TopoDS_Compound compound; BRep_Builder builder; builder.MakeCompound(compound);
                    for (int i = 1; i <= faces.Extent(); ++i) builder.Add(compound, faces(i));
                    boundary = compound;
                }
            }
            BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(localSample).Shape(), boundary);
            if (!distance.IsDone() || distance.NbSolution() < 1) fail(QStringLiteral("无法将参考点投影到精确几何"));
            const gp_Pnt snapped = distance.PointOnShape2(1);
            if (!finitePoint(snapped)) fail(QStringLiteral("精确参考点包含无效数值"));
            refined.pickedPoint = snapped;
            if (shape.ShapeType() == TopAbs_EDGE) {
                BRepAdaptor_Curve curve(TopoDS::Edge(shape));
                double parameter = 0;
                if (distance.SupportTypeShape2(1) == BRepExtrema_IsOnEdge) distance.ParOnEdgeS2(1, parameter);
                else {
                    const double first = curve.FirstParameter(), last = curve.LastParameter();
                    parameter = curve.Value(first).SquareDistance(snapped) <= curve.Value(last).SquareDistance(snapped) ? first : last;
                }
                if (!std::isfinite(parameter)) fail(QStringLiteral("曲线参考点参数无效"));
                refined.curveParameter = std::clamp(parameter, curve.FirstParameter(), curve.LastParameter());
            } else if (shape.ShapeType() == TopAbs_FACE) {
                const auto face = TopoDS::Face(shape);
                const double tolerance = std::max(BRep_Tool::Tolerance(face), Precision::Confusion());
                double uMin, uMax, vMin, vMax; BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
                const auto surface = BRep_Tool::Surface(face);
                if (surface.IsNull()) fail(QStringLiteral("该面缺少精确几何曲面"));
                GeomAPI_ProjectPointOnSurf projection(snapped, surface, uMin, uMax, vMin, vMax, tolerance);
                double bestDistance = std::numeric_limits<double>::infinity();
                for (int i = 1; i <= projection.NbPoints(); ++i) {
                    double u, v; projection.Parameters(i, u, v);
                    BRepClass_FaceClassifier classifier(face, gp_Pnt2d(u, v), tolerance);
                    if (classifier.State() != TopAbs_IN && classifier.State() != TopAbs_ON) continue;
                    const double delta = projection.Point(i).Distance(snapped);
                    if (delta < bestDistance) { bestDistance = delta; refined.surfaceParameters = std::array<double, 2>{{u, v}}; }
                }
                if (!refined.surfaceParameters || bestDistance > tolerance * 10.)
                    fail(QStringLiteral("无法在裁剪面的有效边界内确定参考点参数"));
            }
        }
        entity = std::move(refined);
        if (error) error->clear();
        return true;
    } catch (const MeasurementFailure& failure) {
        if (error) *error = failure.text;
    } catch (const Standard_Failure& failure) {
        if (error) *error = QStringLiteral("精确参考点投影失败：%1").arg(QString::fromUtf8(failure.GetMessageString()));
    } catch (...) {
        if (error) *error = QStringLiteral("精确参考点投影失败");
    }
    return false;
}
} // namespace Stp
