#include "section_state.h"
#include <BRepAdaptor_Surface.hxx>
#include <gp_Ax1.hxx>
#include <cmath>
#include <stdexcept>

namespace Stp {
gp_Pln sectionPlane(const SectionState& s) {
    if (s.rotationAxis < 0 || s.rotationAxis > 2 || !std::isfinite(s.angleDegrees)
        || !std::isfinite(s.offsetMm) || !std::isfinite(s.pivot.X())
        || !std::isfinite(s.pivot.Y()) || !std::isfinite(s.pivot.Z()))
        throw std::invalid_argument("剖切参数无效");
    const gp_Dir axes[] {gp_Dir(1,0,0),gp_Dir(0,1,0),gp_Dir(0,0,1)};
    gp_Trsf rotation; rotation.SetRotation(gp_Ax1(s.pivot, axes[s.rotationAxis]),
        s.angleDegrees * std::acos(-1.) / 180.);
    auto plane = s.base.Transformed(rotation);
    plane.Translate(gp_Vec(plane.Axis().Direction()) * s.offsetMm);
    if (s.reverse) plane.SetAxis(gp_Ax1(plane.Location(), plane.Axis().Direction().Reversed()));
    return plane;
}
gp_Pln sectionPlaneFromFace(const TopoDS_Face& face, const gp_Trsf& world, const gp_Pnt& pick) {
    if (face.IsNull()) throw std::invalid_argument("请选择零件上的平面");
    BRepAdaptor_Surface surface(face);
    if (surface.GetType() != GeomAbs_Plane) throw std::invalid_argument("当前仅支持平面；圆柱面、球面等曲面不能直接作为基准");
    auto plane = surface.Plane().Transformed(world);
    auto normal = plane.Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
    const auto projected = pick.Translated(gp_Vec(normal) * -gp_Vec(plane.Location(),pick).Dot(gp_Vec(normal)));
    return gp_Pln(projected, normal);
}
}
