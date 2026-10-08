#pragma once

#include "measurement.h"
#include <QWidget>
#include <QPointer>
#include <array>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QGroupBox;

namespace Stp {
class CadJobQueue;
class ViewerWidget;

// Each document window owns its draft, immutable result snapshots and overlays.
class MeasurementPanel final : public QWidget {
    Q_OBJECT
public:
    explicit MeasurementPanel(ViewerWidget* viewer, CadJobQueue* jobs, quint64 owner,
                              QWidget* parent = nullptr);
    ~MeasurementPanel() override;
    void setDocument(std::shared_ptr<CadDocument> document);
    void setEnabledForScene(bool enabled);
    void setPickingEnabled(bool enabled);
    void setTreeSelection(const std::vector<int>& selection);
    void clearAll();
    void cancelForSessionSwitch();
    void prepareClose();
    int recordCount() const { return int(m_records.size()); }
    const std::vector<MeasurementRecord>& records() const { return m_records; }
    QWidget* recordsPage() const { return m_recordsPage; }
signals:
    void pickingEnabledChanged(bool enabled);
    void recordsChanged();
    void showRecordsRequested();
private:
    MeasurementKind kind() const;
    MeasurementSelection selection(int slot) const;
    bool between() const;
    void updateOperation();
    void updateKind();
    void updateControls();
    void updatePicking();
    void updateDraftLabels();
    void pickedEntity(const MeasurementEntity& entity, int slot);
    void captureTree(int slot);
    void compute();
    void cancelPending();
    void rebuildRecords();
    void deleteSelected();
    void copyRecords(bool selectedOnly);
    void showRecordDetails(int row);
    std::vector<quint64> selectedRecordIds() const;
    QString entityText(const std::vector<MeasurementEntity>& entities) const;
    void setStatus(const QString& text);
    QPointer<ViewerWidget> m_viewer;
    QPointer<CadJobQueue> m_jobs;
    quint64 m_owner = 0, m_generation = 0, m_nextRecord = 1;
    std::shared_ptr<CadDocument> m_document;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::array<std::vector<MeasurementEntity>, 2> m_draft;
    std::vector<int> m_treeSelection;
    std::vector<MeasurementRecord> m_records;
    bool m_sceneEnabled = false, m_pickingEnabled = false, m_closing = false;
    bool m_rebuildingRecords = false;
    int m_activeSlot = 0;
    QComboBox *m_operation = nullptr, *m_kind = nullptr, *m_coordinates = nullptr;
    std::array<QComboBox*, 2> m_selection {{nullptr, nullptr}};
    std::array<QCheckBox*, 2> m_extent {{nullptr, nullptr}};
    std::array<QPushButton*, 2> m_slot {{nullptr, nullptr}}, m_capture {{nullptr, nullptr}};
    std::array<QLabel*, 2> m_draftLabel {{nullptr, nullptr}};
    std::array<QGroupBox*, 2> m_slotGroup {{nullptr, nullptr}};
    QPushButton *m_compute = nullptr, *m_clearAll = nullptr, *m_delete = nullptr;
    QPushButton *m_copySelected = nullptr, *m_copyAll = nullptr;
    QPushButton *m_showRecords = nullptr, *m_historyClearAll = nullptr;
    QWidget* m_recordsPage = nullptr;
    QTableWidget* m_table = nullptr;
    QSpinBox* m_decimals = nullptr;
    QLabel* m_status = nullptr;
};
} // namespace Stp
