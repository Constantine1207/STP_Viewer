#include "color_edits.h"
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <stdexcept>
#include <algorithm>

namespace Stp {
void validateColorEdits(const CadDocument& doc, const ColorEdits& edits) {
    for (const auto& [index,color] : edits)
        if (index < 0 || size_t(index) >= doc.occurrences.size()
            || doc.occurrences[size_t(index)].partIndex < 0 || !color.isValid() || color.alpha() != 255)
            throw std::invalid_argument("零件着色参数无效");
}
QColor occurrenceBaseColor(const CadDocument& doc, int index) {
    if (doc.xde.IsNull() || index < 0 || size_t(index) >= doc.occurrences.size()) return QColor("#cccccc");
    const auto colors = XCAFDoc_DocumentTool::ColorTool(doc.xde->Main());
    Quantity_ColorRGBA color(Quantity_Color(0.8,0.8,0.8,Quantity_TOC_sRGB));
    const auto apply = [&](const TDF_Label& label) {
        if (label.IsNull()) return;
        const auto mat = XCAFDoc_VisMaterialTool::GetShapeMaterial(label);
        if (!mat.IsNull()) color = mat->BaseColor();
        Quantity_ColorRGBA value;
        if (colors->GetColor(label,XCAFDoc_ColorGen,value)) color=value;
        if (colors->GetColor(label,XCAFDoc_ColorSurf,value)) color=value;
    };
    std::vector<int> chain;
    for (int i=index; i>=0; i=doc.occurrences[size_t(i)].parent) chain.push_back(i);
    for (auto it=chain.rbegin();it!=chain.rend();++it) {
        const auto& o=doc.occurrences[size_t(*it)]; apply(o.definitionLabel); apply(o.label);
    }
    const auto& rgb=color.GetRGB();
    double r,g,b; rgb.Values(r,g,b,Quantity_TOC_sRGB);
    return QColor::fromRgbF(r,g,b,color.Alpha()).convertTo(QColor::Rgb);
}
}
