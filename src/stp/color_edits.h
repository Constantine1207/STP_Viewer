#pragma once
#include "cad_document.h"
#include <QColor>
#include <map>

namespace Stp {
// Persistent STEP edits, keyed by occurrence rather than shared part definition.
using ColorEdits = std::map<int, QColor>;
void validateColorEdits(const CadDocument&, const ColorEdits&);
QColor occurrenceBaseColor(const CadDocument&, int occurrence);
}
