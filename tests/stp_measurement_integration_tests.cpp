#include "document_workspace.h"
#include "main_window.h"
#include "measurement_panel.h"
#include "viewer_widget.h"
#include "cad_job_queue.h"
#include "../src/app/qtopengl_utils.h"
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QClipboard>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool waitFor(const std::function<bool()>& condition,int ms=30000){
    QElapsedTimer timer;timer.start();
    while(!condition()&&timer.elapsed()<ms)QTest::qWait(10);
    return condition();
}
template<typename T>T* control(QObject* object,const char* name){
    auto result=object->findChild<T*>(QString::fromLatin1(name));
    if(!result)if(auto panel=qobject_cast<Stp::MeasurementPanel*>(object))result=panel->recordsPage()->findChild<T*>(QString::fromLatin1(name));
    require(result,"missing measurement control");return result;
}
void fixture(const TopoDS_Shape& shape,const QString& path){
    STEPControl_Writer writer;require(writer.Transfer(shape,STEPControl_AsIs)==IFSelect_RetDone,"fixture transfer failed");
    require(writer.Write(path.toUtf8().constData())==IFSelect_RetDone,"fixture write failed");
}
Stp::MeasurementEntity cylinderFace(Stp::MainWindow* page){
    auto view=control<Stp::ViewerWidget>(page,"viewerWidget");
    auto document=page->document();
    for(size_t i=0;i<document->occurrences.size();++i){
        if(document->occurrences[i].partIndex<0)continue;
        auto entity=view->measurementPartEntity(int(i));
        for(TopExp_Explorer face(entity.shape,TopAbs_FACE);face.More();face.Next()){
            BRepAdaptor_Surface surface(TopoDS::Face(face.Current()));
            if(surface.GetType()!=GeomAbs_Cylinder)continue;
            entity.shape=face.Current();entity.selection=Stp::MeasurementSelection::Face;
            QString error;require(Stp::refineMeasurementPick(entity,gp_Pnt(10,0,15),&error),"precise face pick failed");
            return entity;
        }
    }
    throw std::runtime_error("no cylinder face");
}
void chooseKind(Stp::MeasurementPanel* panel,Stp::MeasurementKind kind){
    auto combo=control<QComboBox>(panel,"measurementKind");const auto index=combo->findData(int(kind));
    require(index>=0,"measurement kind unavailable");combo->setCurrentIndex(index);
}
void compute(Stp::MainWindow* page,Stp::MeasurementKind kind,const Stp::MeasurementEntity& entity,int count){
    auto panel=control<Stp::MeasurementPanel>(page,"measurementPanel");
    chooseKind(panel,kind);
    auto selection=control<QComboBox>(panel,"measurementSelectionA");selection->setCurrentIndex(selection->findData(int(entity.selection)));
    auto view=page->findChild<Stp::ViewerWidget*>();require(view,"no viewer");
    view->measurementEntityPicked(entity,0);
    auto button=control<QPushButton>(panel,"measurementCompute");require(button->isEnabled(),"compute unexpectedly disabled");
    button->click();require(waitFor([&]{return panel->recordCount()==count;}),"measurement did not append record");
}
}

int main(int argc,char** argv){
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#if defined(Q_OS_MACOS)
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    QApplication app(argc,argv);app.setStyle(QStyleFactory::create("Fusion"));
    auto palette=app.palette();
    for(auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})palette.setColor(role,QColor("#233a56"));
    palette.setColor(QPalette::Window,QColor("#eef2f7"));palette.setColor(QPalette::Base,Qt::white);
    palette.setColor(QPalette::Button,QColor("#f0f4f9"));palette.setColor(QPalette::AlternateBase,QColor("#f6f8fb"));
    app.setPalette(palette);
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):QStringLiteral("build/validation/measurement");QDir().mkpath(output);
    QJsonObject report;
    try{
        QTemporaryDir fixtures;require(fixtures.isValid(),"fixture directory unavailable");
        const auto path1=fixtures.filePath("cylinder.step"),path2=fixtures.filePath("sphere.step");
        fixture(BRepPrimAPI_MakeCylinder(10,30).Shape(),path1);fixture(BRepPrimAPI_MakeSphere(5).Shape(),path2);
        QFile::remove(output+"/measurement-cylinder.step");QFile::copy(path1,output+"/measurement-cylinder.step");
        Stp::DocumentWorkspace workspace;workspace.resize(1440,900);workspace.show();
        workspace.openFiles({path1,path2});
        auto tabs=workspace.findChild<QTabWidget*>(QStringLiteral("documentTabs"));
        if(!tabs)tabs=workspace.findChild<QTabWidget*>();require(tabs,"no document tabs");
        require(waitFor([&]{return workspace.documentCount()==2&&workspace.jobQueue()->idle();}),"files did not load");
        tabs->setCurrentIndex(0);QTest::qWait(200);
        auto page=workspace.currentDocument();require(page&&waitFor([&]{return page->document()&&!page->isBusy();}),"cylinder not ready");
        auto view=page->findChild<Stp::ViewerWidget*>();require(view,"no viewer");
        // Use a stable test name independent of the existing viewport's name.
        view->setObjectName(QStringLiteral("viewerWidget"));
        auto panel=control<Stp::MeasurementPanel>(page,"measurementPanel");
        auto action=control<QAction>(page,"toggleMeasurement");require(action->isEnabled(),"measurement entry disabled");
        action->setChecked(true);
        const auto entity=cylinderFace(page);
        compute(page,Stp::MeasurementKind::Radius,entity,1);
        compute(page,Stp::MeasurementKind::Diameter,entity,2);
        require(std::abs(panel->records()[0].result.values.front().value-10)<1e-6,"incorrect radius");
        require(std::abs(panel->records()[1].result.values.front().value-20)<1e-6,"incorrect diameter");
        require(panel->records()[0].id!=panel->records()[1].id,"record IDs reused");
        report["independentRadiusDiameter"]=true;
        compute(page,Stp::MeasurementKind::Area,entity,3);
        auto part=view->measurementPartEntity(entity.occurrence);
        compute(page,Stp::MeasurementKind::Volume,part,4);
        require(control<QTableWidget>(panel,"measurementRecords")->rowCount()==4,"records not all visible");
        auto recordsTable=control<QTableWidget>(panel,"measurementRecords");
        recordsTable->item(0,0)->setCheckState(Qt::Unchecked);
        require(!panel->records()[0].visible&&panel->recordCount()==4,"hiding annotation discarded or failed to hide record");
        recordsTable->item(0,0)->setCheckState(Qt::Checked);
        require(panel->records()[0].visible,"showing annotation failed");
        report["individualAnnotationVisibility"]=true;
        control<QSpinBox>(panel,"measurementDecimals")->setValue(6);
        control<QPushButton>(panel,"measurementCopyAll")->click();
        require(QApplication::clipboard()->text().contains("M1")&&QApplication::clipboard()->text().contains("M4"),"copy omitted records");
        control<QPushButton>(panel,"measurementShowRecords")->click();QTest::qWait(80);
        require(!view->measurementPicking()&&panel->recordCount()==4,"record tab did not pause picking or preserve records");
        workspace.grab().save(output+"/measurement-history.png");view->saveScreenshot(output+"/measurement-model.png");
        report["separateHistoryPausesPicking"]=true;
        control<QTabWidget>(page,"detailsTabs")->setCurrentWidget(panel);QTest::qWait(50);
        require(view->measurementPicking(),"returning to operation tab did not resume picking");
        compute(page,Stp::MeasurementKind::VolumeCenter,part,5);
        require(panel->records().back().request.kind==Stp::MeasurementKind::VolumeCenter,"independent volume center entry missing");
        require(control<QComboBox>(panel,"measurementKind")->findData(int(Stp::MeasurementKind::CenterOfMass))>=0
            &&control<QComboBox>(panel,"measurementKind")->findData(int(Stp::MeasurementKind::Mass))>=0,"physical measurements removed");
        report["independentVolumeCenterRetainsPhysicalEntries"]=true;
        auto& definition=page->document()->parts[size_t(page->document()->occurrences[size_t(part.occurrence)].partIndex)];
        QString densityError;require(Stp::setManualDensity(definition,QStringLiteral("1000"),&densityError),"test density unavailable");
        compute(page,Stp::MeasurementKind::Mass,part,6);
        compute(page,Stp::MeasurementKind::CenterOfMass,part,7);
        const auto oldMass=panel->records()[5].result.values.front().value;
        require(Stp::setManualDensity(definition,QStringLiteral("2000"),&densityError),"test density update failed");
        compute(page,Stp::MeasurementKind::Mass,part,8);
        require(std::abs(panel->records()[7].result.values.front().value-2*oldMass)<1.e-9
            &&panel->records()[5].request.a.front().densityKgM3==1000.,"density update overwrote history or failed to capture new snapshot");
        report["massAndCentersDensitySnapshots"]=true;
        auto center=part;
        for(TopExp_Explorer edge(part.shape,TopAbs_EDGE);edge.More();edge.Next())
            if(BRepAdaptor_Curve(TopoDS::Edge(edge.Current())).GetType()==GeomAbs_Circle){center.shape=edge.Current();break;}
        require(center.shape.ShapeType()==TopAbs_EDGE,"circle center fixture missing");
        center.selection=Stp::MeasurementSelection::FeatureCenter;
        compute(page,Stp::MeasurementKind::Point,center,9);
        auto axis=entity;axis.selection=Stp::MeasurementSelection::FeatureAxis;
        auto vertex=part;TopExp_Explorer firstVertex(part.shape,TopAbs_VERTEX);require(firstVertex.More(),"axis distance vertex missing");
        vertex.shape=firstVertex.Current();vertex.selection=Stp::MeasurementSelection::Vertex;
        control<QComboBox>(panel,"measurementOperation")->setCurrentIndex(1);chooseKind(panel,Stp::MeasurementKind::Distance);
        for(int slot=0;slot<2;++slot){
            const auto selected=slot==0?axis:vertex;
            control<QComboBox>(panel,slot==0?"measurementSelectionA":"measurementSelectionB")->setCurrentIndex(
                control<QComboBox>(panel,slot==0?"measurementSelectionA":"measurementSelectionB")->findData(int(selected.selection)));
            view->measurementEntityPicked(selected,slot);
        }
        control<QPushButton>(panel,"measurementCompute")->click();require(waitFor([&]{return panel->recordCount()==10;}),"axis UI measurement missing");
        require(std::abs(panel->records().back().result.values.front().value-10)<1.e-6,"axis UI distance wrong");
        report["circleCenterAndInfiniteAxisEntries"]=true;
        Stp::MeasurementEntity straight,planar;
        for (TopExp_Explorer edge(part.shape,TopAbs_EDGE);edge.More();edge.Next())
            if (BRepAdaptor_Curve(TopoDS::Edge(edge.Current())).GetType()==GeomAbs_Line) {
                straight=part;straight.shape=edge.Current();straight.selection=Stp::MeasurementSelection::Edge;break;
            }
        for (TopExp_Explorer face(part.shape,TopAbs_FACE);face.More();face.Next())
            if (BRepAdaptor_Surface(TopoDS::Face(face.Current())).GetType()==GeomAbs_Plane) {
                planar=part;planar.shape=face.Current();planar.selection=Stp::MeasurementSelection::Face;break;
            }
        require(!straight.shape.IsNull()&&!planar.shape.IsNull(),"extension fixture missing");
        auto extentA=control<QCheckBox>(panel,"measurementExtentA"),extentB=control<QCheckBox>(panel,"measurementExtentB");
        for (int slot=0;slot<2;++slot) {
            const auto& source=slot==0?straight:planar;
            auto selection=control<QComboBox>(panel,slot==0?"measurementSelectionA":"measurementSelectionB");
            selection->setCurrentIndex(selection->findData(int(source.selection)));view->measurementEntityPicked(source,slot);
            auto extent=slot==0?extentA:extentB;
            require(extent->isVisible()&&extent->isEnabled()&&!extent->isChecked(),"extension eligibility/default incorrect");
            extent->setChecked(true);
        }
        control<QPushButton>(panel,"measurementCompute")->click();
        require(!extentA->isEnabled()&&!extentB->isEnabled(),"extension editable during calculation");
        require(waitFor([&]{return panel->recordCount()==11;}),"both-expanded measurement missing");
        require(panel->records().back().request.a.front().extent==Stp::MeasurementExtent::Infinite
            &&panel->records().back().request.b.front().extent==Stp::MeasurementExtent::Infinite,"extension not captured in snapshot");
        chooseKind(panel,Stp::MeasurementKind::Angle);
        require(extentA->isChecked()&&extentB->isChecked(),"distance-angle switch lost compatible draft settings");
        control<QPushButton>(panel,"measurementCompute")->click();require(waitFor([&]{return panel->recordCount()==12;}),"expanded angle missing");
        require(std::abs(panel->records().back().result.values.front().value-90.)<1.e-6,"expanded angle incorrect");
        extentA->setChecked(false);
        require(panel->records()[10].request.a.front().extent==Stp::MeasurementExtent::Infinite,"checkbox edited old snapshot");
        extentA->setChecked(true);
        view->measurementEntityPicked(straight,0);require(extentA->isChecked(),"eligible repick lost expansion");
        view->measurementEntityPicked(entity,1);
        require(!extentB->isEnabled()&&!extentB->isChecked()&&extentB->toolTip().contains(QStringLiteral("曲面")),"unsupported surface not disabled/explained");
        view->measurementEntityPicked(planar,1);extentB->setChecked(true);
        auto arc=center;arc.selection=Stp::MeasurementSelection::Edge;
        view->measurementEntityPicked(arc,0);
        require(!extentA->isEnabled()&&!extentA->isChecked()&&extentA->toolTip().contains(QStringLiteral("曲线")),"arc extension not disabled/explained");
        view->measurementEntityPicked(straight,0);extentA->setChecked(true);
        control<QPushButton>(panel,"measurementCopyAll")->click();
        require(QApplication::clipboard()->text().contains(QStringLiteral("无限直线"))&&QApplication::clipboard()->text().contains(QStringLiteral("无限平面")),"copy omitted reference extents");
        control<QPushButton>(panel,"measurementShowRecords")->click();QTest::qWait(50);
        require(extentA->isChecked()&&extentB->isChecked()&&!view->measurementPicking(),"history page lost expansion settings");
        control<QTabWidget>(page,"detailsTabs")->setCurrentWidget(panel);
        report["independentInfiniteLinePlaneAndAngle"]=true;
        report["extentEligibilitySnapshotsAndCopy"]=true;
        workspace.grab().save(output+"/measurement-operation.png");
        control<QPushButton>(panel,"measurementShowRecords")->click();QTest::qWait(80);
        workspace.grab().save(output+"/measurement-history.png");
        control<QTabWidget>(page,"detailsTabs")->setCurrentWidget(panel);
        control<QComboBox>(panel,"measurementOperation")->setCurrentIndex(0);
        action->setChecked(false);require(panel->recordCount()==12,"leaving mode discarded history");
        tabs->setCurrentIndex(1);QTest::qWait(200);
        auto sphere=workspace.currentDocument();require(sphere&&sphere!=page,"wrong document routing");
        auto spherePanel=control<Stp::MeasurementPanel>(sphere,"measurementPanel");
        require(spherePanel->recordCount()==0,"records leaked across files");
        tabs->setCurrentIndex(0);QTest::qWait(200);require(panel->recordCount()==12,"tab switch discarded history");
        report["multiDocumentHistory"]=true;
        action->setChecked(true);control<QPushButton>(panel,"measurementClearAll")->click();
        require(panel->recordCount()==0,"clear all left records");
        require(!extentA->isChecked()&&!extentB->isChecked(),"clear did not reset extension states");
        require(!control<QPushButton>(panel,"measurementClearAll")->isEnabled(),"empty clear button enabled");
        std::atomic_bool release(false),started(false);
        auto blocker=workspace.jobQueue()->enqueue(999,[&](std::atomic_bool& cancel,const Stp::ProgressCallback&){
            started=true;while(!release.load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },{},{});
        require(waitFor([&]{return started.load();}),"queue blocker did not start");
        control<QComboBox>(panel,"measurementOperation")->setCurrentIndex(1);chooseKind(panel,Stp::MeasurementKind::Distance);
        for (int slot=0;slot<2;++slot) {
            const auto& source=slot==0?straight:planar;
            auto selection=control<QComboBox>(panel,slot==0?"measurementSelectionA":"measurementSelectionB");
            selection->setCurrentIndex(selection->findData(int(source.selection)));view->measurementEntityPicked(source,slot);
            (slot==0?extentA:extentB)->setChecked(true);
        }
        control<QPushButton>(panel,"measurementCompute")->click();
        require(control<QPushButton>(panel,"measurementClearAll")->isEnabled(),"pending clear button disabled");
        control<QPushButton>(panel,"measurementClearAll")->click();release=true;
        require(waitFor([&]{return workspace.jobQueue()->idle();}),"queue did not drain");QTest::qWait(100);
        require(panel->recordCount()==0,"late callback resurrected a cleared record");
        require(!extentA->isChecked()&&!extentB->isChecked(),"pending extension clear did not reset states");
        report["clearPendingCannotResurrect"]=true;
        report["clearPendingInfiniteCannotResurrect"]=true;
        control<QComboBox>(panel,"measurementOperation")->setCurrentIndex(0);chooseKind(panel,Stp::MeasurementKind::Radius);
        release=false;started=false;
        workspace.jobQueue()->enqueue(999,[&](std::atomic_bool& cancel,const Stp::ProgressCallback&){
            started=true;while(!release.load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },{},{});
        require(waitFor([&]{return started.load();}),"switch blocker did not start");
        view->measurementEntityPicked(entity,0);
        control<QPushButton>(panel,"measurementCompute")->click();
        tabs->setCurrentIndex(1);release=true;
        require(waitFor([&]{return workspace.jobQueue()->idle();}),"switch queue did not drain");QTest::qWait(100);
        require(panel->recordCount()==0&&spherePanel->recordCount()==0,"switch allowed a stale or cross-file result");
        tabs->setCurrentIndex(0);QTest::qWait(100);
        report["switchPendingCannotResurrect"]=true;
        compute(page,Stp::MeasurementKind::Radius,entity,1);
        control<QPushButton>(panel,"measurementDeleteSelected")->click();
        // Deletion may require explicit row selection.
        if(panel->recordCount()){control<QTableWidget>(panel,"measurementRecords")->selectRow(0);control<QPushButton>(panel,"measurementDeleteSelected")->click();}
        require(panel->recordCount()==0,"single record deletion failed");
        require(!control<QPushButton>(panel,"measurementClearAll")->isEnabled(),"clear enabled with an empty idle history");
        report["deleteSingleRecord"]=true;
        compute(page,Stp::MeasurementKind::Radius,entity,1);
        release=false;started=false;
        workspace.jobQueue()->enqueue(999,[&](std::atomic_bool& cancel,const Stp::ProgressCallback&){
            started=true;while(!release.load()&&!cancel.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },{},{});
        require(waitFor([&]{return started.load();}),"close blocker did not start");
        control<QPushButton>(panel,"measurementCompute")->click();
        workspace.closeAll();release=true;
        require(waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();}),"closing sessions failed");
        workspace.openFiles({path1});require(waitFor([&]{auto p=workspace.currentDocument();return p&&p->document()&&!p->isBusy();}),"reopen failed");
        require(control<Stp::MeasurementPanel>(workspace.currentDocument(),"measurementPanel")->recordCount()==0,"reopen restored discarded history");
        report["closePendingAndReopenEmpty"]=true;
        workspace.closeAll();require(waitFor([&]{return workspace.jobQueue()->idle();}),"final queue did not drain");
        report["passed"]=true;
    }catch(const std::exception& error){report["passed"]=false;report["error"]=QString::fromUtf8(error.what());}
    QSaveFile file(output+"/measurement-integration-report.json");
    if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(report).toJson());file.commit();}
    std::cout<<QJsonDocument(report).toJson().toStdString();return report.value("passed").toBool()?0:1;
}
