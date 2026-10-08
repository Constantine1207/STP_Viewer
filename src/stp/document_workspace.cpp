#include "document_workspace.h"
#include "cad_job_queue.h"
#include "main_window.h"
#include "author_signature.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>
#include <algorithm>

namespace Stp {
namespace {
bool isStep(const QString& path) {
    const auto suffix=QFileInfo(path).suffix();
    return suffix.compare("stp",Qt::CaseInsensitive)==0||suffix.compare("step",Qt::CaseInsensitive)==0;
}
QString canonicalPath(const QString& path) {
    const QFileInfo info(path);const auto canonical=info.canonicalFilePath();
    return canonical.isEmpty()?QDir::cleanPath(info.absoluteFilePath()):canonical;
}
QString documentKey(const QString& canonical) {
#ifdef Q_OS_WIN
    return canonical.toCaseFolded();
#else
    return canonical;
#endif
}
QStringList droppedPaths(const QMimeData* data) {
    QStringList paths;for(const auto& url:data->urls())if(url.isLocalFile()&&isStep(url.toLocalFile()))paths.append(url.toLocalFile());
    return paths;
}
void removeMenuActions(QMenu* menu) { for(auto action:menu->actions())menu->removeAction(action); }
}

DocumentWorkspace::DocumentWorkspace(QWidget* parent) : QMainWindow(parent)
{
    setObjectName("documentWorkspace");setWindowTitle(QStringLiteral("STP Viewer · 离线工程查看器"));setAcceptDrops(true);
    m_jobs=new CadJobQueue(this);
    m_pages=new QStackedWidget;setCentralWidget(m_pages);
    auto welcome=new QWidget;welcome->setObjectName("welcomePage");auto welcomeLayout=new QVBoxLayout(welcome);
    auto title=new QLabel(QStringLiteral("STP Viewer"));title->setAlignment(Qt::AlignCenter);title->setStyleSheet("font-size:26px;font-weight:600;");
    auto hint=new QLabel(QStringLiteral("打开或拖入多个 STEP 文件\n每个文件保留独立的三维视图、属性和 BOM"));hint->setAlignment(Qt::AlignCenter);
    auto button=new QPushButton(QStringLiteral("打开 STP"));button->setObjectName("welcomeOpen");button->setMaximumWidth(220);
    welcomeLayout->addStretch();welcomeLayout->addWidget(title);welcomeLayout->addWidget(hint);welcomeLayout->addSpacing(16);
    welcomeLayout->addWidget(button,0,Qt::AlignHCenter);welcomeLayout->addStretch();
    connect(button,&QPushButton::clicked,this,&DocumentWorkspace::chooseFiles);m_pages->addWidget(welcome);
    m_tabs=new QTabWidget;m_tabs->setObjectName("documentTabs");m_tabs->setTabsClosable(true);m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);m_tabs->setUsesScrollButtons(true);m_tabs->tabBar()->setElideMode(Qt::ElideMiddle);
    m_tabs->tabBar()->installEventFilter(this);m_pages->addWidget(m_tabs);
    connect(m_tabs,&QTabWidget::currentChanged,this,&DocumentWorkspace::updateActiveDocument);
    connect(m_tabs,&QTabWidget::tabCloseRequested,this,[this](int index){closeDocument(qobject_cast<MainWindow*>(m_tabs->widget(index)));});

    // A parentless bar is the macOS application's fallback menu, including
    // when focus belongs to an embedded QMainWindow with a hidden local bar.
#ifdef Q_OS_MACOS
    m_nativeMenu=new QMenuBar(nullptr);
#else
    m_nativeMenu=menuBar();m_nativeMenu->setNativeMenuBar(false);
#endif
    auto file=m_nativeMenu->addMenu(QStringLiteral("文件"));
    m_open=file->addAction(QStringLiteral("打开 STP…"),this,&DocumentWorkspace::chooseFiles);
    m_open->setObjectName("workspaceOpen");m_open->setShortcut(QKeySequence::Open);
    m_closeCurrent=file->addAction(QStringLiteral("关闭当前"),this,&DocumentWorkspace::closeCurrent);
    m_closeCurrent->setObjectName("workspaceCloseCurrent");
#ifdef Q_OS_WIN
    m_closeCurrent->setShortcuts({QKeySequence(Qt::CTRL|Qt::Key_W),QKeySequence(QKeySequence::Close)});
#else
    m_closeCurrent->setShortcut(QKeySequence::Close);
#endif
    m_closeAll=file->addAction(QStringLiteral("关闭全部"),this,&DocumentWorkspace::closeAll);m_closeAll->setObjectName("workspaceCloseAll");
    file->addSeparator();
    m_bom=file->addAction(QStringLiteral("导出 Excel BOM"),[this]{if(auto doc=currentDocument())doc->findChild<QAction*>("exportBom")->trigger();});
    m_parts=file->addAction(QStringLiteral("拆分导出 STEP"),[this]{if(auto doc=currentDocument())doc->findChild<QAction*>("exportParts")->trigger();});
    m_bom->setObjectName("workspaceExportBom");m_parts->setObjectName("workspaceExportParts");
    file->addSeparator();auto quit=file->addAction(QStringLiteral("退出"),this,&QWidget::close);quit->setShortcut(QKeySequence::Quit);quit->setMenuRole(QAction::QuitRole);
    m_viewMenu=m_nativeMenu->addMenu(QStringLiteral("视图"));m_helpMenu=m_nativeMenu->addMenu(QStringLiteral("帮助"));
    for(auto action:{m_open,m_closeCurrent,m_closeAll,m_bom,m_parts,quit}){
        action->setParent(this);addAction(action);
    }
    auto corner=new QWidget;auto cornerLayout=new QHBoxLayout(corner);cornerLayout->setContentsMargins(5,2,5,2);
    for(auto action:{m_open,m_closeAll}){auto tool=new QToolButton;tool->setDefaultAction(action);cornerLayout->addWidget(tool);}
    m_tabs->setCornerWidget(corner);
    statusBar()->showMessage(QStringLiteral("完全离线 · 文件不上传"));
    addAuthorSignature(statusBar());
    connect(m_jobs,&CadJobQueue::idleChanged,this,[this](bool){finishQuit();});
    qApp->installEventFilter(this);
    const QRect available=screen()?screen()->availableGeometry():QRect(0,0,1440,920);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    setMinimumSize(900,640);
#endif
    resize(std::min(1440,std::max(320,available.width()-32)),std::min(920,std::max(240,available.height()-64)));
    move(available.center()-QPoint(width()/2,height()/2));updateActions();
}

DocumentWorkspace::~DocumentWorkspace() { delete m_nativeMenu; }

int DocumentWorkspace::documentCount() const { return m_tabs->count(); }
MainWindow* DocumentWorkspace::currentDocument() const { return qobject_cast<MainWindow*>(m_tabs->currentWidget()); }

void DocumentWorkspace::chooseFiles() {
    const auto paths=QFileDialog::getOpenFileNames(this,QStringLiteral("打开 STEP 文件"),{},QStringLiteral("STEP 文件 (*.stp *.step *.STP *.STEP)"));
    if(!paths.isEmpty())openFiles(paths);
}

void DocumentWorkspace::openFiles(const QStringList& paths)
{
    if(m_quitting)return;
    if(auto current=currentDocument())current->commitManualEditor();
    QPointer<MainWindow> first;
    for(const auto& path:paths){
        if(!isStep(path))continue;
        const auto canonical=canonicalPath(path);const auto key=documentKey(canonical);
        auto page=m_documents.value(key);
        if(!page){
            try {page=new MainWindow(m_tabs,m_jobs,++m_nextOwner);}
            catch(const Standard_Failure& error){QMessageBox::warning(this,QStringLiteral("三维窗口创建失败"),QString::fromUtf8(error.GetMessageString()));continue;}
            catch(const std::exception& error){QMessageBox::warning(this,QStringLiteral("窗口创建失败"),QString::fromUtf8(error.what()));continue;}
            m_documents.insert(key,page);m_paths.insert(page,canonical);
            connect(page,&MainWindow::openFilesRequested,this,&DocumentWorkspace::openFiles);
            connect(page,&MainWindow::stateChanged,this,[this,page]{if(currentDocument()==page)updateActions();});
            for(auto name:{"exportBom","exportParts"})connect(page->findChild<QAction*>(name),&QAction::changed,this,[this,page]{if(currentDocument()==page)updateActions();});
            connect(page,&MainWindow::closeReady,this,[this,page]{
                m_closing.remove(page);page->deleteLater();finishQuit();
            });
            m_tabs->addTab(page,QFileInfo(canonical).fileName().replace('&',"&&"));
            page->beginLoadDocument(canonical);
        }
        if(!first)first=page;
    }
    updateLabels();
    if(first)activateDocument(first);
    updateActiveDocument();
}

void DocumentWorkspace::activateDocument(MainWindow* page) {
    if(auto current=currentDocument())current->commitManualEditor();
    m_pages->setCurrentWidget(m_tabs);m_tabs->setCurrentWidget(page);page->setFocus(Qt::OtherFocusReason);
}

void DocumentWorkspace::updateActiveDocument()
{
    if(m_previous){
        m_previous->commitManualEditor();
        if(m_previous!=currentDocument())m_previous->cancelPendingMeasurementForSessionSwitch();
    }
    m_previous=currentDocument();
    m_pages->setCurrentIndex(documentCount()?1:0);
    removeMenuActions(m_viewMenu);removeMenuActions(m_helpMenu);
    if(auto doc=currentDocument()){
        for(auto action:doc->documentViewMenu()->actions())m_viewMenu->addAction(action);
        for(auto action:doc->documentHelpMenu()->actions())m_helpMenu->addAction(action);
        setWindowTitle(QFileInfo(m_paths.value(doc)).fileName()+QStringLiteral(" — STP Viewer"));
    }else setWindowTitle(QStringLiteral("STP Viewer · 离线工程查看器"));
    updateActions();
}

void DocumentWorkspace::updateActions()
{
    auto doc=currentDocument();m_open->setEnabled(!m_quitting);
    statusBar()->setVisible(!doc||m_quitting);
    m_closeCurrent->setEnabled(doc&&!m_quitting);m_closeAll->setEnabled(documentCount()>0&&!m_quitting);
    m_bom->setEnabled(doc&&!m_quitting&&doc->findChild<QAction*>("exportBom")->isEnabled());
    m_parts->setEnabled(doc&&!m_quitting&&doc->findChild<QAction*>("exportParts")->isEnabled());
    m_viewMenu->setEnabled(doc&&!m_quitting);m_helpMenu->setEnabled(doc&&!m_quitting);
}

void DocumentWorkspace::updateLabels()
{
    QHash<QString,int> counts;for(auto it=m_paths.cbegin();it!=m_paths.cend();++it)++counts[QFileInfo(it.value()).fileName()];
    for(int index=0;index<m_tabs->count();++index){
        const auto path=m_paths.value(qobject_cast<MainWindow*>(m_tabs->widget(index)));const QFileInfo info(path);
        auto label=info.fileName();if(counts.value(label)>1)label+=QStringLiteral(" — ")+info.absolutePath();
        m_tabs->setTabText(index,label.replace('&',"&&"));m_tabs->setTabToolTip(index,path);
    }
}

void DocumentWorkspace::closeCurrent() { closeDocument(currentDocument()); }
void DocumentWorkspace::closeAll() { while(m_tabs->count())closeDocument(qobject_cast<MainWindow*>(m_tabs->widget(m_tabs->count()-1))); }
void DocumentWorkspace::closeDocument(MainWindow* page)
{
    if(!page)return;const int index=m_tabs->indexOf(page);if(index<0)return;
    if(auto current=currentDocument())current->commitManualEditor();
    m_documents.remove(documentKey(m_paths.take(page)));m_closing.insert(page);
    m_tabs->removeTab(index);page->hide();
    updateLabels();updateActiveDocument();page->requestSessionClose();
}

bool DocumentWorkspace::eventFilter(QObject* watched,QEvent* event)
{
    if(event->type()==QEvent::FileOpen){
        const auto file=static_cast<QFileOpenEvent*>(event);const auto path=file->file().isEmpty()?file->url().toLocalFile():file->file();
        if(isStep(path)){openFiles({path});show();raise();activateWindow();return true;}
    }
    if(watched==m_tabs->tabBar()&&(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::KeyPress))
        if(auto current=currentDocument())current->commitManualEditor();
    return QMainWindow::eventFilter(watched,event);
}
void DocumentWorkspace::dragEnterEvent(QDragEnterEvent* event) { if(!m_quitting&&!droppedPaths(event->mimeData()).isEmpty())event->acceptProposedAction(); }
void DocumentWorkspace::dropEvent(QDropEvent* event) { openFiles(droppedPaths(event->mimeData()));event->acceptProposedAction(); }

void DocumentWorkspace::closeEvent(QCloseEvent* event)
{
    if(m_closing.isEmpty()&&documentCount()==0&&m_jobs->idle()){event->accept();return;}
    m_quitting=true;closeAll();updateActions();statusBar()->showMessage(QStringLiteral("正在安全结束后台任务…"));
    event->ignore();finishQuit();
}
void DocumentWorkspace::finishQuit()
{
    if(!m_quitting||m_quitScheduled||!m_closing.isEmpty()||!m_jobs->idle())return;
    m_quitScheduled=true;QTimer::singleShot(0,this,[this]{m_quitScheduled=false;close();});
}
} // namespace Stp
