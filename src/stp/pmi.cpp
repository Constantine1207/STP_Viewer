#include "pmi.h"
#include "cad_document.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_CurveType.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <Precision.hxx>
#include <PrsDim_AngleDimension.hxx>
#include <PrsDim_DiameterDimension.hxx>
#include <PrsDim_LengthDimension.hxx>
#include <PrsDim_RadiusDimension.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelSequence.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <XCAFDoc_Datum.hxx>
#include <XCAFDoc_Dimension.hxx>
#include <XCAFDoc_DimTolTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_GeomTolerance.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDimTolObjects_DatumObject.hxx>
#include <XCAFDimTolObjects_DimensionObject.hxx>
#include <XCAFDimTolObjects_GeomToleranceObject.hxx>
#include <gp_Pln.hxx>
#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Stp {
namespace {
QString text(const Handle(TCollection_HAsciiString)& value)
{
    return value.IsNull() ? QString() : QString::fromUtf8(value->ToCString());
}
QString number(double value) { return QString::number(value, 'g', 15); }
template<class T> QString details(const Handle(T)& object)
{
    std::ostringstream stream;
    object->DumpJson(stream);
    return QString::fromStdString(stream.str());
}
template<class T> QString modifierCodes(const T& values)
{
    QStringList result;
    for (int i = values.Lower(); i <= values.Upper(); ++i)
        result << QString::number(static_cast<int>(values.Value(i)));
    return result.join(',');
}
bool labelWithin(const TDF_Label& label, const TDF_Label& parent)
{
    for (TDF_Label current = label; !current.IsNull(); current = current.Father())
        if (current == parent) return true;
    return false;
}
void appendUnique(QStringList& values, const QString& value)
{
    if (!value.isEmpty() && !values.contains(value)) values << value;
}
void addReferences(PmiRecord& record, const TDF_Label& label)
{
    TDF_LabelSequence first, second;
    XCAFDoc_DimTolTool::GetRefShapeLabel(label, first, second);
    for (const auto* labels : { &first, &second }) {
        for (int i = 1; i <= labels->Length(); ++i) {
            const auto& reference = labels->Value(i);
            if (std::none_of(record.referencedLabels.begin(), record.referencedLabels.end(),
                             [&](const TDF_Label& existing) { return existing == reference; }))
                record.referencedLabels.push_back(reference);
        }
    }
}
void resolveOwners(PmiRecord& record, const CadDocument& indexedDocument)
{
    // A checked-instance export scope does not establish new facts about the
    // original PMI coordinate system or remove its cross-part associations.
    const auto& document=indexedDocument.sourceAssemblyContext?*indexedDocument.sourceAssemblyContext:indexedDocument;
    record.associationComplete = !record.referencedLabels.empty();
    for (const auto& reference : record.referencedLabels) {
        bool resolved = false;
        for (const auto& part : document.parts) {
            if (labelWithin(reference, part.label)) {
                appendUnique(record.ownerPartIds, part.id);
                resolved = true;
            }
        }
        for (const auto& occurrence : document.occurrences) {
            if (occurrence.partIndex < 0 || occurrence.partIndex >= static_cast<int>(document.parts.size()))
                continue;
            if (labelWithin(reference, occurrence.label)) {
                appendUnique(record.ownerPartIds, document.parts[occurrence.partIndex].id);
                appendUnique(record.ownerOccurrenceIds, occurrence.id);
                resolved = true;
            }
        }
        record.associationComplete = record.associationComplete && resolved;
    }
    for (const auto& occurrence : document.occurrences) {
        if (occurrence.partIndex >= 0 && occurrence.partIndex < static_cast<int>(document.parts.size()) &&
            record.ownerPartIds.contains(document.parts[occurrence.partIndex].id))
            appendUnique(record.ownerOccurrenceIds, occurrence.id);
    }
    record.crossPart = record.ownerPartIds.size() > 1;
    record.safeForPartExport = record.associationComplete && record.ownerPartIds.size() == 1;
    // With one owner at the identity placement, definition and assembly coordinates
    // coincide. This is sufficient to move the imported presentation by the view's
    // explosion delta without guessing a representation-context transform.
    // Repeated instances and every non-identity placement remain unverified.
    if (record.safeForPartExport && record.ownerOccurrenceIds.size() == 1) {
        const auto owner = std::find_if(document.occurrences.begin(), document.occurrences.end(),
            [&](const Occurrence& occurrence) { return occurrence.id == record.ownerOccurrenceIds.front(); });
        if (owner != document.occurrences.end()) {
            bool identity = true;
            for (int row = 1; row <= 3; ++row)
                for (int column = 1; column <= 4; ++column)
                    identity = identity && std::abs(owner->worldTransform.Value(row, column)
                        - (row == column ? 1.0 : 0.0)) <= 1e-12;
            record.occurrencePlacementVerified = identity;
        }
    }
    // A shape association alone does not prove that imported presentation points
    // are definition-local. Until representation-context transforms are resolved,
    // copying such annotations into a split part could move them to a wrong place.
    record.safeForPartExport = record.safeForPartExport && record.occurrencePlacementVerified;
}
QString dimensionTypeText(XCAFDimTolObjects_DimensionType type)
{
    switch (type) {
    case XCAFDimTolObjects_DimensionType_Location_LinearDistance: return QStringLiteral("线性距离");
    case XCAFDimTolObjects_DimensionType_Location_Angular: return QStringLiteral("角度位置");
    case XCAFDimTolObjects_DimensionType_Size_Angular: return QStringLiteral("角度尺寸");
    case XCAFDimTolObjects_DimensionType_Size_Radius: return QStringLiteral("半径");
    case XCAFDimTolObjects_DimensionType_Size_Diameter: return QStringLiteral("直径");
    case XCAFDimTolObjects_DimensionType_Size_Thickness: return QStringLiteral("厚度");
    default: return QStringLiteral("尺寸类型 %1").arg(static_cast<int>(type));
    }
}
QString dimensionText(const Handle(XCAFDimTolObjects_DimensionObject)& object)
{
    QStringList result { dimensionTypeText(object->GetType()) };
    const auto values = object->GetValues();
    if (values.IsNull() || values->Length() == 0) result << QStringLiteral("数值缺失");
    else if (object->IsDimWithRange())
        result << QStringLiteral("范围 %1 ～ %2").arg(number(object->GetLowerBound()), number(object->GetUpperBound()));
    else {
        result << number(object->GetValue());
        if (object->IsDimWithPlusMinusTolerance())
            result << QStringLiteral("下偏差 %1 / 上偏差 %2").arg(number(object->GetLowerTolValue()), number(object->GetUpperTolValue()));
    }
    if (object->HasQualifier()) result << QStringLiteral("限定符代码 %1").arg(static_cast<int>(object->GetQualifier()));
    if (object->HasAngularQualifier()) result << QStringLiteral("角度限定符代码 %1").arg(static_cast<int>(object->GetAngularQualifier()));
    if (!object->GetModifiers().IsEmpty()) result << QStringLiteral("修饰符代码 %1").arg(modifierCodes(object->GetModifiers()));
    if (object->IsDimWithClassOfTolerance()) {
        Standard_Boolean hole;
        XCAFDimTolObjects_DimensionFormVariance form;
        XCAFDimTolObjects_DimensionGrade grade;
        object->GetClassOfTolerance(hole, form, grade);
        result << QStringLiteral("配合公差：%1，偏差代码 %2，等级代码 %3")
                      .arg(hole ? QStringLiteral("孔") : QStringLiteral("轴")).arg(static_cast<int>(form)).arg(static_cast<int>(grade));
    }
    if (!object->GetPath().IsNull()) result << QStringLiteral("沿曲线路径测量");
    for (int i = 0; i < object->NbDescriptions(); ++i)
        result << QStringLiteral("%1：%2").arg(text(object->GetDescriptionName(i)), text(object->GetDescription(i)));
    return result.join(QStringLiteral("；"));
}
bool nearlyEqual(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= std::max(1e-6, std::max(std::abs(a), std::abs(b)) * 1e-7);
}
std::vector<TopoDS_Shape> referenceShapes(const PmiRecord& record)
{
    std::vector<TopoDS_Shape> result;
    for (const auto& label : record.referencedLabels) {
        TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
        if (!shape.IsNull()) result.push_back(shape);
    }
    return result;
}
bool prepareAngularGeometry(PmiRecord& record)
{
    const auto shapes = referenceShapes(record);
    if (shapes.size() != 2 || shapes[0].ShapeType() != TopAbs_EDGE || shapes[1].ShapeType() != TopAbs_EDGE)
        return false;
    std::array<gp_Pnt, 2> ends[2];
    for (int i = 0; i < 2; ++i) {
        const TopoDS_Edge edge = TopoDS::Edge(shapes[i]);
        if (BRepAdaptor_Curve(edge).GetType() != GeomAbs_Line) return false;
        TopoDS_Vertex first, last;
        TopExp::Vertices(edge, first, last);
        if (first.IsNull() || last.IsNull()) return false;
        ends[i] = { BRep_Tool::Pnt(first), BRep_Tool::Pnt(last) };
    }
    // Require an existing common endpoint. Do not infer an intersection/apex.
    const gp_Pln plane(record.annotationPlane);
    for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
        if (ends[0][i].Distance(ends[1][j]) > Precision::Confusion()) continue;
        record.angleCenter = ends[0][i]; record.point1 = ends[0][1-i]; record.point2 = ends[1][1-j];
        const gp_Vec a(record.angleCenter, record.point1), b(record.angleCenter, record.point2);
        if (a.Magnitude() <= Precision::Confusion() || b.Magnitude() <= Precision::Confusion()) return false;
        // OCCT STEPCAFControl_Reader::convertAngleValue stores degrees in XDE.
        const double measuredDegrees = a.Angle(b) * 180.0 / std::acos(-1.0);
        if (!nearlyEqual(measuredDegrees, record.value) || measuredDegrees < 1e-6 || measuredDegrees >= 180.0 - 1e-6)
            return false;
        return plane.Distance(record.point1) <= 1e-6 && plane.Distance(record.point2) <= 1e-6 && plane.Distance(record.angleCenter) <= 1e-6;
    }
    return false;
}
// Only build simple dimensions when full semantics AND attachment geometry are known.
void prepareDimensionFallback(PmiRecord& record, const Handle(XCAFDimTolObjects_DimensionObject)& object)
{
    const auto values = object->GetValues();
    if (values.IsNull() || values->Length() != 1 || !std::isfinite(object->GetValue()) || object->GetValue() <= Precision::Confusion()) {
        record.reason = QStringLiteral("数值缺失、无效或包含范围/偏差；保留完整语义，未生成简化标注。"); return;
    }
    if (object->HasQualifier() || object->HasAngularQualifier() || !object->GetModifiers().IsEmpty() ||
        object->IsDimWithClassOfTolerance() || object->HasDescriptions() || !object->GetPath().IsNull()) {
        record.reason = QStringLiteral("包含限定符、修饰符、配合公差或附注；未生成会省略这些信息的简化标注。"); return;
    }
    if (!object->HasPlane() || !object->HasTextPoint()) {
        record.reason = QStringLiteral("缺少源标注平面或文字位置，未猜测标注布局。"); return;
    }
    record.annotationPlane = object->GetPlane();
    record.textPosition = object->GetPointTextAttach();
    record.value = object->GetValue();
    record.valueText = number(record.value);
    const gp_Pln plane(record.annotationPlane);
    const auto type = object->GetType();
    if (type == XCAFDimTolObjects_DimensionType_Location_LinearDistance && object->HasPoint() && object->HasPoint2()) {
        record.point1 = object->GetPoint(); record.point2 = object->GetPoint2();
        if (nearlyEqual(record.point1.Distance(record.point2), record.value) && plane.Distance(record.point1) <= 1e-6 && plane.Distance(record.point2) <= 1e-6)
            record.fallback = PmiFallback::Linear;
        else record.reason = QStringLiteral("源尺寸值、连接点或标注平面不一致；未以测量结果替换工程尺寸。");
    } else if (type == XCAFDimTolObjects_DimensionType_Size_Radius || type == XCAFDimTolObjects_DimensionType_Size_Diameter) {
        const auto shapes = referenceShapes(record);
        if (shapes.size() == 1 && shapes.front().ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve curve(TopoDS::Edge(shapes.front()));
            if (curve.GetType() == GeomAbs_Circle) {
                record.circle = curve.Circle();
                const double measured = record.circle.Radius() * (type == XCAFDimTolObjects_DimensionType_Size_Diameter ? 2.0 : 1.0);
                if (nearlyEqual(measured, record.value) && plane.Axis().Direction().IsParallel(record.circle.Axis().Direction(), 1e-7) && plane.Distance(record.circle.Location()) <= 1e-6)
                    record.fallback = type == XCAFDimTolObjects_DimensionType_Size_Diameter ? PmiFallback::Diameter : PmiFallback::Radius;
            }
        }
        if (record.fallback == PmiFallback::None) record.reason = QStringLiteral("未找到与源尺寸数值和平面一致的唯一圆边；未推断圆心或半径。");
    } else if (type == XCAFDimTolObjects_DimensionType_Location_Angular || type == XCAFDimTolObjects_DimensionType_Size_Angular) {
        if (prepareAngularGeometry(record)) {
            record.fallback = PmiFallback::Angular;
            record.valueText += QChar(0x00b0);
        } else record.reason = QStringLiteral("未找到与源角度一致且共端点的两条直边；未猜测顶点、角度分支或单位。");
    } else record.reason = QStringLiteral("此尺寸类型暂无经过验证的语义渲染路径；完整数据保留在标注表中。");
    if (record.fallback != PmiFallback::None) {
        record.status = PmiStatus::SemanticFallback;
        record.reason = QStringLiteral("依据完整语义及原始连接几何生成基本标注；不是源 CAD 图形样式。");
    }
}
void prepareDatumFallback(PmiRecord& record, const Handle(XCAFDimTolObjects_DatumObject)& object)
{
    XCAFDimTolObjects_DatumModifWithValue modifier;
    Standard_Real modifierValue = 0;
    object->GetModifierWithValue(modifier, modifierValue);
    if (object->GetModifiers().IsEmpty() && static_cast<int>(modifier) == 0 && object->GetDatumTarget().IsNull() &&
        !object->HasDatumTargetParams() && object->HasPlane() && object->HasPoint() && object->HasPointText() && !text(object->GetName()).isEmpty()) {
        record.annotationPlane = object->GetPlane(); record.textPosition = object->GetPointTextAttach(); record.point1 = object->GetPoint();
        record.datumText = QStringLiteral("基准 [%1]").arg(text(object->GetName()));
        record.fallback = PmiFallback::DatumText; record.status = PmiStatus::SemanticFallback;
        record.reason = QStringLiteral("按源文字位置显示基准名称；关联几何保留在标注表，不宣称重建源基准符号样式。");
    } else record.reason = QStringLiteral("基准含修饰符/目标，或缺少名称、平面、连接点、文字位置；保留完整语义，未生成简化符号。");
}
void finalizeRecord(PmiRecord& record, const CadDocument& document)
{
    resolveOwners(record, document);
    if (!record.graphicalPresentation.IsNull()) {
        record.status = PmiStatus::SourceGraphics; record.fallback = PmiFallback::None;
        record.reason = QStringLiteral("显示 OCCT 已读取的源图形；原字体、颜色和线型完整性未验证。");
    }
    if (!record.associationComplete) {
        record.reason += QStringLiteral(" 关联形状缺失或无法归属到零件；不随爆炸移动，不保证可随零件导出。");
        if (record.graphicalPresentation.IsNull()) { record.status = PmiStatus::MissingAssociation; record.fallback = PmiFallback::None; }
    }
    if (record.crossPart) record.reason += QStringLiteral(" 跨零件标注：拆分导出不保留为单零件标注，爆炸时应隐藏。");
    record.reason += record.occurrencePlacementVerified
        ? QStringLiteral(" 唯一所属实例为恒等放置；标注可随该零件的爆炸位移移动。")
        : QStringLiteral(" 使用导入坐标；实例坐标映射未独立验证。");
    if (record.name.isEmpty()) record.name = pmiKindText(record.kind) + QStringLiteral(" ") + record.sourceLabelEntry;
    if (record.semanticText.isEmpty()) record.semanticText = QStringLiteral("仅图形标注，未读取到语义数值。");
}
} // namespace

QString pmiKindText(PmiKind kind)
{
    switch (kind) {
    case PmiKind::Dimension: return QStringLiteral("尺寸");
    case PmiKind::Datum: return QStringLiteral("基准");
    case PmiKind::GeometricTolerance: return QStringLiteral("几何公差");
    case PmiKind::Graphical: return QStringLiteral("图形标注");
    case PmiKind::Legacy: return QStringLiteral("旧式尺寸/公差");
    }
    return QStringLiteral("未知标注");
}
QString pmiStatusText(PmiStatus status)
{
    switch (status) {
    case PmiStatus::SourceGraphics: return QStringLiteral("源图形可显示");
    case PmiStatus::SemanticFallback: return QStringLiteral("语义生成显示");
    case PmiStatus::SemanticOnly: return QStringLiteral("仅语义可读");
    case PmiStatus::MissingAssociation: return QStringLiteral("关联不完整");
    case PmiStatus::Unsupported: return QStringLiteral("暂不支持显示");
    case PmiStatus::Failed: return QStringLiteral("读取/显示失败");
    }
    return QStringLiteral("状态未知");
}
PmiCatalog buildPmiCatalog(const CadDocument& document)
{
    PmiCatalog catalog;
    catalog.diagnostics << QStringLiteral("标注完整性未验证：以下数量仅为 OCCT 已转入 XDE 的标注数量；不代表源 STEP 的全部标注。原始 STEP 实体编号未可靠保留。");
    catalog.diagnostics << QStringLiteral("字符型 PMI、部分公差语义和图形样式可能未被解析；缺少图形或几何关联的标注仍保留在表中。");
    catalog.diagnostics << QStringLiteral("标注按导入坐标显示一次，不自动复制至重复实例。仅关联到唯一恒等放置实例的标注随爆炸移动；其他坐标映射未验证的标注在爆炸时隐藏。");
    if (document.xde.IsNull()) { catalog.diagnostics << QStringLiteral("没有可读取的 XDE 文档。"); return catalog; }
    const auto tool = XCAFDoc_DocumentTool::DimTolTool(document.xde->Main());
    NCollection_IndexedDataMap<TDF_Label, TopoDS_Shape> presentations;
    tool->GetGDTPresentations(presentations);
    QSet<QString> processed;
    const auto append = [&](const TDF_Label& label, PmiKind kind) {
        const QString entry = labelEntry(label);
        if (processed.contains(entry)) return;
        processed.insert(entry);
        PmiRecord record;
        record.id = QStringLiteral("pmi:") + entry; record.sourceLabel = label; record.sourceLabelEntry = entry;
        record.kind = kind; record.name = labelName(label);
        addReferences(record, label);
        if (presentations.Contains(label)) record.graphicalPresentation = presentations.FindFromKey(label);
        try {
            Handle(XCAFDoc_Dimension) dimension; Handle(XCAFDoc_Datum) datum; Handle(XCAFDoc_GeomTolerance) tolerance;
            if (label.FindAttribute(XCAFDoc_Dimension::GetID(), dimension)) {
                const auto object = dimension->GetObject(); record.kind = PmiKind::Dimension;
                record.semanticText = dimensionText(object); record.semanticDetails = details(object);
                if (!text(object->GetSemanticName()).isEmpty()) record.name = text(object->GetSemanticName());
                if (record.graphicalPresentation.IsNull()) record.graphicalPresentation = object->GetPresentation();
                if (record.graphicalPresentation.IsNull()) prepareDimensionFallback(record, object);
            } else if (label.FindAttribute(XCAFDoc_Datum::GetID(), datum)) {
                const auto object = datum->GetObject(); record.kind = PmiKind::Datum; record.name = text(object->GetSemanticName());
                record.semanticText = QStringLiteral("基准 %1；系统序位 %2").arg(text(object->GetName())).arg(object->GetPosition());
                if (!object->GetModifiers().IsEmpty()) record.semanticText += QStringLiteral("；修饰符代码 %1").arg(modifierCodes(object->GetModifiers()));
                XCAFDimTolObjects_DatumModifWithValue modifier; Standard_Real value = 0; object->GetModifierWithValue(modifier, value);
                if (static_cast<int>(modifier) != 0) record.semanticText += QStringLiteral("；带值修饰符代码 %1 = %2").arg(static_cast<int>(modifier)).arg(number(value));
                if (object->HasDatumTargetParams() || !object->GetDatumTarget().IsNull())
                    record.semanticText += QStringLiteral("；基准目标类型代码 %1，目标编号 %2").arg(static_cast<int>(object->GetDatumTargetType())).arg(object->GetDatumTargetNumber());
                record.semanticDetails = details(object);
                if (record.graphicalPresentation.IsNull()) record.graphicalPresentation = object->GetPresentation();
                if (record.graphicalPresentation.IsNull()) prepareDatumFallback(record, object);
            } else if (label.FindAttribute(XCAFDoc_GeomTolerance::GetID(), tolerance)) {
                const auto object = tolerance->GetObject(); record.kind = PmiKind::GeometricTolerance; record.name = text(object->GetSemanticName());
                record.semanticText = QStringLiteral("几何公差类型代码 %1；数值 %2；值类型代码 %3；材料要求代码 %4；区域代码 %5 (%6)")
                    .arg(static_cast<int>(object->GetType())).arg(number(object->GetValue())).arg(static_cast<int>(object->GetTypeOfValue()))
                    .arg(static_cast<int>(object->GetMaterialRequirementModifier())).arg(static_cast<int>(object->GetZoneModifier())).arg(number(object->GetValueOfZoneModifier()));
                if (!object->GetModifiers().IsEmpty()) record.semanticText += QStringLiteral("；修饰符代码 %1").arg(modifierCodes(object->GetModifiers()));
                record.semanticText += QStringLiteral("；最大值修饰符 %1").arg(number(object->GetMaxValueModifier()));
                TDF_LabelSequence datums; tool->GetDatumOfTolerLabels(label, datums);
                for (int i = 1; i <= datums.Length(); ++i) { addReferences(record, datums.Value(i)); record.semanticText += QStringLiteral("；基准关联 %1").arg(labelEntry(datums.Value(i))); }
                record.semanticDetails = details(object);
                if (record.graphicalPresentation.IsNull()) record.graphicalPresentation = object->GetPresentation();
                record.status = PmiStatus::Unsupported; record.reason = QStringLiteral("几何公差语义可读；无源图形时不简化或省略修饰符重画公差框。");
            } else if (kind == PmiKind::Legacy) {
                Standard_Integer legacyKind = 0; Handle(TColStd_HArray1OfReal) values; Handle(TCollection_HAsciiString) name, description;
                if (tool->GetDimTol(label, legacyKind, values, name, description)) {
                    QStringList numbers;
                    if (!values.IsNull()) for (int i = values->Lower(); i <= values->Upper(); ++i) numbers << number(values->Value(i));
                    record.name = text(name); record.semanticText = QStringLiteral("旧式类型代码 %1；数值 [%2]；%3").arg(legacyKind).arg(numbers.join(',')).arg(text(description));
                }
                record.status = PmiStatus::Unsupported; record.reason = QStringLiteral("旧式尺寸/公差仅列出源数据；无经过验证的图形重建路径。");
            }
            finalizeRecord(record, document);
        } catch (const Standard_Failure& error) {
            record.status = PmiStatus::Failed; record.reason = QStringLiteral("读取该标注失败：%1").arg(QString::fromUtf8(error.GetMessageString()));
        } catch (const std::exception& error) {
            record.status = PmiStatus::Failed; record.reason = QStringLiteral("读取该标注失败：%1").arg(QString::fromUtf8(error.what()));
        }
        catalog.records.push_back(std::move(record));
    };
    TDF_LabelSequence labels;
    tool->GetDimensionLabels(labels); for (int i = 1; i <= labels.Length(); ++i) append(labels.Value(i), PmiKind::Dimension);
    labels.Clear(); tool->GetDatumLabels(labels); for (int i = 1; i <= labels.Length(); ++i) append(labels.Value(i), PmiKind::Datum);
    labels.Clear(); tool->GetGeomToleranceLabels(labels); for (int i = 1; i <= labels.Length(); ++i) append(labels.Value(i), PmiKind::GeometricTolerance);
    labels.Clear(); tool->GetDimTolLabels(labels); for (int i = 1; i <= labels.Length(); ++i) append(labels.Value(i), PmiKind::Legacy);
    for (int i = 1; i <= presentations.Extent(); ++i) append(presentations.FindKey(i), PmiKind::Graphical);
    return catalog;
}
Handle(AIS_InteractiveObject) createPmiPresentation(const PmiRecord& record, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& message) -> Handle(AIS_InteractiveObject) { if (error) *error = message; return {}; };
    if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread())
        return fail(QStringLiteral("PMI 显示对象必须在主界面线程创建。"));
    try {
        if (!record.graphicalPresentation.IsNull()) {
            Handle(AIS_Shape) presentation = new AIS_Shape(record.graphicalPresentation);
            presentation->SetColor(Quantity_Color(0.06, 0.13, 0.22, Quantity_TOC_sRGB)); presentation->SetWidth(1.5); return presentation;
        }
        Handle(PrsDim_Dimension) dimension;
        switch (record.fallback) {
        case PmiFallback::Linear: dimension = new PrsDim_LengthDimension(record.point1, record.point2, gp_Pln(record.annotationPlane)); break;
        case PmiFallback::Angular:
            dimension = new PrsDim_AngleDimension(record.point1, record.angleCenter, record.point2); dimension->SetCustomPlane(gp_Pln(record.annotationPlane)); break;
        case PmiFallback::Radius: dimension = new PrsDim_RadiusDimension(record.circle); dimension->SetCustomPlane(gp_Pln(record.annotationPlane)); break;
        case PmiFallback::Diameter: dimension = new PrsDim_DiameterDimension(record.circle, gp_Pln(record.annotationPlane)); break;
        case PmiFallback::DatumText: {
            Handle(AIS_TextLabel) label = new AIS_TextLabel;
            label->SetText(TCollection_ExtendedString(record.datumText.toUtf8().constData(), true)); label->SetPosition(record.textPosition);
            label->SetColor(Quantity_Color(0.06, 0.13, 0.22, Quantity_TOC_sRGB)); label->SetHeight(16.0); return label;
        }
        case PmiFallback::None: return fail(record.reason.isEmpty() ? QStringLiteral("此标注没有可显示的图形。") : record.reason);
        }
        if (dimension.IsNull() || !dimension->IsValid()) return fail(QStringLiteral("尺寸连接几何或平面无效；语义数据仍可在标注表中查看。"));
        dimension->SetCustomValue(TCollection_ExtendedString(record.valueText.toUtf8().constData(), true)); dimension->SetTextPosition(record.textPosition);
        if (!dimension->IsValid()) return fail(QStringLiteral("源文字位置与尺寸平面不一致；已保留语义，未猜测布局。"));
        dimension->SetColor(Quantity_Color(0.06, 0.13, 0.22, Quantity_TOC_sRGB)); return dimension;
    } catch (const Standard_Failure& failure) {
        return fail(QStringLiteral("PMI 图形创建失败：%1").arg(QString::fromUtf8(failure.GetMessageString())));
    } catch (const std::exception& failure) { return fail(QStringLiteral("PMI 图形创建失败：%1").arg(QString::fromUtf8(failure.what()))); }
}
bool pmiCanExportWithPart(const PmiRecord& record, const QString& partId)
{
    return record.safeForPartExport && record.ownerPartIds.size() == 1 && record.ownerPartIds.front() == partId;
}

void copyIndependentPmi(const CadDocument& document, int partIndex,
                        const Handle(TDocStd_Document)& target, QStringList& diagnostics)
{
    if (document.xde.IsNull() || target.IsNull() || partIndex < 0 || partIndex >= static_cast<int>(document.parts.size())) {
        diagnostics << QStringLiteral("PMI 复制失败：源文档、目标文档或零件索引无效。");
        return;
    }
    const auto& part = document.parts[partIndex];
    const auto sourceTool = XCAFDoc_DocumentTool::DimTolTool(document.xde->Main());
    const auto targetTool = XCAFDoc_DocumentTool::DimTolTool(target->Main());
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(target->Main());
    TDF_Label targetPart = shapes->FindShape(part.shape, false);
    if (targetPart.IsNull()) {
        diagnostics << QStringLiteral("%1：目标文档未保留相同的零件定义几何，未复制 PMI。 ").arg(part.name);
        return;
    }
    const auto catalog = buildPmiCatalog(document);
    struct Candidate { const PmiRecord* record; TDF_LabelSequence first, second, datums; };
    std::vector<Candidate> candidates;
    QSet<QString> eligible;
    const auto skip = [&](const PmiRecord& record, const QString& reason) {
        diagnostics << QStringLiteral("%1 / 标注 %2：未复制，%3").arg(part.name, record.name, reason);
    };
    const auto remap = [&](const TDF_LabelSequence& source, TDF_LabelSequence& destination) {
        for (int i = 1; i <= source.Length(); ++i) {
            const auto& label = source.Value(i);
            // Occurrence coordinates cannot be safely made part-local merely by
            // replacing a label. Retain only direct definition/subshape links.
            if (!labelWithin(label, part.label)) return false;
            if (label == part.label) { destination.Append(targetPart); continue; }
            const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
            if (shape.IsNull()) return false;
            TDF_Label mapped;
            if (!shapes->FindSubShape(targetPart, shape, mapped)) mapped = shapes->AddSubShape(targetPart, shape);
            if (mapped.IsNull()) return false;
            destination.Append(mapped);
        }
        return true;
    };
    for (const auto& record : catalog.records) {
        if (!record.ownerPartIds.isEmpty() && !record.ownerPartIds.contains(part.id)) continue;
        if (!pmiCanExportWithPart(record, part.id)) {
            skip(record, record.crossPart ? QStringLiteral("关联跨越多个零件")
                : !record.associationComplete ? QStringLiteral("关联不完整或无法确认归属")
                : QStringLiteral("标注与零件定义的坐标映射未验证；未直接复制可能含装配坐标的图形或连接点"));
            continue;
        }
        if (record.status == PmiStatus::Failed || record.kind == PmiKind::Legacy) {
            skip(record, QStringLiteral("此类标注没有经过验证的完整复制路径")); continue;
        }
        Candidate candidate { &record, {}, {}, {} };
        TDF_LabelSequence first, second;
        sourceTool->GetRefShapeLabel(record.sourceLabel, first, second);
        if (first.IsEmpty() && second.IsEmpty()) { skip(record, QStringLiteral("无独立形状关联")); continue; }
        if (!remap(first, candidate.first) || !remap(second, candidate.second)) {
            skip(record, QStringLiteral("形状关联不能无损映射至零件定义坐标")); continue;
        }
        if (record.kind == PmiKind::GeometricTolerance) sourceTool->GetDatumOfTolerLabels(record.sourceLabel, candidate.datums);
        candidates.push_back(candidate); eligible.insert(record.sourceLabelEntry);
    }
    QHash<QString, TDF_Label> copied;
    // Datums first, then dimensions, then tolerances: no dangling datum references.
    for (PmiKind kind : { PmiKind::Datum, PmiKind::Dimension, PmiKind::Graphical, PmiKind::GeometricTolerance }) {
        for (const auto& candidate : candidates) {
            const PmiRecord& record = *candidate.record;
            if (record.kind != kind) continue;
            bool missingDatum = false;
            for (int i = 1; i <= candidate.datums.Length(); ++i)
                if (!copied.contains(labelEntry(candidate.datums.Value(i)))) missingDatum = true;
            if (missingDatum) { skip(record, QStringLiteral("引用的基准未能完整复制")); continue; }
            try {
                TDF_Label newLabel;
                Handle(XCAFDoc_Dimension) dimension;
                Handle(XCAFDoc_Datum) datum;
                Handle(XCAFDoc_GeomTolerance) tolerance;
                if (record.sourceLabel.FindAttribute(XCAFDoc_Dimension::GetID(), dimension)) {
                    newLabel = targetTool->AddDimension();
                    XCAFDoc_Dimension::Set(newLabel)->SetObject(dimension->GetObject());
                    targetTool->SetDimension(candidate.first, candidate.second, newLabel);
                } else if (record.sourceLabel.FindAttribute(XCAFDoc_Datum::GetID(), datum)) {
                    newLabel = targetTool->AddDatum();
                    XCAFDoc_Datum::Set(newLabel)->SetObject(datum->GetObject());
                    TDF_LabelSequence references = candidate.first;
                    for (int i = 1; i <= candidate.second.Length(); ++i) references.Append(candidate.second.Value(i));
                    targetTool->SetDatum(references, newLabel);
                } else if (record.sourceLabel.FindAttribute(XCAFDoc_GeomTolerance::GetID(), tolerance)) {
                    newLabel = targetTool->AddGeomTolerance();
                    XCAFDoc_GeomTolerance::Set(newLabel)->SetObject(tolerance->GetObject());
                    TDF_LabelSequence references = candidate.first;
                    for (int i = 1; i <= candidate.second.Length(); ++i) references.Append(candidate.second.Value(i));
                    targetTool->SetGeomTolerance(references, newLabel);
                    for (int i = 1; i <= candidate.datums.Length(); ++i)
                        targetTool->SetDatumToGeomTol(copied.value(labelEntry(candidate.datums.Value(i))), newLabel);
                } else { skip(record, QStringLiteral("缺少可复制的 XDE 语义属性")); continue; }
                TDataStd_Name::Set(newLabel, TCollection_ExtendedString(record.name.toUtf8().constData(), true));
                copied.insert(record.sourceLabelEntry, newLabel);
            } catch (const Standard_Failure& error) {
                // Do not claim a partially copied document can be written safely.
                throw std::runtime_error(QStringLiteral("%1：复制 PMI %2 失败：%3")
                    .arg(part.name, record.name, QString::fromUtf8(error.GetMessageString())).toStdString());
            }
        }
    }
    if (!catalog.records.empty()) {
        diagnostics << QStringLiteral("%1：已复制 %2 项独立 PMI 及关联到临时 XDE；STEP 文件实际保留情况见导出回读核验，不承诺未被 OCCT 导入的标注。")
                           .arg(part.name).arg(copied.size());
    }
}
} // namespace Stp
