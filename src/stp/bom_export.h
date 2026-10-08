#pragma once
#include "cad_document.h"
#include <QMap>

namespace Stp {
// Writes real XLSX bytes, then atomically commits to path. A cancellation before
// commit preserves any existing target. Throws std::runtime_error on failure/cancel.
void exportBom(const CadDocument& document, const QString& path,
               std::atomic_bool* cancel = nullptr);
void exportPartManifest(const CadDocument& document, const QString& path,
                        const QMap<int, QString>& filenames, const QStringList& diagnostics);
}
