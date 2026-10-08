#include "main_window.h"
#if defined(Q_OS_MACOS)
#include "mac_accessibility.h"
#endif
#include "measurement_panel.h"
#include "author_signature.h"
#include "viewer_widget.h"
#include "bom_export.h"
#include "part_export.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include "cad_job_queue.h"
#include <QDesktopServices>
#include <QUrl>
#include <QTabBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QShortcut>
#include <QPointer>
#include <QShowEvent>
#include <QToolButton>
#include <QMenu>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#endif
#include <algorithm>
#include <exception>
#include <cmath>

namespace Stp {
namespace {
QString number(double value) { return QString::number(value, 'g', 10); }
QString optionalNumber(const std::optional<double>& value) { return value ? number(*value) : QStringLiteral("未提供/不可计算"); }
QTableWidget* table(const QStringList& headers)
{
    auto widget=new QTableWidget(0,headers.size());
    widget->setHorizontalHeaderLabels(headers);
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);
    widget->setAlternatingRowColors(true);
    widget->verticalHeader()->hide();
    widget->horizontalHeader()->setStretchLastSection(true);
    widget->verticalHeader()->setDefaultSectionSize(29);
    return widget;
}
void cell(QTableWidget* table,int row,int col,const QString& text)
{
    auto item=new QTableWidgetItem(text); item->setToolTip(text);
    item->setFlags(item->flags()&~Qt::ItemIsEditable);
    table->setItem(row,col,item);
}
constexpr int FieldRole=Qt::UserRole+1;
constexpr int EditorValueRole=Qt::UserRole+2;
class MissingFieldDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setEditorData(QWidget* editor,const QModelIndex& index) const override {
        if(auto line=qobject_cast<QLineEdit*>(editor)) {
            line->setText(index.data(EditorValueRole).toString());line->selectAll();
        }else QStyledItemDelegate::setEditorData(editor,index);
    }
    void setModelData(QWidget* editor,QAbstractItemModel* model,const QModelIndex& index) const override {
        auto line=qobject_cast<QLineEdit*>(editor);
        if(line)model->setData(index,line->text(),Qt::EditRole);
        else QStyledItemDelegate::setModelData(editor,model,index);
    }
};
void editableField(QTableWidgetItem* item,int partIndex,int field,const QString& value,bool editable,const QString& source)
{
    item->setData(Qt::UserRole,partIndex);item->setData(FieldRole,field);item->setData(EditorValueRole,value);
    if(editable)item->setFlags(item->flags()|Qt::ItemIsEditable);
    item->setToolTip(editable?QStringLiteral("%1；双击填写或修改，清空可撤回。密度单位 kg/m³，必须为有限正数。仅用于本次文件会话和 Excel 计算，同一零件定义的实例共享该值。").arg(source):QStringLiteral("%1；文件提供的属性保留原值。").arg(source));
}
void enableManualEditing(QTableWidget* widget)
{
    widget->setEditTriggers(QAbstractItemView::DoubleClicked|QAbstractItemView::EditKeyPressed);
    widget->setItemDelegate(new MissingFieldDelegate(widget));
    auto editShortcut=new QShortcut(QKeySequence(Qt::Key_F2),widget);
    editShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(editShortcut,&QShortcut::activated,widget,[widget]{
        if(auto item=widget->currentItem();item&&(item->flags()&Qt::ItemIsEditable))widget->editItem(item);
    });
}
QSlider* slider(int value=0)
{
    auto item=new QSlider(Qt::Horizontal); item->setRange(0,100); item->setValue(value); item->setMinimumWidth(95); return item;
}
bool isStep(const QString& path)
{
    const auto suffix=QFileInfo(path).suffix().toLower(); return suffix=="stp" || suffix=="step";
}
} // namespace

MainWindow::MainWindow(QWidget* parent
                       , CadJobQueue* jobs, quint64 owner
                       ) : QMainWindow(parent)
{
#if defined(Q_OS_MACOS)
    installMacAccessibilityCompatibility();
#endif
    m_jobs=jobs;m_owner=owner;m_embedded=jobs!=nullptr;
    if(m_embedded){
        setWindowFlags(Qt::Widget);
        auto localMenu=new QMenuBar(nullptr);localMenu->setNativeMenuBar(false);setMenuBar(localMenu);
    }
    setObjectName("mainWindow"); setWindowTitle(QStringLiteral("STP Viewer · 离线工程查看器"));
    const QRect available=screen()?screen()->availableGeometry():QRect(0,0,1440,920);
    // QScreen reports logical pixels; do not multiply by the display's DPI scale.
    m_compactLayout=available.width()<1280||available.height()<800;
    setAcceptDrops(true);
    buildUi();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(!m_embedded)setMinimumSize(900,640);
#endif
    if(m_embedded){
        menuBar()->setNativeMenuBar(false);menuBar()->hide();
        m_openAction->setVisible(false);m_openAction->setShortcut({});
        const auto toolbar=findChild<QToolBar*>(QStringLiteral("mainToolbar"));
        if(toolbar->actions().size()>1&&toolbar->actions()[1]->isSeparator())toolbar->actions()[1]->setVisible(false);
        for(auto dock:findChildren<QDockWidget*>()){
            dock->setFeatures(QDockWidget::DockWidgetClosable|QDockWidget::DockWidgetMovable);
            dock->setMinimumWidth(dock->objectName()==QStringLiteral("detailsDock")?(m_measurementPanel?340:255):180);
        }
        for(auto action:findChildren<QAction*>())if(!action->shortcut().isEmpty()){
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);addAction(action);
        }
        m_view->setJobQueue(m_jobs,m_owner);
        connect(m_jobs,&CadJobQueue::ownerDrained,this,[this](quint64 owner){if(owner==m_owner)finishSessionClose();});
        connect(m_view,&ViewerWidget::shutdownReady,this,&MainWindow::finishSessionClose);
        return;
    }
    const QSize initial(std::min(1440,std::max(320,available.width()-32)),
                        std::min(920,std::max(240,available.height()-64)));
    resize(initial);
    move(available.center()-QPoint(width()/2,height()/2));
}

MainWindow::~MainWindow()
{
    if(m_cancelFlag) m_cancelFlag->store(true);
    if(m_worker) m_worker->wait();
}

void MainWindow::buildUi()
{
    auto toolbar=addToolBar(QStringLiteral("主工具栏")); toolbar->setObjectName("mainToolbar");
    toolbar->setMovable(false); toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_openAction=toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton),QStringLiteral("打开 STP"),this,&MainWindow::chooseFile);
    m_openAction->setShortcut(QKeySequence::Open); m_openAction->setObjectName("openStep");
    toolbar->addSeparator();
    auto fit=toolbar->addAction(QStringLiteral("适配窗口"),[this]{m_view->fitAll();}); fit->setShortcut(Qt::Key_F);
    fit->setObjectName("fitAll");
    auto views=new QComboBox; views->addItems({QStringLiteral("等轴测"),QStringLiteral("前视"),QStringLiteral("后视"),QStringLiteral("左视"),QStringLiteral("右视"),QStringLiteral("俯视"),QStringLiteral("仰视")});
    toolbar->addWidget(views); connect(views,&QComboBox::currentIndexChanged,this,[this](int index){m_view->standardView(index);});
    auto perspective=toolbar->addAction(QStringLiteral("透视")); perspective->setCheckable(true);
    connect(perspective,&QAction::toggled,this,[this](bool on){m_view->setPerspective(on);});
    toolbar->addAction(QStringLiteral("隔离选中"),[this]{
        if(m_view->selectedOccurrences().empty())return;
        m_view->isolateSelection();
        QSignalBlocker block(m_tree);
        const auto& selected=m_view->selectedOccurrences();
        for(size_t i=0;i<m_treeItems.size();++i)
            if(m_document->occurrences[i].partIndex>=0)
                m_treeItems[i]->setCheckState(0,std::find(selected.begin(),selected.end(),int(i))!=selected.end()?Qt::Checked:Qt::Unchecked);
        scheduleBomRefresh();
    });
    toolbar->addAction(QStringLiteral("显示全部"),[this]{
        if(!canChangeAppearance())return;
        finishAppearanceDrag();const auto before=m_view->appearanceState();
        m_view->showAll(); QSignalBlocker block(m_tree); for(auto item:m_treeItems) item->setCheckState(0,Qt::Checked);
        recordAppearanceChange(before);
        scheduleBomRefresh();
    });
    m_undoAppearanceAction=new QAction(QStringLiteral("还原一步"),this);m_undoAppearanceAction->setObjectName("undoAppearance");
    m_resetAppearanceAction=new QAction(QStringLiteral("全部复原"),this);m_resetAppearanceAction->setObjectName("resetAllAppearance");
    m_undoAppearanceAction->setEnabled(false);m_resetAppearanceAction->setEnabled(false);
    m_undoAppearanceAction->setToolTip(QStringLiteral("还原当前文件最近一次临时透明或隐藏操作；保留勾选、选择、相机与模型状态。"));
    m_resetAppearanceAction->setToolTip(QStringLiteral("清除当前文件的临时透明、临时隐藏和外观还原历史；保留装配树勾选与原始模型。"));
    connect(m_undoAppearanceAction,&QAction::triggered,this,&MainWindow::undoAppearance);
    connect(m_resetAppearanceAction,&QAction::triggered,this,&MainWindow::resetAppearance);
    connect(m_undoAppearanceAction,&QAction::changed,this,&MainWindow::stateChanged);
    connect(m_resetAppearanceAction,&QAction::changed,this,&MainWindow::stateChanged);
    m_appearanceRestoreButton=new QToolButton(toolbar);m_appearanceRestoreButton->setObjectName("appearanceRestoreButton");
    m_appearanceRestoreButton->setPopupMode(QToolButton::MenuButtonPopup);
    m_appearanceRestoreButton->setText(QStringLiteral("还原"));m_appearanceRestoreButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_appearanceRestoreButton->setToolTip(m_undoAppearanceAction->toolTip());m_appearanceRestoreButton->setEnabled(false);
    connect(m_appearanceRestoreButton,&QToolButton::clicked,this,&MainWindow::undoAppearance);
    auto restoreMenu=new QMenu(m_appearanceRestoreButton);restoreMenu->addAction(m_undoAppearanceAction);restoreMenu->addAction(m_resetAppearanceAction);
    m_appearanceRestoreButton->setMenu(restoreMenu);toolbar->addWidget(m_appearanceRestoreButton);
    m_pmiAction=toolbar->addAction(QStringLiteral("显示标注")); m_pmiAction->setCheckable(true); m_pmiAction->setChecked(true);
    m_pmiAction->setObjectName("togglePmi");
    connect(m_pmiAction,&QAction::toggled,this,[this](bool on){m_view->setAnnotationsVisible(on);});
    toolbar->addSeparator();
    m_bomPageAction=toolbar->addAction(QStringLiteral("BOM 与导入诊断"));
    m_bomPageAction->setObjectName("showBomPage");m_bomPageAction->setCheckable(true);
    connect(m_bomPageAction,&QAction::toggled,this,&MainWindow::setBomPageVisible);
    m_bomAction=toolbar->addAction(QStringLiteral("导出 Excel BOM"),this,&MainWindow::saveBom);
    m_partsAction=toolbar->addAction(QStringLiteral("拆分导出 STEP"),this,&MainWindow::saveParts);
    m_bomAction->setObjectName("exportBom");m_partsAction->setObjectName("exportParts");
    m_bomAction->setToolTip(QStringLiteral("导出勾选实例的 BOM，包含人工补充属性与计算质量。"));
    m_partsAction->setToolTip(QStringLiteral("仅拆分勾选的零件定义；STEP 保留原始几何与文件属性，人工输入见 Excel。"));
    m_bomAction->setEnabled(false); m_partsAction->setEnabled(false);
    auto fileMenu=menuBar()->addMenu(QStringLiteral("文件")); fileMenu->addAction(m_openAction); fileMenu->addAction(m_bomAction); fileMenu->addAction(m_partsAction);
    fileMenu->addSeparator(); fileMenu->addAction(QStringLiteral("退出"),this,&QWidget::close);
    auto viewMenu=menuBar()->addMenu(QStringLiteral("视图")); viewMenu->setObjectName("viewMenu");
    viewMenu->addAction(m_undoAppearanceAction);viewMenu->addAction(m_resetAppearanceAction);viewMenu->addSeparator();
    auto helpMenu=menuBar()->addMenu(QStringLiteral("帮助"));helpMenu->setObjectName("helpMenu");
    helpMenu->addAction(QStringLiteral("操作说明"),[this]{QMessageBox::information(this,QStringLiteral("操作说明"),QString::fromUtf8(
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        "抓手关闭：左键拖动旋转\n抓手开启：左键选中零件，按住拖动摆放，松开保留位置\n"
#else
        "左键拖动：旋转\n"
#endif
        "右键单击：零件菜单（临时透明 / 隐藏 / 还原）\n右键拖动：平移\n"
        "滚轮或触控板缩放：缩放\n双击 / F：适配窗口\n点击：选中零件；%2 点击：多选\n\n"
        "可连续打开或拖入多个 STEP，每个文件保留独立标签页。再次打开同一路径会切换到已有标签；标签关闭按钮、%1 或文件菜单可关闭当前或全部文件。关闭会直接丢弃当前会话的人工输入；关闭全部后仍可继续打开。\n\n"
        "点击顶部“BOM 与导入诊断”打开独立页面，点击“返回三维”继续查看模型。装配树勾选框决定 BOM 数量和 Excel/STEP 导出范围；鼠标高亮用于查看属性。缺失材料和密度可双击填写，人工输入仅保留在本次文件会话，进入 Excel 并参与质量计算，原 STEP 属性保持不变。右侧列出三维操作提示。\n\n"
        "右键菜单仅改变当前实例的临时外观：透明显示为 70%，隐藏不改变装配树勾选与 BOM 范围。工具栏“还原一步”保留最近 100 步，箭头菜单“全部复原”清除临时外观与历史。选择、相机、爆炸、剖切及人工属性不会随外观还原。\n\n"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        "装配爆炸区的手掌图标为“抓手”开关：展开手掌为关闭，蓝底抓握手掌为开启。开启后，每次拖动只移动鼠标命中的一个零件实例，沿当前屏幕平面摆放，不旋转零件。再次拖动可继续调整，空白处拖动不改变位置。关闭抓手恢复左键旋转，摆放位置保留。右键平移与滚轮缩放仍可使用。\n\n"
        "装配爆炸区“复位”将所有零件恢复到导入位置并把爆炸归零，保留相机、选择与抓手开关。外观“全部复原”不改变摆放位置。抓手与爆炸叠加，剖切继续使用原始范围。每个标签页独立保留抓手和摆放状态，关闭后重新打开恢复初始状态。摆放只影响显示，工程属性、BOM 与 STEP 导出仍使用原始位置。\n\n"
#endif
        "装配树勾选框控制显隐。爆炸和剖切只影响显示，导出始终使用原始几何。\n\nBOM 为 STEP 几何/装配清单，缺失材料与密度不会自动推断。PMI 的支持状态请查看标注及诊断面板。").arg(QKeySequence(Qt::CTRL|Qt::Key_W).toString(QKeySequence::NativeText),QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText).replace("+", "")));});
    helpMenu->addAction(QStringLiteral("关于与开源许可"),[this]{QMessageBox::about(this,QStringLiteral("STP Viewer 1.0.0"),QStringLiteral(
        "完全离线的 STEP 工程查看器\n\n基于 Mayo v0.10.0（BSD-2-Clause）\nQt 6.8.3 · Open CASCADE 7.9.3\nlibxlsxwriter 1.2.4\n\n依赖及许可文本随发行包提供。标注支持范围和验证状态见随包说明。"));});

    auto center=new QWidget; auto centerLayout=new QVBoxLayout(center); centerLayout->setContentsMargins(8,4,8,4); centerLayout->setSpacing(6);
    m_importFailure=new QWidget;m_importFailure->setObjectName("importFailure");
    auto failureLayout=new QHBoxLayout(m_importFailure);failureLayout->setContentsMargins(4,4,4,4);
    m_importError=new QLabel;m_importError->setObjectName("importError");m_importError->setWordWrap(true);
    auto retry=new QPushButton(QStringLiteral("重试读取"));retry->setObjectName("retryImport");
    failureLayout->addWidget(m_importError,1);failureLayout->addWidget(retry);
    connect(retry,&QPushButton::clicked,this,[this]{beginLoadDocument(m_sessionPath);});
    centerLayout->addWidget(m_importFailure);m_importFailure->hide();
    m_summary=new QLabel(QStringLiteral("打开或拖入 STEP 文件，查看装配结构、几何信息和标注")); m_summary->setObjectName("documentSummary");
    m_summary->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);m_summary->setVisible(!m_compactLayout);
    centerLayout->addWidget(m_summary);
    m_view=new ViewerWidget(this); centerLayout->addWidget(m_view,1);
    connect(m_view,&ViewerWidget::partContextMenuRequested,this,&MainWindow::showPartAppearanceMenu);
    const int availableHeight=screen()?screen()->availableGeometry().height():920;
    m_view->setMinimumSize(180,std::clamp(availableHeight/4,90,180));
    auto hint=new QLabel(QStringLiteral("左键旋转\n右键单击：零件菜单\n右键拖拽平移\n滚轮缩放\n双击适配\n还原一步 / 全部复原：临时透明与隐藏"));
    hint->setObjectName("interactionHint");hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    m_controls=new QWidget;
    auto explosionGroup=new QGroupBox(QStringLiteral("装配爆炸")); auto explosionLayout=new QVBoxLayout(explosionGroup);
    m_scope=new QComboBox; m_scope->setObjectName("explosionScope"); m_scope->addItem(QStringLiteral("总装"),-1);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_partGrabHint=hint;
    m_partGrabAction=new QAction(QStringLiteral("抓手"),this);m_partGrabAction->setObjectName("togglePartGrab");
    m_partGrabAction->setCheckable(true);m_partGrabAction->setEnabled(false);
    QIcon grabIcon;
    grabIcon.addFile(QStringLiteral(":/stp/grab/open.svg"),QSize(),QIcon::Normal,QIcon::Off);
    grabIcon.addFile(QStringLiteral(":/stp/grab/closed.svg"),QSize(),QIcon::Normal,QIcon::On);
    m_partGrabAction->setIcon(grabIcon);
    m_partGrabAction->setToolTip(QStringLiteral("抓手：展开手掌为关闭，抓握手掌为开启。开启后左键拖动单个零件，松开保留位置；再次点击恢复左键旋转。"));
    auto grabButton=new QToolButton;grabButton->setObjectName("partGrabButton");
    grabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);grabButton->setDefaultAction(m_partGrabAction);
    grabButton->setIconSize(QSize(28,28));grabButton->setAccessibleName(QStringLiteral("抓手"));
    auto scopeRow=new QHBoxLayout;scopeRow->addWidget(m_scope,1);scopeRow->addWidget(grabButton);explosionLayout->addLayout(scopeRow);
    viewMenu->addAction(m_partGrabAction);viewMenu->addSeparator();
    connect(m_partGrabAction,&QAction::toggled,this,[this](bool enabled){
        if(enabled&&m_measurementAction)m_measurementAction->setChecked(false);
        m_view->setGrabMode(enabled);updatePartGrabState();emit stateChanged();
    });
#else
    explosionLayout->addWidget(m_scope);
#endif
    auto explodeRow=new QHBoxLayout; m_explode=slider(); m_explode->setObjectName("explosionSlider"); auto explodeValue=new QLabel("0%");
    auto reset=new QPushButton(QStringLiteral("复位")); explodeRow->addWidget(m_explode,1); explodeRow->addWidget(explodeValue); explodeRow->addWidget(reset); explosionLayout->addLayout(explodeRow);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_resetPartPositionsButton=reset;reset->setObjectName("resetPartPositions");
    reset->setToolTip(QStringLiteral("还原全部零件的摆放位置并将爆炸归零，保留相机、选择和抓手开关。"));
    connect(reset,&QPushButton::clicked,this,[this,explodeValue]{
        m_view->finishPartDrag();
        // Explicitly reset even when the slider is already at zero.
        QSignalBlocker block(m_explode);m_explode->setValue(0);
        explodeValue->setText(QStringLiteral("0%"));
        m_view->resetPartPositions();
        m_view->setExplosion(0,m_scope->currentData().toInt());
    });
#else
    connect(reset,&QPushButton::clicked,m_explode,[this]{m_explode->setValue(0);});
#endif
    connect(m_explode,&QSlider::valueChanged,this,[this,explodeValue](int value){explodeValue->setText(QString::number(value)+"%");m_view->setExplosion(value,m_scope->currentData().toInt());});
    connect(m_explode,&QSlider::sliderReleased,m_view,&ViewerWidget::fitAll);
    connect(m_scope,&QComboBox::currentIndexChanged,this,[this]{m_view->setExplosion(m_explode->value(),m_scope->currentData().toInt());});

    auto displayGroup=new QGroupBox(QStringLiteral("显示与透明度")); auto displayLayout=new QVBoxLayout(displayGroup);
    auto displayModes=new QComboBox; displayModes->addItems({QStringLiteral("着色 + 边线"),QStringLiteral("着色"),QStringLiteral("线框")});
    auto displayRow=new QHBoxLayout; displayRow->addWidget(displayModes,1);
    m_meshQuality=new QComboBox; m_meshQuality->setObjectName("meshQuality");
    m_meshQuality->addItems({QStringLiteral("精度：快速"),QStringLiteral("精度：标准"),QStringLiteral("精度：精细")});
    m_meshQuality->setCurrentIndex(1);
    m_meshQuality->setToolTip(QStringLiteral("重新生成显示三角网。精细档耗时与内存较多；精确几何、BOM 和 STEP 导出保持不变。"));
    displayRow->addWidget(m_meshQuality,1); displayLayout->addLayout(displayRow);
    connect(displayModes,&QComboBox::currentIndexChanged,m_view,&ViewerWidget::setDisplayMode);
    connect(m_meshQuality,&QComboBox::currentIndexChanged,this,[this](int quality){
        if(!m_view->setMeshQuality(quality)){QSignalBlocker block(m_meshQuality);m_meshQuality->setCurrentIndex(m_view->meshQuality());}
    });
    connect(m_view,&ViewerWidget::meshQualityStarted,this,[this]{
        setBusy(true,QStringLiteral("更新显示精度…"));m_progress->setValue(0);
    });
    connect(m_view,&ViewerWidget::meshQualityProgress,this,[this](int percent){
        m_progress->setValue(percent);m_status->setText(QStringLiteral("更新显示精度 %1%…").arg(percent));
    });
    connect(m_view,&ViewerWidget::meshQualityFinished,this,[this](int quality){
        setBusy(false);diagnostic(QStringLiteral("显示精度已切换为%1；精确几何、BOM 和导出不变。").arg(m_meshQuality->itemText(quality).mid(3)));
    });
    connect(m_view,&ViewerWidget::meshQualityCancelled,this,[this](int quality){
        QSignalBlocker block(m_meshQuality);m_meshQuality->setCurrentIndex(quality);
        setBusy(false,QStringLiteral("显示精度更新已停止，保留原视图。"));diagnostic(QStringLiteral("显示精度更新已停止，保留原完整视图及选择、显隐、透明度等状态。"));
    });
    connect(m_view,&ViewerWidget::sceneReady,this,[this,displayModes]{m_view->setDisplayMode(displayModes->currentIndex());});
    auto transparencyRow=new QHBoxLayout; m_transparency=slider(); m_transparency->setMaximum(95); m_transparency->setObjectName("transparencySlider");
    m_selectedTransparency=new QCheckBox(QStringLiteral("仅选中")); m_selectedTransparency->setObjectName("selectedTransparency"); transparencyRow->addWidget(m_transparency,1); transparencyRow->addWidget(m_selectedTransparency); displayLayout->addLayout(transparencyRow);
    connect(m_transparency,&QSlider::sliderPressed,this,[this]{
        if(!canChangeAppearance())return;
        finishAppearanceDrag();m_appearanceDragStart=m_view->appearanceState();
    });
    connect(m_transparency,&QSlider::sliderReleased,this,&MainWindow::finishAppearanceDrag);
    connect(m_transparency,&QSlider::valueChanged,this,[this](int value){
        if(m_syncing||!canChangeAppearance())return;
        const auto before=m_view->appearanceState();m_view->setTransparency(value,m_selectedTransparency->isChecked());
        if(m_appearanceDragStart)updateAppearanceActions();else recordAppearanceChange(before);
    });
    connect(m_selectedTransparency,&QCheckBox::toggled,this,[this](bool selectionOnly){
        if(m_syncing||!canChangeAppearance())return;
        finishAppearanceDrag();const auto before=m_view->appearanceState();
        m_view->setTransparency(m_transparency->value(),selectionOnly);recordAppearanceChange(before);
    });

    auto sectionGroup=new QGroupBox(QStringLiteral("剖切")); auto sectionLayout=new QVBoxLayout(sectionGroup);
    auto sectionRow=new QHBoxLayout; m_sectionAxis=new QComboBox; m_sectionAxis->addItems({QStringLiteral("关闭"),QStringLiteral("X 平面"),QStringLiteral("Y 平面"),QStringLiteral("Z 平面")});
    m_sectionAxis->setObjectName("sectionAxis"); m_sectionReverse=new QCheckBox(QStringLiteral("反向")); m_sectionCap=new QCheckBox(QStringLiteral("封盖")); m_sectionCap->setChecked(true);
    sectionRow->addWidget(m_sectionAxis); sectionRow->addWidget(m_sectionReverse); sectionRow->addWidget(m_sectionCap); sectionLayout->addLayout(sectionRow);
    m_sectionPosition=slider(50); m_sectionPosition->setObjectName("sectionSlider"); sectionLayout->addWidget(m_sectionPosition);
    connect(m_sectionAxis,&QComboBox::currentIndexChanged,this,&MainWindow::updateSection); connect(m_sectionPosition,&QSlider::valueChanged,this,&MainWindow::updateSection);
    connect(m_sectionReverse,&QCheckBox::toggled,this,&MainWindow::updateSection); connect(m_sectionCap,&QCheckBox::toggled,this,&MainWindow::updateSection);
    const auto scrollable=[](QWidget* content){
        auto scroll=new QScrollArea;scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);
        scroll->setMinimumSize(0,0);
        // Ignored lets the central layout place the control strip outside its
        // parent's bottom edge, even when the scroll area has a minimum height.
        scroll->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
        // Preserve complete labels inside the content; the viewport may shrink
        // independently and exposes scrollbars instead of truncating controls.
        content->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);
        scroll->setWidget(content);return scroll;
    };
    if(m_compactLayout){
        auto controlsLayout=new QVBoxLayout(m_controls);controlsLayout->setContentsMargins(0,0,0,0);
        controlsLayout->setSizeConstraint(QLayout::SetNoConstraint);
        auto controlTabs=new QTabWidget;controlTabs->setObjectName("viewControlTabs");
        controlTabs->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
        m_controls->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
        controlTabs->addTab(scrollable(explosionGroup),QStringLiteral("装配爆炸"));
        controlTabs->addTab(scrollable(displayGroup),QStringLiteral("显示与透明度"));
        controlTabs->addTab(scrollable(sectionGroup),QStringLiteral("剖切"));
        controlsLayout->addWidget(controlTabs);
        m_controls->setMinimumHeight(80);m_controls->setMaximumHeight(std::clamp(availableHeight/4,100,180));
        centerLayout->addWidget(m_controls);
    }else{
        auto controls=new QHBoxLayout(m_controls);controls->setContentsMargins(0,0,0,0);
        controls->addWidget(explosionGroup,1);controls->addWidget(displayGroup,1);controls->addWidget(sectionGroup,1);
        auto controlScroll=scrollable(m_controls);controlScroll->setObjectName("viewControlsScroll");
        controlScroll->setMinimumHeight(96);controlScroll->setMaximumHeight(190);
        centerLayout->addWidget(controlScroll);
    }
    m_controls->setEnabled(false);
    center->setObjectName("modelPage");
    m_pages=new QStackedWidget;m_pages->setObjectName("workspacePages");
    m_pages->addWidget(center);setCentralWidget(m_pages);

    auto treeDock=new QDockWidget(QStringLiteral("装配结构"),this); treeDock->setObjectName("assemblyDock");
    auto treePanel=new QWidget; auto treeLayout=new QVBoxLayout(treePanel); treeLayout->setContentsMargins(6,6,6,6);
    m_filter=new QLineEdit; m_filter->setPlaceholderText(QStringLiteral("搜索名称 / 路径")); treeLayout->addWidget(m_filter);
    m_tree=new QTreeWidget; m_tree->setObjectName("assemblyTree"); m_tree->setHeaderLabels({QStringLiteral("零件 / 装配"),QStringLiteral("类型")});
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection); m_tree->setColumnWidth(0,210); treeLayout->addWidget(m_tree);
    treeDock->setWidget(treePanel); addDockWidget(Qt::LeftDockWidgetArea,treeDock);
    connect(m_filter,&QLineEdit::textChanged,this,&MainWindow::filterTree);
    connect(m_tree,&QTreeWidget::itemSelectionChanged,this,[this]{if(!m_syncing) selectionChanged(treeSelection(),true);});
    connect(m_tree,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* item,int col){
        if(m_syncing||col!=0||item->checkState(0)==Qt::PartiallyChecked) return;
        m_view->setOccurrenceVisible(item->data(0,Qt::UserRole).toInt(),item->checkState(0)==Qt::Checked);
        scheduleBomRefresh();
    });

    auto detailDock=new QDockWidget(QStringLiteral("属性与标注"),this); detailDock->setObjectName("detailsDock");
    auto detailTabs=new QTabWidget;
    detailTabs->setObjectName(QStringLiteral("detailsTabs"));
    m_properties=table({QStringLiteral("属性"),QStringLiteral("值"),QStringLiteral("来源")}); m_properties->setColumnWidth(0,100);m_properties->setColumnWidth(1,145);
    m_properties->setObjectName("engineeringProperties");
    enableManualEditing(m_properties);
    connect(m_properties,&QTableWidget::itemChanged,this,&MainWindow::applyManualField);
    detailTabs->addTab(m_properties,QStringLiteral("工程信息"));
    auto pmiPanel=new QWidget; auto pmiLayout=new QVBoxLayout(pmiPanel); pmiLayout->setContentsMargins(4,4,4,4);
    m_pmiSummary=new QLabel(QStringLiteral("尚未读取文件")); m_pmiSummary->setWordWrap(true); pmiLayout->addWidget(m_pmiSummary);
    m_pmiTable=table({QStringLiteral("标注"),QStringLiteral("状态"),QStringLiteral("内容 / 原因")}); m_pmiTable->setObjectName("pmiTable"); m_pmiTable->setWordWrap(true); pmiLayout->addWidget(m_pmiTable);
    detailTabs->addTab(pmiPanel,QStringLiteral("PMI 标注"));
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_measurementPanel=new MeasurementPanel(m_view,m_jobs,m_owner,detailTabs);
    m_measurementPanel->setObjectName(QStringLiteral("measurementPanel"));
    const int measurementTab=detailTabs->addTab(m_measurementPanel,QStringLiteral("测量"));
    const int recordsTab=detailTabs->addTab(m_measurementPanel->recordsPage(),QStringLiteral("测量记录"));
    detailDock->setMinimumWidth(340);
    connect(m_measurementPanel,&MeasurementPanel::showRecordsRequested,this,[detailTabs,recordsTab]{detailTabs->setCurrentIndex(recordsTab);});
    m_measurementAction=toolbar->addAction(QStringLiteral("测量"));
    m_measurementAction->setObjectName(QStringLiteral("toggleMeasurement"));
    m_measurementAction->setCheckable(true);m_measurementAction->setEnabled(false);
    m_measurementAction->setToolTip(QStringLiteral("测量点、线、面、体；半径与直径可独立测量，多条结果与标注持续保留。"));
    viewMenu->addAction(m_measurementAction);
    connect(m_measurementAction,&QAction::toggled,this,[this,detailTabs,measurementTab](bool enabled){
        if(enabled){
            setBomPageVisible(false);
            if(m_bomPageAction->isChecked()){QSignalBlocker block(m_bomPageAction);m_bomPageAction->setChecked(false);}
            if(m_partGrabAction)m_partGrabAction->setChecked(false);
            m_view->setGrabMode(false);
            if(auto dock=findChild<QDockWidget*>(QStringLiteral("detailsDock")))dock->show();
            detailTabs->setCurrentIndex(measurementTab);
        }
        m_measurementPanel->setPickingEnabled(enabled);
        updatePartGrabState();emit stateChanged();
    });
    connect(m_measurementPanel,&MeasurementPanel::pickingEnabledChanged,this,[this](bool enabled){
        QSignalBlocker block(m_measurementAction);m_measurementAction->setChecked(enabled);
        if(enabled){if(m_partGrabAction)m_partGrabAction->setChecked(false);m_view->setGrabMode(false);}
    });
    connect(detailTabs,&QTabWidget::currentChanged,this,[this,measurementTab](int index){
        if(index!=measurementTab&&m_measurementAction->isChecked())m_measurementAction->setChecked(false);
    });
#endif
    auto detailPanel=new QWidget;auto detailLayout=new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(6,6,6,6);detailLayout->addWidget(detailTabs,1);
    auto hintGroup=new QGroupBox(QStringLiteral("三维操作"));auto hintLayout=new QVBoxLayout(hintGroup);
    hintGroup->setObjectName(QStringLiteral("viewOperationHints"));
    hintLayout->addWidget(hint);detailLayout->addWidget(hintGroup);
    detailDock->setWidget(detailPanel);
    addDockWidget(Qt::RightDockWidgetArea,detailDock);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    connect(detailTabs,&QTabWidget::currentChanged,this,[this,detailTabs,hintGroup,measurementTab,recordsTab](int index){
        hintGroup->setVisible(index!=measurementTab&&index!=recordsTab);
        if(index==measurementTab&&m_measurementAction->isEnabled())m_measurementPanel->setPickingEnabled(true);
    });
    connect(detailDock,&QDockWidget::visibilityChanged,this,[this,detailDock](bool visible){
        if(!visible&&detailDock->isHidden()&&m_measurementAction)m_measurementAction->setChecked(false);
    });
#endif
    connect(m_pmiTable,&QTableWidget::cellDoubleClicked,this,[this](int row,int){
        if(row<0||size_t(row)>=m_pmi.records.size()) return; const auto& record=m_pmi.records[size_t(row)];
        auto dialog=new QMessageBox(QMessageBox::Information,record.name,record.semanticText+"\n\n"+record.reason,QMessageBox::Ok,this);
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setDetailedText(record.semanticDetails); dialog->open();
    });

    auto bottomTabs=new QTabWidget;
    m_bom=table({QStringLiteral("名称"),QStringLiteral("编号"),QStringLiteral("数量"),QStringLiteral("材料"),QStringLiteral("密度 kg/m³"),QStringLiteral("X mm"),QStringLiteral("Y mm"),QStringLiteral("Z mm"),QStringLiteral("体积 mm³"),QStringLiteral("面积 mm²"),QStringLiteral("质量 kg")});
    m_bom->setObjectName("bomTable");m_bom->setSelectionMode(QAbstractItemView::ExtendedSelection); bottomTabs->addTab(m_bom,QStringLiteral("零件汇总"));
    m_diagnostics=new QTextEdit; m_diagnostics->setReadOnly(true); m_diagnostics->setObjectName("diagnostics"); bottomTabs->addTab(m_diagnostics,QStringLiteral("诊断与日志"));
    enableManualEditing(m_bom);
    connect(m_bom,&QTableWidget::itemChanged,this,&MainWindow::applyManualField);
    bottomTabs->setObjectName("bomPageTabs");
    auto bomPage=new QWidget;bomPage->setObjectName("bomPage");auto bomPageLayout=new QVBoxLayout(bomPage);
    bomPageLayout->setContentsMargins(12,12,12,12);
    auto pageHeader=new QHBoxLayout;auto back=new QPushButton(QStringLiteral("返回三维"));back->setObjectName("returnToModel");
    connect(back,&QPushButton::clicked,this,[this]{m_bomPageAction->setChecked(false);});
    auto pageTitle=new QLabel(QStringLiteral("BOM 与导入诊断"));pageTitle->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    pageHeader->addWidget(back);pageHeader->addWidget(pageTitle);pageHeader->addStretch();
    m_bomScopeSummary=new QLabel;m_bomScopeSummary->setObjectName("bomScopeSummary");m_bomScopeSummary->setWordWrap(true);
    m_bomScopeSummary->setText(QStringLiteral("清单和导出范围由左侧零件 / 装配勾选框决定；导入诊断记录整份文件的读取结果。"));
    bomPageLayout->addLayout(pageHeader);bomPageLayout->addWidget(m_bomScopeSummary);bomPageLayout->addWidget(bottomTabs,1);m_pages->addWidget(bomPage);
    for(auto dock:{treeDock,detailDock})viewMenu->addAction(dock->toggleViewAction());
    viewMenu->addAction(m_bomPageAction);viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("专注三维（隐藏面板）"),[this,treeDock,detailDock]{
        m_bomPageAction->setChecked(false);for(auto dock:{treeDock,detailDock})dock->hide();
    });
    viewMenu->addAction(QStringLiteral("恢复完整布局"),[this,treeDock,detailDock]{
        m_bomPageAction->setChecked(false);
        for(auto dock:{treeDock,detailDock}){dock->setFloating(false);removeDockWidget(dock);}
        addDockWidget(Qt::LeftDockWidgetArea,treeDock);addDockWidget(Qt::RightDockWidgetArea,detailDock);
        for(auto dock:{treeDock,detailDock})dock->show();
        resizeDocks({treeDock,detailDock},{255,m_measurementPanel?360:320},Qt::Horizontal);
    });
    if(m_compactLayout){
        treeDock->hide();
        resizeDocks({detailDock},{m_measurementPanel?360:std::min(300,(screen()?screen()->availableGeometry().width():900)/3)},Qt::Horizontal);
    }
    connect(m_bom,&QTableWidget::itemSelectionChanged,this,[this]{
        if(m_syncing||!m_document) return; std::vector<int> selected;
        const auto included=checkedLeafOccurrences();
        for(const auto& row:m_bom->selectionModel()->selectedRows()) {
            if(row.row()<0||size_t(row.row())>=m_bomPartIndices.size())continue;
            for(int index:included)if(m_document->occurrences[size_t(index)].partIndex==m_bomPartIndices[size_t(row.row())])selected.push_back(index);
        }
        selectionChanged(selected,true);
    });
    connect(m_view,&ViewerWidget::selectionChanged,this,[this](const std::vector<int>& indices){selectionChanged(indices,false);});
    connect(m_view,&ViewerWidget::message,this,&MainWindow::diagnostic);
    connect(m_view,&ViewerWidget::pmiPresentationFailed,this,[this](const QString& id,const QString& error){
        for(size_t i=0;i<m_pmi.records.size();++i){auto& record=m_pmi.records[i];if(record.id!=id)continue;
            record.status=PmiStatus::Failed;record.reason+=QStringLiteral(" 三维显示失败：")+error;
            cell(m_pmiTable,int(i),1,pmiStatusText(record.status));
            cell(m_pmiTable,int(i),2,record.semanticText+"\n"+record.reason);
            if(m_document)m_document->diagnostics.append(QStringLiteral("PMI %1 显示失败：%2").arg(record.name,error));
        }
        const auto failed=std::count_if(m_pmi.records.begin(),m_pmi.records.end(),[](const auto& record){return record.status==PmiStatus::Failed;});
        m_pmiSummary->setText(QStringLiteral("识别 %1 项，显示失败 %2 项。\n请查看逐项状态；源标注完整性未确认。").arg(m_pmi.records.size()).arg(failed));
    });
    connect(m_view,&ViewerWidget::sceneProgress,this,[this](int percent){
        m_progress->setValue(percent);m_status->setText(QStringLiteral("生成三维显示 %1%…").arg(percent));
    });
    connect(m_view,&ViewerWidget::sceneBuildCancelled,this,[this]{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        m_partGrabSceneReady=false;
#endif
        setBusy(false,QStringLiteral("已停止生成三维显示；可导出已读取的 BOM 或重新打开文件。"));
        diagnostic(QStringLiteral("用户取消了三维显示生成，已清除部分显示，源文件未修改。"));
        if(m_embedded&&!m_closePending){m_importError->setText(QStringLiteral("三维显示生成已取消；可导出已读取的 BOM，或点击重试读取恢复模型。"));m_importFailure->show();}
    });
    connect(m_view,&ViewerWidget::sceneReady,this,[this]{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        m_partGrabSceneReady=true;
#endif
        setBusy(false); diagnostic(QStringLiteral("模型显示完成，用时 %1 秒。").arg(number(m_loadTimer.elapsed()/1000.0)));
        const auto loadMs=m_loadTimer.elapsed();
        if(!m_smokeDirectory.isEmpty())QTimer::singleShot(300,this,[this,loadMs]{startSmokeCapture(loadMs);});
    });
    m_status=new QLabel(QStringLiteral("完全离线 · 文件不上传"));m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);statusBar()->addWidget(m_status,1);
    m_progress=new QProgressBar;m_progress->setFixedWidth(180);m_progress->hide();statusBar()->addPermanentWidget(m_progress);
    m_cancel=new QPushButton(QStringLiteral("取消"));m_cancel->hide();statusBar()->addPermanentWidget(m_cancel);
    addAuthorSignature(statusBar());
    connect(m_cancel,&QPushButton::clicked,this,[this]{
        if(m_taskPending&&m_cancelFlag){m_jobs->cancelOwner(m_owner);m_status->setText(QStringLiteral("已请求取消，等待当前 CAD 操作安全结束…"));m_cancel->setEnabled(false);return;}
        if(m_worker&&m_cancelFlag){m_cancelFlag->store(true);m_status->setText(QStringLiteral("已请求取消，等待当前 CAD 操作安全结束…"));m_cancel->setEnabled(false);}
        else m_view->cancelSceneBuild();
    });
    if(!m_compactLayout){
        resizeDocks({treeDock,detailDock},{255,m_measurementPanel?360:320},Qt::Horizontal);
    }
    setStyleSheet(QString::fromUtf8(
        "QMainWindow{background:#eef2f7;} QToolBar{spacing:7px;padding:7px;background:#fff;border-bottom:1px solid #dae1ec;}"
        "QDockWidget::title{background:#e5ebf4;padding:7px;color:#283b52;font-weight:600;}"
        "QTreeWidget,QTableWidget,QTextEdit{background:#fff;alternate-background-color:#f6f8fb;gridline-color:#e8edf4;border:1px solid #dae1ec;}"
        "QHeaderView::section{background:#f0f4f9;padding:6px;border:none;border-bottom:1px solid #d5dfea;}"
        "QGroupBox{font-weight:600;border:1px solid #d4deeb;border-radius:5px;margin-top:9px;padding-top:8px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 4px;}"
        "QPushButton{padding:5px 11px;} QComboBox,QLineEdit{padding:5px;}"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        "QToolButton#partGrabButton{padding:3px 7px;border:1px solid #c5d2e3;border-radius:4px;background:#f0f4f9;color:#233a56;}"
        "QToolButton#partGrabButton:checked{background:#2f6cb0;border-color:#2f6cb0;color:#fff;}"
        "QToolButton#partGrabButton:disabled{color:#929dab;border-color:#dae1ec;}"
#endif
        "QLabel#documentSummary{font-size:14px;color:#233a56;padding:5px;} QLabel#interactionHint{color:#60758b;padding:2px;}"
    ));
    // Measure after the style has supplied the real combo/button/group margins.
    // Reserve horizontal-scrollbar space as well, so narrowing the window never
    // hides the lower row of controls. All measurements are logical pixels.
    m_controls->ensurePolished();
    for(auto group:{explosionGroup,displayGroup,sectionGroup}){
        group->ensurePolished();group->layout()->activate();
    }
    m_controls->layout()->activate();
    const int scrollbarHeight=style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    if(auto scroll=findChild<QScrollArea*>(QStringLiteral("viewControlsScroll"))){
        const int height=m_controls->minimumSizeHint().height()+scrollbarHeight+2*scroll->frameWidth();
        scroll->setFixedHeight(height);
    }else if(auto tabs=findChild<QTabWidget*>(QStringLiteral("viewControlTabs"))){
        tabs->ensurePolished();
        const int contentHeight=std::max({explosionGroup->minimumSizeHint().height(),
            displayGroup->minimumSizeHint().height(),sectionGroup->minimumSizeHint().height()});
        m_controls->setFixedHeight(contentHeight+tabs->tabBar()->sizeHint().height()+scrollbarHeight+4);
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    updatePartGrabState();
#endif
}

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
void MainWindow::updatePartGrabState()
{
    if(!m_partGrabAction||!m_view)return;
    const bool enabled=m_partGrabSceneReady&&m_document&&!m_busy&&!m_closePending;
    m_partGrabAction->setEnabled(enabled);
    if(m_resetPartPositionsButton)m_resetPartPositionsButton->setEnabled(enabled);
    const bool grabbing=m_view->grabMode();
    if(m_partGrabAction->isChecked()!=grabbing)m_partGrabAction->setChecked(grabbing);
    if(m_partGrabHint)m_partGrabHint->setText((grabbing?
        QStringLiteral("抓手开启：左键拖动零件\n松开保留位置；关闭后左键旋转\n"):
        QStringLiteral("抓手关闭：左键旋转\n开启抓手：左键拖动零件并摆放\n"))+
        QStringLiteral("右键单击：零件菜单\n右键拖拽平移 · 滚轮缩放\n双击 / F：适配窗口\n装配爆炸“复位”：恢复零件位置\n外观还原：临时透明与隐藏"));
}
#endif

bool MainWindow::canChangeAppearance() const
{
    return m_view&&m_document&&!m_busy&&!m_closePending;
}

void MainWindow::updateAppearanceActions()
{
    if(!m_undoAppearanceAction||!m_resetAppearanceAction||!m_view)return;
    const auto current=m_view->appearanceState();
    const bool pending=m_appearanceDragStart&&*m_appearanceDragStart!=current;
    const bool hasHistory=!m_appearanceHistory.empty()||pending;
    const bool enabled=canChangeAppearance();
    m_undoAppearanceAction->setEnabled(enabled&&hasHistory);
    m_resetAppearanceAction->setEnabled(enabled&&(hasHistory||current!=TemporaryAppearance{}));
    // Keep the menu arrow usable when only "全部复原" is available.
    m_appearanceRestoreButton->setEnabled(enabled&&(hasHistory||current!=TemporaryAppearance{}));
}

void MainWindow::refreshAppearanceUi()
{
    if(m_document){
        QSignalBlocker block(m_tree);
        const auto hiddenIcon=style()->standardIcon(QStyle::SP_TitleBarShadeButton);
        for(size_t i=0;i<m_treeItems.size()&&i<m_document->occurrences.size();++i){
            const auto& occurrence=m_document->occurrences[i];
            if(occurrence.partIndex<0)continue;
            const bool hidden=m_view->isOccurrenceTemporarilyHidden(int(i));
            auto item=m_treeItems[i];item->setIcon(0,hidden?hiddenIcon:QIcon());
            item->setToolTip(0,occurrence.path+(hidden?QStringLiteral(
                "\n仅三维隐藏，仍参与 BOM；实际统计与导出以勾选状态为准。\n使用“还原一步”或“全部复原”恢复临时显示。"):QString()));
        }
    }
    updateAppearanceActions();
}

void MainWindow::recordAppearanceChange(const TemporaryAppearance& before)
{
    if(before!=m_view->appearanceState()){
        constexpr size_t maxHistory=100;
        if(m_appearanceHistory.size()==maxHistory)m_appearanceHistory.erase(m_appearanceHistory.begin());
        m_appearanceHistory.push_back(before);
    }
    refreshAppearanceUi();
}

void MainWindow::finishAppearanceDrag()
{
    if(!m_appearanceDragStart)return;
    const auto before=std::move(*m_appearanceDragStart);m_appearanceDragStart.reset();
    recordAppearanceChange(before);
}

void MainWindow::applyAppearanceSnapshot(const TemporaryAppearance& state)
{
    QSignalBlocker sliderBlock(m_transparency),selectedBlock(m_selectedTransparency);
    m_transparency->setValue(state.transparency);m_selectedTransparency->setChecked(state.selectionOnly);
    m_view->applyAppearanceState(state);
    refreshAppearanceUi();
}

void MainWindow::undoAppearance()
{
    if(!canChangeAppearance())return;
    finishAppearanceDrag();
    if(m_appearanceHistory.empty())return;
    const auto state=std::move(m_appearanceHistory.back());m_appearanceHistory.pop_back();
    applyAppearanceSnapshot(state);
}

void MainWindow::resetAppearance()
{
    if(!canChangeAppearance())return;
    m_appearanceDragStart.reset();m_appearanceHistory.clear();
    applyAppearanceSnapshot({});
}

void MainWindow::dismissAppearanceMenu()
{
    ++m_appearanceMenuGeneration;
    if(!m_partAppearanceMenu)return;
    m_partAppearanceMenu->setObjectName(QStringLiteral("closedPartAppearanceMenu"));
    m_partAppearanceMenu->close();m_partAppearanceMenu->deleteLater();m_partAppearanceMenu.clear();
}

void MainWindow::clearAppearanceForNewDocument()
{
    dismissAppearanceMenu();m_appearanceDragStart.reset();m_appearanceHistory.clear();
    applyAppearanceSnapshot({});
}

void MainWindow::showPartAppearanceMenu(int occurrence,const QPoint& globalPosition)
{
    if(!canChangeAppearance())return;
    finishAppearanceDrag();dismissAppearanceMenu();
    auto menu=new QMenu(this);menu->setObjectName(QStringLiteral("partAppearanceMenu"));m_partAppearanceMenu=menu;
    auto transparent=menu->addAction(QStringLiteral("透明显示（70%）"));transparent->setObjectName(QStringLiteral("makePartTransparent70"));
    auto hide=menu->addAction(QStringLiteral("隐藏零件"));hide->setObjectName(QStringLiteral("hidePart"));
    const bool partAvailable=occurrence>=0&&size_t(occurrence)<m_document->occurrences.size()
        &&m_document->occurrences[size_t(occurrence)].partIndex>=0&&m_view->isOccurrenceDisplayed(occurrence);
    transparent->setEnabled(partAvailable);hide->setEnabled(partAvailable);
    menu->addSeparator();
    auto undo=menu->addAction(QStringLiteral("还原一步"));undo->setObjectName(QStringLiteral("undoAppearance"));
    auto reset=menu->addAction(QStringLiteral("全部复原"));reset->setObjectName(QStringLiteral("resetAllAppearance"));
    const QPointer<MainWindow> self(this);const QPointer<QMenu> menuGuard(menu);
    const std::weak_ptr<CadDocument> document=m_document;
    const quint64 generation=m_appearanceMenuGeneration;
    const auto validMenu=[self,menuGuard,document,generation]{
        return self&&menuGuard&&self->m_partAppearanceMenu==menuGuard
            &&self->m_appearanceMenuGeneration==generation&&document.lock()==self->m_document
            &&self->canChangeAppearance();
    };
    const auto validPart=[self,validMenu,occurrence]{
        return validMenu()&&occurrence>=0
            &&size_t(occurrence)<self->m_document->occurrences.size()
            &&self->m_document->occurrences[size_t(occurrence)].partIndex>=0
            &&self->m_view->isOccurrenceDisplayed(occurrence);
    };
    // Popup actions are session-bound proxies. The persistent toolbar/native
    // actions cannot guard against a dismissed menu's delayed activation.
    undo->setEnabled(m_undoAppearanceAction->isEnabled());reset->setEnabled(m_resetAppearanceAction->isEnabled());
    connect(m_undoAppearanceAction,&QAction::changed,undo,[self,validMenu,undo]{
        undo->setEnabled(validMenu()&&self->m_undoAppearanceAction->isEnabled());
    });
    connect(m_resetAppearanceAction,&QAction::changed,reset,[self,validMenu,reset]{
        reset->setEnabled(validMenu()&&self->m_resetAppearanceAction->isEnabled());
    });
    connect(undo,&QAction::triggered,this,[self,validMenu]{if(validMenu())self->undoAppearance();});
    connect(reset,&QAction::triggered,this,[self,validMenu]{if(validMenu())self->resetAppearance();});
    connect(transparent,&QAction::triggered,this,[self,validPart,occurrence]{
        if(!validPart())return;
        self->finishAppearanceDrag();const auto before=self->m_view->appearanceState();
        self->m_view->setOccurrenceTemporaryTransparency(occurrence,70);self->recordAppearanceChange(before);
    });
    connect(hide,&QAction::triggered,this,[self,validPart,occurrence]{
        if(!validPart())return;
        self->finishAppearanceDrag();const auto before=self->m_view->appearanceState();
        self->m_view->setOccurrenceTemporarilyHidden(occurrence,true);self->recordAppearanceChange(before);
    });
    connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);
    menu->popup(globalPosition);
}

void MainWindow::setBomPageVisible(bool visible)
{
    if(!m_pages || (m_pages->currentIndex()==1)==visible)return;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_view->finishPartDrag();
#endif
    commitManualEditor();
    auto assembly=findChild<QDockWidget*>(QStringLiteral("assemblyDock"));
    auto details=findChild<QDockWidget*>(QStringLiteral("detailsDock"));
    if(visible){
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if(m_measurementAction)m_measurementAction->setChecked(false);
#endif
        refreshBom();
        m_assemblyDockWasVisible=!assembly->isHidden();m_detailsDockWasVisible=!details->isHidden();
        for(auto dock:{assembly,details}){dock->hide();dock->toggleViewAction()->setEnabled(false);}
        m_pages->setCurrentIndex(1);
    }else{
        m_pages->setCurrentIndex(0);
        assembly->setVisible(m_assemblyDockWasVisible);details->setVisible(m_detailsDockWasVisible);
        for(auto dock:{assembly,details})dock->toggleViewAction()->setEnabled(true);
    }
}

std::vector<int> MainWindow::checkedLeafOccurrences() const
{
    std::vector<int> included;
    if(!m_document)return included;
    for(size_t index=0;index<m_treeItems.size();++index)
        if(m_document->occurrences[index].partIndex>=0&&m_treeItems[index]->checkState(0)==Qt::Checked)
            included.push_back(int(index));
    return included;
}

std::shared_ptr<CadDocument> MainWindow::processingDocument() const
{
    if(!m_document)return {};
    auto result=std::make_shared<CadDocument>(scopedBomDocument(*m_document,checkedLeafOccurrences()));
    result->diagnostics.append(QStringLiteral("本次清单与导出仅包含左侧勾选的叶级零件实例，数量按勾选实例累计；未勾选项不计入。导入诊断仍反映整份文件的读取情况。"));
    return result;
}

void MainWindow::scheduleBomRefresh()
{
    if(m_bomRefreshPending||m_closePending)return;
    m_bomRefreshPending=true;
    QTimer::singleShot(0,this,[this]{m_bomRefreshPending=false;refreshBom();});
}

void MainWindow::refreshBom()
{
    if(!m_document||m_closePending)return;
    const bool wasSyncing=m_syncing;m_syncing=true;
    QSignalBlocker block(m_bom);
    std::vector<int> quantities(m_document->parts.size(),0);
    const auto included=checkedLeafOccurrences();
    for(int index:included)++quantities[size_t(m_document->occurrences[size_t(index)].partIndex)];
    m_bomPartIndices.clear();
    for(size_t part=0;part<quantities.size();++part)if(quantities[part]>0)m_bomPartIndices.push_back(int(part));
    m_bom->setRowCount(int(m_bomPartIndices.size()));
    m_bom->clearSelection();
    for(size_t row=0;row<m_bomPartIndices.size();++row){
        const int partIndex=m_bomPartIndices[row];const auto& part=m_document->parts[size_t(partIndex)];
        const auto material=effectiveMaterial(part);const auto density=effectiveDensity(part);
        const QStringList values{part.name,part.number,QString::number(quantities[size_t(partIndex)]),
            material.isEmpty()?QStringLiteral("双击填写"):material,density?number(*density):QStringLiteral("双击填写"),
            part.bboxValid?number(part.bboxMm[0]):QStringLiteral("不可计算"),part.bboxValid?number(part.bboxMm[1]):QStringLiteral("不可计算"),
            part.bboxValid?number(part.bboxMm[2]):QStringLiteral("不可计算"),optionalNumber(part.volumeMm3),optionalNumber(part.areaMm2),optionalNumber(effectiveMass(part))};
        for(int col=0;col<values.size();++col){cell(m_bom,int(row),col,values[col]);m_bom->item(int(row),col)->setData(Qt::UserRole,partIndex);}
        editableField(m_bom->item(int(row),3),partIndex,1,material,materialEditable(part),part.manualMaterial?QStringLiteral("人工输入"):materialEditable(part)?QStringLiteral("文件未提供"):QStringLiteral("文件"));
        editableField(m_bom->item(int(row),4),partIndex,2,density?number(*density):QString(),densityEditable(part),part.manualDensityKgM3?QStringLiteral("人工输入"):densityEditable(part)?QStringLiteral("文件未提供"):QStringLiteral("文件/单位换算"));
    }
    m_bom->resizeColumnsToContents();m_bom->setColumnWidth(0,180);
    m_bomScopeSummary->setText(QStringLiteral("已勾选 %1 种零件 / %2 个实例 · 双击缺失材料、密度填写（kg/m³；仅本次文件会话，参与质量计算）\n清单和导出仅处理勾选项；导入诊断记录整份文件的读取结果。")
        .arg(m_bomPartIndices.size()).arg(included.size()));
    const auto selectedParts=m_currentSelection.empty()?std::vector<int>{}:selectedPartIndices(*m_document,m_currentSelection);
    for(size_t row=0;row<m_bomPartIndices.size();++row)
        if(std::find(selectedParts.begin(),selectedParts.end(),m_bomPartIndices[row])!=selectedParts.end())
            m_bom->selectionModel()->select(m_bom->model()->index(int(row),0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
    m_bomAction->setEnabled(!m_busy&&!included.empty());m_partsAction->setEnabled(!m_busy&&!included.empty());
    m_syncing=wasSyncing;
}

void MainWindow::applyManualField(QTableWidgetItem* item)
{
    if(m_syncing||m_busy||!m_document||!item||!(item->flags()&Qt::ItemIsEditable))return;
    const int field=item->data(FieldRole).toInt();const int partIndex=item->data(Qt::UserRole).toInt();
    if((field!=1&&field!=2)||partIndex<0||size_t(partIndex)>=m_document->parts.size())return;
    auto& part=m_document->parts[size_t(partIndex)];QString error;
    const bool accepted=field==1?setManualMaterial(part,item->text()):setManualDensity(part,item->text(),&error);
    if(accepted)m_status->setText(QStringLiteral("人工属性已更新；同一定义的实例共享输入，质量按有效体积和密度计算。"));
    else m_status->setText(error.isEmpty()?QStringLiteral("文件提供的属性保留原值。"):error);
    // Wait for the delegate to finish committing before replacing table items.
    QTimer::singleShot(0,this,[this]{refreshBom();showProperties(m_currentSelection);});
}

void MainWindow::commitManualEditor()
{
    // Tab switches can happen before the slider's normal release signal.
    finishAppearanceDrag();dismissAppearanceMenu();
    auto focused=qobject_cast<QLineEdit*>(QApplication::focusWidget());
    for(auto table:{m_properties,m_bom}){
        auto editor=focused&&table->isAncestorOf(focused)?focused:nullptr;
        // A programmatic tab change may already have moved focus or hidden the
        // ancestor page when currentChanged is emitted. The delegate editor is
        // still a table child until its queued close event is processed.
        if(!editor)for(auto candidate:table->findChildren<QLineEdit*>())if(!candidate->isHidden()){editor=candidate;break;}
        if(!editor)continue;
        auto delegate=table->itemDelegate();
        delegate->commitData(editor);
        delegate->closeEditor(editor,QAbstractItemDelegate::NoHint);
        return;
    }
}

QMenu* MainWindow::documentViewMenu() const { return menuBar()->findChild<QMenu*>(QStringLiteral("viewMenu")); }
QMenu* MainWindow::documentHelpMenu() const { return menuBar()->findChild<QMenu*>(QStringLiteral("helpMenu")); }

void MainWindow::cancelPendingMeasurementForSessionSwitch()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->cancelForSessionSwitch();
#endif
}

void MainWindow::requestSessionClose()
{
    if(m_closePending)return;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->prepareClose();
    m_view->finishPartDrag();
#endif
    m_closePending=true;hide();setEnabled(false);
    dismissAppearanceMenu();m_appearanceDragStart.reset();m_appearanceHistory.clear();updateAppearanceActions();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    updatePartGrabState();
#endif
    if(m_cancelFlag)m_cancelFlag->store(true);
    m_view->requestSessionClose();
    if(m_jobs)m_jobs->cancelOwner(m_owner);
    finishSessionClose();
}

void MainWindow::finishSessionClose()
{
    if(!m_closePending||m_closeReady||m_taskPending||(m_jobs&&m_jobs->hasJobs(m_owner))||m_view->hasPendingWork())return;
    m_closeReady=true;m_busy=false;emit closeReady();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if(!m_embedded||m_layoutInitialized)return;
    m_layoutInitialized=true;
    // Embedded pages obtain their real width only after QTabWidget lays out
    // the stack. Earlier resizeDocks calls can otherwise collapse both sides.
    QTimer::singleShot(0,this,[this]{
        if(m_closePending)return;
        auto assembly=findChild<QDockWidget*>(QStringLiteral("assemblyDock"));
        auto details=findChild<QDockWidget*>(QStringLiteral("detailsDock"));
        resizeDocks({assembly,details},{255,m_measurementPanel?360:320},Qt::Horizontal);
    });
}

void MainWindow::setBusy(bool busy,const QString& label)
{
    if(m_closePending)return;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(busy)m_view->finishPartDrag();
#endif
    if(busy){finishAppearanceDrag();dismissAppearanceMenu();}
    m_busy=busy; m_openAction->setEnabled(!busy);m_bomAction->setEnabled(!busy&&bool(m_document));m_partsAction->setEnabled(!busy&&bool(m_document));
    const bool hasIncluded=m_document&&!checkedLeafOccurrences().empty();
    m_bomAction->setEnabled(!busy&&hasIncluded);m_partsAction->setEnabled(!busy&&hasIncluded);
    m_properties->setEnabled(!busy);
    m_controls->setEnabled(!busy&&bool(m_document));m_tree->setEnabled(!busy);m_bom->setEnabled(!busy);
    if(auto toolbar=findChild<QToolBar*>(QStringLiteral("mainToolbar")))toolbar->setEnabled(!busy);
    m_progress->setVisible(busy);m_cancel->setVisible(busy);m_cancel->setEnabled(true);
    m_status->setText(label.isEmpty()?QStringLiteral("完全离线 · 文件不上传"):label);
    updateAppearanceActions();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    updatePartGrabState();
    if(m_measurementPanel)m_measurementPanel->setEnabledForScene(!busy&&m_partGrabSceneReady&&bool(m_document));
    if(m_measurementAction)m_measurementAction->setEnabled(!busy&&m_partGrabSceneReady&&bool(m_document));
#endif
    emit stateChanged();
}

void MainWindow::runTask(const QString& title,std::function<void(std::atomic_bool&,const ProgressCallback&)> work,std::function<void(const QString&)> done)
{
    if(m_jobs){
        if(m_taskPending||m_closePending)return;
        m_taskPending=true;setBusy(true,title);m_progress->setValue(0);
        QPointer<MainWindow> self(this);
        m_cancelFlag=m_jobs->enqueue(m_owner,std::move(work),[self](int percent,const QString& stage){
            if(!self||self->m_closePending)return;
            self->m_progress->setValue(percent);self->m_status->setText(stage);
        },[self,done=std::move(done)](const QString& error){
            if(!self)return;
            self->m_taskPending=false;self->m_cancelFlag.reset();
            if(self->m_closePending){self->finishSessionClose();return;}
            self->setBusy(false);if(!error.isEmpty())self->diagnostic(error);
            done(error);
        });
        return;
    }
    if(m_worker) return;
    m_cancelFlag=std::make_shared<std::atomic_bool>(false);const auto flag=m_cancelFlag;
    auto result=std::make_shared<QString>();
    const ProgressCallback progress=[this](int percent,const QString& stage){QMetaObject::invokeMethod(this,[this,percent,stage]{m_progress->setValue(percent);m_status->setText(stage);},Qt::QueuedConnection);};
    m_worker=QThread::create([work,flag,progress,result]{
        try {work(*flag,progress);}
        catch(const Standard_Failure& e){*result=QString::fromUtf8(e.GetMessageString());}
        catch(const std::exception& e){*result=QString::fromUtf8(e.what());}
        catch(...){*result=QStringLiteral("发生未识别的 CAD 错误。");}
    });
    connect(m_worker,&QThread::finished,this,[this,result,done]{
        auto finished=m_worker;m_worker=nullptr;finished->deleteLater();setBusy(false);
        if(!result->isEmpty()) diagnostic(*result);
        done(*result);
        if(m_closePending) close();
    });
    setBusy(true,title);m_progress->setValue(0);m_worker->start();
}

void MainWindow::chooseFile()
{
    if(m_embedded){
        const auto paths=QFileDialog::getOpenFileNames(this,QStringLiteral("打开 STEP 文件"),{},QStringLiteral("STEP 文件 (*.stp *.step *.STP *.STEP)"));
        if(!paths.isEmpty())emit openFilesRequested(paths);return;
    }
    auto path=QFileDialog::getOpenFileName(this,QStringLiteral("打开 STEP 文件"),{},QStringLiteral("STEP 文件 (*.stp *.step *.STP *.STEP)"));
    if(!path.isEmpty()) openStep(path);
}

void MainWindow::openStep(const QString& path)
{
    if(m_embedded){emit openFilesRequested({path});return;}
    beginLoadDocument(path);
}

void MainWindow::beginLoadDocument(const QString& path)
{
    if(m_closePending)return;
    if(m_busy||m_worker) return;
    if(!isStep(path)){QMessageBox::warning(this,QStringLiteral("格式不支持"),QStringLiteral("请选择 .stp 或 .step 文件。"));return;}
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->setDocument(nullptr);
    if(m_measurementAction)m_measurementAction->setChecked(false);
    m_partGrabSceneReady=false;m_view->setGrabMode(false);updatePartGrabState();
#endif
    clearAppearanceForNewDocument();
    m_sessionPath=path;m_importFailure->hide();
    m_bomPageAction->setChecked(false);
    m_loadTimer.start();diagnostic(QStringLiteral("打开：%1").arg(path));
    auto document=std::make_shared<std::shared_ptr<CadDocument>>();auto pmi=std::make_shared<PmiCatalog>();
    const int quality=m_view->meshQuality();
    runTask(QStringLiteral("读取 STEP…"),[path,document,pmi,quality](std::atomic_bool& cancel,const ProgressCallback& progress){
        *document=loadStep(path,progress,cancel);
        if(cancel.load())throw std::runtime_error("已取消导入");
        progress(94,QStringLiteral("读取尺寸、基准与公差标注…"));*pmi=buildPmiCatalog(**document);
        (*document)->diagnostics.append(pmi->diagnostics);
        if(cancel.load())throw std::runtime_error("已取消导入");
        prepareDisplayMeshes(**document,quality,cancel,[&progress](int percent,const QString& stage){progress(95+percent*4/100,stage);});
    },[this,document,pmi](const QString& error){
        if(!error.isEmpty()) {
            if(!m_smokeDirectory.isEmpty()){QTimer::singleShot(0,qApp,[]{qApp->exit(2);});return;}
            if(m_embedded){m_importError->setText(QStringLiteral("读取未完成：%1").arg(error));m_importFailure->show();emit stateChanged();return;}
            if(!error.contains(QStringLiteral("取消")))QMessageBox::warning(this,QStringLiteral("导入未完成"),error);return;
        }
        m_document=*document;m_pmi=*pmi;populateDocument();
        setBusy(true,QStringLiteral("建立三维显示…"));m_progress->setValue(98);m_view->setDocument(m_document,m_pmi,true);
    });
}

void MainWindow::populateDocument()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->setDocument(m_document);
#endif
    m_currentSelection.clear();
    m_syncing=true; m_tree->clear();m_treeItems.clear();m_scope->clear();m_scope->addItem(QStringLiteral("总装"),-1);
    for(size_t i=0;i<m_document->occurrences.size();++i){
        const auto& occurrence=m_document->occurrences[i];auto item=new QTreeWidgetItem;
        item->setText(0,occurrence.name);item->setText(1,occurrence.partIndex>=0?QStringLiteral("零件"):QStringLiteral("装配"));item->setToolTip(0,occurrence.path);
        item->setData(0,Qt::UserRole,int(i));item->setFlags(item->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);item->setCheckState(0,Qt::Checked);
        if(occurrence.parent>=0&&size_t(occurrence.parent)<m_treeItems.size())m_treeItems[size_t(occurrence.parent)]->addChild(item);else m_tree->addTopLevelItem(item);
        m_treeItems.push_back(item);if(occurrence.partIndex<0)m_scope->addItem(occurrence.path,int(i));
    }
    m_tree->expandToDepth(1);m_explode->setValue(0);m_transparency->setValue(0);m_sectionAxis->setCurrentIndex(0);
    const auto quantities=partQuantities(*m_document);m_bom->setRowCount(int(m_document->parts.size()));
    int instanceCount=0;
    for(int quantity:quantities)instanceCount+=quantity;
    refreshBom();
    m_summary->setText(QStringLiteral("%1   ·   %2 种零件 / %3 个实例   ·   原单位：%4   ·   几何显示单位：mm").arg(QFileInfo(m_document->sourcePath).fileName()).arg(m_document->parts.size()).arg(instanceCount).arg(m_document->sourceUnits));
    setWindowTitle(QFileInfo(m_document->sourcePath).fileName()+QStringLiteral(" — STP Viewer"));
    m_pmiTable->setRowCount(int(m_pmi.records.size()));int renderable=0;
    for(size_t i=0;i<m_pmi.records.size();++i){const auto& record=m_pmi.records[i];
        cell(m_pmiTable,int(i),0,record.name.isEmpty()?pmiKindText(record.kind):record.name);cell(m_pmiTable,int(i),1,pmiStatusText(record.status));
        cell(m_pmiTable,int(i),2,record.semanticText+"\n"+record.reason);
        if(record.status==PmiStatus::SourceGraphics||record.status==PmiStatus::SemanticFallback)++renderable;
    }
    m_pmiSummary->setText(QStringLiteral("识别 %1 项，具备显示数据 %2 项。\n源文件标注完整性未确认；双击查看语义和诊断。").arg(m_pmi.records.size()).arg(renderable));
    m_pmiTable->resizeColumnsToContents();m_pmiTable->setColumnWidth(0,110);m_pmiTable->setColumnWidth(1,120);
    for(const auto& message:m_document->diagnostics)diagnostic(message);
    m_syncing=false;m_currentSelection.clear();showProperties({});
}

std::vector<int> MainWindow::treeSelection() const
{std::vector<int> output;for(auto item:m_tree->selectedItems())output.push_back(item->data(0,Qt::UserRole).toInt());return output;}

void MainWindow::selectionChanged(const std::vector<int>& indices,bool updateScene)
{
    if(!m_document||m_syncing)return;m_syncing=true;m_currentSelection=indices;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->setTreeSelection(indices);
#endif
    if(updateScene)m_view->selectOccurrences(indices);
    m_tree->clearSelection();m_bom->clearSelection();
    auto parts=selectedPartIndices(*m_document,indices);
    // Empty selection means nothing in the UI; export's empty list separately means all.
    if(!indices.empty()){
        for(int index:indices)if(index>=0&&size_t(index)<m_treeItems.size())m_treeItems[size_t(index)]->setSelected(true);
        for(int part:parts){
            const auto found=std::find(m_bomPartIndices.begin(),m_bomPartIndices.end(),part);
            if(found==m_bomPartIndices.end())continue;const int row=int(found-m_bomPartIndices.begin());
            const auto index=m_bom->model()->index(row,0);m_bom->selectionModel()->select(index,QItemSelectionModel::Select|QItemSelectionModel::Rows);
        }
    }
    showProperties(indices);m_syncing=false;
}

void MainWindow::showProperties(const std::vector<int>& indices)
{
    QSignalBlocker block(m_properties);
    m_properties->setRowCount(0);auto add=[this](const QString& key,const QString& value,const QString& source){const int row=m_properties->rowCount();m_properties->insertRow(row);cell(m_properties,row,0,key);cell(m_properties,row,1,value);cell(m_properties,row,2,source);};
    if(!m_document)return;
    if(indices.empty()){add(QStringLiteral("文件"),QFileInfo(m_document->sourcePath).fileName(),QStringLiteral("文件"));add(QStringLiteral("零件定义"),QString::number(m_document->parts.size()),QStringLiteral("装配结构"));add(QStringLiteral("说明"),QStringLiteral("选择零件查看属性；包围盒不是工程标注尺寸。"),QStringLiteral("提示"));return;}
    const int index=indices.front();if(index<0||size_t(index)>=m_document->occurrences.size())return;
    const auto& occurrence=m_document->occurrences[size_t(index)];
    add(QStringLiteral("名称"),occurrence.name,QStringLiteral("文件/回退名称"));add(QStringLiteral("路径"),occurrence.path,QStringLiteral("装配结构"));
    const auto position=occurrence.worldTransform.TranslationPart();add(QStringLiteral("位置 mm"),QStringLiteral("%1, %2, %3").arg(number(position.X()),number(position.Y()),number(position.Z())),QStringLiteral("装配变换"));
    if(occurrence.partIndex<0){add(QStringLiteral("直接子项"),QString::number(occurrence.children.size()),QStringLiteral("装配结构"));return;}
    const auto& part=m_document->parts[size_t(occurrence.partIndex)];
    add(QStringLiteral("编号"),part.number.isEmpty()?QStringLiteral("文件未提供"):part.number,QStringLiteral("文件"));
    const auto material=effectiveMaterial(part);const auto density=effectiveDensity(part);
    const auto materialSource=part.manualMaterial?QStringLiteral("人工输入"):materialEditable(part)?QStringLiteral("未提供·可填写"):QStringLiteral("文件");
    const auto densitySource=part.manualDensityKgM3?QStringLiteral("人工输入"):densityEditable(part)?QStringLiteral("未提供·可填写"):QStringLiteral("文件/单位换算");
    add(QStringLiteral("材料"),material.isEmpty()?QStringLiteral("双击填写"):material,materialSource);
    editableField(m_properties->item(m_properties->rowCount()-1,1),occurrence.partIndex,1,material,materialEditable(part),materialSource);
    add(QStringLiteral("密度 kg/m³"),density?number(*density):QStringLiteral("双击填写"),densitySource);
    editableField(m_properties->item(m_properties->rowCount()-1,1),occurrence.partIndex,2,density?number(*density):QString(),densityEditable(part),densitySource);
    add(QStringLiteral("包围盒 mm"),part.bboxValid?QStringLiteral("X %1 × Y %2 × Z %3").arg(number(part.bboxMm[0]),number(part.bboxMm[1]),number(part.bboxMm[2])):QStringLiteral("不可计算"),QStringLiteral("几何计算·局部轴"));
    add(QStringLiteral("体积 mm³"),optionalNumber(part.volumeMm3),QStringLiteral("几何计算"));add(QStringLiteral("表面积 mm²"),optionalNumber(part.areaMm2),QStringLiteral("几何计算"));
    add(QStringLiteral("质量 kg"),optionalNumber(effectiveMass(part)),part.manualDensityKgM3?QStringLiteral("体积 × 人工密度"):QStringLiteral("体积 × 文件密度"));
    add(QStringLiteral("选中项数"),QString::number(indices.size()),QStringLiteral("当前选择"));
}

void MainWindow::saveBom()
{
    if(!m_document||m_busy)return;
    commitManualEditor();
    if(checkedLeafOccurrences().empty())return;
    QString path=QFileDialog::getSaveFileName(this,QStringLiteral("导出 Excel BOM"),QFileInfo(m_document->sourcePath).completeBaseName()+"_BOM.xlsx",QStringLiteral("Excel 工作簿 (*.xlsx)"));
    if(path.isEmpty())return;if(!path.endsWith(".xlsx",Qt::CaseInsensitive))path+=".xlsx";
    const auto document=processingDocument();
    runTask(QStringLiteral("导出 Excel BOM…"),[document,path](std::atomic_bool& cancel,const ProgressCallback& progress){progress(30,QStringLiteral("写入工程 BOM…"));exportBom(*document,path,&cancel);progress(100,QStringLiteral("Excel 导出完成"));},
        [this,path](const QString& error){if(error.isEmpty())QMessageBox::information(this,QStringLiteral("导出完成"),path);else if(error.contains(QStringLiteral("取消")))m_status->setText(QStringLiteral("已取消 Excel 导出，目标文件未修改。"));else QMessageBox::warning(this,QStringLiteral("导出失败"),error);});
}

void MainWindow::saveParts()
{
    if(!m_document||m_busy)return;
    commitManualEditor();
    if(checkedLeafOccurrences().empty())return;const std::vector<int> selected;
    const auto directory=QFileDialog::getExistingDirectory(this,QStringLiteral("选择导出目录（自动创建新文件夹）"));if(directory.isEmpty())return;
    const auto document=processingDocument();
    auto result=std::make_shared<ExportResult>();
    runTask(QStringLiteral("导出精确 STEP 零件…"),[document,selected,directory,result](std::atomic_bool& cancel,const ProgressCallback& progress){*result=exportParts(*document,selected,directory,progress,cancel);},
        [this,result](const QString& error){if(!error.isEmpty()){QMessageBox::warning(this,QStringLiteral("导出未完成"),error);return;}
            for(const auto& msg:result->diagnostics)diagnostic(msg);
            m_status->setText(QStringLiteral("拆分导出完成：%1 种零件，已保存到 %2").arg(result->files.size()).arg(result->directory));
            diagnostic(QStringLiteral("拆分结果目录：%1（%2 个 STEP 文件及 manifest.xlsx）").arg(result->directory).arg(result->files.size()));
            auto message=new QMessageBox(QMessageBox::Information,QStringLiteral("拆分导出完成"),
                QStringLiteral("已导出 %1 种零件：\n%2\n\n清单：%3").arg(result->files.size()).arg(result->directory,result->manifestPath),QMessageBox::Ok,this);
            message->setAttribute(Qt::WA_DeleteOnClose);message->setObjectName("splitExportComplete");
            message->setInformativeText(QStringLiteral("结果保存在所选目录内新建的 STP_parts_ 文件夹中。"));
            #ifdef Q_OS_MACOS
            const auto folderCaption=QStringLiteral("在 Finder 中打开");
#else
            const auto folderCaption=QStringLiteral("在资源管理器中打开");
#endif
            auto openResult=message->addButton(folderCaption,QMessageBox::ActionRole);
            openResult->setObjectName("openSplitExportFolder");
            connect(openResult,&QPushButton::clicked,message,[directory=result->directory]{QDesktopServices::openUrl(QUrl::fromLocalFile(directory));});
            message->open();
        });
}

void MainWindow::diagnostic(const QString& text)
{
    if(m_closePending)return;
    m_diagnostics->append(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"),text.toHtmlEscaped()));
}
void MainWindow::updateSection(){m_view->setSection(m_sectionAxis->currentIndex()-1,m_sectionPosition->value(),m_sectionReverse->isChecked(),m_sectionCap->isChecked());}
void MainWindow::filterTree(const QString& text)
{
    for(auto item:m_treeItems)item->setHidden(!text.isEmpty());
    for(auto item:m_treeItems)if(text.isEmpty()||item->text(0).contains(text,Qt::CaseInsensitive)||item->toolTip(0).contains(text,Qt::CaseInsensitive)){
        item->setHidden(false);for(auto parent=item->parent();parent;parent=parent->parent()){parent->setHidden(false);if(!text.isEmpty())parent->setExpanded(true);}
    }
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event){
    if(m_embedded){
        for(const auto& url:event->mimeData()->urls())if(url.isLocalFile()&&isStep(url.toLocalFile())){event->acceptProposedAction();return;}
        return;
    }
    if(!m_busy&&event->mimeData()->hasUrls()&&event->mimeData()->urls().size()==1&&isStep(event->mimeData()->urls().front().toLocalFile()))event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event){
    if(m_embedded){QStringList paths;for(const auto& url:event->mimeData()->urls())if(url.isLocalFile()&&isStep(url.toLocalFile()))paths.append(url.toLocalFile());
        if(!paths.isEmpty()){emit openFilesRequested(paths);event->acceptProposedAction();}return;}
    if(!event->mimeData()->urls().isEmpty())openStep(event->mimeData()->urls().front().toLocalFile());
}
void MainWindow::closeEvent(QCloseEvent* event)
{
    if(m_embedded){requestSessionClose();event->ignore();return;}
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if(m_measurementPanel)m_measurementPanel->prepareClose();
    m_view->finishPartDrag();
#endif
    dismissAppearanceMenu();m_appearanceDragStart.reset();m_appearanceHistory.clear();
    m_closePending=true;updateAppearanceActions();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    updatePartGrabState();
#endif
    if(m_worker){m_closePending=true;m_cancelFlag->store(true);m_status->setText(QStringLiteral("正在结束后台任务，然后关闭…"));event->ignore();return;}
    event->accept();
}
void MainWindow::runSmokeTest(const QString& input,const QString& outputDirectory)
{m_smokeDirectory=outputDirectory;QTimer::singleShot(100,this,[this,input]{openStep(input);});}

void MainWindow::startSmokeCapture(qint64 loadMs)
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // Exercise the shipped panel and queue, including history accumulation,
    // before the existing portable-bundle screenshots and export checks.
    if(m_measurementPanel&&!m_measurementSmokeStarted){
        m_measurementSmokeStarted=true;
        MeasurementEntity first,second;
        bool radial=false;
        for(size_t i=0;i<m_document->occurrences.size()&&first.shape.IsNull();++i){
            if(m_document->occurrences[i].partIndex<0)continue;
            const auto part=m_view->measurementPartEntity(int(i));
            for(TopExp_Explorer edge(part.shape,TopAbs_EDGE);edge.More();edge.Next()){
                if(BRepAdaptor_Curve(TopoDS::Edge(edge.Current())).GetType()==GeomAbs_Circle){
                    first=part;first.shape=edge.Current();first.selection=MeasurementSelection::Edge;second=first;radial=true;break;
                }
            }
            if(first.shape.IsNull()){
                TopExp_Explorer vertex(part.shape,TopAbs_VERTEX),edge(part.shape,TopAbs_EDGE);
                if(vertex.More()&&edge.More()){
                    first=part;first.shape=vertex.Current();first.selection=MeasurementSelection::Vertex;
                    second=part;second.shape=edge.Current();second.selection=MeasurementSelection::Edge;
                }
            }
        }
        if(first.shape.IsNull()){diagnostic(QStringLiteral("测量自检未找到参考几何"));QTimer::singleShot(0,qApp,[]{qApp->exit(2);});return;}
        m_measurementAction->setChecked(true);
        std::vector<MeasurementRequest> checks;
        checks.push_back({radial?MeasurementKind::Radius:MeasurementKind::Point,MeasurementCoordinates::Original,{first},{}});
        checks.push_back({radial?MeasurementKind::Diameter:MeasurementKind::Length,MeasurementCoordinates::Original,{second},{}});
        if(radial){
            auto center=first;center.selection=MeasurementSelection::FeatureCenter;
            auto axis=first;axis.selection=MeasurementSelection::FeatureAxis;
            checks.push_back({MeasurementKind::Point,MeasurementCoordinates::Original,{center},{}});
            checks.push_back({MeasurementKind::Distance,MeasurementCoordinates::Original,{axis},{center}});
        }
        const bool referenceFixture=QFileInfo(m_document->sourcePath).fileName()==QStringLiteral("measurement-reference.step");
        if(referenceFixture){
            const auto part=m_view->measurementPartEntity(first.occurrence);
            const int definition=m_document->occurrences[size_t(first.occurrence)].partIndex;
            QString error;
            if(!setManualDensity(m_document->parts[size_t(definition)],QStringLiteral("1000"),&error)){
                diagnostic(error);QTimer::singleShot(0,qApp,[]{qApp->exit(2);});return;
            }
            for(auto kind:{MeasurementKind::VolumeCenter,MeasurementKind::Mass,MeasurementKind::CenterOfMass})
                checks.push_back({kind,MeasurementCoordinates::Original,{part},{}});
        }
        std::vector<double> extensionDistances;
        const int extensionStart=int(checks.size());
        const bool extensionFixture=referenceFixture || QFileInfo(m_document->sourcePath).fileName()==QStringLiteral("measurement-extension.step");
        if(extensionFixture) {
            MeasurementEntity line,plane,point;
            for(size_t i=0;i<m_document->occurrences.size();++i) {
                if(m_document->occurrences[i].partIndex<0)continue;
                const auto part=m_view->measurementPartEntity(int(i));
                if(line.shape.IsNull()) for(TopExp_Explorer edge(part.shape,TopAbs_EDGE);edge.More();edge.Next())
                    if(BRepAdaptor_Curve(TopoDS::Edge(edge.Current())).GetType()==GeomAbs_Line) {
                        line=part;line.shape=edge.Current();line.selection=MeasurementSelection::Edge;line.extent=MeasurementExtent::Infinite;break;
                    }
                if(plane.shape.IsNull()) for(TopExp_Explorer face(part.shape,TopAbs_FACE);face.More();face.Next())
                    if(BRepAdaptor_Surface(TopoDS::Face(face.Current())).GetType()==GeomAbs_Plane) {
                        plane=part;plane.shape=face.Current();plane.selection=MeasurementSelection::Face;plane.extent=MeasurementExtent::Infinite;break;
                    }
            }
            const auto lineReference=measurementReference(line,MeasurementCoordinates::Original);
            const auto planeReference=measurementReference(plane,MeasurementCoordinates::Original);
            if(!lineReference || !planeReference) {diagnostic(QStringLiteral("无限拓展自检缺少参考线或面"));qApp->exit(2);return;}
            double farthest=-1.;gp_Pnt worldPoint;
            for(size_t i=0;i<m_document->occurrences.size();++i) {
                if(m_document->occurrences[i].partIndex<0)continue;
                const auto part=m_view->measurementPartEntity(int(i));
                for(TopExp_Explorer vertex(part.shape,TopAbs_VERTEX);vertex.More();vertex.Next()) {
                    const auto sample=BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current())).Transformed(part.originalTransform);
                    const double distance=planeReference->plane->Distance(sample);
                    if(distance>farthest) {farthest=distance;worldPoint=sample;point=part;point.shape=vertex.Current();point.selection=MeasurementSelection::Vertex;}
                }
            }
            checks.push_back({MeasurementKind::Distance,MeasurementCoordinates::Original,{line},{point}});
            checks.push_back({MeasurementKind::Distance,MeasurementCoordinates::Original,{plane},{point}});
            checks.push_back({MeasurementKind::Distance,MeasurementCoordinates::Original,{line},{plane}});
            extensionDistances={lineReference->line->Distance(worldPoint),planeReference->plane->Distance(worldPoint),
                lineReference->line->Direction().Dot(planeReference->plane->Axis().Direction())==0.
                    ?planeReference->plane->Distance(lineReference->line->Location()):0.};
        }
        setProperty("measurementExtensionSmoke",extensionFixture && extensionDistances.size()==3);
        setProperty("measurementCentersSmoke",referenceFixture);
        setProperty("measurementSmokeExpected",int(checks.size()));
        setProperty("measurementReferenceSmoke",radial);
        auto stage=std::make_shared<int>(0);
        auto connection=std::make_shared<QMetaObject::Connection>();
        auto timeout=new QTimer(this);timeout->setSingleShot(true);
        connect(timeout,&QTimer::timeout,this,[this,connection]{
            QObject::disconnect(*connection);diagnostic(QStringLiteral("测量面板自检未完成"));qApp->exit(2);
        });
        const auto launch=[this](const MeasurementRequest& request){
            auto operation=m_measurementPanel->findChild<QComboBox*>(QStringLiteral("measurementOperation"));
            operation->setCurrentIndex(request.b.empty()?0:1);
            auto types=m_measurementPanel->findChild<QComboBox*>(QStringLiteral("measurementKind"));
            types->setCurrentIndex(types->findData(int(request.kind)));
            for(int slot=0;slot<(request.b.empty()?1:2);++slot){
                const auto& entity=slot==0?request.a.front():request.b.front();
                auto selection=m_measurementPanel->findChild<QComboBox*>(slot==0?QStringLiteral("measurementSelectionA"):QStringLiteral("measurementSelectionB"));
                selection->setCurrentIndex(selection->findData(int(entity.selection)));
                m_view->measurementEntityPicked(entity,slot);
                auto extent=m_measurementPanel->findChild<QCheckBox*>(slot==0?QStringLiteral("measurementExtentA"):QStringLiteral("measurementExtentB"));
                extent->setChecked(entity.extent==MeasurementExtent::Infinite);
            }
            m_measurementPanel->findChild<QPushButton*>(QStringLiteral("measurementCompute"))->click();
        };
        *connection=connect(m_measurementPanel,&MeasurementPanel::recordsChanged,this,
            [this,loadMs,stage,connection,timeout,launch,checks,extensionStart,extensionDistances]{
                if(m_measurementPanel->recordCount()!=*stage+1)return;
                if(*stage>=extensionStart && !extensionDistances.empty()) {
                    const auto& actual=m_measurementPanel->records().back();
                    const auto& expected=checks[size_t(*stage)];
                    if(actual.request.a.front().extent!=expected.a.front().extent || actual.request.b.front().extent!=expected.b.front().extent
                        || std::abs(actual.result.values.front().value-extensionDistances[size_t(*stage-extensionStart)])>1.e-6) {
                        diagnostic(QStringLiteral("打包应用无限拓展实际计算或快照错误"));qApp->exit(2);return;
                    }
                }
                ++(*stage);
                if(*stage<int(checks.size())){launch(checks[size_t(*stage)]);return;}
                QObject::disconnect(*connection);timeout->stop();timeout->deleteLater();
                m_measurementAction->setChecked(false);
                QTimer::singleShot(50,this,[this,loadMs]{startSmokeCapture(loadMs);});
            });
        timeout->start(30000);launch(checks.front());return;
    }
#endif
    m_bomPageAction->setChecked(false);
    QDir().mkpath(m_smokeDirectory);
    auto report=std::make_shared<QJsonObject>();
    (*report)["input"]=m_document->sourcePath;(*report)["parts"]=int(m_document->parts.size());
    (*report)["occurrences"]=int(m_document->occurrences.size());(*report)["pmiRecords"]=int(m_pmi.records.size());
    (*report)["loadMs"]=double(loadMs);(*report)["inputBytes"]=double(QFileInfo(m_document->sourcePath).size());
    (*report)["qtChineseTranslationLoaded"]=qApp->property("qtChineseTranslationLoaded").toBool();
    (*report)["qtCancelText"]=QCoreApplication::translate("QPlatformTheme","Cancel");
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    (*report)["measurementSmokePassed"]=m_measurementPanel&&m_measurementPanel->recordCount()==property("measurementSmokeExpected").toInt();
    (*report)["measurementReferenceSmokePassed"]=property("measurementReferenceSmoke").toBool()
        &&(*report)["measurementSmokePassed"].toBool();
    (*report)["measurementCentersSmokePassed"]=property("measurementCentersSmoke").toBool()
        &&(*report)["measurementSmokePassed"].toBool();
    (*report)["measurementExtensionSmokePassed"]=property("measurementExtensionSmoke").toBool()
        &&(*report)["measurementSmokePassed"].toBool();
    (*report)["measurementRecordCount"]=m_measurementPanel?m_measurementPanel->recordCount():0;
    if(m_measurementPanel){
        QJsonArray measurements;
        for(const auto& record:m_measurementPanel->records()){
            QJsonObject measurement;measurement["id"]=QStringLiteral("M%1").arg(record.id);
            measurement["kind"]=measurementKindText(record.request.kind);
            measurement["result"]=measurementResultText(record.result,6);
            if(!record.request.a.empty())measurement["referenceA"]=measurementReferenceText(record.request.a.front());
            if(!record.request.b.empty())measurement["referenceB"]=measurementReferenceText(record.request.b.front());
            measurements.append(measurement);
        }
        (*report)["measurements"]=measurements;
    }
#endif
    const QRect available=screen()?screen()->availableGeometry():QRect();
    (*report)["layout"]=QJsonObject{{"screenAvailableLogicalWidth",available.width()},{"screenAvailableLogicalHeight",available.height()},
        {"windowLogicalWidth",width()},{"windowLogicalHeight",height()},{"viewportLogicalWidth",m_view->width()},
        {"viewportLogicalHeight",m_view->height()},{"devicePixelRatio",devicePixelRatioF()},{"compactMode",m_compactLayout}};
    auto capture=[this,report]{
        m_view->standardView(0);
        QTimer::singleShot(150,this,[this,report]{
            (*report)["screenshot"]=m_view->saveScreenshot(m_smokeDirectory+"/model.png");
            (*report)["windowScreenshot"]=grab().save(m_smokeDirectory+"/window.png");
            (*report)["bomHiddenOnModelPage"]=!m_bom->isVisible()&&!m_diagnostics->isVisible();
            m_explode->setValue(70);m_view->fitAll();
            QTimer::singleShot(150,this,[this,report]{
                (*report)["explodedScreenshot"]=m_view->saveScreenshot(m_smokeDirectory+"/exploded.png");
                m_explode->setValue(0);m_view->fitAll();
                m_sectionAxis->setCurrentIndex(1);m_transparency->setValue(35);m_pmiAction->setChecked(false);
                QTimer::singleShot(150,this,[this,report]{
                    (*report)["sectionScreenshot"]=m_view->saveScreenshot(m_smokeDirectory+"/section.png");
                    m_bomPageAction->setChecked(true);layout()->activate();
                    auto tabs=findChild<QTabWidget*>(QStringLiteral("bomPageTabs"));tabs->setCurrentIndex(0);
                    (*report)["bomPageScreenshot"]=grab().save(m_smokeDirectory+"/bom-page.png");
                    tabs->setCurrentIndex(1);
                    (*report)["diagnosticsPageScreenshot"]=grab().save(m_smokeDirectory+"/diagnostics-page.png");
                    tabs->setCurrentIndex(0);m_bomPageAction->setChecked(false);layout()->activate();
                    auto result=std::make_shared<ExportResult>();
                    const auto document=processingDocument();
                    (*report)["checkedInstancesAtExport"]=int(checkedLeafOccurrences().size());
                    // Keep modified view state active: exports must use original geometry.
                    (*report)["exportWhileViewModified"]=true;
                    runTask(QStringLiteral("运行导出自检"),[document,result,this](std::atomic_bool& cancel,const ProgressCallback& progress){
                        exportBom(*document,m_smokeDirectory+"/BOM.xlsx");
                        *result=exportParts(*document,{},m_smokeDirectory,progress,cancel);
                    },[this,report,result](const QString& error){
                        (*report)["error"]=error;(*report)["exportedFiles"]=QJsonArray::fromStringList(result->files);
                        (*report)["diagnostics"]=QJsonArray::fromStringList(m_document->diagnostics);
                        (*report)["exportDiagnostics"]=QJsonArray::fromStringList(result->diagnostics);
                        QSaveFile file(m_smokeDirectory+"/smoke-report.json");
                        const bool saved=file.open(QIODevice::WriteOnly)&&file.write(QJsonDocument(*report).toJson())>0&&file.commit();
                        bool passed=error.isEmpty()&&saved&&(*report)["screenshot"].toBool()&&(*report)["sectionScreenshot"].toBool();
                        passed=passed&&(*report)["bomHiddenOnModelPage"].toBool()&&(*report)["bomPageScreenshot"].toBool()&&(*report)["diagnosticsPageScreenshot"].toBool();
                        QTimer::singleShot(0,qApp,[passed]{qApp->exit(passed?0:2);});
                    });
                });
            });
        });
    };
    // Measure actual completed QOpenGLWidget frames while rotating the camera.
    auto viewport=m_view->findChild<QOpenGLWidget*>("cadViewport");
    if(!viewport){capture();return;}
    auto count=std::make_shared<int>(0);auto elapsed=std::make_shared<QElapsedTimer>();
    auto connection=std::make_shared<QMetaObject::Connection>();auto done=std::make_shared<bool>(false);
    auto timeout=new QTimer(this);timeout->setSingleShot(true);
    auto finish=[report,count,elapsed,connection,done,timeout,capture]{
        if(*done)return;*done=true;QObject::disconnect(*connection);timeout->stop();timeout->deleteLater();
        (*report)["rotationFrames"]=*count;(*report)["rotationMs"]=double(elapsed->elapsed());
        (*report)["rotationFps"]=*count*1000.0/std::max<qint64>(1,elapsed->elapsed());capture();
    };
    connect(timeout,&QTimer::timeout,this,finish);
    *connection=connect(viewport,&QOpenGLWidget::frameSwapped,this,[this,count,viewport,finish]{
        if(++*count>=60){finish();return;}
        const double angle=*count*0.04;
        m_view->view()->Camera()->SetDirection(gp_Dir(std::cos(angle),std::sin(angle),-0.6));
        viewport->update();
    });
    elapsed->start();timeout->start(30000);viewport->update();
}

} // namespace Stp
