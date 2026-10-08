#pragma once
#include "cad_document.h"
#include "pmi.h"
#include <QMainWindow>
#include <QElapsedTimer>
#include <functional>
#include "appearance_state.h"
#include <QPointer>
#include <optional>

class QAction; class QCheckBox; class QComboBox; class QLabel; class QLineEdit;
class QProgressBar; class QPushButton; class QSlider; class QTableWidget; class QTableWidgetItem;
class QTextEdit; class QThread; class QTreeWidget; class QTreeWidgetItem;
class QStackedWidget;
class QMenu;
class QToolButton;

namespace Stp {
class ViewerWidget;
class CadJobQueue;
class MeasurementPanel;
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr
                        , CadJobQueue* jobs = nullptr, quint64 owner = 0
                        );
    ~MainWindow() override;
    void openStep(const QString& path);
    void runSmokeTest(const QString& input, const QString& outputDirectory);
    void beginLoadDocument(const QString& path);
    void requestSessionClose();
    void commitManualEditor();
    void cancelPendingMeasurementForSessionSwitch();
    bool isBusy() const { return m_busy; }
    bool isClosing() const { return m_closePending; }
    quint64 sessionId() const { return m_owner; }
    std::shared_ptr<CadDocument> document() const { return m_document; }
    QMenu* documentViewMenu() const;
    QMenu* documentHelpMenu() const;
    int appearanceUndoCount() const { return int(m_appearanceHistory.size()); }
    void undoAppearance();
    void resetAppearance();
signals:
    void openFilesRequested(const QStringList& paths);
    void stateChanged();
    void closeReady();
protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void closeEvent(QCloseEvent*) override;
    void showEvent(QShowEvent*) override;
private:
    void buildUi();
    void populateDocument();
    void selectionChanged(const std::vector<int>& indices, bool updateScene);
    void showProperties(const std::vector<int>& indices);
    void chooseFile();
    void saveBom();
    void saveParts();
    void runTask(const QString& title, std::function<void(std::atomic_bool&,const ProgressCallback&)> work,
                 std::function<void(const QString&)> done);
    void setBusy(bool busy, const QString& label = {});
    void diagnostic(const QString& text);
    void updateSection();
    void filterTree(const QString& text);
    void startSmokeCapture(qint64 loadMs);
    void setBomPageVisible(bool visible);
    std::vector<int> checkedLeafOccurrences() const;
    void refreshBom();
    void scheduleBomRefresh();
    void applyManualField(QTableWidgetItem* item);
    std::shared_ptr<CadDocument> processingDocument() const;
    void finishSessionClose();
    bool canChangeAppearance() const;
    void recordAppearanceChange(const TemporaryAppearance& before);
    void finishAppearanceDrag();
    void applyAppearanceSnapshot(const TemporaryAppearance& state);
    void refreshAppearanceUi();
    void updateAppearanceActions();
    void showPartAppearanceMenu(int occurrence, const QPoint& globalPosition);
    void dismissAppearanceMenu();
    void clearAppearanceForNewDocument();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    void updatePartGrabState();
    MeasurementPanel* m_measurementPanel = nullptr;
    QAction* m_measurementAction = nullptr;
    bool m_measurementSmokeStarted = false;
#endif
    std::vector<int> treeSelection() const;
    ViewerWidget* m_view = nullptr;
    QTreeWidget* m_tree = nullptr;
    QTableWidget *m_bom = nullptr, *m_properties = nullptr, *m_pmiTable = nullptr;
    QTextEdit* m_diagnostics = nullptr;
    QLineEdit* m_filter = nullptr;
    QLabel *m_summary = nullptr, *m_status = nullptr, *m_pmiSummary = nullptr;
    QProgressBar* m_progress = nullptr;
    QPushButton* m_cancel = nullptr;
    QSlider *m_explode = nullptr, *m_transparency = nullptr, *m_sectionPosition = nullptr;
    QComboBox *m_scope = nullptr, *m_sectionAxis = nullptr, *m_meshQuality = nullptr;
    QCheckBox *m_sectionReverse = nullptr, *m_sectionCap = nullptr, *m_selectedTransparency = nullptr;
    QAction *m_openAction = nullptr, *m_bomAction = nullptr, *m_partsAction = nullptr, *m_pmiAction = nullptr;
    QWidget* m_controls = nullptr;
    QStackedWidget* m_pages = nullptr;
    QAction* m_bomPageAction = nullptr;
    bool m_assemblyDockWasVisible = false, m_detailsDockWasVisible = false;
    bool m_bomRefreshPending = false;
    std::vector<int> m_bomPartIndices;
    QLabel* m_bomScopeSummary = nullptr;
    CadJobQueue* m_jobs = nullptr;
    quint64 m_owner = 0;
    bool m_embedded = false, m_closeReady = false, m_taskPending = false;
    bool m_layoutInitialized = false;
    QString m_sessionPath;
    QWidget* m_importFailure = nullptr;
    QLabel* m_importError = nullptr;
    QAction* m_undoAppearanceAction = nullptr;
    QAction* m_resetAppearanceAction = nullptr;
    QToolButton* m_appearanceRestoreButton = nullptr;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QAction* m_partGrabAction = nullptr;
    QPushButton* m_resetPartPositionsButton = nullptr;
    QLabel* m_partGrabHint = nullptr;
    bool m_partGrabSceneReady = false;
#endif
    QPointer<QMenu> m_partAppearanceMenu;
    std::vector<TemporaryAppearance> m_appearanceHistory;
    std::optional<TemporaryAppearance> m_appearanceDragStart;
    quint64 m_appearanceMenuGeneration = 0;
    std::shared_ptr<CadDocument> m_document;
    PmiCatalog m_pmi;
    std::vector<QTreeWidgetItem*> m_treeItems;
    std::vector<int> m_currentSelection;
    QThread* m_worker = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    bool m_syncing = false, m_busy = false, m_closePending = false;
    bool m_compactLayout = false;
    QString m_smokeDirectory;
    QElapsedTimer m_loadTimer;
};
} // namespace Stp
