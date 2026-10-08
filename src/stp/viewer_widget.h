#pragma once

#include "cad_document.h"
#include "pmi.h"
#include <QWidget>
#include <AIS_InteractiveContext.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <V3d_View.hxx>
#include <Bnd_Box.hxx>
#include <QPoint>
#include <QPointer>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Face.hxx>
#include <set>
#include "appearance_state.h"
#include "measurement.h"

namespace Mayo { class IWidgetOccView; }
class QThread;
class QTimer;
class QShowEvent;
class AIS_Shape;

namespace Stp {
class CadJobQueue;

// Run before displaying a document, on the serial CAD worker. Only its
// triangulation cache changes; exact surfaces, labels and properties remain.
void prepareDisplayMeshes(CadDocument& document, int quality, std::atomic_bool& cancel,
                          const ProgressCallback& progress = {});

class ViewerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    ~ViewerWidget() override;
    void setJobQueue(CadJobQueue* queue, quint64 owner);
    void requestSessionClose();
    bool hasPendingWork() const;
    void setDocument(std::shared_ptr<CadDocument> document, const PmiCatalog& pmi,
                     bool meshesPrepared = false);
    void clearDocument();
    void cancelSceneBuild();
    void selectOccurrences(const std::vector<int>& indices);
    void setOccurrenceVisible(int index, bool visible);
    void isolateSelection();
    void showAll();
    void setTransparency(int percent, bool selectionOnly);
    TemporaryAppearance appearanceState() const;
    void applyAppearanceState(const TemporaryAppearance& state);
    bool setOccurrenceTemporaryTransparency(int index, int percent);
    bool setOccurrenceTemporarilyHidden(int index, bool hidden);
    bool isOccurrenceTemporarilyHidden(int index) const;
    bool isOccurrenceDisplayed(int index) const;
    int effectiveOccurrenceTransparency(int index) const;
    void setSection(int axis, int position, bool reverse, bool cap);
    void setExplosion(int percent, int scope);
    void setGrabMode(bool enabled);
    bool grabMode() const { return m_grabMode; }
    void resetPartPositions();
    void finishPartDrag();
    void setMeasurementPicking(bool enabled, MeasurementSelection type, int slot = 0);
    bool measurementPicking() const { return m_measurementPicking; }
    MeasurementEntity measurementPartEntity(int occurrence) const;
    void setMeasurementHighlights(const std::vector<MeasurementEntity>& a,
                                  const std::vector<MeasurementEntity>& b);
    void addMeasurementRecord(const MeasurementRecord& record);
    void setMeasurementDecimals(int decimals);
    void removeMeasurementRecord(quint64 id);
    void setMeasurementRecordVisible(quint64 id, bool visible);
    void clearMeasurementRecords();
    bool measurementRecordPositionChanged(quint64 id) const;
    void setAnnotationsVisible(bool visible);
    void fitAll();
    void standardView(int view);
    void setPerspective(bool enabled);
    void setDisplayMode(int mode);
    bool setMeshQuality(int quality);
    int meshQuality() const { return m_meshQuality; }
    bool saveScreenshot(const QString& filename);
    const std::vector<int>& selectedOccurrences() const { return m_selected; }
    Handle(V3d_View) view() const { return m_view; }
    Handle(AIS_InteractiveContext) context() const { return m_context; }
signals:
    void selectionChanged(const std::vector<int>& indices);
    void sceneReady();
    void sceneProgress(int percent);
    void sceneBuildCancelled();
    void meshQualityStarted();
    void meshQualityProgress(int percent);
    void meshQualityFinished(int quality);
    void meshQualityCancelled(int quality);
    void pmiPresentationFailed(const QString& recordId, const QString& reason);
    void message(const QString& text);
    void shutdownReady();
    void partContextMenuRequested(int occurrence, const QPoint& globalPosition);
    void measurementEntityPicked(const Stp::MeasurementEntity& entity, int slot);
    void measurementPositionsChanged();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
private:
    struct Item {
        int occurrence = -1;
        Handle(AIS_InteractiveObject) object;
        gp_Trsf base;
        gp_Vec exploded;
        bool visible = true;
        int transparency = 0;
        gp_Vec manualOffset;
    };
    struct Annotation {
        Handle(AIS_InteractiveObject) object;
        int occurrence = -1;
        bool assemblyWide = true;
    };
    std::vector<int> leafIndices(const std::vector<int>& input) const;
    void updateAnnotations();
    void applyMeasurementSelectionModes();
    bool pickMeasurementEntity(const QPoint& position);
    gp_Trsf occurrenceDisplayTransform(int occurrence) const;
    bool measurementEntityPositionChanged(const MeasurementEntity&, MeasurementCoordinates) const;
    void updateMeasurementPresentations();
    void updateMeasurementLabelLayout();
    void rebuildMeasurementHighlights();
    void applyItemTransform(const Item& item);
    bool beginPartDrag(const QPoint& position);
    void updatePartDrag(const QPoint& position);
    bool partDragPoint(const QPoint& position, gp_Pnt& point) const;
    void updateGrabCursor();
    void applyTransparency();
    bool itemDisplayed(const Item& item) const;
    void applyItemVisibility(const Item& item);
    void finishMeshQuality(bool commit, int quality);
    void completeMeshWork(int generation, int quality,
                          const std::shared_ptr<std::atomic_bool>& cancel,
                          const QString& error);
    void startScene(const PmiCatalog& pmi, bool meshesPrepared);
    void maybeStartScene();
    void stopSceneTimers();
    void redraw();
    gp_Pnt occurrenceCenter(int index) const;
    Bnd_Box occurrenceBounds(int index) const;
    Mayo::IWidgetOccView* m_surface = nullptr;
    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;
    Handle(Graphic3d_ClipPlane) m_clip;
    std::shared_ptr<CadDocument> m_document;
    std::vector<Item> m_items;
    std::vector<Item> m_pendingItems;
    std::vector<std::pair<TopoDS_Face, Handle(Poly_Triangulation)>> m_meshSnapshot;
    std::vector<Annotation> m_annotations;
    struct MeasurementOverlay {
        MeasurementRecord record;
        std::vector<Handle(AIS_InteractiveObject)> objects;
        gp_Pnt labelAnchor;
        Handle(AIS_Shape) labelLeader;
    };
    std::map<quint64, MeasurementOverlay> m_measurementOverlays;
    std::array<std::vector<MeasurementEntity>, 2> m_measurementEntities;
    std::array<std::vector<Handle(AIS_InteractiveObject)>, 2> m_measurementHighlights;
    bool m_measurementPicking = false;
    MeasurementSelection m_measurementSelection = MeasurementSelection::Part;
    int m_measurementSlot = 0;
    int m_measurementDecimals = 3;
    std::vector<int> m_selected;
    Bnd_Box m_bounds;
    QPoint m_press, m_last;
    Qt::MouseButton m_button = Qt::NoButton;
    bool m_dragged = false;
    bool m_grabMode = false;
    int m_grabOccurrence = -1;
    gp_Pnt m_grabPlanePoint, m_grabStartPoint;
    gp_Vec m_grabPlaneNormal, m_grabStartOffset;
    bool m_annotationsVisible = true;
    bool m_building = false;
    int m_explosion = 0;
    int m_sceneGeneration = 0;
    int m_meshQuality = 1, m_displayMode = 0, m_transparency = 0;
    bool m_selectionTransparency = false, m_meshChanging = false;
    std::map<int, int> m_instanceTransparency;
    std::set<int> m_temporarilyHidden;
    QThread* m_meshWorker = nullptr;
    std::shared_ptr<std::atomic_bool> m_meshCancel;
    QPointer<CadJobQueue> m_jobQueue;
    quint64 m_jobOwner = 0;
    bool m_meshQueued = false, m_sessionClosing = false;
    bool m_initialScenePending = false, m_initialMeshesPrepared = false;
    PmiCatalog m_initialPmi;
    QPointer<QTimer> m_sceneTimer, m_meshTimer;
};
} // namespace Stp
