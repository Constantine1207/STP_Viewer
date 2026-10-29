#pragma once
#include "cad_document.h"
#include <gp_Pnt.hxx>
#include <map>

namespace Stp {
// World-space rigid deltas, keyed by occurrence index, never by shared definition.
using PlacementEdits = std::map<int, gp_Trsf>;
bool samePlacement(const gp_Trsf& a, const gp_Trsf& b, double tolerance = 1.e-10);
gp_Trsf placementDelta(const PlacementEdits& edits, int occurrence);
gp_Trsf placedWorldTransform(const CadDocument&, const PlacementEdits&, int occurrence);
gp_Pnt placedPartCenter(const CadDocument&, const PlacementEdits&, int occurrence);
void validatePlacementEdits(const CadDocument&, const PlacementEdits&);
} // namespace Stp
