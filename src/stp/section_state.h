#pragma once
#include <gp_Pln.hxx>
#include <TopoDS_Face.hxx>
#include <QString>

namespace Stp {
// Display-only, world-space plane. Every update is evaluated from the base,
// never accumulated from a previously rounded/rotated result.
struct SectionState {
    bool enabled = false, reverse = false, cap = true, objectColor = true, showPlane = false;
    gp_Pln base;
    gp_Pnt pivot;
    int rotationAxis = 0;
    double angleDegrees = 0., offsetMm = 0.;
};
gp_Pln sectionPlane(const SectionState&);
gp_Pln sectionPlaneFromFace(const TopoDS_Face&, const gp_Trsf& world,
                           const gp_Pnt& worldPick);
}
