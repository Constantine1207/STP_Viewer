#pragma once
#include "part_placement.h"
namespace Stp {
Handle(TDocStd_Document) buildPlacedAssembly(const CadDocument&, const PlacementEdits&,
                                           std::atomic_bool& cancel);
// Writes a new complete assembly. Display offsets, visibility and explosion are excluded.
void exportPlacedAssembly(const CadDocument&, const PlacementEdits&, const QString& path,
                          const ProgressCallback&, std::atomic_bool& cancel);
} // namespace Stp
