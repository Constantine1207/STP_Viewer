#include "stp/document_workspace.h"
#include "stp/main_window.h"
#include "stp/viewer_widget.h"
#include "stp/section_panel.h"
#include "stp/part_transform_panel.h"
#include "stp/assembly_export.h"
#include "stp/cad_job_queue.h"
#include "app/qtopengl_utils.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <Quantity_Color.hxx>
#include <TDataStd_Name.hxx>
#include <QApplication>
#include <QImage>
#include <cmath>
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTest>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <V3d_TypeOfOrientation.hxx>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void waitFor(const std::function<bool()>& predicate){QElapsedTimer timer;timer.start();while(!predicate()){
    if(timer.elapsed()>90000)throw std::runtime_error("GUI timeout");QTest::qWait(20);}}
int leaf(const Stp::CadDocument& doc,const QString& suffix){for(size_t i=0;i<doc.occurrences.size();++i)
    if(doc.occurrences[i].partIndex>=0&&doc.occurrences[i].path.endsWith(suffix))return int(i);throw std::runtime_error("missing leaf");}
void select(Stp::MainWindow* window,int index){auto tree=window->findChild<QTreeWidget*>("assemblyTree");tree->clearSelection();
    for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toInt()==index){(*it)->setSelected(true);return;}}
void makeFixture(const QString& path){
    Stp::CadDocument doc;doc.xde=new TDocStd_Document("BinXCAF");doc.sourcePath="cap-fixture.step";
    XCAFDoc_DocumentTool::Set(doc.xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(doc.xde,.001);
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());const auto colors=XCAFDoc_DocumentTool::ColorTool(doc.xde->Main());
    auto box=shapes->AddShape(BRepPrimAPI_MakeBox(20,20,20).Shape(),false);TDataStd_Name::Set(box,TCollection_ExtendedString("Box"));
    BRepBuilderAPI_MakePolygon outer;for(auto p:{gp_Pnt(80,0,0),gp_Pnt(100,0,0),gp_Pnt(100,20,0),gp_Pnt(80,20,0)})outer.Add(p);outer.Close();
    BRepBuilderAPI_MakePolygon inner;for(auto p:{gp_Pnt(86,6,0),gp_Pnt(86,14,0),gp_Pnt(94,14,0),gp_Pnt(94,6,0)})inner.Add(p);inner.Close();
    BRepBuilderAPI_MakeFace face(outer.Wire());face.Add(inner.Wire());
    auto ring=shapes->AddShape(BRepPrimAPI_MakePrism(face.Face(),gp_Vec(0,0,20)).Shape(),false);TDataStd_Name::Set(ring,TCollection_ExtendedString("Ring"));
    TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);auto root=shapes->AddShape(compound,false);TDataStd_Name::Set(root,TCollection_ExtendedString("Assembly"));
    auto add=[&](const TDF_Label& part,const char* name,double x,const QColor& c){gp_Trsf t;t.SetTranslation(gp_Vec(x,0,0));
        const auto label=shapes->AddComponent(root,part,TopLoc_Location(t));TDataStd_Name::Set(label,TCollection_ExtendedString(name));
        colors->SetColor(label,Quantity_Color(c.redF(),c.greenF(),c.blueF(),Quantity_TOC_sRGB),XCAFDoc_ColorGen);};
    add(box,"Red",0,QColor("#e52b24"));add(box,"Blue",40,QColor("#2366d0"));add(ring,"Ring",0,QColor("#279540"));
    shapes->UpdateAssemblies();Stp::rebuildIndex(doc);std::atomic_bool cancel(false);Stp::exportPlacedAssembly(doc,{},path,{},cancel);
}
QColor pixel(const QImage& image,Stp::ViewerWidget* view,const gp_Pnt& point){int x,y;view->view()->Convert(point.X(),point.Y(),point.Z(),x,y);
    require(x>=2&&y>=2&&x<image.width()-2&&y<image.height()-2,"sample outside framebuffer");return image.pixelColor(x,y);}
}
int main(int argc,char** argv){
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);
    QTemporaryDir temporary;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    app.setOrganizationName("STPSectionColorTest");app.setApplicationName("STPSectionColorTest");
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):temporary.filePath("evidence");QDir().mkpath(output);
    try{
        const QString source=temporary.filePath(QStringLiteral("剖切 颜色.step"));makeFixture(source);
        Stp::DocumentWorkspace workspace;workspace.resize(1350,950);workspace.show();workspace.openFiles({source});
        auto window=workspace.currentDocument();waitFor([&]{return window->findChild<QAction*>("editPlacement")->isEnabled();});
        auto view=window->findChild<Stp::ViewerWidget*>();auto panel=window->findChild<Stp::SectionPanel*>();
        QObject::connect(view,&Stp::ViewerWidget::message,[](const QString& text){std::cout<<text.toStdString()<<'\n';});
        const int red=leaf(*window->document(),"/Red"),blue=leaf(*window->document(),"/Blue"),ring=leaf(*window->document(),"/Ring");
        select(window,red);
        QTimer::singleShot(0,[]{for(auto w:QApplication::topLevelWidgets())if(auto d=qobject_cast<QColorDialog*>(w)){d->setCurrentColor(QColor("#e52b24"));d->accept();}});
        window->findChild<QAction*>("setPartColor")->trigger();require(window->colorEdits().count(red)==1&&window->hasPlacementChanges(),"color dialog or dirty marker");
        require(window->setPartColors({red,blue},QColor("#8030b0")),"multi-part color");
        window->findChild<QAction*>("undoPartColor")->trigger();require(view->occurrenceColor(blue)!=QColor("#8030b0"),"color undo");
        window->findChild<QAction*>("redoPartColor")->trigger();require(view->occurrenceColor(blue)==QColor("#8030b0"),"color redo");
        require(window->restorePartColors({blue}),"restore color");require(view->occurrenceColor(blue).blue()>view->occurrenceColor(blue).red()*2,"sibling color restore");
        require(window->setPartColors({red},QColor("#e52b24")),"set red");
        // Check the actual body pixels as well as the independently styled caps.
        window->findChild<QAction*>("background_white")->trigger();view->view()->SetProj(V3d_Zpos);view->fitAll();
        window->setPartColors({red},QColor("#8030b0"));view->selectOccurrences({});QTest::qWait(150);
        require(view->saveScreenshot(output+"/recolored-body.png"),"body screenshot");
        const auto body=pixel(QImage(output+"/recolored-body.png"),view,gp_Pnt(10,10,20));
        require(body.red()>body.green()*1.7&&body.blue()>body.green()*1.7,"body pixels did not follow recolor");
        window->setPartColors({red},QColor("#e52b24"));
        // Exercise actual planar-face picking in the OpenGL viewport.
        panel->findChild<QPushButton*>("pickSectionPlane")->click();require(view->sectionPicking(),"pick mode not entered");
        auto viewport=view->findChild<QWidget*>("cadViewport");int px,py;view->view()->Convert(10.,10.,20.,px,py);
        const double ratio=viewport->devicePixelRatioF();QTest::mouseClick(viewport,Qt::LeftButton,Qt::NoModifier,QPoint(qRound(px/ratio),qRound(py/ratio)));
        waitFor([&]{return !view->sectionPicking();});require(panel->findChild<QComboBox*>("sectionAxis")->currentIndex()==4,"face picking not applied");
        require(std::abs(view->sectionState().base.Location().Z()-20.)<1.e-6,"picked plane world position");
        panel->findChild<QPushButton*>("pickSectionPlane")->click();QTest::keyClick(viewport,Qt::Key_Escape);require(!view->sectionPicking(),"Esc did not cancel picking");
        panel->findChild<QComboBox*>("sectionRotationAxis")->setCurrentIndex(1);panel->findChild<QDoubleSpinBox*>("sectionAngle")->setValue(30);
        panel->findChild<QDoubleSpinBox*>("sectionOffset")->setValue(-10);
        require(std::abs(Stp::sectionPlane(view->sectionState()).Axis().Direction().X())>.49,"rotation UI not connected");
        view->view()->SetProj(V3d_XposYnegZpos);view->fitAll();QTest::qWait(100);require(workspace.grab().save(output+"/custom-plane-window.png"),"plane screenshot");
        // A projected interior point verifies per-instance cap colors and the hole.
        panel->findChild<QComboBox*>("sectionAxis")->setCurrentIndex(3);panel->findChild<QCheckBox*>("sectionReverse")->setChecked(true);
        view->selectOccurrences({});view->view()->SetProj(V3d_Zpos);view->fitAll();QTest::qWait(150);
        require(view->saveScreenshot(output+"/caps.png"),"cap screenshot");QImage image(output+"/caps.png");
        auto r=pixel(image,view,gp_Pnt(10,10,10)),b=pixel(image,view,gp_Pnt(50,10,10));
        auto g=pixel(image,view,gp_Pnt(82,10,10)),hole=pixel(image,view,gp_Pnt(90,10,10));
        require(r.red()>r.green()*1.6&&r.red()>r.blue()*1.6,"red cap color");
        require(b.blue()>b.red()*1.6,"blue cap color");require(g.green()>g.red()*1.4,"ring cap color");
        require(hole.red()>230&&hole.green()>230&&hole.blue()>230,"cap incorrectly fills through-hole");
        require(window->setPartColors({red},QColor("#e09010")),"recolor capped part");
        view->selectOccurrences({});QTest::qWait(100);view->saveScreenshot(output+"/recolored-caps.png");image.load(output+"/recolored-caps.png");
        r=pixel(image,view,gp_Pnt(10,10,10));require(r.red()>r.blue()*2&&r.green()>r.blue()*2,"cap did not follow recolor");
        view->setTransparency(60,false);view->setTransparency(0,false);require(view->occurrenceColor(red)==QColor("#e09010"),"transparency erased color");
        view->setMeshQuality(0);waitFor([&]{return !window->isBusy()&&!view->hasPendingWork();});
        require(view->occurrenceColor(red)==QColor("#e09010")&&view->sectionState().enabled,"mesh rebuild lost color/section");
        // Placement is persistent; section and cap remain display-only.
        select(window,red);window->findChild<QAction*>("editPlacement")->trigger();auto placement=window->findChild<Stp::PartTransformPanel*>();
        placement->findChild<QDoubleSpinBox*>("placementTranslation0")->setValue(5);placement->findChild<QPushButton*>("applyPlacement")->click();
        const auto destination=temporary.filePath(QStringLiteral("完整几何 新颜色.stp"));
        QTimer::singleShot(0,[&]{for(auto w:QApplication::topLevelWidgets())if(auto d=qobject_cast<QFileDialog*>(w)){d->selectFile(destination);QMetaObject::invokeMethod(d,"accept",Qt::DirectConnection);}});
        workspace.findChild<QAction*>("workspaceSaveAssembly")->trigger();waitFor([&]{return QFile::exists(destination)&&!window->isBusy()&&!window->hasPlacementChanges();});
        std::atomic_bool cancel(false);auto loaded=Stp::loadStep(destination,{},cancel);const int back=leaf(*loaded,"/Red");
        require(Stp::occurrenceBaseColor(*loaded,back)==QColor("#e09010"),"saved color roundtrip");
        require(Stp::samePlacement(loaded->occurrences[back].worldTransform,Stp::placedWorldTransform(*window->document(),placement->committedEdits(),red),1.e-7),"saved placement roundtrip");
        const auto& part=loaded->parts[loaded->occurrences[back].partIndex];require(part.volumeMm3&&std::abs(*part.volumeMm3-8000)<1.e-5,"section cut exported geometry");
        const auto secondPath=temporary.filePath("second.step");require(QFile::copy(source,secondPath),"second fixture");workspace.openFiles({secondPath});
        auto second=workspace.currentDocument();waitFor([&]{return second->findChild<QAction*>("editPlacement")->isEnabled();});
        require(second->colorEdits().empty()&&!second->findChild<Stp::ViewerWidget*>()->sectionState().enabled,"color/section leaked across documents");
        auto tabs=workspace.findChild<QTabWidget*>("documentTabs");tabs->setCurrentWidget(window);
        require(window->restorePartColors({red})&&window->hasPlacementChanges(),"restore after save is not dirty");
        QTimer::singleShot(0,[]{for(auto w:QApplication::topLevelWidgets())if(auto box=qobject_cast<QMessageBox*>(w))box->done(QMessageBox::Cancel);});
        workspace.closeDocument(window);require(workspace.documentCount()==2,"cancel close discarded color edits");
        window->findChild<QAction*>("undoPartColor")->trigger();require(!window->hasPlacementChanges(),"undo back to saved state remains dirty");
        workspace.closeAll();waitFor([&]{return workspace.documentCount()==0&&workspace.jobQueue()->idle();});
        QFile report(output+"/section-color-report.json");require(report.open(QIODevice::WriteOnly),"write report");
        report.write(QJsonDocument(QJsonObject{{"status","passed"},{"actualFacePicking",true},{"capColorsAndThroughHole",true},
            {"colorDialogUndoRedo",true},{"placementColorFullGeometryStepRoundtrip",true},{"meshAndDocumentIsolation",true},{"unsavedClose",true}}).toJson());
        std::cout<<"PASS: planar face mouse picking/Esc, plane controls, actual per-part cap pixels and through-hole, color dialog/multi/undo/redo/restore, transparency/mesh, STEP File Save with full geometry and placement/color, tabs and unsaved close\n";return 0;
    }catch(const Standard_Failure& e){std::cerr<<e.GetMessageString()<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
