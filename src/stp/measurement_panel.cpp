#include "measurement_panel.h"

#include "cad_job_queue.h"
#include "viewer_widget.h"
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace Stp {
namespace {
template<typename Enum> void addChoice(QComboBox* box, const QString& text, Enum value)
{
    box->addItem(text, int(value));
}
void setChoice(QComboBox* box, int value)
{
    const int index = box->findData(value);
    if (index >= 0) box->setCurrentIndex(index);
}
QString measurementControlName(const char* prefix, int slot)
{
    return QString::fromLatin1(prefix) + (slot == 0 ? QStringLiteral("A") : QStringLiteral("B"));
}
QString entityDetails(const std::vector<MeasurementEntity>& entities)
{
    QStringList names;
    for (const auto& entity : entities) {
        QString name = entity.name.isEmpty() ? QStringLiteral("对象 %1").arg(entity.occurrence + 1) : entity.name;
        const auto reference = measurementReferenceText(entity);
        if (!reference.isEmpty()) name += QStringLiteral("（%1）").arg(reference);
        names.push_back(entity.occurrenceId.isEmpty() ? name : name + QStringLiteral(" [") + entity.occurrenceId + QStringLiteral("]"));
    }
    return names.join(QStringLiteral("、"));
}
QString compactResult(const MeasurementRecord& record, int decimals)
{
    const auto number = [decimals](double value) {
        return QString::number(value == 0 ? 0. : value, 'f', decimals);
    };
    if ((record.request.kind == MeasurementKind::Point || record.request.kind == MeasurementKind::CenterOfMass || record.request.kind == MeasurementKind::VolumeCenter)
        && !record.result.anchors.empty()) {
        const auto& point = record.result.anchors.front();
        QString center = QStringLiteral("XYZ");
        if ((record.request.kind == MeasurementKind::CenterOfMass || record.request.kind == MeasurementKind::VolumeCenter) && !record.result.values.empty()) {
            center = record.result.values.back().label;
            if (center.endsWith(QLatin1Char('Z'))) center.chop(1);
            center = center.trimmed();
        }
        return QStringLiteral("(%1, %2, %3)\n%4 · mm").arg(number(point.X()), number(point.Y()), number(point.Z()), center);
    }
    if (record.result.values.empty()) return QStringLiteral("测量完成");
    const auto& value = record.result.values.front();
    return number(value.value) + (value.unit.isEmpty() ? QString{} : QStringLiteral(" ") + value.unit);
}
QString detailedRecord(const MeasurementRecord& record, int decimals, bool positionChanged)
{
    QStringList lines;
    lines.push_back(QStringLiteral("M%1  %2  %3").arg(record.id).arg(measurementKindText(record.request.kind), measurementCoordinatesText(record.request.coordinates)));
    lines.push_back(QStringLiteral("A: ") + entityDetails(record.request.a));
    if (!record.request.b.empty()) lines.push_back(QStringLiteral("B: ") + entityDetails(record.request.b));
    lines.push_back(measurementResultText(record.result, decimals));
    if (positionChanged) lines.push_back(QStringLiteral("位置已变化；数值为测量时的快照。"));
    return lines.join(QLatin1Char('\n'));
}
}

MeasurementPanel::MeasurementPanel(ViewerWidget* viewer, CadJobQueue* jobs, quint64 owner, QWidget* parent)
    : QWidget(parent), m_viewer(viewer), m_jobs(jobs), m_owner(owner)
{
    setObjectName(QStringLiteral("measurementPanel"));
    setAccessibleName(QStringLiteral("工程测量"));
    if (!m_jobs) m_jobs = new CadJobQueue(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    auto* instructions = new QLabel(QStringLiteral("单击拾取 · 拖动旋转 · 自动保留"), this);
    layout->addWidget(instructions);
    auto* typeRow = new QHBoxLayout;
    typeRow->setSpacing(4);
    m_operation = new QComboBox(this);
    m_operation->setObjectName(QStringLiteral("measurementOperation"));
    m_operation->setAccessibleName(QStringLiteral("测量方式"));
    m_operation->addItems({QStringLiteral("测量对象"), QStringLiteral("测量之间")});
    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("measurementKind"));
    m_kind->setAccessibleName(QStringLiteral("测量类型"));
    typeRow->addWidget(new QLabel(QStringLiteral("方式"), this));
    typeRow->addWidget(m_operation, 1);
    typeRow->addWidget(new QLabel(QStringLiteral("类型"), this));
    typeRow->addWidget(m_kind, 1);
    layout->addLayout(typeRow);
    auto* positionRow = new QHBoxLayout;
    m_coordinates = new QComboBox(this);
    m_coordinates->setObjectName(QStringLiteral("measurementCoordinates"));
    m_coordinates->setAccessibleName(QStringLiteral("测量坐标模式"));
    addChoice(m_coordinates, QStringLiteral("原始装配位置"), MeasurementCoordinates::Original);
    addChoice(m_coordinates, QStringLiteral("当前显示位置"), MeasurementCoordinates::Displayed);
    positionRow->addWidget(new QLabel(QStringLiteral("位置"), this));
    positionRow->addWidget(m_coordinates, 1);
    layout->addLayout(positionRow);
    auto* slotButtons = new QButtonGroup(this);
    slotButtons->setExclusive(true);
    for (int slot = 0; slot < 2; ++slot) {
        const QString letter = slot == 0 ? QStringLiteral("A") : QStringLiteral("B");
        m_slotGroup[slot] = new QGroupBox(this);
        m_slotGroup[slot]->setObjectName(measurementControlName("measurementSlotGroup", slot));
        auto* slotLayout = new QVBoxLayout(m_slotGroup[slot]);
        slotLayout->setContentsMargins(6, 6, 6, 6);
        slotLayout->setSpacing(4);
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        m_slot[slot] = new QPushButton(QStringLiteral("拾取 ") + letter, this);
        m_slot[slot]->setObjectName(measurementControlName("measurementSlot", slot));
        m_slot[slot]->setAccessibleName(QStringLiteral("拾取测量对象 ") + letter);
        m_slot[slot]->setCheckable(true);
        slotButtons->addButton(m_slot[slot], slot);
        row->addWidget(m_slot[slot]);
        m_selection[slot] = new QComboBox(this);
        m_selection[slot]->setObjectName(measurementControlName("measurementSelection", slot));
        m_selection[slot]->setAccessibleName(QStringLiteral("对象 ") + letter + QStringLiteral(" 选择模式"));
        m_selection[slot]->setMinimumContentsLength(4);
        m_selection[slot]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_selection[slot]->setToolTip(QStringLiteral("圆心：解析圆或圆弧；轴线：圆、圆弧、圆柱面或圆锥面的无限轴。边按拾取处切线、面按拾取处法线测角。"));
        row->addWidget(m_selection[slot], 1);
        m_capture[slot] = new QPushButton(QStringLiteral("装配树"), this);
        m_capture[slot]->setObjectName(measurementControlName("measurementCaptureTree", slot));
        m_capture[slot]->setAccessibleName(QStringLiteral("使用装配树选中项作为对象 ") + letter);
        m_capture[slot]->setToolTip(QStringLiteral("汇总选中装配及其所有零件实例，自动去除重复选择。"));
        row->addWidget(m_capture[slot]);
        slotLayout->addLayout(row);
        m_extent[slot] = new QCheckBox(QStringLiteral("无限拓展"), m_slotGroup[slot]);
        m_extent[slot]->setObjectName(measurementControlName("measurementExtent", slot));
        m_extent[slot]->setAccessibleName(QStringLiteral("对象 ") + letter + QStringLiteral(" 无限拓展"));
        slotLayout->addWidget(m_extent[slot]);
        connect(m_extent[slot], &QCheckBox::toggled, this, [this, slot](bool checked) {
            for (auto& entity : m_draft[slot]) entity.extent = checked ? MeasurementExtent::Infinite : MeasurementExtent::Bounded;
            updateDraftLabels();
            if (m_viewer) m_viewer->setMeasurementHighlights(m_draft[0], between() ? m_draft[1] : std::vector<MeasurementEntity>{});
            setStatus(checked ? QStringLiteral("对象 %1 使用无限参考几何；显示大小不参与计算。").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"))
                              : QStringLiteral("对象 %1 使用实际有界几何。").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B")));
        });
        m_draftLabel[slot] = new QLabel(this);
        m_draftLabel[slot]->setObjectName(measurementControlName("measurementDraft", slot));
        m_draftLabel[slot]->setWordWrap(true);
        m_draftLabel[slot]->setFixedHeight(fontMetrics().lineSpacing() * 2 + 2);
        m_draftLabel[slot]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_draftLabel[slot]->setTextInteractionFlags(Qt::TextSelectableByMouse);
        slotLayout->addWidget(m_draftLabel[slot]);
        layout->addWidget(m_slotGroup[slot]);
        connect(m_slot[slot], &QPushButton::clicked, this, [this, slot] {
            m_activeSlot = slot; setPickingEnabled(true); updatePicking();
        });
        connect(m_selection[slot], QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, slot] {
            const QSignalBlocker extentBlock(m_extent[slot]);
            m_extent[slot]->setChecked(false);
            m_draft[slot].clear(); updateDraftLabels();
            if (m_viewer) m_viewer->setMeasurementHighlights(m_draft[0], between() ? m_draft[1] : std::vector<MeasurementEntity>{});
            updatePicking(); updateControls();
        });
        connect(m_capture[slot], &QPushButton::clicked, this, [this, slot] { captureTree(slot); });
    }
    m_slot[0]->setChecked(true);
    auto* statusRow = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("measurementStatus"));
    m_status->setWordWrap(true);
    m_status->setFixedHeight(fontMetrics().lineSpacing() * 3 + 4);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusRow->addWidget(m_status, 1);
    auto* statusDetails = new QPushButton(QStringLiteral("详情"), this);
    statusDetails->setObjectName(QStringLiteral("measurementStatusDetails"));
    connect(statusDetails, &QPushButton::clicked, this, [this] {
        auto* dialog = new QMessageBox(QMessageBox::Information, QStringLiteral("测量状态"),
            m_status->toolTip(), QMessageBox::Ok, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->open();
    });
    statusRow->addWidget(statusDetails);
    layout->addLayout(statusRow);
    layout->addStretch(1);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    m_compute = new QPushButton(QStringLiteral("测量"), this);
    m_compute->setObjectName(QStringLiteral("measurementCompute"));
    m_compute->setAccessibleName(QStringLiteral("计算并保留测量结果"));
    m_showRecords = new QPushButton(QStringLiteral("查看记录（0）"), this);
    m_showRecords->setObjectName(QStringLiteral("measurementShowRecords"));
    m_clearAll = new QPushButton(QStringLiteral("清除所有测量"), this);
    m_clearAll->setObjectName(QStringLiteral("measurementClearAll"));
    m_clearAll->setAccessibleName(QStringLiteral("清除所有测量及标注"));
    m_clearAll->setStyleSheet(QStringLiteral("QPushButton { font-weight: bold; }"));
    actions->addWidget(m_compute);
    actions->addWidget(m_showRecords);
    actions->addWidget(m_clearAll);
    layout->addLayout(actions);
    connect(m_showRecords, &QPushButton::clicked, this, &MeasurementPanel::showRecordsRequested);

    m_recordsPage = new QWidget(this);
    m_recordsPage->setObjectName(QStringLiteral("measurementRecordsPage"));
    auto* historyLayout = new QVBoxLayout(m_recordsPage);
    historyLayout->setContentsMargins(6, 6, 6, 6);
    auto* precisionRow = new QHBoxLayout;
    precisionRow->addWidget(new QLabel(QStringLiteral("显示小数位"), m_recordsPage));
    m_decimals = new QSpinBox(m_recordsPage);
    m_decimals->setObjectName(QStringLiteral("measurementDecimals"));
    m_decimals->setAccessibleName(QStringLiteral("测量结果显示小数位"));
    m_decimals->setRange(3, 6); m_decimals->setValue(3);
    precisionRow->addWidget(m_decimals); precisionRow->addStretch();
    historyLayout->addLayout(precisionRow);
    m_table = new QTableWidget(m_recordsPage);
    m_table->setObjectName(QStringLiteral("measurementRecords"));
    m_table->setAccessibleName(QStringLiteral("保留的测量结果"));
    m_table->setColumnCount(6); m_table->setMinimumSize(0, 0);
    m_table->setHorizontalHeaderLabels({QStringLiteral("显"), QStringLiteral("编号"), QStringLiteral("类型"),
                                       QStringLiteral("结果"), QStringLiteral("对象"), QStringLiteral("位置")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->setToolTip(QStringLiteral("双击查看完整数值、坐标及对象快照。勾选框控制三维标注显示。"));
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setMinimumSectionSize(20);
    for (int column=0; column<6; ++column) m_table->setColumnWidth(column, std::array<int,6>{{24,38,64,150,150,130}}[size_t(column)]);
    historyLayout->addWidget(m_table, 1);
    auto* tools = new QHBoxLayout;
    m_copySelected = new QPushButton(QStringLiteral("复制选中"), m_recordsPage);
    m_copySelected->setObjectName(QStringLiteral("measurementCopySelected"));
    m_copyAll = new QPushButton(QStringLiteral("复制全部"), m_recordsPage);
    m_copyAll->setObjectName(QStringLiteral("measurementCopyAll"));
    m_delete = new QPushButton(QStringLiteral("删除选中"), m_recordsPage);
    m_delete->setObjectName(QStringLiteral("measurementDeleteSelected"));
    tools->addWidget(m_copySelected); tools->addWidget(m_copyAll); tools->addWidget(m_delete);
    historyLayout->addLayout(tools);
    m_historyClearAll = new QPushButton(QStringLiteral("清除所有测量"), m_recordsPage);
    m_historyClearAll->setObjectName(QStringLiteral("measurementHistoryClearAll"));
    m_historyClearAll->setStyleSheet(QStringLiteral("QPushButton { font-weight: bold; }"));
    historyLayout->addWidget(m_historyClearAll);
    connect(m_historyClearAll, &QPushButton::clicked, this, &MeasurementPanel::clearAll);

    connect(m_operation, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateOperation(); });
    connect(m_kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateKind(); });
    connect(m_compute, &QPushButton::clicked, this, &MeasurementPanel::compute);
    connect(m_clearAll, &QPushButton::clicked, this, &MeasurementPanel::clearAll);
    connect(m_delete, &QPushButton::clicked, this, &MeasurementPanel::deleteSelected);
    connect(m_copySelected, &QPushButton::clicked, this, [this] { copyRecords(true); });
    connect(m_copyAll, &QPushButton::clicked, this, [this] { copyRecords(false); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MeasurementPanel::updateControls);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { showRecordDetails(row); });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_rebuildingRecords || item->column() != 0) return;
        const quint64 id = item->data(Qt::UserRole).toULongLong();
        for (auto& record : m_records) if (record.id == id) {
            record.visible = item->checkState() == Qt::Checked;
            if (m_viewer) m_viewer->setMeasurementRecordVisible(id, record.visible);
            emit recordsChanged();
            break;
        }
    });
    connect(m_decimals, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int decimals) {
        rebuildRecords();
        if (m_viewer) m_viewer->setMeasurementDecimals(decimals);
    });
    if (m_viewer) {
        connect(m_viewer, &ViewerWidget::measurementEntityPicked, this, &MeasurementPanel::pickedEntity);
        connect(m_viewer, &ViewerWidget::measurementPositionsChanged, this, &MeasurementPanel::rebuildRecords);
    }
    updateOperation();
    setStatus(QStringLiteral("启用测量后选择对象。"));
}

void MeasurementPanel::setStatus(const QString& text)
{
    m_status->setText(text);
    m_status->setToolTip(text);
}

MeasurementPanel::~MeasurementPanel() { cancelPending(); }

MeasurementKind MeasurementPanel::kind() const
{
    return MeasurementKind(m_kind->currentData().toInt());
}
MeasurementSelection MeasurementPanel::selection(int slot) const
{
    return MeasurementSelection(m_selection[slot]->currentData().toInt());
}
bool MeasurementPanel::between() const { return m_operation->currentIndex() == 1; }

void MeasurementPanel::setDocument(std::shared_ptr<CadDocument> document)
{
    clearAll();
    m_document = std::move(document);
    m_treeSelection.clear();
    m_nextRecord = 1;
    if (!m_document) m_sceneEnabled = false;
    setStatus(m_document ? QStringLiteral("选择对象后点击“测量”；结果将自动保留。") : QStringLiteral("请打开 STEP 文件。"));
    updateControls();
    updatePicking();
}

void MeasurementPanel::setEnabledForScene(bool enabled)
{
    m_sceneEnabled = enabled;
    updatePicking();
    updateControls();
}

void MeasurementPanel::setPickingEnabled(bool enabled)
{
    const bool changed = m_pickingEnabled != enabled;
    m_pickingEnabled = enabled && !m_closing;
    updatePicking();
    updateControls();
    if (changed) emit pickingEnabledChanged(m_pickingEnabled);
}

void MeasurementPanel::setTreeSelection(const std::vector<int>& selection)
{
    m_treeSelection = selection;
    updateControls();
}

void MeasurementPanel::updateOperation()
{
    const int previous = m_kind->currentData().toInt();
    const QSignalBlocker blocker(m_kind);
    m_kind->clear();
    if (between()) {
        addChoice(m_kind, measurementKindText(MeasurementKind::Distance), MeasurementKind::Distance);
        addChoice(m_kind, measurementKindText(MeasurementKind::Angle), MeasurementKind::Angle);
    } else {
        for (auto value : {MeasurementKind::Point, MeasurementKind::Length, MeasurementKind::Radius,
                           MeasurementKind::Diameter, MeasurementKind::Area, MeasurementKind::Volume,
                           MeasurementKind::Mass, MeasurementKind::CenterOfMass, MeasurementKind::VolumeCenter})
            addChoice(m_kind, measurementKindText(value), value);
    }
    setChoice(m_kind, previous);
    if (!between()) {
        for (int slot = 0; slot < 2; ++slot) {
            const QSignalBlocker extentBlock(m_extent[slot]);
            m_extent[slot]->setChecked(false);
            for (auto& entity : m_draft[slot]) entity.extent = MeasurementExtent::Bounded;
        }
        m_activeSlot = 0;
        m_slot[0]->setChecked(true);
    }
    m_slotGroup[1]->setVisible(between());
    updateKind();
}

void MeasurementPanel::updateKind()
{
    const auto value = kind();
    std::vector<MeasurementSelection> allowed;
    if (value == MeasurementKind::Point) allowed = {MeasurementSelection::Vertex, MeasurementSelection::PointOnGeometry, MeasurementSelection::FeatureCenter};
    else if (value == MeasurementKind::Length) allowed = {MeasurementSelection::Edge};
    else if (value == MeasurementKind::Radius || value == MeasurementKind::Diameter) allowed = {MeasurementSelection::Edge, MeasurementSelection::Face};
    else if (value == MeasurementKind::Area) allowed = {MeasurementSelection::Face, MeasurementSelection::Solid, MeasurementSelection::Part};
    else if (value == MeasurementKind::Volume || value == MeasurementKind::Mass || value == MeasurementKind::CenterOfMass || value == MeasurementKind::VolumeCenter)
        allowed = {MeasurementSelection::Part, MeasurementSelection::Solid};
    else if (value == MeasurementKind::Angle) allowed = {MeasurementSelection::Edge, MeasurementSelection::Face, MeasurementSelection::FeatureAxis};
    else allowed = {MeasurementSelection::Vertex, MeasurementSelection::PointOnGeometry, MeasurementSelection::FeatureCenter,
        MeasurementSelection::Edge, MeasurementSelection::FeatureAxis, MeasurementSelection::Face, MeasurementSelection::Solid, MeasurementSelection::Part};
    const auto label = [](MeasurementSelection mode) {
        switch (mode) {
        case MeasurementSelection::Vertex: return QStringLiteral("顶点");
        case MeasurementSelection::PointOnGeometry: return QStringLiteral("曲线曲面点");
        case MeasurementSelection::FeatureCenter: return QStringLiteral("圆心");
        case MeasurementSelection::Edge: return QStringLiteral("边 / 圆弧");
        case MeasurementSelection::FeatureAxis: return QStringLiteral("轴线");
        case MeasurementSelection::Face: return QStringLiteral("面");
        case MeasurementSelection::Solid: return QStringLiteral("单实体");
        case MeasurementSelection::Part: return QStringLiteral("整个零件");
        }
        return QString();
    };
    for (int slot = 0; slot < 2; ++slot) {
        const int previous = m_selection[slot]->currentData().toInt();
        const QSignalBlocker block(m_selection[slot]);
        m_selection[slot]->clear();
        for (auto mode : allowed) addChoice(m_selection[slot], label(mode), mode);
        setChoice(m_selection[slot], previous);
        if (previous != int(selection(slot))) {
            const QSignalBlocker extentBlock(m_extent[slot]);
            m_extent[slot]->setChecked(false);
        }
        if (!m_draft[slot].empty() && m_draft[slot].front().selection != selection(slot)) m_draft[slot].clear();
    }
    updateDraftLabels();
    updatePicking();
    updateControls();
    if (m_viewer) m_viewer->setMeasurementHighlights(m_draft[0], between() ? m_draft[1] : std::vector<MeasurementEntity>{});
}

void MeasurementPanel::updatePicking()
{
    if (m_viewer)
        m_viewer->setMeasurementPicking(m_pickingEnabled && m_sceneEnabled && m_document && !m_closing && !m_cancel,
                                       selection(m_activeSlot), m_activeSlot);
}

void MeasurementPanel::updateControls()
{
    const bool available = m_document && m_sceneEnabled && !m_closing;
    const bool editable = available && !m_cancel;
    m_operation->setEnabled(editable);
    m_kind->setEnabled(editable);
    m_coordinates->setEnabled(editable);
    for (int slot = 0; slot < 2; ++slot) {
        m_slot[slot]->setEnabled(editable);
        m_selection[slot]->setEnabled(editable);
        const bool extentMode = between() && (selection(slot) == MeasurementSelection::Edge || selection(slot) == MeasurementSelection::Face);
        m_extent[slot]->setVisible(extentMode);
        QString reason = QStringLiteral("先拾取解析直线边或平面面，再启用无限拓展。");
        bool extensible = extentMode && !m_draft[slot].empty();
        if (extensible) for (auto entity : m_draft[slot]) {
            entity.extent = MeasurementExtent::Infinite;
            if (!measurementReference(entity, MeasurementCoordinates::Original, &reason)) { extensible = false; break; }
        }
        m_extent[slot]->setEnabled(editable && extensible);
        m_extent[slot]->setToolTip(extensible ? QStringLiteral("直线向两端无限延伸；平面忽略原外轮廓及孔洞。A/B 分别设置，历史结果不变。") : reason);
        if (extentMode && !extensible) m_draftLabel[slot]->setToolTip(entityDetails(m_draft[slot]) + QLatin1Char('\n') + reason);
        const bool acceptsParts = kind() == MeasurementKind::Distance || kind() == MeasurementKind::Area
            || kind() == MeasurementKind::Volume || kind() == MeasurementKind::Mass
            || kind() == MeasurementKind::CenterOfMass || kind() == MeasurementKind::VolumeCenter;
        m_capture[slot]->setEnabled(editable && acceptsParts && !m_treeSelection.empty());
    }
    m_compute->setEnabled(editable && !m_draft[0].empty() && (!between() || !m_draft[1].empty()));
    m_compute->setText(m_cancel ? QStringLiteral("计算中…") : QStringLiteral("测量"));
    m_clearAll->setEnabled(!m_closing && (!m_records.empty() || m_cancel));
    m_historyClearAll->setEnabled(m_clearAll->isEnabled());
    m_showRecords->setText(QStringLiteral("查看记录（%1）").arg(m_records.size()));
    const bool selected = !selectedRecordIds().empty();
    m_delete->setEnabled(!m_closing && selected);
    m_copySelected->setEnabled(selected);
    m_copyAll->setEnabled(!m_records.empty());
}

QString MeasurementPanel::entityText(const std::vector<MeasurementEntity>& entities) const
{
    if (entities.empty()) return QStringLiteral("尚未选择");
    QStringList names;
    for (const auto& entity : entities) {
        QString name = entity.name;
        if (name.isEmpty()) name = entity.occurrenceId;
        if (name.isEmpty()) name = QStringLiteral("对象 %1").arg(entity.occurrence + 1);
        const auto reference = measurementReferenceText(entity);
        if (!reference.isEmpty()) name += QStringLiteral("（%1）").arg(reference);
        names.push_back(name);
    }
    if (names.size() > 3) return QStringLiteral("%1 等 %2 个实例").arg(names.mid(0, 3).join(QStringLiteral("、"))).arg(names.size());
    return names.join(QStringLiteral("、"));
}

void MeasurementPanel::updateDraftLabels()
{
    for (int slot = 0; slot < 2; ++slot) {
        const QString text = entityText(m_draft[slot]);
        m_draftLabel[slot]->setText(m_draft[slot].empty() ? text : QStringLiteral("已选择 %1 项 · %2\n%3").arg(m_draft[slot].size()).arg(measurementReferenceText(m_draft[slot].front()), text.left(40)));
        m_draftLabel[slot]->setToolTip(entityDetails(m_draft[slot]));
    }
}

void MeasurementPanel::pickedEntity(const MeasurementEntity& entity, int slot)
{
    if (!m_document || !m_sceneEnabled || m_closing || m_cancel || !m_pickingEnabled || slot < 0 || slot > 1 || (slot == 1 && !between())) return;
    if (entity.shape.IsNull()) return;
    auto picked = entity;
    picked.direction = MeasurementDirection::Geometry;
    picked.extent = MeasurementExtent::Bounded;
    QString extensionError;
    const bool extentMode = between() && (picked.selection == MeasurementSelection::Edge || picked.selection == MeasurementSelection::Face);
    if (extentMode) {
        auto infinite = picked;
        infinite.extent = MeasurementExtent::Infinite;
        if (!measurementReference(infinite, MeasurementCoordinates::Original, &extensionError)) {
            const QSignalBlocker extentBlock(m_extent[slot]);
            m_extent[slot]->setChecked(false);
        } else if (m_extent[slot]->isChecked()) picked.extent = MeasurementExtent::Infinite;
    }
    m_draft[slot] = {picked};
    setStatus(extensionError.isEmpty() ? QStringLiteral("已选择对象 %1，可继续选择或点击“测量”。").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"))
                                      : extensionError + QStringLiteral("；当前对象按有界几何测量。"));
    updateDraftLabels();
    if (m_viewer) m_viewer->setMeasurementHighlights(m_draft[0], between() ? m_draft[1] : std::vector<MeasurementEntity>{});
    // Distance and angle naturally proceed from A to B after the first pick.
    if (between() && slot == 0 && m_draft[1].empty()) {
        m_activeSlot = 1;
        m_slot[1]->setChecked(true);
        updatePicking();
    }
    updateControls();
}

void MeasurementPanel::captureTree(int slot)
{
    if (!m_document || !m_viewer || !m_sceneEnabled || m_cancel || m_closing || slot < 0 || slot > 1 || (slot == 1 && !between()) || !m_capture[slot]->isEnabled()) return;
    std::set<int> visited, leaves;
    std::function<void(int)> visit = [&](int index) {
        if (index < 0 || index >= int(m_document->occurrences.size()) || !visited.insert(index).second) return;
        const auto& occurrence = m_document->occurrences[size_t(index)];
        if (occurrence.partIndex >= 0 && occurrence.partIndex < int(m_document->parts.size())) leaves.insert(index);
        for (int child : occurrence.children) visit(child);
    };
    for (int index : m_treeSelection) visit(index);
    setChoice(m_selection[slot], int(MeasurementSelection::Part));
    m_draft[slot].clear();
    for (int index : leaves) {
        auto entity = m_viewer->measurementPartEntity(index);
        if (!entity.shape.IsNull()) m_draft[slot].push_back(std::move(entity));
    }
    m_activeSlot = slot;
    m_slot[slot]->setChecked(true);
    setPickingEnabled(true);
    updateDraftLabels();
    m_viewer->setMeasurementHighlights(m_draft[0], between() ? m_draft[1] : std::vector<MeasurementEntity>{});
    setStatus(m_draft[slot].empty() ? QStringLiteral("选中项没有可测量的零件实例。")
                                          : QStringLiteral("对象 %1 已捕获 %2 个零件实例。").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B")).arg(m_draft[slot].size()));
    updateControls();
}

void MeasurementPanel::compute()
{
    if (!m_document || !m_jobs || !m_sceneEnabled || m_closing || m_cancel || m_draft[0].empty() || (between() && m_draft[1].empty())) return;
    MeasurementRequest request;
    request.kind = kind();
    request.coordinates = MeasurementCoordinates(m_coordinates->currentData().toInt());
    request.a = m_draft[0];
    if (between()) request.b = m_draft[1];
    // Fresh instance placements and session material supplements are captured at
    // calculation time; precise picked topology and parameters stay unchanged.
    for (auto* entities : {&request.a, &request.b}) for (auto& entity : *entities) {
        if (!m_viewer) continue;
        const auto fresh = m_viewer->measurementPartEntity(entity.occurrence);
        if (fresh.shape.IsNull()) {
            setStatus(QStringLiteral("选中对象已不可用，请重新选择。"));
            return;
        }
        entity.originalTransform = fresh.originalTransform;
        entity.displayTransform = fresh.displayTransform;
        entity.densityKgM3 = fresh.densityKgM3;
    }
    const quint64 generation = ++m_generation;
    const auto document = m_document;
    const auto result = std::make_shared<MeasurementResult>();
    const QPointer<MeasurementPanel> guard(this);
    setStatus(QStringLiteral("测量已排队…"));
    m_cancel = m_jobs->enqueue(m_owner,
        [request, result, document](std::atomic_bool& cancel, const ProgressCallback& progress) {
            *result = calculateMeasurement(request, cancel, progress);
        },
        [guard, generation](int percent, const QString& stage) {
            if (guard && guard->m_generation == generation && !guard->m_closing)
                guard->setStatus(stage + QStringLiteral(" %1%").arg(percent));
        },
        [guard, generation, document, request, result](const QString& error) {
            if (!guard || guard->m_generation != generation || guard->m_document != document || guard->m_closing) return;
            guard->m_cancel.reset();
            if (!error.isEmpty()) guard->setStatus(error);
            else if (!result->success) guard->setStatus(result->error.isEmpty() ? QStringLiteral("所选几何不支持此测量。") : result->error);
            else {
                MeasurementRecord record;
                record.id = guard->m_nextRecord++;
                record.request = request;
                record.result = *result;
                guard->m_records.push_back(record);
                if (guard->m_viewer) guard->m_viewer->addMeasurementRecord(record);
                guard->rebuildRecords();
                guard->m_table->scrollToBottom();
                guard->setStatus(QStringLiteral("M%1 已保留。%2").arg(record.id).arg(record.result.note));
                emit guard->recordsChanged();
            }
            // A direct recordsChanged subscriber may close/destroy its page.
            if (!guard) return;
            guard->updatePicking();
            guard->updateControls();
        });
    updatePicking();
    updateControls();
}

void MeasurementPanel::cancelPending()
{
    ++m_generation;
    if (m_cancel) m_cancel->store(true);
    m_cancel.reset();
}

void MeasurementPanel::clearAll()
{
    cancelPending();
    for (auto* dialog : findChildren<QDialog*>(QStringLiteral("measurementDetails"))) dialog->close();
    m_records.clear();
    m_draft[0].clear();
    m_draft[1].clear();
    for (auto* extent : m_extent) {
        const QSignalBlocker blocker(extent);
        extent->setChecked(false);
    }
    m_activeSlot = 0;
    if (m_slot[0]) m_slot[0]->setChecked(true);
    if (m_viewer) {
        m_viewer->clearMeasurementRecords();
        m_viewer->setMeasurementHighlights({}, {});
    }
    updateDraftLabels();
    rebuildRecords();
    setStatus(QStringLiteral("已清除所有测量、标注和选择。"));
    updatePicking();
    updateControls();
    emit recordsChanged();
}

void MeasurementPanel::prepareClose()
{
    m_closing = true;
    cancelPending();
    for (auto* dialog : findChildren<QDialog*>(QStringLiteral("measurementDetails"))) dialog->close();
    setPickingEnabled(false);
    updateControls();
}

void MeasurementPanel::cancelForSessionSwitch()
{
    if (!m_cancel) return;
    cancelPending();
    setStatus(QStringLiteral("文件已切换，待计算测量已取消。"));
    updatePicking();
    updateControls();
}

std::vector<quint64> MeasurementPanel::selectedRecordIds() const
{
    std::vector<quint64> ids;
    if (!m_table || !m_table->selectionModel()) return ids;
    for (const auto& index : m_table->selectionModel()->selectedRows()) {
        if (auto* item = m_table->item(index.row(), 0)) ids.push_back(item->data(Qt::UserRole).toULongLong());
    }
    return ids;
}

void MeasurementPanel::rebuildRecords()
{
    const auto selectionIds = selectedRecordIds();
    const QSignalBlocker blocker(m_table);
    m_rebuildingRecords = true;
    m_table->setRowCount(int(m_records.size()));
    for (int row = 0; row < int(m_records.size()); ++row) {
        const auto& record = m_records[size_t(row)];
        QString objects = QStringLiteral("A: ") + entityText(record.request.a);
        if (!record.request.b.empty()) objects += QStringLiteral("\nB: ") + entityText(record.request.b);
        QString position = measurementCoordinatesText(record.request.coordinates);
        const bool positionChanged = m_viewer && m_viewer->measurementRecordPositionChanged(record.id);
        if (positionChanged) position += QStringLiteral("\n位置已变化，标注暂隐藏");
        const std::array<QString, 6> texts {{QString(), QStringLiteral("M%1").arg(record.id),
            measurementKindText(record.request.kind), compactResult(record, m_decimals->value()), objects, position}};
        for (int column = 0; column < 6; ++column) {
            auto* item = new QTableWidgetItem(texts[size_t(column)]);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(record.id));
            item->setToolTip(texts[size_t(column)]);
            if (column == 3) item->setToolTip(detailedRecord(record, m_decimals->value(), positionChanged));
            if (column == 4) {
                QString detail = QStringLiteral("A: ") + entityDetails(record.request.a);
                if (!record.request.b.empty()) detail += QStringLiteral("\nB: ") + entityDetails(record.request.b);
                item->setToolTip(detail);
            }
            if (column == 0) {
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                item->setCheckState(record.visible ? Qt::Checked : Qt::Unchecked);
                item->setToolTip(positionChanged ? QStringLiteral("位置变化后标注暂时隐藏；数值为测量时的快照。") : QStringLiteral("显示或隐藏本条三维标注"));
            }
            m_table->setItem(row, column, item);
        }
        if (std::find(selectionIds.begin(), selectionIds.end(), record.id) != selectionIds.end())
            m_table->selectionModel()->select(m_table->model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        m_table->setRowHeight(row, record.request.kind == MeasurementKind::Point || record.request.kind == MeasurementKind::CenterOfMass || record.request.kind == MeasurementKind::VolumeCenter ? 52 : 36);
    }
    m_rebuildingRecords = false;
    updateControls();
}

void MeasurementPanel::deleteSelected()
{
    const auto ids = selectedRecordIds();
    if (ids.empty()) return;
    for (auto* dialog : findChildren<QDialog*>(QStringLiteral("measurementDetails"))) {
        const quint64 id = dialog->property("measurementRecordId").toULongLong();
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) dialog->close();
    }
    for (quint64 id : ids) if (m_viewer) m_viewer->removeMeasurementRecord(id);
    m_records.erase(std::remove_if(m_records.begin(), m_records.end(), [&](const MeasurementRecord& record) {
        return std::find(ids.begin(), ids.end(), record.id) != ids.end();
    }), m_records.end());
    rebuildRecords();
    emit recordsChanged();
}

void MeasurementPanel::copyRecords(bool selectedOnly)
{
    const auto ids = selectedRecordIds();
    QStringList lines;
    for (const auto& record : m_records) {
        if (selectedOnly && std::find(ids.begin(), ids.end(), record.id) == ids.end()) continue;
        lines.push_back(detailedRecord(record, 12, m_viewer && m_viewer->measurementRecordPositionChanged(record.id)));
        lines.push_back(QString());
    }
    if (!lines.empty()) QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void MeasurementPanel::showRecordDetails(int row)
{
    if (row < 0 || row >= int(m_records.size())) return;
    const auto& record = m_records[size_t(row)];
    const QString text = detailedRecord(record, 12, m_viewer && m_viewer->measurementRecordPositionChanged(record.id));
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("measurementDetails"));
    dialog->setProperty("measurementRecordId", QVariant::fromValue<qulonglong>(record.id));
    dialog->setWindowTitle(QStringLiteral("M%1 · %2").arg(record.id).arg(measurementKindText(record.request.kind)));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    auto* layout = new QVBoxLayout(dialog);
    auto* details = new QTextEdit(dialog);
    details->setReadOnly(true);
    details->setPlainText(text);
    details->setAccessibleName(QStringLiteral("完整测量结果快照"));
    layout->addWidget(details);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copy = buttons->addButton(QStringLiteral("复制完整结果"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, dialog, [text] { QApplication::clipboard()->setText(text); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->resize(480, 360);
    dialog->show();
}
} // namespace Stp
