#include "customer_envelope.h"
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepGProp.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Stp {
namespace {
constexpr double repairTolerance = 1.e-4; // mm
class RepairProgress final : public Message_ProgressIndicator {
public:
    explicit RepairProgress(std::atomic_bool& cancel) : cancel_(cancel) {}
    Standard_Boolean UserBreak() override { return cancel_.load(); }
protected:
    void Show(const Message_ProgressScope&, Standard_Boolean) override {}
private:
    std::atomic_bool& cancel_;
};
void checkCancel(std::atomic_bool& cancel) {
    if (cancel.load()) throw std::runtime_error("外包络处理已取消");
}
double shapeVolume(const TopoDS_Shape& shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.Mass();
}
QString statusText(BRepCheck_Status status) {
    switch (status) {
    case BRepCheck_UnorientableShape: return QStringLiteral("面或边界环定向错误 (UnorientableShape)");
    case BRepCheck_InvalidImbricationOfWires: return QStringLiteral("边界环嵌套错误 (InvalidImbricationOfWires)");
    case BRepCheck_NotClosed: return QStringLiteral("边界未闭合 (NotClosed)");
    case BRepCheck_BadOrientationOfSubshape: return QStringLiteral("子形体方向错误 (BadOrientationOfSubshape)");
    case BRepCheck_InvalidPointOnCurve: return QStringLiteral("顶点与曲线不一致 (InvalidPointOnCurve)");
    case BRepCheck_InvalidCurveOnSurface: return QStringLiteral("曲面参数线无效 (InvalidCurveOnSurface)");
    case BRepCheck_InvalidSameParameterFlag: return QStringLiteral("三维曲线与曲面参数线参数不一致 (InvalidSameParameterFlag)");
    case BRepCheck_SelfIntersectingWire: return QStringLiteral("边界环自相交 (SelfIntersectingWire)");
    case BRepCheck_NoError: return {};
    default: return QStringLiteral("拓扑检查错误，内核状态 %1").arg(int(status));
    }
}
void appendStatuses(const BRepCheck_ListOfStatus& statuses, QStringList& problems) {
    for (auto status : statuses) {
        const auto text = statusText(status);
        if (!text.isEmpty() && !problems.contains(text)) problems.append(text);
    }
}
struct ShapeCheck {
    int solids = 0;
    bool supported = true;
    double volume = 0;
    QStringList problems;
    bool valid() const { return supported && solids > 0 && problems.isEmpty(); }
};
void inspectShape(const TopoDS_Shape& shape, ShapeCheck& result, std::atomic_bool& cancel) {
    checkCancel(cancel);
    if (shape.IsNull()) {
        result.supported = false;
        result.problems.append(QStringLiteral("空几何"));
        return;
    }
    if (shape.ShapeType() == TopAbs_COMPOUND || shape.ShapeType() == TopAbs_COMPSOLID) {
        const auto before = result.solids;
        for (TopoDS_Iterator it(shape); it.More(); it.Next()) inspectShape(it.Value(), result, cancel);
        if (result.solids == before) {
            result.supported = false;
            result.problems.append(QStringLiteral("几何组未包含实体"));
        }
        return;
    }
    if (shape.ShapeType() != TopAbs_SOLID) {
        result.supported = false;
        result.problems.append(QStringLiteral("包含开放曲面、线或非实体几何"));
        return;
    }
    ++result.solids;
    const double volume = shapeVolume(shape);
    result.volume += volume;
    if (!std::isfinite(volume)) result.problems.append(QStringLiteral("实体 %1 体积非有限数").arg(result.solids));
    else if (volume <= 0) result.problems.append(QStringLiteral("实体 %1 体积为零或方向反向").arg(result.solids));
    BRepCheck_Analyzer analyzer(shape);
    if (analyzer.IsValid()) return;
    QStringList statuses;
    TopTools_IndexedMapOfShape subshapes;
    TopExp::MapShapes(shape, subshapes);
    for (int i = 1; i <= subshapes.Extent(); ++i) {
        checkCancel(cancel);
        const auto checked = analyzer.Result(subshapes(i));
        if (checked.IsNull()) continue;
        appendStatuses(checked->Status(), statuses);
        for (checked->InitContextIterator(); checked->MoreShapeInContext(); checked->NextShapeInContext())
            appendStatuses(checked->StatusOnShape(), statuses);
    }
    if (statuses.empty()) statuses.append(QStringLiteral("实体拓扑有效性检查失败"));
    for (const auto& status : statuses)
        result.problems.append(QStringLiteral("实体 %1 — %2").arg(result.solids).arg(status));
}
std::array<double, 6> shapeBounds(const TopoDS_Shape& shape) {
    Bnd_Box box;
    BRepBndLib::AddOptimal(shape, box, false, false);
    if (box.IsVoid() || box.IsOpen()) throw std::runtime_error("修复边界无效");
    std::array<double, 6> result;
    box.Get(result[0], result[1], result[2], result[3], result[4], result[5]);
    return result;
}
TopoDS_Shape repairInput(const TopoDS_Shape& source, const gp_Trsf& placement,
    const TopoDS_Shape& original, const ShapeCheck& before, EnvelopeInputIssue& issue, std::atomic_bool& cancel) {
    // Some imported p-curves fail checks only when an instance location is attached.
    // Materialize the same rigid transform on a copy before attempting ShapeFix.
    auto candidate = BRepBuilderAPI_Transform(source, placement, true).Shape();
    ShapeCheck after;
    inspectShape(candidate, after, cancel);
    issue.repairMethod = QStringLiteral("重建装配位置下的几何");
    if (!after.valid()) {
        ShapeFix_Shape fixer(candidate);
        fixer.SetPrecision(1.e-7);
        fixer.SetMaxTolerance(repairTolerance);
        Handle(RepairProgress) repairProgress = new RepairProgress(cancel);
        fixer.Perform(repairProgress->Start());
        checkCancel(cancel);
        candidate = fixer.Shape();
        if (candidate.IsNull()) return original;
        if (candidate.ShapeType() == TopAbs_SOLID) {
            auto solid = TopoDS::Solid(candidate);
            if (!BRepLib::OrientClosedSolid(solid)) throw std::runtime_error("实体方向修复失败");
            candidate = solid;
        }
        after = {};
        inspectShape(candidate, after, cancel);
        issue.repairMethod += QStringLiteral("，修复拓扑与方向");
    }
    issue.after = after.problems;
    issue.preparedVolume = after.volume;
    if (!after.valid()) return original;
    const auto originalBounds = shapeBounds(original);
    const auto candidateBounds = shapeBounds(candidate);
    for (size_t i = 0; i < originalBounds.size(); ++i)
        issue.boundsChange = std::max(issue.boundsChange, std::abs(originalBounds[i] - candidateBounds[i]));
    const double volumeChange = std::abs(after.volume - std::abs(before.volume));
    if (after.solids != before.solids || !std::isfinite(before.volume)
        || issue.boundsChange > repairTolerance
        || volumeChange > std::max(1.e-6, std::abs(before.volume) * 1.e-6)) {
        issue.after.append(QStringLiteral("修复后实体数量、边界或体积变化超限，候选修复未采用"));
        return original;
    }
    issue.valid = true;
    issue.repaired = true;
    return candidate;
}
}
EnvelopeInspection inspectEnvelopeInputs(const CadDocument& doc, const PlacementEdits& edits,
    const std::vector<int>& selected, bool repair, std::atomic_bool& cancel, const ProgressCallback& progress) {
    checkCancel(cancel);
    validatePlacementEdits(doc, edits);
    const auto scope = envelopeScope(doc, selected);
    if (scope.empty()) throw std::runtime_error("请先选择至少一个实体零件");
    EnvelopeInspection inspection;
    for (int occurrence : scope) {
        checkCancel(cancel);
        const auto& item = doc.occurrences.at(occurrence);
        if (item.partIndex < 0 || size_t(item.partIndex) >= doc.parts.size()) throw std::runtime_error("零件索引无效");
        if (progress) progress(int(18 * inspection.issues.size() / scope.size()),
            QStringLiteral("检查输入 %1/%2：%3").arg(inspection.issues.size() + 1).arg(scope.size()).arg(item.name));
        EnvelopeInputIssue issue;
        issue.occurrence = occurrence;
        issue.name = item.name;
        issue.path = item.path;
        try {
            const auto& source = doc.parts[item.partIndex].shape;
            BRepBuilderAPI_Copy copy(source, true, false);
            const auto placement = placedWorldTransform(doc, edits, occurrence);
            const auto original = copy.Shape().Moved(TopLoc_Location(placement));
            inspection.originalPieces.push_back({original, QColor("#b0b0b0"), occurrence});
            ShapeCheck before;
            // Validate in assembly coordinates, exactly as the subsequent Boolean inputs.
            inspectShape(original, before, cancel);
            issue.solidCount = before.solids;
            issue.originalVolume = before.volume;
            issue.preparedVolume = before.volume;
            issue.before = before.problems;
            issue.after = before.problems;
            issue.valid = before.valid();
            auto prepared = original;
            if (!issue.valid && repair && before.supported)
                prepared = repairInput(source, placement, original, before, issue, cancel);
            if (issue.valid) inspection.preparedPieces.push_back({prepared, QColor("#b0b0b0"), occurrence});
        } catch (const Standard_Failure& e) {
            checkCancel(cancel);
            issue.valid = false;
            issue.after.append(QStringLiteral("几何检查或修复异常：%1").arg(QString::fromUtf8(e.GetMessageString())));
        } catch (const std::exception& e) {
            checkCancel(cancel);
            issue.valid = false;
            issue.after.append(QString::fromUtf8(e.what()));
        }
        inspection.issues.push_back(std::move(issue));
    }
    checkCancel(cancel);
    return inspection;
}
int EnvelopeInspection::invalidCount() const {
    return int(std::count_if(issues.begin(), issues.end(), [](const auto& issue) { return !issue.valid; }));
}
QString EnvelopeInspection::text() const {
    QStringList lines;
    int solids = 0, repaired = 0;
    for (const auto& issue : issues) { solids += issue.solidCount; repaired += issue.repaired; }
    lines.append(QStringLiteral("装配位置输入检查：%1 个实例 / %2 个实体；未通过 %3 个实例；已采用修复 %4 个实例。")
        .arg(issues.size()).arg(solids).arg(invalidCount()).arg(repaired));
    for (const auto& issue : issues) {
        if (issue.valid && !issue.repaired) continue;
        lines.append(QStringLiteral("\n[%1] %2\n%3\n%4").arg(issue.occurrence).arg(issue.name, issue.path,
            issue.repaired ? QStringLiteral("已修复，需剖切复核") : QStringLiteral("未通过")));
        lines.append(QStringLiteral("原始检查：\n%1").arg(issue.before.join('\n')));
        if (!issue.after.isEmpty()) lines.append(QStringLiteral("%1：\n%2").arg(
            !issue.repairMethod.isEmpty() && !issue.repaired ? QStringLiteral("修复候选检查（未采用）") : QStringLiteral("当前检查"),
            issue.after.join('\n')));
        if (issue.repaired) lines.append(QStringLiteral("%1；修复前后体积 %2 → %3 mm³；边界最大变化 %4 mm")
            .arg(issue.repairMethod).arg(issue.originalVolume, 0, 'g', 10).arg(issue.preparedVolume, 0, 'g', 10).arg(issue.boundsChange, 0, 'g', 6));
    }
    if (invalidCount()) lines.append(QStringLiteral("\n请定位并修复列出的实例后重新生成。保留零件也需要通过输入检查。"));
    if (repaired) lines.append(QStringLiteral("\n修复容差上限 0.0001 mm。体积和边界检查通过后，仍需检查外形细节及安装接口。"));
    return lines.join('\n');
}
QJsonObject EnvelopeInspection::report() const {
    QJsonArray entries;
    for (const auto& issue : issues) entries.append(QJsonObject {
        {"occurrence", issue.occurrence}, {"name", issue.name}, {"path", issue.path},
        {"solidCount", issue.solidCount}, {"valid", issue.valid}, {"repaired", issue.repaired},
        {"repairMethod", issue.repairMethod}, {"originalVolumeMm3", issue.originalVolume},
        {"preparedVolumeMm3", issue.preparedVolume}, {"maxBoundsChangeMm", issue.boundsChange},
        {"before", QJsonArray::fromStringList(issue.before)}, {"after", QJsonArray::fromStringList(issue.after)} });
    return {{"coordinateSystem", "assembly"}, {"invalidOccurrences", invalidCount()},
        {"repairToleranceMm", repairTolerance}, {"instances", entries}};
}
} // namespace Stp
