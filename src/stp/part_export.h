#pragma once
#include "cad_document.h"
class STEPCAFControl_Writer;

namespace Stp {
struct ExportResult {
    QString directory, manifestPath;
    QStringList files, diagnostics;
};
QString safePartFilename(const QString& proposed, int fallbackIndex);
// Repair OCCT 7.9.3's generated density unit exponents before serializing.
// Call only on a fresh writer populated from validated g/cm3 XDE material data.
void prepareStepMaterialExport(STEPCAFControl_Writer& writer);
// Empty selection means all leaf definitions. Output is always a new directory.
ExportResult exportParts(const CadDocument& document,
                         const std::vector<int>& selectedOccurrences,
                         const QString& parentDirectory, const ProgressCallback& progress,
                         std::atomic_bool& cancel);
}
