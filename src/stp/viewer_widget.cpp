#include "viewer_widget.h"
#include "cad_job_queue.h"
#include "../app/widget_occ_view.h"
#include <AIS_Shape.hxx>
#include <AIS_Point.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Prs3d.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_RenderingParams.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopTools_MapOfShape.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <XCAFPrs_Style.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QNativeGestureEvent>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QShowEvent>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <QStringList>
#include <QRectF>

namespace Stp {
namespace {
bool sameTransform(const gp_Trsf& a, const gp_Trsf& b)
{
    for (int row = 1; row <= 3; ++row)
        for (int column = 1; column <= 4; ++column)
            if (std::abs(a.Value(row, column) - b.Value(row, column)) > 1.e-7) return false;
    return true;
}

Handle(AIS_InteractiveObject) featureGuide(const MeasurementEntity& entity, MeasurementCoordinates coordinates,
    const Quantity_Color& color)
{
    if (entity.selection != MeasurementSelection::FeatureCenter && entity.selection != MeasurementSelection::FeatureAxis
        && entity.extent != MeasurementExtent::Infinite) return {};
    const auto feature = entity.selection == MeasurementSelection::FeatureCenter
        ? measurementFeatureReference(entity, coordinates) : std::optional<MeasurementFeature>{};
    if (entity.selection == MeasurementSelection::FeatureCenter && feature && feature->center) {
        Handle(AIS_Point) point = new AIS_Point(new Geom_CartesianPoint(*feature->center));
        point->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_PLUS, color, 3.));
        return point;
    }
    const auto reference = measurementReference(entity, coordinates);
    if (!reference || (!reference->line && !reference->plane)) return {};
    const gp_Ax1 axis = reference->line ? reference->line->Position() : reference->plane->Axis();
    const auto& placement = coordinates == MeasurementCoordinates::Original ? entity.originalTransform : entity.displayTransform;
    Bnd_Box box; BRepBndLib::AddOptimal(entity.shape.Moved(TopLoc_Location(placement)), box, false, true);
    gp_Pnt middle = axis.Location();
    double half = 10.;
    if (!box.IsVoid() && !box.IsOpen()) {
        double x0,y0,z0,x1,y1,z1; box.Get(x0,y0,z0,x1,y1,z1);
        const gp_Pnt center((x0+x1)/2.,(y0+y1)/2.,(z0+z1)/2.);
        if (reference->plane) middle = center.Translated(gp_Vec(axis.Direction()) * -gp_Vec(axis.Location(),center).Dot(gp_Vec(axis.Direction())));
        else middle.Translate(gp_Vec(axis.Direction()) * gp_Vec(axis.Location(),center).Dot(gp_Vec(axis.Direction())));
        half = std::max(10., std::sqrt(box.SquareExtent()) * 0.65);
    }
    if (reference->plane) {
        middle = middle.Translated(gp_Vec(axis.Direction()) * -gp_Vec(axis.Location(),middle).Dot(gp_Vec(axis.Direction())));
        const gp_Vec u(reference->plane->Position().XDirection()), v(reference->plane->Position().YDirection());
        const gp_Pnt corners[] {middle.Translated((u+v)*half), middle.Translated((v-u)*half),
                               middle.Translated((-u-v)*half), middle.Translated((u-v)*half)};
        TopoDS_Compound outline; BRep_Builder builder; builder.MakeCompound(outline);
        for (int i=0;i<4;++i) builder.Add(outline,BRepBuilderAPI_MakeEdge(corners[i],corners[(i+1)%4]).Shape());
        builder.Add(outline,BRepBuilderAPI_MakeEdge(middle,middle.Translated(gp_Vec(axis.Direction())*(half*.35))).Shape());
        Handle(AIS_Shape) guide=new AIS_Shape(outline);
        guide->Attributes()->SetWireAspect(new Prs3d_LineAspect(color,Aspect_TOL_DASH,2.));
        return guide;
    }
    const gp_Vec direction(axis.Direction());
    const auto start = middle.Translated(direction * -half), end = middle.Translated(direction * half);
    const gp_Vec basis = std::abs(axis.Direction().X()) < .8 ? gp_Vec(1,0,0) : gp_Vec(0,1,0);
    const gp_Vec side = direction.Crossed(basis).Normalized();
    TopoDS_Compound wire; BRep_Builder builder; builder.MakeCompound(wire);
    builder.Add(wire,BRepBuilderAPI_MakeEdge(start,end).Shape());
    for (double sign : {-1.,1.}) {
        builder.Add(wire,BRepBuilderAPI_MakeEdge(end,end.Translated(direction*(-half*.15)+side*(half*.07*sign))).Shape());
        builder.Add(wire,BRepBuilderAPI_MakeEdge(start,start.Translated(direction*(half*.15)+side*(half*.07*sign))).Shape());
    }
    Handle(AIS_Shape) guide = new AIS_Shape(wire);
    guide->Attributes()->SetWireAspect(new Prs3d_LineAspect(color,Aspect_TOL_DASH,2.));
    return guide;
}

std::vector<Handle(AIS_InteractiveObject)> measurementObjects(const MeasurementRecord& record, int decimals, double pixelRatio)
{
    std::vector<Handle(AIS_InteractiveObject)> objects;
    const auto color = Quantity_Color(0.12, 0.36, 0.83, Quantity_TOC_sRGB);
    for (const auto* group : {&record.request.a, &record.request.b}) for (const auto& entity : *group) {
        const auto guide = featureGuide(entity, record.request.coordinates, color);
        if (!guide.IsNull()) { guide->SetColor(color); objects.push_back(guide); }
    }
    const auto line = [&](const gp_Pnt& a, const gp_Pnt& b) {
        if (a.Distance(b) <= 1.e-9) return;
        Handle(AIS_Shape) object = new AIS_Shape(BRepBuilderAPI_MakeEdge(a, b).Shape());
        object->SetColor(color); object->SetWidth(2.0); objects.push_back(object);
    };
    const auto marker = [&](const gp_Pnt& point) {
        Handle(AIS_Point) object = new AIS_Point(new Geom_CartesianPoint(point));
        object->SetColor(color); objects.push_back(object);
    };
    const auto& result = record.result;
    gp_Pnt labelPosition;
    if (!result.anchors.empty()) {
        labelPosition = result.anchors.front();
        for (const auto& point : result.anchors) marker(point);
    }
    QString symbol;
    switch (record.request.kind) {
    case MeasurementKind::Point: symbol = !record.request.a.empty() && record.request.a.front().selection == MeasurementSelection::FeatureCenter
        ? QStringLiteral("C") : QStringLiteral("P"); break;
    case MeasurementKind::Length: symbol = QStringLiteral("L"); break;
    case MeasurementKind::Area: symbol = QStringLiteral("A"); break;
    case MeasurementKind::Volume: symbol = QStringLiteral("V"); break;
    case MeasurementKind::Mass: symbol = QStringLiteral("m"); break;
    case MeasurementKind::CenterOfMass: symbol = QStringLiteral("CoM"); break;
    case MeasurementKind::VolumeCenter: symbol = QStringLiteral("CoV"); break;
    case MeasurementKind::Distance: symbol = QStringLiteral("Min d"); break;
    case MeasurementKind::Angle: symbol = QStringLiteral("Angle"); break;
    case MeasurementKind::Radius: symbol = QStringLiteral("R"); break;
    case MeasurementKind::Diameter: symbol = QStringLiteral("Ø"); break;
    }
    const auto hasInfiniteReference = [](const std::vector<MeasurementEntity>& entities) {
        return std::any_of(entities.begin(), entities.end(), [](const MeasurementEntity& entity) {
            return entity.selection == MeasurementSelection::FeatureAxis || entity.extent == MeasurementExtent::Infinite;
        });
    };
    const bool compactReferenceLabel = (record.request.kind == MeasurementKind::Distance || record.request.kind == MeasurementKind::Angle)
        && (hasInfiniteReference(record.request.a) || hasInfiniteReference(record.request.b));
    QString label = compactReferenceLabel ? symbol : QStringLiteral("M%1 %2").arg(record.id).arg(symbol);
    if ((record.request.kind == MeasurementKind::Point || record.request.kind == MeasurementKind::CenterOfMass
         || record.request.kind == MeasurementKind::VolumeCenter)
        && !result.anchors.empty()) {
        // COM results also contain volume, mass and the volume center. Its
        // display anchor is the selected mass/geometric center, never values[0].
        const auto& center = result.anchors.front();
        label += QStringLiteral(" (%1, %2, %3) mm").arg(center.X(), 0, 'f', decimals)
            .arg(center.Y(), 0, 'f', decimals).arg(center.Z(), 0, 'f', decimals);
    } else if (!result.values.empty()) {
        QString unit = result.values.front().unit;
        unit.replace(QStringLiteral("mm²"), QStringLiteral("mm^2"));
        unit.replace(QStringLiteral("mm³"), QStringLiteral("mm^3"));
        unit.replace(QStringLiteral("°"), QStringLiteral("deg"));
        label += QStringLiteral(" %1 %2").arg(result.values.front().value, 0, 'f', decimals).arg(unit);
    }
    if (result.circle && (record.request.kind == MeasurementKind::Radius
                          || record.request.kind == MeasurementKind::Diameter)) {
        const auto& circle = *result.circle;
        const auto center = circle.Location();
        // Spread successive R/diameter leaders around their exact circle;
        // layout changes the witness direction, never the measured radius.
        const double leaderAngle = double((record.id ? record.id - 1 : 0) % 12) * (std::acos(-1.0) / 6.0);
        const gp_Vec radial = gp_Vec(circle.XAxis().Direction()) * std::cos(leaderAngle)
                           + gp_Vec(circle.YAxis().Direction()) * std::sin(leaderAngle);
        const auto end = center.Translated(radial * circle.Radius());
        const auto start = record.request.kind == MeasurementKind::Diameter
                         ? center.Translated(radial * -circle.Radius()) : center;
        Handle(AIS_Shape) outline = new AIS_Shape(BRepBuilderAPI_MakeEdge(circle).Shape());
        outline->SetColor(color); outline->SetWidth(1.5); objects.push_back(outline);
        marker(start); marker(end); line(start, end); labelPosition = end;
        const double value = circle.Radius() * (record.request.kind == MeasurementKind::Diameter ? 2.0 : 1.0);
        label = QStringLiteral("M%1 %2%3 mm").arg(record.id)
            .arg(record.request.kind == MeasurementKind::Diameter ? QStringLiteral("Ø") : QStringLiteral("R"))
            .arg(value, 0, 'f', decimals);
    } else if ((record.request.kind == MeasurementKind::Radius || record.request.kind == MeasurementKind::Diameter)
               && result.anchors.size() >= 2) {
        const auto center = result.anchors[0];
        const auto end = result.anchors[1];
        const auto start = record.request.kind == MeasurementKind::Diameter
                         ? center.Translated(gp_Vec(center, end) * -1.0) : center;
        marker(start); line(start, end); labelPosition = end;
        const double value = center.Distance(end) * (record.request.kind == MeasurementKind::Diameter ? 2.0 : 1.0);
        label = QStringLiteral("M%1 %2%3 mm").arg(record.id)
            .arg(record.request.kind == MeasurementKind::Diameter ? QStringLiteral("Ø") : QStringLiteral("R"))
            .arg(value, 0, 'f', decimals);
    } else if (record.request.kind == MeasurementKind::Distance && result.anchors.size() >= 2) {
        line(result.anchors[0], result.anchors[1]);
        labelPosition = gp_Pnt((result.anchors[0].XYZ() + result.anchors[1].XYZ()) * 0.5);
    } else if (record.request.kind == MeasurementKind::Angle && result.anchors.size() >= 3) {
        line(result.anchors[0], result.anchors[1]); line(result.anchors[0], result.anchors[2]);
        labelPosition = result.anchors[0];
        gp_Vec a(result.anchors[0], result.anchors[1]), b(result.anchors[0], result.anchors[2]);
        const double radius = std::min(a.Magnitude(), b.Magnitude()) * 0.55;
        if (radius > 1.e-9) {
            a.Normalize(); b.Normalize(); const gp_Vec normal = a.Crossed(b);
            if (normal.SquareMagnitude() > 1.e-16) {
                const double angle = std::acos(std::clamp(a.Dot(b), -1.0, 1.0));
                const gp_Circ arc(gp_Ax2(result.anchors[0], gp_Dir(normal), gp_Dir(a)), radius);
                Handle(AIS_Shape) object = new AIS_Shape(BRepBuilderAPI_MakeEdge(arc, 0, angle).Shape());
                object->SetColor(color); object->SetWidth(2.0); objects.push_back(object);
                const gp_Vec perpendicular = gp_Vec(gp_Dir(normal)).Crossed(a);
                labelPosition = result.anchors[0].Translated((a * std::cos(angle * 0.5)
                    + perpendicular * std::sin(angle * 0.5)) * radius);
            }
        }
    }
    Handle(AIS_TextLabel) text = new AIS_TextLabel;
    text->SetText(TCollection_ExtendedString(label.toUtf8().constData(), true));
    text->SetPosition(labelPosition); text->SetHeight(14.0 * pixelRatio); text->SetColor(color);
    text->SetDisplayType(Aspect_TODT_SUBTITLE);
    text->SetHJustification(Graphic3d_HTA_LEFT);
    text->SetVJustification(Graphic3d_VTA_CENTER);
    text->SetColorSubTitle(Quantity_NOC_WHITE);
    text->SetZLayer(Graphic3d_ZLayerId_Topmost);
    objects.push_back(text);
    for (const auto& object : objects) {
        object->SetInfiniteState(true); object->SetZLayer(Graphic3d_ZLayerId_Topmost);
    }
    return objects;
}

struct MeshSettings { double coefficient, angle; };
MeshSettings meshSettings(int quality)
{
    // Relative chordal deviation and angular deviation, in radians.
    const MeshSettings settings[] {{0.004, 0.70}, {0.001, 0.35}, {0.00025, 0.12}};
    return settings[std::clamp(quality, 0, 2)];
}
void configureMesh(const Handle(AIS_Shape)& object, int quality, bool meshesPrepared = true)
{
    const auto settings = meshSettings(quality);
    object->SetOwnDeviationCoefficient(settings.coefficient);
    object->SetOwnDeviationAngle(settings.angle);
    object->Attributes()->SetTypeOfDeflection(Aspect_TOD_RELATIVE);
    // Instance rotations can shrink their axis-aligned box. Letting AIS choose
    // another relative tolerance would remesh shared geometry on the GUI thread.
    // Prepared definitions already have the chosen mesh, regardless of location.
    object->Attributes()->SetAutoTriangulation(!meshesPrepared);
    // A definition was meshed once on the CAD worker. New AIS occurrences
    // must reuse it rather than clear the same mesh for every reference.
    object->Attributes()->UpdatePreviousDeviationCoefficient();
    object->Attributes()->UpdatePreviousDeviationAngle();
}
class MeshProgress final : public Message_ProgressIndicator {
public:
    explicit MeshProgress(std::atomic_bool& cancel) : m_cancel(cancel) {}
protected:
    Standard_Boolean UserBreak() override { return m_cancel.load(); }
    void Show(const Message_ProgressScope&, Standard_Boolean) override {}
private:
    std::atomic_bool& m_cancel;
};
// A leaf presentation does not traverse its containing assemblies. Supply their
// colors as defaults, leaving OCCT to resolve definition, occurrence and face
// overrides within the leaf. This avoids writing view properties into XDE.
class OccurrencePresentation final : public XCAFPrs_AISObject {
public:
    OccurrencePresentation(const TDF_Label& label, XCAFPrs_Style inherited)
        : XCAFPrs_AISObject(label), m_inherited(std::move(inherited)) {}
protected:
    void DefaultStyle(XCAFPrs_Style& style) const override {
        XCAFPrs_AISObject::DefaultStyle(style);
        if (m_inherited.IsSetColorSurf()) style.SetColorSurf(m_inherited.GetColorSurfRGBA());
        if (m_inherited.IsSetColorCurv()) style.SetColorCurv(m_inherited.GetColorCurv());
    }
private:
    XCAFPrs_Style m_inherited;
};

XCAFPrs_Style inheritedColors(const CadDocument& document, int occurrenceIndex)
{
    XCAFPrs_Style style;
    const auto colors = XCAFDoc_DocumentTool::ColorTool(document.xde->Main());
    const auto apply = [&](const TDF_Label& label) {
        if (label.IsNull()) return;
        Quantity_ColorRGBA color;
        if (colors->GetColor(label, XCAFDoc_ColorGen, color)) {
            style.SetColorSurf(color); style.SetColorCurv(color.GetRGB());
        }
        if (colors->GetColor(label, XCAFDoc_ColorSurf, color)) style.SetColorSurf(color);
        if (colors->GetColor(label, XCAFDoc_ColorCurv, color)) style.SetColorCurv(color.GetRGB());
    };
    std::vector<int> ancestors;
    for (int parent = document.occurrences[size_t(occurrenceIndex)].parent;
         parent >= 0; parent = document.occurrences[size_t(parent)].parent)
        ancestors.push_back(parent);
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const auto& ancestor = document.occurrences[size_t(*it)];
        apply(ancestor.definitionLabel);
        if (ancestor.label != ancestor.definitionLabel) apply(ancestor.label);
    }
    return style;
}
} // namespace

void prepareDisplayMeshes(CadDocument& document, int quality, std::atomic_bool& cancel,
                          const ProgressCallback& progress)
{
    const auto settings = meshSettings(quality);
    // Clear all definitions before meshing: distinct definitions can share
    // faces, and clearing a later definition must not invalidate an earlier one.
    for (const auto& part : document.parts) {
        if (cancel.load()) throw std::runtime_error("已取消显示精度更新");
        BRepTools::Clean(part.shape);
    }
    for (size_t i = 0; i < document.parts.size(); ++i) {
        if (cancel.load()) throw std::runtime_error("已取消显示精度更新");
        const auto& part = document.parts[i];
        if (progress) progress(int(100 * i / std::max<size_t>(document.parts.size(), 1)),
            QStringLiteral("生成显示三角网 %1/%2…").arg(i + 1).arg(document.parts.size()));
        Bnd_Box box; BRepBndLib::Add(part.shape, box, false);
        const double deflection = std::max(Prs3d::GetDeflection(box, settings.coefficient, 0.1), 1.e-6);
        Handle(MeshProgress) indicator = new MeshProgress(cancel);
        BRepMesh_IncrementalMesh mesh;
        mesh.SetShape(part.shape);
        mesh.ChangeParameters().Deflection = deflection;
        mesh.ChangeParameters().Angle = settings.angle;
        mesh.ChangeParameters().InParallel = false;
        mesh.Perform(indicator->Start());
        if (cancel.load()) throw std::runtime_error("已取消显示精度更新");
        if (!mesh.IsDone()) {
            const auto warning = QStringLiteral("显示三角网未完整生成：%1；精确几何仍保留。").arg(part.name);
            document.diagnostics.append(warning);
        }
    }
    if (progress) progress(100, QStringLiteral("显示三角网已生成"));
}

ViewerWidget::ViewerWidget(QWidget* parent) : QWidget(parent)
{
    auto driver = Mayo::QOpenGLWidgetOccView::createCompatibleGraphicsDriver();
    m_viewer = new V3d_Viewer(driver);
    m_viewer->SetDefaultLights();
    m_viewer->SetLightOn();
    m_context = new AIS_InteractiveContext(m_viewer);
    m_context->SetDisplayMode(AIS_Shaded, false);
    m_context->DefaultDrawer()->SetFaceBoundaryDraw(true);
    m_context->DefaultDrawer()->SetMaximalChordialDeviation(0.1);
    // A filled selection overlay would make a transparent selected part look
    // opaque. Outline highlighting keeps both source and temporary alpha visible.
    m_context->SelectionStyle()->SetDisplayMode(AIS_WireFrame);
    m_context->HighlightStyle()->SetDisplayMode(AIS_WireFrame);
    m_view = m_viewer->CreateView();
    // Qt owns the current context and framebuffer. V3d camera setters must not
    // render synchronously from mouse/slider handlers outside paintGL().
    m_view->SetImmediateUpdate(false);
    m_view->SetBgGradientColors(Quantity_Color(0.91, 0.94, 0.98, Quantity_TOC_sRGB),
                               Quantity_Color(0.98, 0.99, 1.0, Quantity_TOC_sRGB),
                               Aspect_GradientFillMethod_Vertical, false);
    m_view->SetProj(V3d_XposYnegZpos);
    m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
    m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_GRAY50, 0.09, V3d_ZBUFFER);
    m_surface = new Mayo::QOpenGLWidgetOccView(m_view, this);
    m_surface->widget()->setObjectName("cadViewport");
    m_surface->widget()->setFocusPolicy(Qt::StrongFocus);
    m_surface->widget()->installEventFilter(this);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_surface->widget());
    m_clip = new Graphic3d_ClipPlane();
    m_clip->SetOn(false);
    m_clip->SetCapping(true);
    m_clip->SetCappingColor(Quantity_Color(0.92, 0.57, 0.20, Quantity_TOC_sRGB));
    m_view->AddClipPlane(m_clip);
    // Inactive document tabs have never initialized their QOpenGLWidget yet.
    // The first frame establishes the native OCCT window before building AIS.
    connect(static_cast<QOpenGLWidget*>(m_surface->widget()), &QOpenGLWidget::frameSwapped,
            this, &ViewerWidget::maybeStartScene);
}

ViewerWidget::~ViewerWidget()
{
    if (m_meshCancel) m_meshCancel->store(true);
    if (m_meshWorker) m_meshWorker->wait();
    ++m_sceneGeneration;
    stopSceneTimers();
    auto viewport = static_cast<QOpenGLWidget*>(m_surface->widget());
    const bool current = viewport->context() && viewport->isValid();
    if (current) viewport->makeCurrent();
    m_context->RemoveAll(false);
    m_items.clear(); m_pendingItems.clear(); m_annotations.clear();
    m_measurementOverlays.clear(); m_measurementEntities = {};
    for (auto& slot : m_measurementHighlights) slot.clear();
    // OCCT's presentations and view must release GPU resources while their
    // owning Qt context still exists. removeTab alone does not destroy these.
    m_context.Nullify();
    if (!m_view->Window().IsNull()) m_view->Remove();
    m_view.Nullify();
    m_viewer.Nullify();
    if (current) viewport->doneCurrent();
}

void ViewerWidget::setJobQueue(CadJobQueue* queue, quint64 owner)
{
    Q_ASSERT(!hasPendingWork());
    m_jobQueue = queue;
    m_jobOwner = owner;
}

bool ViewerWidget::hasPendingWork() const
{
    return m_meshWorker || m_meshQueued || m_building;
}

void ViewerWidget::stopSceneTimers()
{
    for (const auto& timer : {m_sceneTimer, m_meshTimer}) {
        if (!timer) continue;
        timer->stop();
        timer->deleteLater();
    }
    m_sceneTimer.clear(); m_meshTimer.clear();
}

void ViewerWidget::requestSessionClose()
{
    if (m_sessionClosing) return;
    setMeasurementPicking(false, m_measurementSelection, m_measurementSlot);
    finishPartDrag();
    m_sessionClosing = true;
    clearMeasurementRecords();
    if (m_meshCancel) m_meshCancel->store(true);
    ++m_sceneGeneration;
    stopSceneTimers();
    m_initialScenePending = false;
    m_initialPmi = {};
    m_building = false;
    m_meshChanging = false;
    m_surface->widget()->setUpdatesEnabled(false);
    if (m_jobQueue) m_jobQueue->cancelOwner(m_jobOwner);
    // Running CAD jobs retain this page through the workspace's cleanup list.
    // Do not touch shared meshes or synchronously wait here.
    if (!m_meshWorker && !m_meshQueued) emit shutdownReady();
}

void ViewerWidget::clearDocument()
{
    setMeasurementPicking(false, m_measurementSelection, m_measurementSlot);
    clearMeasurementRecords();
    setGrabMode(false);
    finishPartDrag();
    if (m_meshCancel) m_meshCancel->store(true);
    if (m_meshWorker) m_meshWorker->wait();
    ++m_sceneGeneration;
    stopSceneTimers();
    m_initialScenePending = false; m_initialPmi = {};
    m_context->RemoveAll(false);
    m_items.clear(); m_annotations.clear(); m_selected.clear();
    m_pendingItems.clear(); m_meshSnapshot.clear(); m_meshChanging = false;
    m_surface->widget()->setUpdatesEnabled(true);
    m_document.reset(); m_bounds.SetVoid(); m_explosion = 0;
    m_building = false;
    m_transparency = 0;
    m_selectionTransparency = false;
    m_instanceTransparency.clear(); m_temporarilyHidden.clear();
    m_clip->SetOn(false);
    redraw();
}

void ViewerWidget::cancelSceneBuild()
{
    if (!m_building) return;
    if (m_meshChanging) {
        if (m_meshCancel) m_meshCancel->store(true);
        if (m_meshQueued && m_jobQueue) m_jobQueue->cancelOwner(m_jobOwner);
        if (!m_meshWorker && !m_meshQueued) finishMeshQuality(false, m_meshQuality);
        return;
    }
    clearDocument();
    emit sceneBuildCancelled();
}

void ViewerWidget::setDocument(std::shared_ptr<CadDocument> document, const PmiCatalog& pmi, bool meshesPrepared)
{
    if (m_sessionClosing) return;
    clearDocument();
    m_document = std::move(document);
    m_building = true;
    m_initialScenePending = true;
    m_initialPmi = pmi;
    m_initialMeshesPrepared = meshesPrepared;
    maybeStartScene();
}

void ViewerWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] { maybeStartScene(); redraw(); });
}

void ViewerWidget::maybeStartScene()
{
    if (!m_initialScenePending || m_sessionClosing) return;
    auto viewport = static_cast<QOpenGLWidget*>(m_surface->widget());
    if (!viewport->context() || !viewport->isValid() || m_view->Window().IsNull()) return;
    m_initialScenePending = false;
    const auto pmi = std::move(m_initialPmi);
    m_initialPmi = {};
    startScene(pmi, m_initialMeshesPrepared);
}

void ViewerWidget::startScene(const PmiCatalog& pmi, bool meshesPrepared)
{
    const int generation = m_sceneGeneration;
    auto cursor = std::make_shared<size_t>(0);
    auto pmiCursor = std::make_shared<size_t>(0);
    auto timer = new QTimer(this);
    m_sceneTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, timer, cursor, pmiCursor, generation, pmi, meshesPrepared]() {
        if (generation != m_sceneGeneration || !m_document) { timer->stop(); timer->deleteLater(); return; }
        QElapsedTimer budget; budget.start();
        const size_t end = std::min(*cursor + 32, m_document->occurrences.size());
        for (; *cursor < end && budget.elapsed() < 10; ++*cursor) {
            const auto& occurrence = m_document->occurrences[*cursor];
            if (occurrence.partIndex < 0) continue;
            try {
                // XCAF presentation resolves definition, occurrence and per-face styles.
                Handle(XCAFPrs_AISObject) object = new OccurrencePresentation(
                    occurrence.label, inheritedColors(*m_document, int(*cursor)));
                // Freeze the source styles until an explicit restoration; a mode
                // recompute must not silently erase a temporary transparency.
                object->DispatchStyles(false);
                configureMesh(object, m_meshQuality, meshesPrepared);
                const gp_Trsf local = XCAFDoc_ShapeTool::GetLocation(occurrence.label).Transformation();
                const gp_Trsf parentTransform = occurrence.worldTransform * local.Inverted();
                object->SetLocalTransformation(parentTransform);
                object->Attributes()->SetFaceBoundaryDraw(true);
                m_context->Display(object, false);
                m_items.push_back({int(*cursor), object, parentTransform, gp_Vec(), true});
                m_bounds.Add(occurrenceBounds(int(*cursor)));
            } catch (const Standard_Failure& error) {
                const auto warning = QStringLiteral("显示失败：%1 — %2").arg(occurrence.path, QString::fromUtf8(error.GetMessageString()));
                m_document->diagnostics.append(warning); emit message(warning);
            }
        }
        emit sceneProgress(int(90 * *cursor / std::max<size_t>(m_document->occurrences.size(), 1)));
        if (generation != m_sceneGeneration || !m_document) { timer->stop(); timer->deleteLater(); return; }
        if (*cursor < m_document->occurrences.size()) { redraw(); return; }
        const size_t pmiEnd = std::min(*pmiCursor + 16, pmi.records.size());
        for (; *pmiCursor < pmiEnd && budget.elapsed() < 10; ++*pmiCursor) {
            const auto& record = pmi.records[*pmiCursor];
            QString error;
            auto object = createPmiPresentation(record, &error);
            if (object.IsNull()) {
                if (!error.isEmpty() && (record.status == PmiStatus::SourceGraphics || record.status == PmiStatus::SemanticFallback)) {
                    emit message(QStringLiteral("标注未显示：%1 — %2").arg(record.name,error));
                    emit pmiPresentationFailed(record.id, error);
                }
                continue;
            }
            int owner = -1;
            if (record.occurrencePlacementVerified && record.associationComplete && !record.crossPart && record.ownerOccurrenceIds.size() == 1) {
                const QString id = record.ownerOccurrenceIds.front();
                for (size_t i = 0; i < m_document->occurrences.size(); ++i)
                    if (m_document->occurrences[i].id == id) { owner = int(i); break; }
            }
            // Imported graphical PMI retains its source placement. Ambiguous assembly
            // associations are never duplicated or assigned a guessed transform.
            try {
                m_context->Display(object, false);
                m_context->Deactivate(object);
                m_annotations.push_back({object, owner, owner < 0});
            } catch (const Standard_Failure& failure) {
                m_context->Remove(object, false);
                const QString reason = QStringLiteral("标注显示失败：%1").arg(QString::fromUtf8(failure.GetMessageString()));
                emit message(QStringLiteral("%1 — %2").arg(record.name, reason));
                emit pmiPresentationFailed(record.id, reason);
            }
        }
        emit sceneProgress(90 + int(10 * *pmiCursor / std::max<size_t>(pmi.records.size(), 1)));
        if (generation != m_sceneGeneration || !m_document) { timer->stop(); timer->deleteLater(); return; }
        if (*pmiCursor < pmi.records.size()) { redraw(); return; }
        timer->stop(); timer->deleteLater();
        applyTransparency();
        for (const auto& item : m_items) applyItemVisibility(item);
        updateAnnotations();
        fitAll();
        m_building = false;
        applyMeasurementSelectionModes();
        emit sceneReady();
    });
    timer->start(0);
}

std::vector<int> ViewerWidget::leafIndices(const std::vector<int>& input) const
{
    std::set<int> leaves;
    if (!m_document) return {};
    std::function<void(int)> visit = [&](int index) {
        if (index < 0 || size_t(index) >= m_document->occurrences.size()) return;
        const auto& occurrence = m_document->occurrences[size_t(index)];
        if (occurrence.partIndex >= 0) leaves.insert(index);
        for (int child : occurrence.children) visit(child);
    };
    for (int index : input) visit(index);
    return {leaves.begin(), leaves.end()};
}

bool ViewerWidget::itemDisplayed(const Item& item) const
{
    return item.visible && m_temporarilyHidden.find(item.occurrence) == m_temporarilyHidden.end();
}

void ViewerWidget::applyItemVisibility(const Item& item)
{
    if (itemDisplayed(item)) m_context->Display(item.object, false);
    else m_context->Erase(item.object, false);
}

TemporaryAppearance ViewerWidget::appearanceState() const
{
    return {m_transparency, m_selectionTransparency, m_instanceTransparency, m_temporarilyHidden};
}

void ViewerWidget::applyAppearanceState(const TemporaryAppearance& state)
{
    if (m_sessionClosing || m_building) return;
    m_transparency = std::clamp(state.transparency, 0, 95);
    m_selectionTransparency = state.selectionOnly;
    m_instanceTransparency.clear(); m_temporarilyHidden.clear();
    if (m_document) {
        const auto validLeaf = [this](int index) {
            return index >= 0 && size_t(index) < m_document->occurrences.size()
                && m_document->occurrences[size_t(index)].partIndex >= 0;
        };
        for (const auto& [index, percent] : state.instanceTransparency)
            if (validLeaf(index)) m_instanceTransparency.emplace(index, std::clamp(percent, 0, 95));
        for (int index : state.hidden)
            if (validLeaf(index)) m_temporarilyHidden.insert(index);
    }
    applyTransparency();
    for (const auto& item : m_items) applyItemVisibility(item);
    selectOccurrences(m_selected);
    updateAnnotations(); redraw();
}

bool ViewerWidget::setOccurrenceTemporaryTransparency(int index, int percent)
{
    if (m_sessionClosing || m_building || !m_document || index < 0
        || size_t(index) >= m_document->occurrences.size()
        || m_document->occurrences[size_t(index)].partIndex < 0) return false;
    m_instanceTransparency[index] = std::clamp(percent, 0, 95);
    applyTransparency(); redraw();
    return true;
}

bool ViewerWidget::setOccurrenceTemporarilyHidden(int index, bool hidden)
{
    if (m_sessionClosing || m_building || !m_document || index < 0
        || size_t(index) >= m_document->occurrences.size()
        || m_document->occurrences[size_t(index)].partIndex < 0) return false;
    if (hidden) m_temporarilyHidden.insert(index); else m_temporarilyHidden.erase(index);
    for (const auto& item : m_items)
        if (item.occurrence == index) applyItemVisibility(item);
    selectOccurrences(m_selected);
    updateAnnotations(); redraw();
    return true;
}

bool ViewerWidget::isOccurrenceTemporarilyHidden(int index) const
{
    return m_temporarilyHidden.find(index) != m_temporarilyHidden.end();
}

bool ViewerWidget::isOccurrenceDisplayed(int index) const
{
    for (const auto& item : m_items)
        if (item.occurrence == index) return itemDisplayed(item) && m_context->IsDisplayed(item.object);
    return false;
}

int ViewerWidget::effectiveOccurrenceTransparency(int index) const
{
    if (!m_document || index < 0 || size_t(index) >= m_document->occurrences.size()
        || m_document->occurrences[size_t(index)].partIndex < 0) return 0;
    if (const auto override = m_instanceTransparency.find(index); override != m_instanceTransparency.end())
        return override->second;
    const bool selected = std::find(m_selected.begin(), m_selected.end(), index) != m_selected.end();
    return m_selectionTransparency && !selected ? 0 : m_transparency;
}

void ViewerWidget::selectOccurrences(const std::vector<int>& indices)
{
    m_selected = leafIndices(indices);
    m_context->ClearSelected(false);
    for (const auto& item : m_items) {
        if (itemDisplayed(item) && std::find(m_selected.begin(), m_selected.end(), item.occurrence) != m_selected.end())
            m_context->AddOrRemoveSelected(item.object, false);
    }
    if (m_selectionTransparency) applyTransparency();
    redraw();
}

void ViewerWidget::setOccurrenceVisible(int index, bool visible)
{
    const auto leaves = leafIndices({index});
    for (auto& item : m_items) {
        if (std::find(leaves.begin(), leaves.end(), item.occurrence) == leaves.end()) continue;
        item.visible = visible;
        applyItemVisibility(item);
    }
    updateAnnotations(); redraw();
}

void ViewerWidget::isolateSelection()
{
    if (m_selected.empty()) return;
    for (auto& item : m_items) {
        item.visible = std::find(m_selected.begin(), m_selected.end(), item.occurrence) != m_selected.end();
        applyItemVisibility(item);
    }
    updateAnnotations(); fitAll();
}

void ViewerWidget::showAll()
{
    m_temporarilyHidden.clear();
    for (auto& item : m_items) { item.visible = true; m_context->Display(item.object, false); }
    // Erase removes AIS owners from selection. Restore highlight from our
    // logical selection after revealing temporarily hidden instances.
    selectOccurrences(m_selected);
    updateAnnotations(); redraw();
}

void ViewerWidget::setTransparency(int percent, bool selectionOnly)
{
    m_transparency = std::clamp(percent, 0, 95);
    m_selectionTransparency = selectionOnly;
    // An explicit slider command replaces context-menu overrides only in the
    // slider's current scope. Selection changes merely reapply the appearance.
    if (!selectionOnly) m_instanceTransparency.clear();
    else for (int index : m_selected) m_instanceTransparency.erase(index);
    applyTransparency(); redraw();
}

void ViewerWidget::applyTransparency()
{
    for (auto& item : m_items) {
        const bool selected = std::find(m_selected.begin(), m_selected.end(), item.occurrence) != m_selected.end();
        int percent = m_selectionTransparency && !selected ? 0 : m_transparency;
        if (const auto override = m_instanceTransparency.find(item.occurrence);
            override != m_instanceTransparency.end()) percent = override->second;
        if (item.transparency == percent) continue;
        if (percent == 0) {
            // SetTransparency(0) alone destroys imported per-face alpha. Re-read
            // the source styles after clearing the temporary global override.
            item.object->UnsetTransparency();
            const auto sourceObject = Handle(XCAFPrs_AISObject)::DownCast(item.object);
            if (!sourceObject.IsNull()) sourceObject->DispatchStyles(false);
            m_context->Redisplay(item.object, false);
        } else {
            m_context->SetTransparency(item.object, percent / 100.0, false);
        }
        item.transparency = percent;
    }
}

void ViewerWidget::setSection(int axis, int position, bool reverse, bool cap)
{
    if (axis < 0 || m_bounds.IsVoid()) { m_clip->SetOn(false); redraw(); return; }
    double x0,y0,z0,x1,y1,z1; m_bounds.Get(x0,y0,z0,x1,y1,z1);
    const double low[] {x0,y0,z0}, high[] {x1,y1,z1};
    const double offset = low[axis] + (high[axis]-low[axis]) * position / 100.0;
    double normal[3] {0,0,0}; normal[axis] = reverse ? -1.0 : 1.0;
    gp_Pnt point; point.SetCoord(axis+1, offset);
    m_clip->SetEquation(gp_Pln(point, gp_Dir(normal[0],normal[1],normal[2])));
    m_clip->SetCapping(cap); m_clip->SetOn(true); redraw();
}

Bnd_Box ViewerWidget::occurrenceBounds(int index) const
{
    Bnd_Box box;
    if (!m_document || index < 0 || size_t(index) >= m_document->occurrences.size()) return box;
    const auto& occurrence = m_document->occurrences[size_t(index)];
    if (occurrence.partIndex >= 0) {
        auto shape = m_document->parts[size_t(occurrence.partIndex)].shape;
        shape.Location(TopLoc_Location(occurrence.worldTransform));
        BRepBndLib::Add(shape, box);
    }
    for (int child : occurrence.children) box.Add(occurrenceBounds(child));
    return box;
}

gp_Pnt ViewerWidget::occurrenceCenter(int index) const
{
    auto box = occurrenceBounds(index);
    if (box.IsVoid()) return gp_Pnt();
    double a,b,c,d,e,f; box.Get(a,b,c,d,e,f);
    return gp_Pnt((a+d)/2, (b+e)/2, (c+f)/2);
}

void ViewerWidget::updateGrabCursor()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (m_grabMode)
        m_surface->widget()->setCursor(m_grabOccurrence >= 0 && m_dragged ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    else m_surface->widget()->unsetCursor();
#endif
}

void ViewerWidget::finishPartDrag()
{
    m_grabOccurrence = -1;
    m_button = Qt::NoButton;
    m_dragged = false;
    updateGrabCursor();
}

void ViewerWidget::setGrabMode(bool enabled)
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (enabled && m_measurementPicking) setMeasurementPicking(false, m_measurementSelection, m_measurementSlot);
    finishPartDrag();
    m_grabMode = enabled && !m_sessionClosing;
    updateGrabCursor();
#else
    Q_UNUSED(enabled);
#endif
}

gp_Trsf ViewerWidget::occurrenceDisplayTransform(int index) const
{
    if (!m_document || index < 0 || size_t(index) >= m_document->occurrences.size()) return {};
    for (const auto& item : m_items) {
        if (item.occurrence != index) continue;
        gp_Trsf shift; shift.SetTranslation(item.exploded + item.manualOffset);
        return shift * m_document->occurrences[size_t(index)].worldTransform;
    }
    return m_document->occurrences[size_t(index)].worldTransform;
}

MeasurementEntity ViewerWidget::measurementPartEntity(int index) const
{
    MeasurementEntity entity;
    if (!m_document || index < 0 || size_t(index) >= m_document->occurrences.size()) return entity;
    const auto& occurrence = m_document->occurrences[size_t(index)];
    if (occurrence.partIndex < 0 || size_t(occurrence.partIndex) >= m_document->parts.size()) return entity;
    const auto& part = m_document->parts[size_t(occurrence.partIndex)];
    entity.occurrence = index; entity.occurrenceId = occurrence.id; entity.name = occurrence.path;
    entity.shape = part.shape; entity.selection = MeasurementSelection::Part;
    entity.originalTransform = occurrence.worldTransform;
    entity.displayTransform = occurrenceDisplayTransform(index);
    entity.densityKgM3 = effectiveDensity(part);
    return entity;
}

void ViewerWidget::applyMeasurementSelectionModes()
{
    if (m_sessionClosing || m_building) return;
    for (const auto& item : m_items) {
        m_context->Deactivate(item.object);
        if (!m_measurementPicking) { m_context->Activate(item.object, 0, false); continue; }
        switch (m_measurementSelection) {
        case MeasurementSelection::Vertex: m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_VERTEX), false); break;
        case MeasurementSelection::PointOnGeometry:
            m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_EDGE), false);
            m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_FACE), false); break;
        case MeasurementSelection::Edge: m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_EDGE), false); break;
        case MeasurementSelection::FeatureCenter: m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_EDGE), false); break;
        case MeasurementSelection::FeatureAxis:
            m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_EDGE), false);
            m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_FACE), false); break;
        case MeasurementSelection::Face: m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_FACE), false); break;
        case MeasurementSelection::Solid: m_context->Activate(item.object, AIS_Shape::SelectionMode(TopAbs_SOLID), false); break;
        case MeasurementSelection::Part: m_context->Activate(item.object, 0, false); break;
        }
    }
}

void ViewerWidget::setMeasurementPicking(bool enabled, MeasurementSelection type, int slot)
{
    finishPartDrag();
    if (enabled) setGrabMode(false);
    m_measurementPicking = enabled && !m_sessionClosing && bool(m_document);
    m_measurementSelection = type; m_measurementSlot = std::clamp(slot, 0, 1);
    if (m_measurementPicking) m_context->ClearSelected(false);
    applyMeasurementSelectionModes();
    if (!m_measurementPicking && !m_building && !m_sessionClosing) selectOccurrences(m_selected);
    redraw();
}

bool ViewerWidget::pickMeasurementEntity(const QPoint& position)
{
    if (!m_measurementPicking || m_building || m_sessionClosing || !m_document) return false;
    m_context->MoveTo(position.x(), position.y(), m_view, false);
    if (!m_context->HasDetected()) return false;
    const auto object = m_context->DetectedInteractive();
    const auto owner = Handle(StdSelect_BRepOwner)::DownCast(m_context->DetectedOwner());
    for (const auto& item : m_items) {
        if (item.object != object || !itemDisplayed(item)) continue;
        auto entity = measurementPartEntity(item.occurrence);
        if (entity.shape.IsNull()) return false;
        entity.selection = m_measurementSelection;
        if (m_measurementSelection != MeasurementSelection::Part) {
            if (owner.IsNull() || !owner->HasShape()) return false;
            // Owners contain the leaf's placement; their Location contains the
            // parent/view placement. Normalize both into the definition frame.
            const auto localHit = owner->Shape().Moved(owner->Location())
                .Moved(TopLoc_Location(entity.displayTransform.Inverted()));
            TopTools_IndexedMapOfShape candidates;
            TopExp::MapShapes(entity.shape, localHit.ShapeType(), candidates);
            TopoDS_Shape canonical;
            for (int i = 1; i <= candidates.Extent(); ++i) {
                const auto& candidate = candidates(i);
                if (candidate.IsPartner(localHit)
                    && sameTransform(candidate.Location().Transformation(), localHit.Location().Transformation())) {
                    canonical = candidate; break;
                }
            }
            if (canonical.IsNull()) {
                emit message(QStringLiteral("无法确认所选子几何的实例位置，请重新选择。")); return false;
            }
            entity.shape = canonical;
            if ((m_measurementSelection == MeasurementSelection::Vertex && canonical.ShapeType() != TopAbs_VERTEX)
                || (m_measurementSelection == MeasurementSelection::Edge && canonical.ShapeType() != TopAbs_EDGE)
                || (m_measurementSelection == MeasurementSelection::Face && canonical.ShapeType() != TopAbs_FACE)
                || (m_measurementSelection == MeasurementSelection::Solid && canonical.ShapeType() != TopAbs_SOLID)) return false;
        }
        const auto selector = m_context->MainSelector();
        for (int rank = 1; rank <= selector->NbPicked(); ++rank) {
            if (selector->Picked(rank) != m_context->DetectedOwner()) continue;
            if (entity.selection != MeasurementSelection::Part && entity.selection != MeasurementSelection::Solid) {
                const auto localSample = selector->PickedPoint(rank).Transformed(entity.displayTransform.Inverted());
                QString error;
                if (!refineMeasurementPick(entity, localSample, &error)) {
                    if (!error.isEmpty()) emit message(error);
                    return false;
                }
            }
            break;
        }
        m_measurementEntities[size_t(m_measurementSlot)] = {entity};
        rebuildMeasurementHighlights();
        emit measurementEntityPicked(entity, m_measurementSlot);
        redraw(); return true;
    }
    return false;
}

void ViewerWidget::setMeasurementHighlights(const std::vector<MeasurementEntity>& a,
                                          const std::vector<MeasurementEntity>& b)
{
    m_measurementEntities = {a, b}; rebuildMeasurementHighlights(); redraw();
}

void ViewerWidget::rebuildMeasurementHighlights()
{
    for (auto& slot : m_measurementHighlights) {
        for (const auto& object : slot) m_context->Remove(object, false);
        slot.clear();
    }
    if (m_building || m_sessionClosing) return;
    const Quantity_Color colors[] {Quantity_Color(0.95, 0.47, 0.08, Quantity_TOC_sRGB),
                                  Quantity_Color(0.17, 0.65, 0.30, Quantity_TOC_sRGB)};
    for (size_t slot = 0; slot < m_measurementEntities.size(); ++slot) {
        for (const auto& entity : m_measurementEntities[slot]) {
            if (entity.shape.IsNull() || !isOccurrenceDisplayed(entity.occurrence)) continue;
            const auto transform = occurrenceDisplayTransform(entity.occurrence);
            Handle(AIS_InteractiveObject) object;
            if (entity.selection == MeasurementSelection::PointOnGeometry && entity.pickedPoint)
                object = new AIS_Point(new Geom_CartesianPoint(entity.pickedPoint->Transformed(transform)));
            else object = new AIS_Shape(entity.shape.Moved(TopLoc_Location(transform)));
            object->SetColor(colors[slot]); object->SetWidth(3.0);
            object->SetInfiniteState(true); object->SetZLayer(Graphic3d_ZLayerId_Top);
            m_context->Display(object, AIS_WireFrame, -1, false); m_context->Deactivate(object);
            m_measurementHighlights[slot].push_back(object);
            if (entity.selection == MeasurementSelection::FeatureCenter || entity.selection == MeasurementSelection::FeatureAxis
                || entity.extent == MeasurementExtent::Infinite) {
                auto fresh = entity; fresh.displayTransform = transform;
                const auto guide = featureGuide(fresh, MeasurementCoordinates::Displayed, colors[slot]);
                if (!guide.IsNull()) {
                    guide->SetColor(colors[slot]); guide->SetInfiniteState(true); guide->SetZLayer(Graphic3d_ZLayerId_Topmost);
                    m_context->Display(guide, 0, -1, false); m_context->Deactivate(guide);
                    m_measurementHighlights[slot].push_back(guide);
                }
            }
        }
    }
}

bool ViewerWidget::measurementEntityPositionChanged(const MeasurementEntity& entity, MeasurementCoordinates coordinates) const
{
    if (!m_document || entity.occurrence < 0 || size_t(entity.occurrence) >= m_document->occurrences.size()
        || m_document->occurrences[size_t(entity.occurrence)].id != entity.occurrenceId) return true;
    const auto expected = coordinates == MeasurementCoordinates::Original ? entity.originalTransform : entity.displayTransform;
    return !sameTransform(expected, occurrenceDisplayTransform(entity.occurrence));
}

bool ViewerWidget::measurementRecordPositionChanged(quint64 id) const
{
    const auto found = m_measurementOverlays.find(id);
    if (found == m_measurementOverlays.end()) return false;
    const auto& request = found->second.record.request;
    for (const auto* entities : {&request.a, &request.b})
        for (const auto& entity : *entities)
            if (measurementEntityPositionChanged(entity, request.coordinates)) return true;
    return false;
}

void ViewerWidget::addMeasurementRecord(const MeasurementRecord& record)
{
    if (m_sessionClosing || !record.result.success) return;
    removeMeasurementRecord(record.id);
    try {
        MeasurementOverlay overlay; overlay.record = record;
        overlay.objects = measurementObjects(record, m_measurementDecimals, m_surface->widget()->devicePixelRatioF());
        for (const auto& object : overlay.objects) {
            const auto text = Handle(AIS_TextLabel)::DownCast(object);
            if (!text.IsNull()) overlay.labelAnchor = text->Position();
        }
        m_measurementOverlays.emplace(record.id, std::move(overlay));
        updateMeasurementPresentations(); redraw();
    } catch (const Standard_Failure& failure) {
        emit message(QStringLiteral("测量标注生成失败：%1").arg(QString::fromUtf8(failure.GetMessageString())));
    }
}

void ViewerWidget::setMeasurementDecimals(int decimals)
{
    decimals = std::clamp(decimals, 3, 6);
    if (m_measurementDecimals == decimals || m_sessionClosing) return;
    m_measurementDecimals = decimals;
    for (auto& [id, overlay] : m_measurementOverlays) {
        Q_UNUSED(id);
        for (const auto& object : overlay.objects) m_context->Remove(object, false);
        overlay.objects = measurementObjects(overlay.record, m_measurementDecimals, m_surface->widget()->devicePixelRatioF());
        overlay.labelLeader.Nullify();
        for (const auto& object : overlay.objects) {
            const auto text = Handle(AIS_TextLabel)::DownCast(object);
            if (!text.IsNull()) overlay.labelAnchor = text->Position();
        }
    }
    updateMeasurementPresentations(); redraw();
}

void ViewerWidget::removeMeasurementRecord(quint64 id)
{
    const auto found = m_measurementOverlays.find(id);
    if (found == m_measurementOverlays.end()) return;
    for (const auto& object : found->second.objects) m_context->Remove(object, false);
    m_measurementOverlays.erase(found); redraw();
}

void ViewerWidget::setMeasurementRecordVisible(quint64 id, bool visible)
{
    const auto found = m_measurementOverlays.find(id);
    if (found == m_measurementOverlays.end()) return;
    found->second.record.visible = visible; updateMeasurementPresentations(); redraw();
}

void ViewerWidget::clearMeasurementRecords()
{
    for (const auto& entry : m_measurementOverlays)
        for (const auto& object : entry.second.objects) m_context->Remove(object, false);
    m_measurementOverlays.clear(); m_measurementEntities = {};
    rebuildMeasurementHighlights(); redraw();
}

void ViewerWidget::updateMeasurementPresentations()
{
    for (auto& [id, overlay] : m_measurementOverlays) {
        bool visible = overlay.record.visible && !measurementRecordPositionChanged(id) && !m_sessionClosing;
        for (const auto* entities : {&overlay.record.request.a, &overlay.record.request.b})
            for (const auto& entity : *entities) visible = visible && isOccurrenceDisplayed(entity.occurrence);
        for (const auto& object : overlay.objects) {
            if (visible) { m_context->Display(object, 0, -1, false); m_context->Deactivate(object); }
            else m_context->Erase(object, false);
        }
    }
    rebuildMeasurementHighlights();
    emit measurementPositionsChanged();
}

void ViewerWidget::updateMeasurementLabelLayout()
{
    if (m_building || m_sessionClosing || m_view->Window().IsNull()
        || !m_surface->widget()->isVisible() || m_measurementOverlays.empty()) return;
    const double ratio = m_surface->widget()->devicePixelRatioF();
    const double height = 14.0 * ratio, gap = 8.0 * ratio;
    const double width = m_surface->widget()->width() * ratio;
    const double viewportHeight = m_surface->widget()->height() * ratio;
    std::vector<QRectF> occupied;
    for (auto& [id, overlay] : m_measurementOverlays) {
        Q_UNUSED(id);
        Handle(AIS_TextLabel) text;
        for (const auto& object : overlay.objects) {
            const auto candidate = Handle(AIS_TextLabel)::DownCast(object);
            // Each record has one measured-value label; reference guides carry no text.
            if (!candidate.IsNull()) text = candidate;
        }
        if (text.IsNull() || !m_context->IsDisplayed(text)) continue;
        int anchorX, anchorY;
        m_view->Convert(overlay.labelAnchor.X(), overlay.labelAnchor.Y(), overlay.labelAnchor.Z(), anchorX, anchorY);
        const double labelWidth = std::min(width - gap * 2.0, std::max(40.0 * ratio, text->Text().Length() * height * 0.64));
        const double x = std::clamp(double(anchorX) + 20.0 * ratio, gap, std::max(gap, width - gap - labelWidth));
        const double baseline = std::clamp(double(anchorY), height * 0.5 + gap, std::max(height * 0.5 + gap, viewportHeight - height * 0.5 - gap));
        QRectF labelBox(x, baseline - height * 0.5, labelWidth, height);
        // Prefer the anchor's neighbourhood, stepping above/below to keep
        // successive records readable even when their exact centers coincide.
        for (int step = 0; step < 100; ++step) {
            const int row = (step + 1) / 2;
            const double y = baseline + (step % 2 ? row : -row) * (height + gap);
            QRectF candidate(x, y - height * 0.5, labelWidth, height);
            if (candidate.top() < gap || candidate.bottom() > viewportHeight - gap) continue;
            bool collision = false;
            for (const auto& previous : occupied)
                if (candidate.adjusted(-gap * 0.5, -gap * 0.5, gap * 0.5, gap * 0.5).intersects(previous)) { collision = true; break; }
            if (!collision) { labelBox = candidate; break; }
        }
        occupied.push_back(labelBox);
        double ox, oy, oz, dx, dy, dz;
        m_view->ConvertWithProj(qRound(labelBox.left()), qRound(labelBox.center().y()), ox, oy, oz, dx, dy, dz);
        const gp_Pnt origin(ox, oy, oz); const gp_Vec ray(dx, dy, dz), normal(m_view->Camera()->Direction());
        const double denominator = ray.Dot(normal);
        if (std::abs(denominator) <= 1.e-12) continue;
        const gp_Pnt position = origin.Translated(ray * (gp_Vec(origin, overlay.labelAnchor).Dot(normal) / denominator));
        if (position.Distance(text->Position()) <= 1.e-7) continue;
        text->SetPosition(position); m_context->Redisplay(text, false);
        if (position.Distance(overlay.labelAnchor) > 1.e-7) {
            const auto edge = BRepBuilderAPI_MakeEdge(overlay.labelAnchor, position).Shape();
            if (overlay.labelLeader.IsNull()) {
                overlay.labelLeader = new AIS_Shape(edge);
                overlay.labelLeader->SetColor(Quantity_Color(0.12, 0.36, 0.83, Quantity_TOC_sRGB));
                overlay.labelLeader->SetWidth(1.0); overlay.labelLeader->SetInfiniteState(true);
                overlay.labelLeader->SetZLayer(Graphic3d_ZLayerId_Topmost);
                overlay.objects.push_back(overlay.labelLeader);
                m_context->Display(overlay.labelLeader, 0, -1, false); m_context->Deactivate(overlay.labelLeader);
            } else {
                overlay.labelLeader->SetShape(edge); m_context->Redisplay(overlay.labelLeader, false);
            }
        }
    }
}

void ViewerWidget::applyItemTransform(const Item& item)
{
    gp_Trsf shift;
    shift.SetTranslation(item.exploded + item.manualOffset);
    m_context->SetLocation(item.object, TopLoc_Location(shift * item.base));
}

void ViewerWidget::resetPartPositions()
{
    finishPartDrag();
    for (auto& item : m_items) {
        item.manualOffset = gp_Vec();
        item.exploded = gp_Vec();
        applyItemTransform(item);
    }
    m_explosion = 0;
    m_context->UpdateSelected(false);
    updateAnnotations();
    redraw();
}

bool ViewerWidget::partDragPoint(const QPoint& position, gp_Pnt& point) const
{
    double x, y, z, dx, dy, dz;
    m_view->ConvertWithProj(position.x(), position.y(), x, y, z, dx, dy, dz);
    const gp_Pnt origin(x, y, z);
    const gp_Vec direction(dx, dy, dz);
    const double denominator = direction.Dot(m_grabPlaneNormal);
    if (std::abs(denominator) < 1.e-12) return false;
    const double distance = gp_Vec(origin, m_grabPlanePoint).Dot(m_grabPlaneNormal) / denominator;
    point = origin.Translated(direction * distance);
    return std::isfinite(point.X()) && std::isfinite(point.Y()) && std::isfinite(point.Z());
}

bool ViewerWidget::beginPartDrag(const QPoint& position)
{
    m_context->MoveTo(position.x(), position.y(), m_view, false);
    if (!m_context->HasDetected()) {
        selectOccurrences({});
        emit selectionChanged(m_selected);
        return false;
    }
    const auto object = m_context->DetectedInteractive();
    for (const auto& item : m_items) {
        if (item.object != object || !itemDisplayed(item)) continue;
        // Selection-only transparency can recompute presentations. Capture
        // the hit before changing selection or notifying the rest of the UI.
        const int occurrence = item.occurrence;
        const gp_Vec startOffset = item.manualOffset;
        const int generation = m_sceneGeneration;
        const auto selector = m_context->MainSelector();
        bool found = false;
        for (int rank = 1; rank <= selector->NbPicked(); ++rank) {
            if (selector->Picked(rank) != m_context->DetectedOwner()) continue;
            m_grabPlanePoint = selector->PickedPoint(rank);
            found = true;
            break;
        }
        selectOccurrences({occurrence});
        emit selectionChanged(m_selected);
        if (!found || generation != m_sceneGeneration || !m_grabMode || m_building || m_sessionClosing) return false;
        m_grabPlaneNormal = gp_Vec(m_view->Camera()->Direction());
        if (!partDragPoint(position, m_grabStartPoint)) return false;
        m_grabStartOffset = startOffset;
        m_grabOccurrence = occurrence;
        return true;
    }
    return false;
}

void ViewerWidget::updatePartDrag(const QPoint& position)
{
    if (m_grabOccurrence < 0) return;
    gp_Pnt point;
    if (!partDragPoint(position, point)) return;
    for (auto& item : m_items) {
        if (item.occurrence != m_grabOccurrence) continue;
        if (!itemDisplayed(item)) { finishPartDrag(); return; }
        item.manualOffset = m_grabStartOffset + gp_Vec(m_grabStartPoint, point);
        applyItemTransform(item);
        break;
    }
    m_context->UpdateSelected(false);
    updateAnnotations();
    redraw();
}

void ViewerWidget::setExplosion(int percent, int scope)
{
    finishPartDrag();
    if (!m_document || m_bounds.IsVoid()) return;
    const bool previouslyExploded = m_explosion > 0;
    percent = std::clamp(percent, 0, 100);
    std::vector<int> branches;
    if (scope >= 0 && size_t(scope) < m_document->occurrences.size())
        branches = m_document->occurrences[size_t(scope)].children;
    else {
        for (size_t i=0; i<m_document->occurrences.size(); ++i)
            if (m_document->occurrences[i].parent < 0) branches.push_back(int(i));
        if (branches.size()==1 && m_document->occurrences[size_t(branches[0])].partIndex<0)
            branches = m_document->occurrences[size_t(branches[0])].children;
    }
    // Always restore previous transforms, including when switching to a leaf
    // or an assembly with only one child while already exploded.
    for (auto& item : m_items) item.exploded = gp_Vec();
    m_explosion = branches.size() > 1 ? percent : 0;
    Bnd_Box scopeBox;
    for (int index : branches) scopeBox.Add(occurrenceBounds(index));
    if (!scopeBox.IsVoid() && m_explosion > 0) {
    double a,b,c,d,e,f; scopeBox.Get(a,b,c,d,e,f);
    gp_Pnt center((a+d)/2,(b+e)/2,(c+f)/2);
    const double travel = std::max(std::sqrt((d-a)*(d-a)+(e-b)*(e-b)+(f-c)*(f-c)),1.0) * percent / 100.0;
    const gp_Vec directions[] {gp_Vec(1,0,0),gp_Vec(-1,0,0),gp_Vec(0,1,0),gp_Vec(0,-1,0),gp_Vec(0,0,1),gp_Vec(0,0,-1)};
    for (size_t bIndex=0; bIndex<branches.size(); ++bIndex) {
        auto direction = gp_Vec(center, occurrenceCenter(branches[bIndex]));
        if (direction.Magnitude()<1e-7) direction = directions[bIndex%6];
        direction.Normalize(); direction.Multiply(travel);
        const auto leaves = leafIndices({branches[bIndex]});
        for (auto& item : m_items)
            if (std::find(leaves.begin(),leaves.end(),item.occurrence)!=leaves.end()) item.exploded = direction;
    }
    }
    for (const auto& item : m_items) applyItemTransform(item);
    m_context->UpdateSelected(false);
    updateAnnotations(); redraw();
    if (m_explosion > 0 && !previouslyExploded)
        emit message(QStringLiteral("爆炸仅改变显示位置；跨零件或关联不明确的标注暂时隐藏，复位后恢复。"));
}

void ViewerWidget::setAnnotationsVisible(bool visible) { m_annotationsVisible=visible; updateAnnotations(); redraw(); }
void ViewerWidget::updateAnnotations()
{
    const bool displaced = m_explosion > 0 || std::any_of(m_items.begin(), m_items.end(),
        [](const Item& item) { return item.manualOffset.SquareMagnitude() > 1.e-16; });
    for (const auto& annotation : m_annotations) {
        bool visible = m_annotationsVisible && !(displaced && annotation.assemblyWide);
        gp_Trsf shift;
        for (const auto& item : m_items) if (item.occurrence==annotation.occurrence) {
            visible = visible && itemDisplayed(item); shift.SetTranslation(item.exploded + item.manualOffset); break;
        }
        m_context->SetLocation(annotation.object, TopLoc_Location(shift));
        if (visible) { m_context->Display(annotation.object, false); m_context->Deactivate(annotation.object); }
        else m_context->Erase(annotation.object, false);
    }
    updateMeasurementPresentations();
}

void ViewerWidget::fitAll()
{
    finishPartDrag();
    Bnd_Box modelBounds;
    // Transform-persistent text can still contribute to OCCT's scene-fit
    // calculation despite InfiniteState. Fit only displayed model geometry.
    for (const auto& item : m_items) {
        if (!itemDisplayed(item)) continue;
        const auto shape = Handle(AIS_Shape)::DownCast(item.object);
        if (shape.IsNull() || shape->Shape().IsNull()) continue;
        BRepBndLib::Add(shape->Shape().Moved(TopLoc_Location(item.object->Transformation())), modelBounds);
    }
    if (!modelBounds.IsVoid()) {
        m_view->FitAll(modelBounds, 0.08, false);
        m_view->Camera()->ZFitAll(1.0, modelBounds, modelBounds);
    }
    redraw();
}
void ViewerWidget::standardView(int view)
{
    finishPartDrag();
    const V3d_TypeOfOrientation orientations[] {V3d_XposYnegZpos,V3d_Yneg,V3d_Ypos,V3d_Xneg,V3d_Xpos,V3d_Zpos,V3d_Zneg};
    m_view->SetProj(orientations[std::clamp(view,0,6)]); fitAll();
}
void ViewerWidget::setPerspective(bool enabled)
{ finishPartDrag(); m_view->Camera()->SetProjectionType(enabled ? Graphic3d_Camera::Projection_Perspective : Graphic3d_Camera::Projection_Orthographic); fitAll(); }
void ViewerWidget::setDisplayMode(int mode)
{
    m_displayMode = std::clamp(mode, 0, 2);
    for (const auto& item : m_items) m_context->SetDisplayMode(item.object, mode==2 ? AIS_WireFrame : AIS_Shaded, false);
    for (const auto& item : m_items) { item.object->Attributes()->SetFaceBoundaryDraw(mode==0); m_context->Redisplay(item.object,false); }
    redraw();
}

bool ViewerWidget::setMeshQuality(int quality)
{
    quality = std::clamp(quality, 0, 2);
    if (m_sessionClosing || m_building || m_meshWorker || m_meshQueued) return false;
    if (quality == m_meshQuality) return true;
    if (!m_document || m_items.empty()) { m_meshQuality = quality; return true; }

    finishPartDrag();
    m_building = true; m_meshChanging = true;
    m_meshCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_meshCancel;
    const auto document = m_document;
    const int generation = ++m_sceneGeneration;
    const auto error = std::make_shared<QString>();
    TopTools_MapOfShape visited;
    for (const auto& part : document->parts) {
        for (TopExp_Explorer faces(part.shape, TopAbs_FACE); faces.More(); faces.Next()) {
            if (!visited.Add(faces.Current())) continue;
            const auto face = TopoDS::Face(faces.Current()); TopLoc_Location location;
            m_meshSnapshot.emplace_back(face, BRep_Tool::Triangulation(face, location));
        }
    }
    // Keep the old framebuffer and AIS presentations intact. While the CAD
    // worker updates shared triangulations, neither painting nor picking may
    // read that document. The window's progress/cancel controls stay active.
    m_surface->widget()->setUpdatesEnabled(false);
    emit meshQualityStarted();
    const QPointer<ViewerWidget> guard(this);
    if (m_jobQueue) {
        m_meshQueued = true;
        m_meshCancel = m_jobQueue->enqueue(m_jobOwner,
            [document, quality](std::atomic_bool& queueCancel, const ProgressCallback& progress) {
                prepareDisplayMeshes(*document, quality, queueCancel, progress);
            },
            [guard, generation](int percent, const QString&) {
                if (guard && !guard->m_sessionClosing && generation == guard->m_sceneGeneration
                    && guard->m_meshChanging) emit guard->meshQualityProgress(percent * 70 / 100);
            },
            [guard, generation, quality](const QString& queueError) {
                if (!guard) return;
                const auto queueCancel = guard->m_meshCancel;
                guard->m_meshQueued = false;
                guard->completeMeshWork(generation, quality, queueCancel, queueError);
            });
        return true;
    }
    const ProgressCallback progress = [guard, generation](int percent, const QString&) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, percent] {
            if (guard && !guard->m_sessionClosing && generation == guard->m_sceneGeneration
                && guard->m_meshChanging) emit guard->meshQualityProgress(percent * 70 / 100);
        }, Qt::QueuedConnection);
    };
    m_meshWorker = QThread::create([document, quality, cancel, progress, error] {
        try { prepareDisplayMeshes(*document, quality, *cancel, progress); }
        catch (const Standard_Failure& failure) { *error = QString::fromUtf8(failure.GetMessageString()); }
        catch (const std::exception& failure) { *error = QString::fromUtf8(failure.what()); }
        catch (...) { *error = QStringLiteral("发生未识别的显示三角网错误"); }
    });
    m_meshWorker->setParent(this);
    connect(m_meshWorker, &QThread::finished, this, [this, generation, quality, cancel, error] {
        auto finished = m_meshWorker; m_meshWorker = nullptr; finished->deleteLater();
        completeMeshWork(generation, quality, cancel, *error);
    });
    m_meshWorker->start();
    return true;
}

void ViewerWidget::completeMeshWork(int generation, int quality,
                                   const std::shared_ptr<std::atomic_bool>& cancel,
                                   const QString& error)
{
    if (m_sessionClosing) {
        m_meshChanging = false; m_building = false;
        m_meshSnapshot.clear(); m_meshCancel.reset();
        emit shutdownReady();
        return;
    }
    if (generation != m_sceneGeneration || !m_document) return;
    if (cancel->load() || !error.isEmpty()) {
        if (!cancel->load()) {
            const auto warning = QStringLiteral("显示精度更新失败，已保留原视图：%1").arg(error);
            m_document->diagnostics.append(warning); emit message(warning);
        }
        finishMeshQuality(false, quality); return;
    }
    // Construct replacement AIS objects invisibly, in short GUI batches.
    // All original per-occurrence view state is carried into the new item.
    auto cursor = std::make_shared<size_t>(0);
    auto timer = new QTimer(this);
    m_meshTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, timer, cursor, generation, quality, cancel] {
        if (m_sessionClosing || generation != m_sceneGeneration || !m_meshChanging) { timer->stop(); timer->deleteLater(); return; }
        if (cancel->load()) { timer->stop(); timer->deleteLater(); finishMeshQuality(false, quality); return; }
        QElapsedTimer budget; budget.start();
        const size_t end = std::min(*cursor + 16, m_items.size());
        for (; *cursor < end && budget.elapsed() < 10; ++*cursor) {
            const auto& original = m_items[*cursor];
            const auto& occurrence = m_document->occurrences[size_t(original.occurrence)];
            Handle(XCAFPrs_AISObject) object;
            try {
                object = new OccurrencePresentation(occurrence.label, inheritedColors(*m_document, original.occurrence));
                object->DispatchStyles(false); configureMesh(object, quality);
                gp_Trsf shift; shift.SetTranslation(original.exploded + original.manualOffset);
                object->SetLocalTransformation(shift * original.base);
                object->Attributes()->SetFaceBoundaryDraw(m_displayMode == 0);
                m_context->Display(object, m_displayMode == 2 ? AIS_WireFrame : AIS_Shaded, 0, false);
                if (original.transparency > 0) m_context->SetTransparency(object, original.transparency / 100.0, false);
                m_context->Erase(object, false);
                auto replacement = original; replacement.object = object;
                m_pendingItems.push_back(std::move(replacement));
            } catch (const Standard_Failure& failure) {
                if (!object.IsNull()) m_context->Remove(object, false);
                const auto warning = QStringLiteral("显示精度更新失败，已保留原视图：%1 — %2")
                    .arg(occurrence.path, QString::fromUtf8(failure.GetMessageString()));
                m_document->diagnostics.append(warning); emit message(warning);
                timer->stop(); timer->deleteLater(); finishMeshQuality(false, quality); return;
            }
        }
        emit meshQualityProgress(70 + int(30 * *cursor / std::max<size_t>(m_items.size(), 1)));
        if (m_sessionClosing || generation != m_sceneGeneration || !m_meshChanging) { timer->stop(); timer->deleteLater(); return; }
        if (*cursor < m_items.size()) return;
        timer->stop(); timer->deleteLater(); finishMeshQuality(true, quality);
    });
    timer->start(0);
}

void ViewerWidget::finishMeshQuality(bool commit, int quality)
{
    if (m_sessionClosing) {
        m_meshSnapshot.clear(); m_meshCancel.reset();
        m_building = false; m_meshChanging = false;
        emit shutdownReady();
        return;
    }
    if (commit) {
        for (const auto& item : m_items) m_context->Remove(item.object, false);
        m_items.swap(m_pendingItems); m_pendingItems.clear();
        m_context->ClearSelected(false);
        for (const auto& item : m_items) {
            if (!itemDisplayed(item)) continue;
            m_context->Display(item.object, false);
            if (std::find(m_selected.begin(), m_selected.end(), item.occurrence) != m_selected.end())
                m_context->AddOrRemoveSelected(item.object, false);
        }
        m_meshQuality = quality;
    } else {
        for (const auto& item : m_pendingItems) m_context->Remove(item.object, false);
        m_pendingItems.clear();
        // Restore the original face meshes as well as the old GPU scene. Edge
        // polygon caches may have been cleared; OCCT regenerates those from
        // exact curves when subsequently needed.
        BRep_Builder builder;
        for (const auto& snapshot : m_meshSnapshot) builder.UpdateFace(snapshot.first, snapshot.second);
    }
    m_meshSnapshot.clear();
    m_building = false; m_meshChanging = false; m_meshCancel.reset();
    applyMeasurementSelectionModes(); updateMeasurementPresentations();
    m_surface->widget()->setUpdatesEnabled(true); redraw();
    if (commit) emit meshQualityFinished(m_meshQuality);
    else emit meshQualityCancelled(m_meshQuality);
}

void ViewerWidget::redraw() { if (m_surface) { updateMeasurementLabelLayout(); m_surface->redraw(); } }
bool ViewerWidget::saveScreenshot(const QString& filename)
{
    auto widget=dynamic_cast<QOpenGLWidget*>(m_surface->widget());
    return widget && widget->grabFramebuffer().save(filename);
}

bool ViewerWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched!=m_surface->widget()) return QWidget::eventFilter(watched,event);
    if (event->type() == QEvent::DevicePixelRatioChange) {
        for (const auto& [id, overlay] : m_measurementOverlays) {
            Q_UNUSED(id);
            for (const auto& object : overlay.objects) {
                const auto text = Handle(AIS_TextLabel)::DownCast(object);
                if (text.IsNull()) continue;
                text->SetHeight(14.0 * m_surface->widget()->devicePixelRatioF());
                m_context->Redisplay(text, false);
            }
        }
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse
        || event->type() == QEvent::Resize) finishPartDrag();
#endif
    if (m_view->Window().IsNull()) return QWidget::eventFilter(watched,event);
    // Right-button dragging belongs to CAD navigation, including macOS's
    // context-menu event generated by the same mouse gesture.
    if (event->type()==QEvent::ContextMenu) { event->accept(); return true; }
    if ((m_building || m_sessionClosing) && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick
        || event->type() == QEvent::Wheel || event->type() == QEvent::NativeGesture)) return true;
    const double ratio=m_surface->widget()->devicePixelRatioF();
    const auto point = [ratio](QPointF p) { return QPoint(qRound(p.x()*ratio),qRound(p.y()*ratio)); };
    if (event->type()==QEvent::MouseButtonPress) {
        auto mouse=static_cast<QMouseEvent*>(event);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        // A second button must not replace a held grab with camera navigation.
        if (m_grabMode && m_button == Qt::LeftButton && mouse->button() != Qt::LeftButton) return true;
#endif
        m_button=mouse->button(); m_press=point(mouse->position()); m_last=m_press; m_dragged=false;
        if (m_button==Qt::LeftButton) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            if (m_grabMode) {
                m_grabOccurrence = -1;
                beginPartDrag(m_press);
                updateGrabCursor();
            } else
#endif
                m_view->StartRotation(m_press.x(),m_press.y());
        }
        return true;
    }
    if (event->type()==QEvent::MouseMove) {
        auto mouse=static_cast<QMouseEvent*>(event); const auto pos=point(mouse->position());
        if (m_button!=Qt::NoButton && !mouse->buttons().testFlag(m_button)) finishPartDrag();
        if (m_button!=Qt::NoButton) {
            if ((pos-m_press).manhattanLength()>4*ratio) m_dragged=true;
            if (m_dragged) {
                if (m_button==Qt::LeftButton) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
                    if (m_grabMode) { updateGrabCursor(); updatePartDrag(pos); } else
#endif
                        m_view->Rotation(pos.x(),pos.y());
                }
                // Pan(false) restores the camera saved at a prior operation.
                // These are incremental deltas, so start from the current
                // camera on each move rather than resetting to that old state.
                else if (m_button==Qt::RightButton || m_button==Qt::MiddleButton)
                    m_view->Pan(pos.x()-m_last.x(),m_last.y()-pos.y(),1.0,true);
                m_last=pos; redraw();
            }
        } else { m_context->MoveTo(pos.x(),pos.y(),m_view,false); redraw(); }
        return true;
    }
    if (event->type()==QEvent::MouseButtonRelease) {
        auto mouse=static_cast<QMouseEvent*>(event); const auto pos=point(mouse->position());
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (m_grabMode && m_button == Qt::LeftButton) {
            if (mouse->button() != Qt::LeftButton) return true;
            if (m_dragged || (pos - m_press).manhattanLength() > 4 * ratio) updatePartDrag(pos);
            finishPartDrag();
            return true;
        }
#endif
        if (m_button == Qt::RightButton && mouse->button() == Qt::RightButton) {
            // Release can be farther away even when an intermediate move was
            // missed. Any drag belongs exclusively to CAD panning.
            const bool dragged = m_dragged || (pos - m_press).manhattanLength() > 4 * ratio;
            m_button = Qt::NoButton;
            if (!dragged) {
                m_context->MoveTo(pos.x(), pos.y(), m_view, false);
                int hit = -1;
                if (m_context->HasDetected()) {
                    const auto object = m_context->DetectedInteractive();
                    for (const auto& item : m_items)
                        if (item.object == object && itemDisplayed(item)) { hit = item.occurrence; break; }
                }
                if (hit >= 0) {
                    selectOccurrences({hit});
                    emit selectionChanged(m_selected);
                } else redraw();
                emit partContextMenuRequested(hit, mouse->globalPosition().toPoint());
            }
            return true;
        }
        if (m_button==Qt::LeftButton && !m_dragged) {
            if (m_measurementPicking) {
                pickMeasurementEntity(pos); m_button = Qt::NoButton; return true;
            }
            m_context->MoveTo(pos.x(),pos.y(),m_view,false);
            m_context->SelectDetected(mouse->modifiers().testFlag(Qt::ControlModifier)||mouse->modifiers().testFlag(Qt::MetaModifier)
                                     ? AIS_SelectionScheme_XOR : AIS_SelectionScheme_Replace);
            m_selected.clear();
            for(m_context->InitSelected();m_context->MoreSelected();m_context->NextSelected()) {
                const auto object=m_context->SelectedInteractive();
                for(const auto& item:m_items) if(item.object==object) m_selected.push_back(item.occurrence);
            }
            if (m_selectionTransparency) applyTransparency();
            emit selectionChanged(m_selected); redraw();
        }
        m_button=Qt::NoButton; return true;
    }
    if (event->type()==QEvent::MouseButtonDblClick) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (m_grabMode && m_grabOccurrence >= 0 && m_dragged) return true;
#endif
        fitAll(); return true;
    }
    if (event->type()==QEvent::Wheel) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (m_grabMode && m_button == Qt::LeftButton) return true;
#endif
        auto wheel=static_cast<QWheelEvent*>(event);
        const double delta=wheel->angleDelta().y()!=0 ? wheel->angleDelta().y()/120.0 : wheel->pixelDelta().y()/40.0;
        m_view->SetScale(std::clamp(m_view->Scale()*std::pow(1.15,delta),1e-9,1e9)); redraw(); return true;
    }
    if (event->type()==QEvent::NativeGesture) {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (m_grabMode && m_button == Qt::LeftButton) return true;
#endif
        auto gesture=static_cast<QNativeGestureEvent*>(event);
        if(gesture->gestureType()==Qt::ZoomNativeGesture) { m_view->SetScale(m_view->Scale()*std::exp(gesture->value())); redraw(); return true; }
    }
    return QWidget::eventFilter(watched,event);
}
} // namespace Stp
