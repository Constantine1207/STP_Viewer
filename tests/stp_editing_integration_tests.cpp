#include "main_window.h"
#include "document_workspace.h"
#include "part_transform_panel.h"
#include "viewer_widget.h"
#include "cad_job_queue.h"
#include "../src/app/qtopengl_utils.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
template<class F> void waitFor(F predicate){QElapsedTimer clock;clock.start();while(!predicate()){
    if(clock.elapsed()>90000)throw std::runtime_error("scene timeout");QTest::qWait(10);
}}
int firstLeaf(const Stp::CadDocument& d){for(size_t i=0;i<d.occurrences.size();++i)if(d.occurrences[i].partIndex>=0)return int(i);throw std::runtime_error("no leaf");}
}
int main(int argc,char** argv){
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);
    if(argc<2||argc>3){std::cerr<<"Expected STEP fixture path [screenshot directory]\n";return 2;}
    QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    app.setOrganizationName("STPViewerEditingTest");app.setApplicationName("STPViewerEditingTest");
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    try{
        Stp::DocumentWorkspace workspace;workspace.resize(1200,800);workspace.show();workspace.openFiles({QString::fromLocal8Bit(argv[1])});
        auto first=workspace.currentDocument();require(first,"first document");
        waitFor([&]{return first->findChild<QAction*>("editPlacement")->isEnabled();});
        const int index=firstLeaf(*first->document());auto tree=first->findChild<QTreeWidget*>("assemblyTree");
        QTreeWidgetItemIterator it(tree);for(;*it;++it)if((*it)->data(0,Qt::UserRole).toInt()==index){tree->clearSelection();(*it)->setSelected(true);break;}
        first->findChild<QAction*>("editPlacement")->trigger();
        auto panel=first->findChild<Stp::PartTransformPanel*>();auto view=first->findChild<Stp::ViewerWidget*>();
        const auto original=view->measurementPartEntity(index).displayTransform;
        auto x=panel->findChild<QDoubleSpinBox*>("placementTranslation0");x->setValue(20);
        panel->findChild<QPushButton*>("previewPlacement")->click();
        gp_Trsf delta;delta.SetTranslation(gp_Vec(20,0,0));
        require(Stp::samePlacement(view->measurementPartEntity(index).displayTransform,delta*original),"preview not displayed");
        if(argc==3){QDir().mkpath(QString::fromLocal8Bit(argv[2]));QTest::qWait(100);
            require(workspace.grab().save(QDir(QString::fromLocal8Bit(argv[2])).filePath("placement-preview.png")),"preview screenshot");}
        panel->findChild<QPushButton*>("applyPlacement")->click();require(first->hasPlacementChanges(),"modified marker missing");
        require(Stp::samePlacement(view->measurementPartEntity(index).displayTransform,
            Stp::placedWorldTransform(*first->document(),panel->committedEdits(),index)),"display differs from saved placement");
        // Mesh replacement must retain the editor transform, independently of temporary offsets.
        view->setMeshQuality(view->meshQuality()==0?1:0);waitFor([&]{return !first->isBusy()&&!view->hasPendingWork();});
        require(Stp::samePlacement(view->measurementPartEntity(index).displayTransform,delta*original),"mesh rebuild lost edit");
        first->findChild<QAction*>("background_dark")->trigger();
        const auto dark=view->view()->BackgroundColor();
        panel->findChild<QComboBox*>("placementMode")->setCurrentIndex(1);
        panel->findChild<QComboBox*>("placementAxis")->setCurrentIndex(2);
        panel->findChild<QComboBox*>("placementPivot")->setCurrentIndex(2);
        panel->findChild<QDoubleSpinBox*>("placementAngle")->setValue(30);
        panel->findChild<QPushButton*>("previewPlacement")->click();
        if(argc==3){QTest::qWait(100);require(workspace.grab().save(QDir(QString::fromLocal8Bit(argv[2])).filePath("rotation-dark-preview.png")),"rotation screenshot");}
        panel->cancelPreview();panel->findChild<QComboBox*>("placementMode")->setCurrentIndex(0);
        const QString secondPath=settings.filePath("second.step");require(QFile::copy(QString::fromLocal8Bit(argv[1]),secondPath),"copy fixture");
        workspace.openFiles({secondPath});auto second=workspace.currentDocument();require(second!=first,"second document");
        waitFor([&]{return second->findChild<QAction*>("editPlacement")->isEnabled();});
        auto secondView=second->findChild<Stp::ViewerWidget*>();
        require(secondView->view()->BackgroundColor().Distance(dark)<1.e-8,"new tab background not restored");
        require(!second->hasPlacementChanges(),"edits leaked to another document");
        second->findChild<QAction*>("background_white")->trigger();
        require(view->view()->BackgroundColor().Distance(secondView->view()->BackgroundColor())<1.e-8,"background not shared across tabs");
        auto tabs=workspace.findChild<QTabWidget*>("documentTabs");tabs->setCurrentWidget(first);
        require(tabs->tabText(tabs->currentIndex()).endsWith(" *"),"tab modified indicator missing");
        QTimer::singleShot(0,[]{for(auto widget:QApplication::topLevelWidgets())if(auto box=qobject_cast<QMessageBox*>(widget))box->done(QMessageBox::Cancel);});
        workspace.closeDocument(first);require(workspace.documentCount()==2,"cancelled close discarded edited document");
        const auto savedPath=settings.filePath("edited.stp");
        QTimer::singleShot(0,[&]{for(auto widget:QApplication::topLevelWidgets())if(auto dialog=qobject_cast<QFileDialog*>(widget)){
            dialog->selectFile(savedPath);QMetaObject::invokeMethod(dialog,"accept",Qt::DirectConnection);}});
        workspace.findChild<QAction*>("workspaceSaveAssembly")->trigger();
        waitFor([&]{return QFile::exists(savedPath)&&!first->isBusy()&&!first->hasPlacementChanges();});
        std::atomic_bool cancel(false);const auto saved=Stp::loadStep(savedPath,{},cancel);
        // Real assemblies can have identical names and paths. Match each instance once,
        // by both path and placement; a name-only lookup can select an unchanged sibling.
        std::vector<bool> used(saved->occurrences.size(),false);size_t sourceLeaves=0,savedLeaves=0;
        for(const auto& o:saved->occurrences)if(o.partIndex>=0)++savedLeaves;
        for(size_t i=0;i<first->document()->occurrences.size();++i){
            const auto& source=first->document()->occurrences[i];if(source.partIndex<0)continue;++sourceLeaves;
            const auto expected=Stp::placedWorldTransform(*first->document(),panel->committedEdits(),int(i));
            bool matched=false;
            for(size_t j=0;j<saved->occurrences.size();++j){const auto& o=saved->occurrences[j];
                if(!used[j]&&o.partIndex>=0&&o.path.endsWith(source.path)&&Stp::samePlacement(o.worldTransform,expected)){
                    used[j]=true;matched=true;break;}}
            require(matched,"UI-exported instance placement differs from display");
        }
        require(sourceLeaves==savedLeaves,"UI export changed instance count");
        require(!tabs->tabText(tabs->currentIndex()).endsWith(" *"),"saved tab remains dirty");
        if(argc==3){QTest::qWait(100);require(workspace.grab().save(QDir(QString::fromLocal8Bit(argv[2])).filePath("background-white.png")),"background screenshot");}
        workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();});
        std::cout<<"PASS: UI selection/preview/apply, displayed-vs-export placement, mesh rebuild, backgrounds across tabs, document isolation, dirty marker, cancelled close, File Save STEP and reopen\n";return 0;
    }catch(const Standard_Failure& e){std::cerr<<e.GetMessageString()<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
