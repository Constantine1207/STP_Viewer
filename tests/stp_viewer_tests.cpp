#include "viewer_widget.h"
#include "../src/app/qtopengl_utils.h"
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Graphic3d_Camera.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TColStd_ListOfInteger.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool nearEqual(double a,double b){return std::abs(a-b)<1.e-6;}
bool same(const gp_Trsf& a,const gp_Trsf& b){
    for(int r=1;r<=3;++r)for(int c=1;c<=4;++c)if(!nearEqual(a.Value(r,c),b.Value(r,c)))return false;
    return true;
}
void load(Stp::ViewerWidget& view,const std::shared_ptr<Stp::CadDocument>& document,const Stp::PmiCatalog& pmi={}){
    std::atomic_bool cancel(false);Stp::prepareDisplayMeshes(*document,view.meshQuality(),cancel);
    QEventLoop loop;bool ready=false;QTimer timeout;timeout.setSingleShot(true);
    auto connection=QObject::connect(&view,&Stp::ViewerWidget::sceneReady,&loop,[&]{ready=true;loop.quit();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    timeout.start(30000);view.setDocument(document,pmi,true);loop.exec();QObject::disconnect(connection);
    require(ready,"viewer did not finish building scene");QTest::qWait(100);
}
std::vector<Handle(XCAFPrs_AISObject)> objects(Stp::ViewerWidget& view){
    AIS_ListOfInteractive items;view.context()->DisplayedObjects(items);
    std::vector<Handle(XCAFPrs_AISObject)> result;
    for(const auto& item:items){auto shape=Handle(XCAFPrs_AISObject)::DownCast(item);if(!shape.IsNull())result.push_back(shape);}
    return result;
}
std::vector<Handle(XCAFPrs_AISObject)> allObjects(Stp::ViewerWidget& view){
    AIS_ListOfInteractive items;view.context()->ObjectsInside(items);
    std::vector<Handle(XCAFPrs_AISObject)> result;
    for(const auto& item:items){auto shape=Handle(XCAFPrs_AISObject)::DownCast(item);if(!shape.IsNull())result.push_back(shape);}
    return result;
}
void quality(Stp::ViewerWidget& view,int value,bool cancel=false){
    QEventLoop loop;bool finished=false,cancelled=false;QTimer timeout;timeout.setSingleShot(true);
    auto ready=QObject::connect(&view,&Stp::ViewerWidget::meshQualityFinished,&loop,[&](int){finished=true;loop.quit();});
    auto stopped=QObject::connect(&view,&Stp::ViewerWidget::meshQualityCancelled,&loop,[&](int){cancelled=true;loop.quit();});
    auto progress=QObject::connect(&view,&Stp::ViewerWidget::meshQualityProgress,&loop,[&](int percent){if(cancel&&percent>=70)view.cancelSceneBuild();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    timeout.start(30000);require(view.setMeshQuality(value),"mesh quality change refused");loop.exec();
    QObject::disconnect(ready);QObject::disconnect(stopped);QObject::disconnect(progress);
    require(cancel?cancelled:finished,"mesh quality change did not finish/cancel");QTest::qWait(30);
}
int triangleCount(const TopoDS_Shape& shape){
    int result=0;
    for(TopExp_Explorer faces(shape,TopAbs_FACE);faces.More();faces.Next()){
        TopLoc_Location location;const auto mesh=BRep_Tool::Triangulation(TopoDS::Face(faces.Current()),location);
        if(!mesh.IsNull())result+=mesh->NbTriangles();
    }
    return result;
}
void drag(QWidget* viewport,Qt::MouseButton button,QPointF from,QPointF to){
    QMouseEvent press(QEvent::MouseButtonPress,from,from,button,button,Qt::NoModifier);
    QApplication::sendEvent(viewport,&press);
    QMouseEvent move(QEvent::MouseMove,to,to,Qt::NoButton,button,Qt::NoModifier);
    QApplication::sendEvent(viewport,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,to,to,button,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(viewport,&release);
}
void testRightPan(Stp::ViewerWidget& view,QWidget* viewport)
{
    const auto camera=view.view()->Camera();
    const Handle(Graphic3d_Camera) initial=new Graphic3d_Camera(camera);
    const auto selected=view.context()->NbSelected();
    const auto gesture=[viewport](QPointF from,QPointF to,int steps){
        QMouseEvent press(QEvent::MouseButtonPress,from,from,Qt::RightButton,Qt::RightButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&press);
        for(int i=1;i<=steps;++i){
            const auto pos=from+(to-from)*double(i)/steps;
            QMouseEvent move(QEvent::MouseMove,pos,pos,Qt::NoButton,Qt::RightButton,Qt::NoModifier);
            QApplication::sendEvent(viewport,&move);
        }
        QMouseEvent release(QEvent::MouseButtonRelease,to,to,Qt::RightButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&release);
    };
    for(bool perspective:{false,true}){
        camera->Copy(initial);view.setPerspective(perspective);
        const Handle(Graphic3d_Camera) before=new Graphic3d_Camera(camera);
        gesture({400,300},{490,360},6);
        const auto center=camera->Center();
        require(center.Distance(before->Center())>1.e-6,"Mac right drag did not pan");
        require((gp_Vec(before->Eye(),camera->Eye())-gp_Vec(before->Center(),center)).Magnitude()<1.e-6,"Mac pan did not translate eye and center equally");
        require(nearEqual(camera->Eye().Distance(center),before->Eye().Distance(before->Center())),"Mac pan changed eye-center distance");
        require(camera->Direction().IsEqual(before->Direction(),1.e-10)&&camera->Up().IsEqual(before->Up(),1.e-10),"Mac right pan rotated camera");
        require(nearEqual(camera->Scale(),before->Scale()),"Mac right pan changed zoom");
        require(std::abs(gp_Vec(before->Center(),center).Dot(gp_Vec(before->Direction())))<1.e-6,"Mac right pan moved along viewing direction");
        gesture({490,360},{400,300},6);
        require(camera->Center().Distance(before->Center())<1.e-6,"Mac reverse pan did not return to start");
        camera->Copy(before);gesture({400,300},{490,360},1);
        require(camera->Center().Distance(center)<1.e-6,"Mac pan depends on number of mouse move events");
        // A blank click keeps selection. A click on a part now deliberately
        // replaces selection and requests the per-instance context menu.
        QPointF blank;bool foundBlank=false;
        for(const QPointF candidate:{QPointF(8,8),QPointF(viewport->width()-8,8),QPointF(viewport->width()-8,viewport->height()-8)}){
            view.context()->MoveTo(qRound(candidate.x()*viewport->devicePixelRatioF()),qRound(candidate.y()*viewport->devicePixelRatioF()),view.view(),false);
            if(!view.context()->HasDetected()){blank=candidate;foundBlank=true;break;}
        }
        require(foundBlank,"Mac pan fixture has no blank click position");gesture(blank,blank,1);
        require(camera->Center().Distance(center)<1.e-6&&view.context()->NbSelected()==selected,"Mac blank right click moved camera or changed selection");
        QMouseEvent hover(QEvent::MouseMove,{550,400},{550,400},Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&hover);
        require(camera->Center().Distance(center)<1.e-6,"Mac pan continued after right-button release");
        QMouseEvent missedPress(QEvent::MouseButtonPress,{400,300},{400,300},Qt::RightButton,Qt::RightButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&missedPress);
        QMouseEvent withoutButton(QEvent::MouseMove,{550,400},{550,400},Qt::NoButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(viewport,&withoutButton);
        require(camera->Center().Distance(center)<1.e-6,"Mac pan continued after a missed release event");
    }
    QContextMenuEvent context(QContextMenuEvent::Mouse,{450,320},viewport->mapToGlobal(QPoint(450,320)));context.setAccepted(false);
    QApplication::sendEvent(viewport,&context);require(context.isAccepted(),"Mac viewport did not consume right-click context menu");
    camera->Copy(initial);
    QMouseEvent press(QEvent::MouseButtonPress,{400,300},{400,300},Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier);
    QMouseEvent move(QEvent::MouseMove,{470,350},{470,350},Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,{470,350},{470,350},Qt::LeftButton,Qt::NoButton,Qt::ShiftModifier);
    QApplication::sendEvent(viewport,&press);QApplication::sendEvent(viewport,&move);QApplication::sendEvent(viewport,&release);
    require(!camera->Direction().IsEqual(initial->Direction(),1.e-6),"Mac left drag with Shift did not keep rotation gesture");
    camera->Copy(initial);
    std::cout<<"RIGHT PAN PASS: orthographic/perspective, six-step and single-step equivalence, reverse, equal eye/center translation, unchanged orientation/zoom/selection, release/missed-release/context-menu, left rotation\n";
}
bool colorNear(const Quantity_ColorRGBA& a,const Quantity_ColorRGBA& b){
    return a.GetRGB().Distance(b.GetRGB())<1.e-6&&nearEqual(a.Alpha(),b.Alpha());
}
Quantity_ColorRGBA drawerColor(const Handle(Prs3d_Drawer)& drawer){
    const auto aspect=drawer->ShadingAspect()->Aspect();
    return Quantity_ColorRGBA(aspect->InteriorColor(),aspect->FrontMaterial().Alpha());
}
using StyleMap=std::vector<std::pair<TopoDS_Shape,Quantity_ColorRGBA>>;
bool sameStyleShape(const TopoDS_Shape& a,const TopoDS_Shape& b){
    if(a.IsPartner(b))return true;
    if(a.ShapeType()!=TopAbs_COMPOUND||b.ShapeType()!=TopAbs_COMPOUND)return false;
    std::vector<TopoDS_Shape> aChildren,bChildren;
    for(TopoDS_Iterator it(a);it.More();it.Next())aChildren.push_back(it.Value());
    for(TopoDS_Iterator it(b);it.More();it.Next())bChildren.push_back(it.Value());
    if(aChildren.size()!=bChildren.size())return false;std::vector<bool> matched(bChildren.size(),false);
    for(const auto& child:aChildren){bool found=false;for(size_t i=0;i<bChildren.size();++i)
        if(!matched[i]&&sameStyleShape(child,bChildren[i])){matched[i]=true;found=true;break;}if(!found)return false;}
    return true;
}
bool sameStyleMap(const StyleMap& a,const StyleMap& b){
    if(a.size()!=b.size())return false;std::vector<bool> matched(b.size(),false);
    for(const auto& style:a){bool found=false;for(size_t i=0;i<b.size();++i)
        if(!matched[i]&&sameStyleShape(style.first,b[i].first)&&colorNear(style.second,b[i].second)){matched[i]=true;found=true;break;}if(!found)return false;}
    return true;
}
StyleMap customStyles(const Handle(XCAFPrs_AISObject)& object){
    StyleMap result;for(AIS_DataMapOfShapeDrawer::Iterator it(object->CustomAspectsMap());it.More();it.Next())
        result.emplace_back(it.Key(),drawerColor(it.Value()));return result;
}
void testAppearanceAndPmiFailure(Stp::ViewerWidget& view){
    auto doc=std::make_shared<Stp::CadDocument>();doc->xde=new TDocStd_Document("BinXCAF");
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    auto colors=XCAFDoc_DocumentTool::ColorTool(doc->xde->Main());
    const auto plain=shapes->AddShape(BRepPrimAPI_MakeBox(8,8,8).Shape(),false);
    const auto colored=shapes->AddShape(BRepPrimAPI_MakeBox(10,10,10).Shape(),false);
    const Quantity_ColorRGBA red(Quantity_NOC_RED,0.6f),blue(Quantity_NOC_BLUE,0.7f),green(Quantity_NOC_GREEN,0.65f);
    const Quantity_ColorRGBA yellow(Quantity_NOC_YELLOW,0.8f),cyan(Quantity_NOC_CYAN1,0.4f),purple(Quantity_NOC_PURPLE,0.9f);
    const auto makeAssembly=[&](){TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);return shapes->AddShape(compound,true);};
    const auto locate=[](double x,double y){gp_Trsf transform;transform.SetTranslation(gp_Vec(x,y,0));return TopLoc_Location(transform);};
    const auto root=makeAssembly(),sub=makeAssembly();
    shapes->AddComponent(sub,plain,locate(0,0));
    shapes->AddComponent(sub,colored,locate(15,0));
    const auto coloredInstance=shapes->AddComponent(sub,colored,locate(30,0));
    const auto greenSub=shapes->AddComponent(root,sub,locate(0,0));
    shapes->AddComponent(root,sub,locate(0,30));
    shapes->AddComponent(root,plain,locate(0,60));
    colors->SetColor(root,red,XCAFDoc_ColorGen);colors->SetColor(sub,blue,XCAFDoc_ColorGen);
    colors->SetColor(greenSub,green,XCAFDoc_ColorSurf);
    colors->SetColor(colored,yellow,XCAFDoc_ColorSurf);colors->SetColor(coloredInstance,purple,XCAFDoc_ColorSurf);
    TopExp_Explorer faces(XCAFDoc_ShapeTool::GetShape(colored),TopAbs_FACE);
    colors->SetColor(shapes->AddSubShape(colored,faces.Current()),cyan,XCAFDoc_ColorSurf);
    shapes->UpdateAssemblies();Stp::rebuildIndex(*doc);load(view,doc);
    const auto presentations=objects(view);require(presentations.size()==7,"appearance fixture leaf count incorrect");
    struct Snapshot{Handle(XCAFPrs_AISObject) object;Quantity_ColorRGBA color;std::vector<std::pair<TopoDS_Shape,Quantity_ColorRGBA>> custom;};
    std::vector<Snapshot> original;int inheritedRed=0,inheritedBlue=0,inheritedGreen=0,faceColors=0,partColors=0,instanceColors=0;
    for(const auto& object:presentations){
        Snapshot snapshot{object,drawerColor(object->Attributes()),{}};
        inheritedRed+=colorNear(snapshot.color,red);inheritedBlue+=colorNear(snapshot.color,blue);inheritedGreen+=colorNear(snapshot.color,green);
        for(AIS_DataMapOfShapeDrawer::Iterator it(object->CustomAspectsMap());it.More();it.Next()){
            const auto color=drawerColor(it.Value());snapshot.custom.emplace_back(it.Key(),color);
            faceColors+=colorNear(color,cyan);partColors+=colorNear(color,yellow);instanceColors+=colorNear(color,purple);
        }
        original.push_back(std::move(snapshot));
    }
    require(inheritedRed==1&&inheritedBlue==3&&inheritedGreen==3,"ancestor color defaults or nearest occurrence priority lost");
    require(faceColors==4&&partColors==2&&instanceColors==2,"face, definition or occurrence color override lost");
    const auto checkTemporaryTransparency=[&](const Snapshot& snapshot,double transparency){
        const auto global=drawerColor(snapshot.object->Attributes());
        require(nearEqual(global.Alpha(),1-transparency),"temporary transparency did not affect inherited style");
        require(global.GetRGB().Distance(snapshot.color.GetRGB())<1.e-6,"temporary transparency changed source color");
        for(const auto& entry:snapshot.custom){
            const auto* drawer=snapshot.object->CustomAspectsMap().Seek(entry.first);
            require(drawer&&nearEqual(drawerColor(*drawer).Alpha(),1-transparency),"temporary transparency did not affect imported face/instance alpha");
            require(drawerColor(*drawer).GetRGB().Distance(entry.second.GetRGB())<1.e-6,"temporary transparency changed imported face/instance color");
        }
    };
    // Imported alpha must not leave some colored faces outside the temporary
    // override. Check the effective material alpha, including after recompute.
    view.setTransparency(55,false);
    for(const auto& snapshot:original)checkTemporaryTransparency(snapshot,0.55);
    view.setDisplayMode(0);view.setDisplayMode(1);
    for(const auto& snapshot:original)checkTemporaryTransparency(snapshot,0.55);
    view.setTransparency(0,false);QTest::qWait(50);
    for(const auto& snapshot:original){
        require(colorNear(drawerColor(snapshot.object->Attributes()),snapshot.color),"original inherited alpha not restored");
        require(snapshot.object->CustomAspectsMap().Size()==int(snapshot.custom.size()),"custom style count changed after transparency reset");
        for(const auto& entry:snapshot.custom){
            const auto* drawer=snapshot.object->CustomAspectsMap().Seek(entry.first);
            require(drawer&&colorNear(drawerColor(*drawer),entry.second),"original face/instance color or alpha not restored");
        }
    }
    const auto selected=std::find_if(original.begin(),original.end(),[](const Snapshot& snapshot){return !snapshot.custom.empty();});
    require(selected!=original.end(),"appearance fixture must include a colored face selection");
    int selectedOccurrence=-1;
    for(size_t i=0;i<doc->occurrences.size();++i)
        if(doc->occurrences[i].label==selected->object->GetLabel()&&same(doc->occurrences[i].worldTransform,
            selected->object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(selected->object->GetLabel()).Transformation())){selectedOccurrence=int(i);break;}
    require(selectedOccurrence>=0,"selected appearance instance missing");
    view.selectOccurrences({selectedOccurrence});view.setTransparency(55,true);
    for(const auto& snapshot:original){
        if(snapshot.object==selected->object)checkTemporaryTransparency(snapshot,0.55);
        else {
            require(colorNear(drawerColor(snapshot.object->Attributes()),snapshot.color),"selected transparency affected an unrelated inherited style");
            for(const auto& entry:snapshot.custom){
                const auto* drawer=snapshot.object->CustomAspectsMap().Seek(entry.first);
                require(drawer&&colorNear(drawerColor(*drawer),entry.second),"selected transparency affected an unrelated face/instance style");
            }
        }
    }
    view.setTransparency(0,true);
    require(colorNear(drawerColor(selected->object->Attributes()),selected->color),"selected inherited alpha did not restore");
    for(const auto& entry:selected->custom){
        const auto* drawer=selected->object->CustomAspectsMap().Seek(entry.first);
        require(drawer&&colorNear(drawerColor(*drawer),entry.second),"selected face/instance alpha did not restore");
    }
    // Selection-only is a complete view override, not a patch applied to the
    // previous global transparency. Changing the selection must restore all
    // source alpha on the formerly selected object.
    view.setTransparency(55,false);view.setTransparency(55,true);
    for(const auto& snapshot:original){
        if(snapshot.object==selected->object)checkTemporaryTransparency(snapshot,0.55);
        else require(colorNear(drawerColor(snapshot.object->Attributes()),snapshot.color),"switching to selected transparency retained an unrelated override");
    }
    const auto other=std::find_if(original.begin(),original.end(),[&](const Snapshot& snapshot){return snapshot.object!=selected->object;});
    int otherOccurrence=-1;for(size_t i=0;i<doc->occurrences.size();++i)if(doc->occurrences[i].label==other->object->GetLabel()&&same(doc->occurrences[i].worldTransform,
        other->object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(other->object->GetLabel()).Transformation())){otherOccurrence=int(i);break;}
    view.selectOccurrences({otherOccurrence});checkTemporaryTransparency(*other,0.55);
    require(colorNear(drawerColor(selected->object->Attributes()),selected->color),"changing selection did not restore previous source alpha");
    view.setTransparency(0,true);
    for(const auto& snapshot:original){
        require(colorNear(drawerColor(snapshot.object->Attributes()),snapshot.color),"zero transparency left an unrelated override");
        for(const auto& entry:snapshot.custom){const auto* drawer=snapshot.object->CustomAspectsMap().Seek(entry.first);
            require(drawer&&colorNear(drawerColor(*drawer),entry.second),"zero transparency did not restore source face alpha");}
    }
    // A catalog can promise a supported presentation, yet invalid geometry can
    // still prevent creation. The UI must receive the actual failure by ID.
    Stp::PmiCatalog pmi;Stp::PmiRecord invalid;invalid.id="invalid-pmi";invalid.status=Stp::PmiStatus::SemanticFallback;
    invalid.fallback=Stp::PmiFallback::Linear;invalid.point1=invalid.point2=gp_Pnt();pmi.records.push_back(invalid);
    int failures=0;auto connection=QObject::connect(&view,&Stp::ViewerWidget::pmiPresentationFailed,[&](const QString& id,const QString& reason){
        require(id==invalid.id&&!reason.isEmpty(),"PMI presentation failure did not identify record/reason");++failures;
    });
    load(view,doc,pmi);QObject::disconnect(connection);require(failures==1,"PMI creation failure not reported to UI");
}

void testMeshQuality(Stp::ViewerWidget& view){
    auto doc=std::make_shared<Stp::CadDocument>();doc->xde=new TDocStd_Document("BinXCAF");
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    const auto colors=XCAFDoc_DocumentTool::ColorTool(doc->xde->Main());
    const auto part=shapes->AddShape(BRepPrimAPI_MakeSphere(25).Shape(),false);
    const Quantity_ColorRGBA blue(Quantity_NOC_BLUE,0.65f),red(Quantity_NOC_RED,0.8f);
    colors->SetColor(part,blue,XCAFDoc_ColorSurf);
    TopExp_Explorer face(XCAFDoc_ShapeTool::GetShape(part),TopAbs_FACE);
    colors->SetColor(shapes->AddSubShape(part,face.Current()),red,XCAFDoc_ColorSurf);
    TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);const auto root=shapes->AddShape(compound,true);
    gp_Trsf shift;shift.SetTranslation(gp_Vec(70,0,0));
    shapes->AddComponent(root,part,TopLoc_Location());shapes->AddComponent(root,part,TopLoc_Location(shift));
    shapes->UpdateAssemblies();Stp::rebuildIndex(*doc);load(view,doc);
    require(view.meshQuality()==1,"default mesh quality should be standard");
    const auto sourceObject=allObjects(view).front();const auto sourceColor=drawerColor(sourceObject->Attributes());
    const auto sourceCustom=customStyles(sourceObject);
    const auto initialParts=doc->parts;const auto initialOccurrences=doc->occurrences;
    const auto quantities=Stp::partQuantities(*doc);const auto exact=doc->parts.front().shape;
    GProp_GProps volume,area;BRepGProp::VolumeProperties(exact,volume);BRepGProp::SurfaceProperties(exact,area);
    Bnd_Box initialBox;BRepBndLib::AddOptimal(exact,initialBox,false,false);
    double initialBounds[6];initialBox.Get(initialBounds[0],initialBounds[1],initialBounds[2],initialBounds[3],initialBounds[4],initialBounds[5]);
    std::vector<int> leaves;for(size_t i=0;i<doc->occurrences.size();++i)if(doc->occurrences[i].partIndex>=0)leaves.push_back(int(i));
    view.selectOccurrences({leaves.front()});view.setTransparency(45,true);
    view.setOccurrenceVisible(leaves.back(),false);view.setExplosion(60,-1);view.setSection(0,45,true,true);
    view.setPerspective(true);const auto cameraCenter=view.view()->Camera()->Center();const auto cameraDirection=view.view()->Camera()->Direction();
    const auto clip=view.view()->ClipPlanes()->First();const auto equation=clip->GetEquation();
    struct State{TDF_Label label;gp_Trsf transformation;bool visible;Quantity_ColorRGBA color;StyleMap custom;};
    std::vector<State> states;
    for(const auto& object:allObjects(view)){
        State state{object->GetLabel(),object->LocalTransformation(),view.context()->IsDisplayed(object),drawerColor(object->Attributes()),{}};
        state.custom=customStyles(object);
        states.push_back(state);
    }
    const auto check=[&]{
        require(allObjects(view).size()==states.size(),"precision switch leaked or lost presentations");
        for(const auto& state:states){
            const auto items=allObjects(view);const auto found=std::find_if(items.begin(),items.end(),[&](const auto& object){return object->GetLabel()==state.label;});
            require(found!=items.end(),"precision switch lost occurrence label");const auto object=*found;
            require(same(object->LocalTransformation(),state.transformation),"precision switch changed explosion or original position");
            require(bool(view.context()->IsDisplayed(object))==state.visible,"precision switch changed visibility");
            require(colorNear(drawerColor(object->Attributes()),state.color),"precision switch changed source color or temporary alpha");
            require(sameStyleMap(customStyles(object),state.custom),"precision switch changed face color or alpha");
        }
        require(view.context()->NbSelected()==1&&view.selectedOccurrences()==std::vector<int>{leaves.front()},"precision switch changed selection");
        require(clip->IsOn()&&clip->IsCapping()&&clip->GetEquation()==equation,"precision switch changed clipping state");
        require(view.view()->Camera()->Center().Distance(cameraCenter)<1.e-6&&view.view()->Camera()->Direction().IsEqual(cameraDirection,1.e-6),"precision switch changed camera");
        require(Stp::partQuantities(*doc)==quantities&&doc->parts.front().shape.IsSame(exact),"precision switch changed exact shape identity or BOM quantity");
        require(doc->parts.front().bboxMm==initialParts.front().bboxMm&&doc->parts.front().volumeMm3==initialParts.front().volumeMm3&&doc->parts.front().areaMm2==initialParts.front().areaMm2&&doc->parts.front().massKg==initialParts.front().massKg,"precision switch changed BOM geometry fields");
        GProp_GProps nowVolume,nowArea;BRepGProp::VolumeProperties(exact,nowVolume);BRepGProp::SurfaceProperties(exact,nowArea);
        require(nearEqual(volume.Mass(),nowVolume.Mass())&&nearEqual(area.Mass(),nowArea.Mass()),"precision switch changed exact volume or surface area");
        Bnd_Box nowBox;BRepBndLib::AddOptimal(exact,nowBox,false,false);double bounds[6];nowBox.Get(bounds[0],bounds[1],bounds[2],bounds[3],bounds[4],bounds[5]);
        for(int i=0;i<6;++i)require(nearEqual(initialBounds[i],bounds[i]),"precision switch changed exact bounding box");
        for(size_t i=0;i<doc->occurrences.size();++i)require(same(doc->occurrences[i].worldTransform,initialOccurrences[i].worldTransform),"precision switch changed source occurrence position");
    };
    quality(view,0);check();const int fast=triangleCount(exact);
    quality(view,1);check();const int standard=triangleCount(exact);
    quality(view,2);check();const int fine=triangleCount(exact);
    require(fast>0&&fast<standard&&standard<fine,"precision tiers did not change actual curved-face triangle count");
    quality(view,0,true);check();require(view.meshQuality()==2&&triangleCount(exact)==fine,"cancelled mesh quality update changed committed tier or face meshes");
    quality(view,0);check();require(triangleCount(exact)==fast,"fine-to-fast did not coarsen actual tessellation");
    view.setTransparency(0,true);
    for(const auto& object:allObjects(view)){
        require(colorNear(drawerColor(object->Attributes()),sourceColor),"precision rebuild cannot restore source definition alpha");
        require(sameStyleMap(customStyles(object),sourceCustom),"precision rebuild cannot restore source face alpha");
    }
    std::cout<<"Mesh triangles fast/standard/fine: "<<fast<<"/"<<standard<<"/"<<fine<<"\n";
}

void testPreparedRotatedMesh(Stp::ViewerWidget& view){
    view.clearDocument();require(view.setMeshQuality(1),"could not restore default precision");
    auto doc=std::make_shared<Stp::CadDocument>();doc->xde=new TDocStd_Document("BinXCAF");
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    const auto part=shapes->AddShape(BRepPrimAPI_MakeCylinder(5,200).Shape(),false);
    TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);const auto root=shapes->AddShape(compound,true);
    shapes->AddComponent(root,part,TopLoc_Location());
    gp_Trsf rotation;rotation.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,1,0)),M_PI/4);
    shapes->AddComponent(root,part,TopLoc_Location(rotation));shapes->UpdateAssemblies();Stp::rebuildIndex(*doc);
    std::atomic_bool cancel(false);Stp::prepareDisplayMeshes(*doc,1,cancel);
    const auto exact=doc->parts.front().shape;const int prepared=triangleCount(exact);
    QEventLoop loop;bool ready=false;QTimer timeout;timeout.setSingleShot(true);
    const auto connection=QObject::connect(&view,&Stp::ViewerWidget::sceneReady,&loop,[&]{ready=true;loop.quit();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);timeout.start(30000);
    view.setDocument(doc,{},true);loop.exec();QObject::disconnect(connection);
    require(ready&&prepared>0&&triangleCount(exact)==prepared,"AIS display remeshed a rotated prepared definition");
    require(objects(view).size()==2,"rotated mesh fixture lost an occurrence");
    for(const auto& object:objects(view))require(!object->Attributes()->IsAutoTriangulation(),"prepared AIS must not mesh on the GUI thread");
    view.setDisplayMode(2);view.setDisplayMode(0);
    require(triangleCount(exact)==prepared,"display mode change remeshed a rotated prepared definition");
    std::cout<<"Prepared rotated cylinder triangles before/after display: "<<prepared<<"/"<<triangleCount(exact)<<"\n";
}
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
struct GrabPick { QPoint logical; gp_Pnt world; };
GrabPick grabPick(Stp::ViewerWidget& view,const Handle(XCAFPrs_AISObject)& object)
{
    auto viewport=view.findChild<QWidget*>("cadViewport");
    const double ratio=viewport->devicePixelRatioF();
    Bnd_Box box;BRepBndLib::Add(XCAFDoc_ShapeTool::GetShape(object->GetLabel()),box);
    require(!box.IsVoid(),"grab fixture has no shape bounds");
    double low[3],high[3];box.Get(low[0],low[1],low[2],high[0],high[1],high[2]);
    // The XCAF shape already contains the component's own local placement.
    // Sampling its current AIS placement also works after an earlier drag.
    for(int axis=0;axis<3;++axis)for(double side:{0.,1.})for(double a:{.25,.5,.75})for(double b:{.25,.5,.75}){
        double coordinates[3];for(int i=0;i<3;++i)coordinates[i]=(low[i]+high[i])*.5;
        coordinates[axis]=low[axis]+side*(high[axis]-low[axis]);
        coordinates[(axis+1)%3]=low[(axis+1)%3]+a*(high[(axis+1)%3]-low[(axis+1)%3]);
        coordinates[(axis+2)%3]=low[(axis+2)%3]+b*(high[(axis+2)%3]-low[(axis+2)%3]);
        const auto world=gp_Pnt(coordinates[0],coordinates[1],coordinates[2]).Transformed(object->LocalTransformation());
        int x,y;view.view()->Convert(world.X(),world.Y(),world.Z(),x,y);
        const QPoint logical(qRound(x/ratio),qRound(y/ratio));
        if(!viewport->rect().adjusted(8,8,-100,-70).contains(logical))continue;
        view.context()->MoveTo(qRound(logical.x()*ratio),qRound(logical.y()*ratio),view.view(),false);
        if(!view.context()->HasDetected()||view.context()->DetectedInteractive()!=object)continue;
        const auto selector=view.context()->MainSelector();
        for(int rank=1;rank<=selector->NbPicked();++rank)
            if(selector->Picked(rank)==view.context()->DetectedOwner())return {logical,selector->PickedPoint(rank)};
    }
    throw std::runtime_error("could not pick the grab fixture's visible instance");
}
void grabMouse(QWidget* viewport,QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons)
{
    QMouseEvent event(type,point,point,button,buttons,Qt::NoModifier);
    QApplication::sendEvent(viewport,&event);
}
void grabGesture(QWidget* viewport,QPoint from,QPoint to,int steps,bool releaseOnly=false)
{
    grabMouse(viewport,QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton);
    if(!releaseOnly)for(int i=1;i<=steps;++i)
        grabMouse(viewport,QEvent::MouseMove,QPointF(from)+(QPointF(to)-from)*double(i)/steps,Qt::NoButton,Qt::LeftButton);
    grabMouse(viewport,QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton);
}
void testPartGrab(Stp::ViewerWidget& view)
{
    const QString evidence=QString::fromLocal8Bit(qgetenv("STP_GRAB_TEST_OUTPUT"));
    const auto screenshot=[&](const QString& name){if(evidence.isEmpty())return;require(QDir().mkpath(evidence),"could not create grab evidence directory");QTest::qWait(100);require(view.saveScreenshot(QDir(evidence).filePath(name)),"could not save grab evidence screenshot");};
    auto doc=std::make_shared<Stp::CadDocument>();doc->xde=new TDocStd_Document("BinXCAF");
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    const auto part=shapes->AddShape(BRepPrimAPI_MakeBox(12,10,8).Shape(),false);
    const auto assembly=[&]{TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);return shapes->AddShape(compound,true);};
    const auto locate=[](double x,double y,double angle){gp_Trsf transform;transform.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,0,1)),angle);transform.SetTranslationPart(gp_Vec(x,y,0));return TopLoc_Location(transform);};
    const auto sub=assembly(),root=assembly();
    shapes->AddComponent(sub,part,locate(0,0,0));shapes->AddComponent(sub,part,locate(22,0,.2));
    shapes->AddComponent(root,sub,locate(0,0,.25));shapes->AddComponent(root,sub,locate(0,34,-.2));
    shapes->UpdateAssemblies();Stp::rebuildIndex(*doc);
    std::vector<int> leaves;for(size_t i=0;i<doc->occurrences.size();++i)if(doc->occurrences[i].partIndex>=0)leaves.push_back(int(i));
    require(leaves.size()==4,"grab fixture must have four repeated nested instances");
    const auto sourceOccurrences=doc->occurrences;const auto sourceParts=doc->parts;const auto sourceQuantities=Stp::partQuantities(*doc);
    GProp_GProps sourceVolume;BRepGProp::VolumeProperties(doc->parts.front().shape,sourceVolume);
    Stp::PmiCatalog catalog;Stp::PmiRecord owned,ambiguous;
    owned.id="grab-owned";owned.status=Stp::PmiStatus::SourceGraphics;owned.associationComplete=true;owned.occurrencePlacementVerified=true;
    owned.ownerOccurrenceIds={doc->occurrences[size_t(leaves.front())].id};
    owned.graphicalPresentation=BRepBuilderAPI_MakeEdge(gp_Pnt(0,0,12),gp_Pnt(12,0,12));
    ambiguous.id="grab-assembly";ambiguous.status=Stp::PmiStatus::SourceGraphics;ambiguous.crossPart=true;
    ambiguous.graphicalPresentation=BRepBuilderAPI_MakeEdge(gp_Pnt(0,1,12),gp_Pnt(12,1,12));
    catalog.records={owned,ambiguous};load(view,doc,catalog);view.setPerspective(false);view.standardView(0);
    auto viewport=view.findChild<QWidget*>("cadViewport");require(viewport,"grab viewport missing");
    struct Instance {int occurrence;Handle(XCAFPrs_AISObject) object;gp_Trsf base;};
    std::vector<Instance> instances;
    for(int occurrence:leaves){
        Handle(XCAFPrs_AISObject) found;
        for(const auto& object:allObjects(view))if(object->GetLabel()==doc->occurrences[size_t(occurrence)].label&&same(
            object->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(object->GetLabel()).Transformation(),doc->occurrences[size_t(occurrence)].worldTransform)){found=object;break;}
        require(!found.IsNull(),"grab fixture lost a nested occurrence");instances.push_back({occurrence,found,found->LocalTransformation()});
    }
    require(instances[0].object->GetLabel()==instances[2].object->GetLabel(),"grab fixture must repeat an occurrence label across subassemblies");
    auto& target=instances.front();Handle(AIS_Shape) ownedPmi,assemblyPmi;
    AIS_ListOfInteractive scene;view.context()->ObjectsInside(scene);
    for(const auto& object:scene){const auto annotation=Handle(AIS_Shape)::DownCast(object);if(annotation.IsNull())continue;
        if(annotation->Shape().IsSame(owned.graphicalPresentation))ownedPmi=annotation;
        if(annotation->Shape().IsSame(ambiguous.graphicalPresentation))assemblyPmi=annotation;}
    require(!ownedPmi.IsNull()&&!assemblyPmi.IsNull(),"grab fixture's PMI was not displayed");
    const auto checkSource=[&]{
        require(Stp::partQuantities(*doc)==sourceQuantities&&doc->parts.front().shape.IsSame(sourceParts.front().shape),"grab changed exact geometry identity or BOM quantity");
        require(doc->parts.front().bboxMm==sourceParts.front().bboxMm&&doc->parts.front().volumeMm3==sourceParts.front().volumeMm3&&doc->parts.front().areaMm2==sourceParts.front().areaMm2,"grab changed source geometry properties");
        for(size_t i=0;i<sourceOccurrences.size();++i){const auto& original=sourceOccurrences[i];const auto& current=doc->occurrences[i];
            require(same(current.worldTransform,original.worldTransform)&&same(current.localTransform,original.localTransform)&&same(XCAFDoc_ShapeTool::GetLocation(current.label).Transformation(),original.localTransform),"grab modified source occurrence placement");}
        GProp_GProps currentVolume;BRepGProp::VolumeProperties(doc->parts.front().shape,currentVolume);require(nearEqual(sourceVolume.Mass(),currentVolume.Mass()),"grab changed source solid volume");
    };
    const auto checkReset=[&]{view.resetPartPositions();for(const auto& item:instances)require(same(item.object->LocalTransformation(),item.base),"grab reset did not restore every instance");
        require(view.context()->IsDisplayed(assemblyPmi)&&same(ownedPmi->LocalTransformation(),gp_Trsf()),"grab reset did not restore PMI placement/visibility");};
    const auto checkScreen=[&](const GrabPick& pick,QPoint end,const gp_Trsf& before){
        const gp_Vec offset=target.object->LocalTransformation().TranslationPart()-before.TranslationPart();
        const auto moved=pick.world.Translated(offset);int x,y;view.view()->Convert(moved.X(),moved.Y(),moved.Z(),x,y);
        const double ratio=viewport->devicePixelRatioF();
        require(std::abs(x-qRound(end.x()*ratio))<=2&&std::abs(y-qRound(end.y()*ratio))<=2,"grabbed surface point did not follow the cursor");
        require(std::abs(offset.Dot(gp_Vec(view.view()->Camera()->Direction())))<1.e-6,"grab moved along view depth");
        for(int row=1;row<=3;++row)for(int column=1;column<=3;++column)require(nearEqual(before.Value(row,column),target.object->LocalTransformation().Value(row,column)),"grab rotated or scaled the part");
    };
    view.setGrabMode(true);require(view.grabMode()&&viewport->cursor().shape()==Qt::OpenHandCursor,"Mac grab mode did not enable with an open-hand cursor");
    for(bool perspective:{false,true})for(double zoom:{.8,1.}){
        checkReset();view.setPerspective(perspective);view.standardView(0);view.view()->SetScale(view.view()->Scale()*zoom);
        const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(view.view()->Camera());
        const auto pick=grabPick(view,target.object);const QPoint end=pick.logical+QPoint(54,33);
        grabGesture(viewport,pick.logical,pick.logical,1);
        require(view.selectedOccurrences()==std::vector<int>{target.occurrence}&&same(target.object->LocalTransformation(),target.base),"grab click did not select only the hit instance without moving it");
        grabGesture(viewport,pick.logical,end,6);const auto sixSteps=target.object->LocalTransformation();checkScreen(pick,end,target.base);
        require(viewport->cursor().shape()==Qt::OpenHandCursor,"grab release did not restore the open-hand cursor");
        require(view.view()->Camera()->Eye().Distance(camera->Eye())<1.e-8&&view.view()->Camera()->Center().Distance(camera->Center())<1.e-8&&view.view()->Camera()->Direction().IsEqual(camera->Direction(),1.e-10)&&nearEqual(view.view()->Camera()->Scale(),camera->Scale()),"grab changed the camera");
        for(size_t i=1;i<instances.size();++i)require(same(instances[i].object->LocalTransformation(),instances[i].base),"grab moved another instance of the repeated definition");
        require(!view.context()->IsDisplayed(assemblyPmi)&&view.context()->IsDisplayed(ownedPmi),"grab did not hide ambiguous PMI while retaining instance PMI");
        require((ownedPmi->LocalTransformation().TranslationPart()-(sixSteps.TranslationPart()-target.base.TranslationPart())).Modulus()<1.e-6,"instance PMI did not follow the grabbed part");
        grabMouse(viewport,QEvent::MouseMove,end+QPoint(24,10),Qt::NoButton,Qt::NoButton);require(same(target.object->LocalTransformation(),sixSteps),"grab continued after release");
        checkReset();grabGesture(viewport,pick.logical,end,1);require(same(target.object->LocalTransformation(),sixSteps),"grab displacement depends on mouse-event count");
        checkReset();grabGesture(viewport,pick.logical,end,1,true);require(same(target.object->LocalTransformation(),sixSteps),"grab release ignored its final cursor position");
        checkSource();
    }
    // Repeated grabs accumulate display translation. Normal explosion updates
    // must preserve it, including the slider returning to zero.
    checkReset();view.setPerspective(false);view.standardView(0);
    auto pick=grabPick(view,target.object);grabGesture(viewport,pick.logical,pick.logical+QPoint(30,20),2);
    const auto firstManual=target.object->LocalTransformation();pick=grabPick(view,target.object);
    grabGesture(viewport,pick.logical,pick.logical+QPoint(26,16),2);checkScreen(pick,pick.logical+QPoint(26,16),firstManual);
    const auto manual=target.object->LocalTransformation();view.setExplosion(35,-1);const auto exploded=target.object->LocalTransformation();
    require(!same(manual,exploded),"grab fixture explosion moved no parts");view.fitAll();
    pick=grabPick(view,target.object);grabGesture(viewport,pick.logical,pick.logical+QPoint(35,20),3);
    const auto dragWhileExploded=target.object->LocalTransformation();const gp_Vec additional=dragWhileExploded.TranslationPart()-exploded.TranslationPart();
    view.setExplosion(0,-1);
    require((target.object->LocalTransformation().TranslationPart()-manual.TranslationPart()-additional.XYZ()).Modulus()<1.e-6,"zero explosion erased manual displacement");
    view.setExplosion(20,-1);view.setExplosion(20,target.occurrence);
    require((target.object->LocalTransformation().TranslationPart()-manual.TranslationPart()-additional.XYZ()).Modulus()<1.e-6,"explosion scope switch erased manual displacement");
    checkReset();pick=grabPick(view,target.object);grabGesture(viewport,pick.logical,pick.logical+QPoint(40,24),2);
    const auto retained=target.object->LocalTransformation();view.setTransparency(45,true);view.setOccurrenceVisible(target.occurrence,false);view.setOccurrenceVisible(target.occurrence,true);
    require(same(target.object->LocalTransformation(),retained),"visibility/transparency change erased the grabbed position");
    const Handle(Graphic3d_Camera) camera=new Graphic3d_Camera(view.view()->Camera());
    drag(viewport,Qt::RightButton,{400,300},{455,325});require(view.view()->Camera()->Center().Distance(camera->Center())>1.e-6&&same(target.object->LocalTransformation(),retained),"grab mode disabled right panning or changed the part");
    const double scale=view.view()->Scale();QWheelEvent wheel({400,300},{400,300},{0,0},{0,120},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(viewport,&wheel);require(view.view()->Scale()>scale&&same(target.object->LocalTransformation(),retained),"grab mode disabled wheel zoom or changed the part");view.view()->Camera()->Copy(camera);
    const auto blank=QPoint(8,8);view.context()->MoveTo(qRound(blank.x()*viewport->devicePixelRatioF()),qRound(blank.y()*viewport->devicePixelRatioF()),view.view(),false);
    require(!view.context()->HasDetected(),"grab fixture has no blank corner");
    grabGesture(viewport,blank,blank+QPoint(50,35),2);require(same(target.object->LocalTransformation(),retained)&&view.view()->Camera()->Direction().IsEqual(camera->Direction(),1.e-10),"blank grab moved the part or rotated the camera");
    for(QEvent::Type interruption:{QEvent::FocusOut,QEvent::Hide}){
        pick=grabPick(view,target.object);grabMouse(viewport,QEvent::MouseButtonPress,pick.logical,Qt::LeftButton,Qt::LeftButton);
        grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(18,12),Qt::NoButton,Qt::LeftButton);
        require(viewport->cursor().shape()==Qt::ClosedHandCursor,"active grab did not use a closed-hand cursor");
        const Handle(Graphic3d_Camera) heldCamera=new Graphic3d_Camera(view.view()->Camera());
        QWheelEvent heldWheel(pick.logical,pick.logical,{0,0},{0,120},Qt::LeftButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(viewport,&heldWheel);
        QNativeGestureEvent zoom(Qt::ZoomNativeGesture,QPointingDevice::primaryPointingDevice(),2,pick.logical,pick.logical,pick.logical,.5,{});QApplication::sendEvent(viewport,&zoom);
        grabMouse(viewport,QEvent::MouseButtonDblClick,pick.logical,Qt::LeftButton,Qt::LeftButton);
        require(nearEqual(view.view()->Camera()->Scale(),heldCamera->Scale())&&view.view()->Camera()->Center().Distance(heldCamera->Center())<1.e-8&&view.view()->Camera()->Eye().Distance(heldCamera->Eye())<1.e-8,"wheel/native zoom/double-click changed the camera during a grab");
        const auto last=target.object->LocalTransformation();QEvent event(interruption);QApplication::sendEvent(viewport,&event);
        require(viewport->cursor().shape()==Qt::OpenHandCursor,"interrupted grab did not restore the open-hand cursor");
        grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(60,40),Qt::NoButton,Qt::LeftButton);
        grabMouse(viewport,QEvent::MouseButtonRelease,pick.logical+QPoint(60,40),Qt::LeftButton,Qt::NoButton);
        require(same(target.object->LocalTransformation(),last),"grab continued after focus loss or viewport hiding");
    }
    pick=grabPick(view,target.object);grabMouse(viewport,QEvent::MouseButtonPress,pick.logical,Qt::LeftButton,Qt::LeftButton);
    grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(17,11),Qt::NoButton,Qt::LeftButton);const auto beforeMissedRelease=target.object->LocalTransformation();
    grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(60,40),Qt::NoButton,Qt::NoButton);
    grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(80,55),Qt::NoButton,Qt::LeftButton);
    require(same(target.object->LocalTransformation(),beforeMissedRelease),"grab resumed after a missing release was detected");view.finishPartDrag();
    pick=grabPick(view,target.object);grabMouse(viewport,QEvent::MouseButtonPress,pick.logical,Qt::LeftButton,Qt::LeftButton);
    grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(10,8),Qt::NoButton,Qt::LeftButton);
    screenshot("grab-moved.png");const auto beforeQuality=target.object->LocalTransformation();const int nextQuality=view.meshQuality()==0?1:0;quality(view,nextQuality);
    for(auto& instance:instances){const gp_Trsf expected=instance.occurrence==target.occurrence?beforeQuality:instance.base;Handle(XCAFPrs_AISObject) replacement;
        for(const auto& object:allObjects(view))if(object->GetLabel()==instance.object->GetLabel()&&same(object->LocalTransformation(),expected)){replacement=object;break;}
        require(!replacement.IsNull(),"mesh precision rebuild erased or transferred a grabbed instance's placement");instance.object=replacement;}
    grabMouse(viewport,QEvent::MouseMove,pick.logical+QPoint(50,35),Qt::NoButton,Qt::LeftButton);
    grabMouse(viewport,QEvent::MouseButtonRelease,pick.logical+QPoint(50,35),Qt::LeftButton,Qt::NoButton);
    require(same(target.object->LocalTransformation(),beforeQuality)&&viewport->cursor().shape()==Qt::OpenHandCursor,"mesh rebuild did not terminate the old grab gesture");
    require(!view.context()->IsDisplayed(assemblyPmi)&&(ownedPmi->LocalTransformation().TranslationPart()-(beforeQuality.TranslationPart()-target.base.TranslationPart())).Modulus()<1.e-6,"mesh rebuild lost grabbed PMI state");
    checkSource();checkReset();screenshot("grab-reset.png");view.setGrabMode(false);require(!view.grabMode()&&viewport->cursor().shape()!=Qt::OpenHandCursor&&viewport->cursor().shape()!=Qt::ClosedHandCursor,"Mac grab mode did not disable and restore the cursor");
    const auto direction=view.view()->Camera()->Direction();drag(viewport,Qt::LeftButton,{400,300},{465,340});require(!direction.IsEqual(view.view()->Camera()->Direction(),1.e-6),"turning grab mode off did not restore left rotation");
    view.setGrabMode(true);view.clearDocument();load(view,doc,catalog);require(!view.grabMode(),"new document retained old grab mode");
    for(const auto& object:allObjects(view)){bool original=false;for(const auto& instance:instances)if(object->GetLabel()==instance.object->GetLabel()&&same(object->LocalTransformation(),instance.base)){original=true;break;}require(original,"reopened document retained a grabbed display position");}
    checkSource();
    std::cout<<"PART GRAB PASS: orthographic/perspective cursor tracking, repeated nested-instance isolation, single/six-step/release-only equivalence, accumulation, explosion/reset, PMI, navigation, missing release/focus/hide, precision rebuild, mode restoration, source geometry/BOM immutability\n";
}

void testMeasurementPickingAndHistory(Stp::ViewerWidget& view)
{
    using namespace Stp;
    auto doc = std::make_shared<CadDocument>(); doc->xde = new TDocStd_Document("BinXCAF");
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    const auto part = shapes->AddShape(BRepPrimAPI_MakeBox(12, 17, 23).Shape(), false);
    const auto assembly = [&] { TopoDS_Compound compound; BRep_Builder().MakeCompound(compound); return shapes->AddShape(compound, true); };
    const auto root = assembly(), nested = assembly();
    gp_Trsf local; local.SetTranslation(gp_Vec(7, 11, 13));
    shapes->AddComponent(nested, part, TopLoc_Location(local));
    gp_Trsf parent; parent.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(0, 0, 1)), 0.43);
    parent.SetTranslationPart(gp_Vec(31, -14, 8));
    shapes->AddComponent(root, nested, TopLoc_Location(parent));
    gp_Trsf repeated; repeated.SetTranslation(gp_Vec(-45, 28, 3));
    shapes->AddComponent(root, nested, TopLoc_Location(repeated));
    shapes->UpdateAssemblies(); rebuildIndex(*doc); load(view, doc);
    std::vector<int> leaves;
    for (size_t index = 0; index < doc->occurrences.size(); ++index)
        if (doc->occurrences[index].partIndex >= 0) leaves.push_back(int(index));
    require(leaves.size() == 2, "measurement fixture must have repeated nested placements");
    const int occurrence = leaves.front();
    view.selectOccurrences({occurrence}); view.isolateSelection(); view.standardView(0); view.fitAll();
    auto viewport = view.findChild<QWidget*>("cadViewport");
    const double ratio = viewport->devicePixelRatioF();
    MeasurementEntity picked; int pickedSlot = -1;
    const auto connection = QObject::connect(&view, &ViewerWidget::measurementEntityPicked,
        [&](const MeasurementEntity& entity, int slot) { picked = entity; pickedSlot = slot; });
    const auto diagnostics = QObject::connect(&view, &ViewerWidget::message,
        [](const QString& message) { std::cerr << "MEASUREMENT DIAGNOSTIC: " << message.toStdString() << '\n'; });
    const auto performPick = [&](MeasurementSelection type, int slot) {
        std::cout << "MEASUREMENT PICK type=" << int(type) << " slot=" << slot
                  << " projection=" << int(view.view()->Camera()->ProjectionType()) << std::endl;
        picked = {}; pickedSlot = -1; view.setMeasurementPicking(true, type, slot);
        const auto original = view.measurementPartEntity(occurrence);
        const auto candidatesType = type == MeasurementSelection::Vertex ? TopAbs_VERTEX
                                  : type == MeasurementSelection::Edge ? TopAbs_EDGE : TopAbs_FACE;
        for (TopExp_Explorer it(original.shape, candidatesType); it.More(); it.Next()) {
            gp_Pnt point;
            if (candidatesType == TopAbs_VERTEX) point = BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));
            else if (candidatesType == TopAbs_EDGE) {
                BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
                if (curve.LastParameter() - curve.FirstParameter() < 1.e-9) continue;
                point = curve.Value((curve.FirstParameter() + curve.LastParameter()) * 0.5);
            } else { GProp_GProps props; BRepGProp::SurfaceProperties(it.Current(), props); point = props.CentreOfMass(); }
            point.Transform(original.displayTransform);
            int x, y; view.view()->Convert(point.X(), point.Y(), point.Z(), x, y);
            const QPoint logical(qRound(x / ratio), qRound(y / ratio));
            if (!viewport->rect().contains(logical)) continue;
            QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, logical);
            if (pickedSlot == slot) break;
        }
        require(pickedSlot == slot && picked.occurrence == occurrence, "measurement exact entity click failed");
        require(picked.occurrenceId == original.occurrenceId && same(picked.originalTransform, original.originalTransform)
                && same(picked.displayTransform, original.displayTransform), "measurement click lost nested instance placement");
        if (type != MeasurementSelection::Part) {
            TopTools_IndexedMapOfShape canonical; TopExp::MapShapes(original.shape, picked.shape.ShapeType(), canonical);
            require(canonical.Contains(picked.shape), "measurement click retained placed owner topology instead of definition topology");
        }
        require(view.selectedOccurrences() == std::vector<int>{occurrence}, "measurement click changed normal occurrence selection");
        if (type == MeasurementSelection::PointOnGeometry) require(picked.pickedPoint.has_value(), "point-on-geometry click has no precise point");
        if (type==MeasurementSelection::Edge || type==MeasurementSelection::Face) {
            auto extended=picked;extended.extent=MeasurementExtent::Infinite;
            const auto reference=measurementReference(extended,MeasurementCoordinates::Original);
            require(reference.has_value(),"native straight/planar pick cannot resolve infinite reference");
            auto local=extended;local.originalTransform=gp_Trsf();
            const auto localReference=measurementReference(local,MeasurementCoordinates::Original);
            require(localReference.has_value(),"native canonical infinite reference invalid");
            const auto expected=localReference->anchor.Transformed(original.originalTransform);
            require(reference->anchor.Distance(expected)<1.e-6,"native expanded reference transform applied twice");
            view.fitAll();const double scale=view.view()->Camera()->Scale();
            AIS_ListOfInteractive previousObjects;view.context()->ObjectsInside(previousObjects);
            std::vector<Handle(AIS_InteractiveObject)> previous(previousObjects.begin(),previousObjects.end());
            view.setMeasurementHighlights({extended},{});
            MeasurementRecord expandedRecord;expandedRecord.id=200;
            expandedRecord.request={MeasurementKind::Distance,MeasurementCoordinates::Displayed,{extended},{view.measurementPartEntity(occurrence)}};
            std::atomic_bool cancelled{false};expandedRecord.result=calculateMeasurement(expandedRecord.request,cancelled);
            require(expandedRecord.result.success,"native extended reference calculation failed");
            view.addMeasurementRecord(expandedRecord);view.fitAll();
            require(nearEqual(scale,view.view()->Camera()->Scale()),"infinite reference guides changed fitAll range");
            AIS_ListOfInteractive objects;view.context()->ObjectsInside(objects);
            int resultLabelCount=0;
            for (const auto& item:objects) if (item->IsInfinite()) {
                TColStd_ListOfInteger modes;view.context()->ActivatedModes(item,modes);
                require(modes.IsEmpty(),"infinite line/plane guide or result is selectable");
                const auto badge=Handle(AIS_TextLabel)::DownCast(item);
                if (!badge.IsNull() && std::find(previous.begin(),previous.end(),item)==previous.end()) {
                    const TCollection_AsciiString label(badge->Text(),'?');
                    ++resultLabelCount;
                    require(QString::fromUtf8(label.ToCString()).startsWith(QStringLiteral("Min d ")),
                            "expanded reference retained a record number or auxiliary badge");
                }
            }
            require(resultLabelCount==1,"expanded reference must show only its measured value label");
            view.removeMeasurementRecord(200);view.setMeasurementHighlights({},{});
        }

    };
    for (bool perspective : {false, true}) {
        view.setPerspective(perspective); view.fitAll();
        for (auto type : {MeasurementSelection::Vertex, MeasurementSelection::Edge, MeasurementSelection::Face,
                          MeasurementSelection::Solid, MeasurementSelection::Part, MeasurementSelection::PointOnGeometry})
            performPick(type, type == MeasurementSelection::Face ? 1 : 0);
    }
    view.setMeasurementPicking(false, MeasurementSelection::Part);
    view.setMeasurementHighlights({}, {}); view.showAll();
    view.fitAll(); const double modelFitScale = view.view()->Camera()->Scale();
    MeasurementRecord record; record.id = 101; record.request.kind = MeasurementKind::Distance;
    record.request.a = {view.measurementPartEntity(leaves[0])}; record.request.b = {view.measurementPartEntity(leaves[1])};
    std::atomic_bool cancel(false); record.result = calculateMeasurement(record.request, cancel);
    require(record.result.success, "measurement viewer fixture distance failed"); view.addMeasurementRecord(record);
    MeasurementRecord radius; radius.id = 102; radius.request.kind = MeasurementKind::Radius;
    radius.request.a = record.request.a; radius.result.success = true;
    // A far-away diagnostic annotation must not change the model fit range.
    const auto center = gp_Pnt(10006, 8, 10).Transformed(radius.request.a.front().originalTransform);
    radius.result.circle = gp_Circ(gp_Ax2(center, gp_Dir(0, 0, 1)), 5);
    radius.result.anchors = {center, center.Translated(gp_Vec(5, 0, 0))};
    radius.result.values = {{QStringLiteral("半径"), QStringLiteral("mm"), 5}}; view.addMeasurementRecord(radius);
    view.fitAll(); require(nearEqual(modelFitScale, view.view()->Camera()->Scale()), "measurement annotations expanded fitAll model range");
    auto labels = [&] { AIS_ListOfInteractive all; view.context()->ObjectsInside(all); std::vector<Handle(AIS_TextLabel)> result;
        for (const auto& object : all) { auto label = Handle(AIS_TextLabel)::DownCast(object); if (!label.IsNull()) result.push_back(label); } return result; };
    require(labels().size() == 2, "successive measurements did not retain two labels");
    MeasurementRecord diameter = radius; diameter.id = 106; diameter.request.kind = MeasurementKind::Diameter;
    diameter.result.values = {{QStringLiteral("直径"), QStringLiteral("mm"), 10}};
    view.addMeasurementRecord(diameter);
    gp_Pnt radiusLabelPoint, diameterLabelPoint; bool haveRadius = false, haveDiameter = false;
    for (const auto& label : labels()) {
        const TCollection_AsciiString utf8(label->Text(), '?'); const auto text = QString::fromUtf8(utf8.ToCString());
        if (text.startsWith("M102 ")) { radiusLabelPoint = label->Position(); haveRadius = true; }
        if (text.startsWith("M106 ")) { diameterLabelPoint = label->Position(); haveDiameter = true; }
    }
    require(haveRadius && haveDiameter && radiusLabelPoint.Distance(diameterLabelPoint) > 1.,
            "successive radius and diameter annotations share the same leader anchor");
    view.removeMeasurementRecord(106);
    const auto boundedCircles = [&] { AIS_ListOfInteractive all; view.context()->ObjectsInside(all); int count = 0;
        for (const auto& item : all) { auto object = Handle(AIS_Shape)::DownCast(item);
            if (object.IsNull() || object->Shape().ShapeType() != TopAbs_EDGE) continue;
            BRepAdaptor_Curve curve(TopoDS::Edge(object->Shape()));
            count += curve.GetType() == GeomAbs_Circle && curve.LastParameter() - curve.FirstParameter() < 2.0 * M_PI - 1.e-6;
        } return count; };
    const int originalArcs = boundedCircles();
    MeasurementRecord angle; angle.id = 104; angle.request.kind = MeasurementKind::Angle;
    angle.request.a = record.request.a; angle.request.b = record.request.b; angle.result.success = true;
    const auto vertex = record.result.anchors.front();
    angle.result.anchors = {vertex, vertex.Translated(gp_Vec(10, 0, 0)), vertex.Translated(gp_Vec(0, 10, 0))};
    angle.result.values = {{QStringLiteral("夹角"), QStringLiteral("°"), 90}}; view.addMeasurementRecord(angle);
    require(labels().size() == 3 && boundedCircles() == originalArcs + 1, "angle measurement lacks its bounded 3D arc");
    view.removeMeasurementRecord(104); require(labels().size() == 2, "removing one angle record changed other history");
    MeasurementRecord centerRecord; centerRecord.id = 105; centerRecord.request.kind = MeasurementKind::CenterOfMass;
    centerRecord.request.a = record.request.a; centerRecord.result.success = true;
    centerRecord.result.anchors = {gp_Pnt(42, 13, 7)};
    centerRecord.result.values = {{QStringLiteral("体积"), QStringLiteral("mm³"), 100},
        {QStringLiteral("质量"), QStringLiteral("kg"), 10}, {QStringLiteral("体积中心 X"), QStringLiteral("mm"), 9999},
        {QStringLiteral("质量中心 X"), QStringLiteral("mm"), 42}};
    view.addMeasurementRecord(centerRecord); bool centerText = false;
    for (const auto& label : labels()) {
        const TCollection_AsciiString utf8(label->Text(), '?');
        centerText |= QString::fromUtf8(utf8.ToCString()) == QStringLiteral("M105 CoM (42.000, 13.000, 7.000) mm");
    }
    require(centerText, "COM annotation used property-list values instead of the actual center anchor");
    view.removeMeasurementRecord(105);
    MeasurementRecord areaCenter = centerRecord; areaCenter.id = 107; areaCenter.request.kind = MeasurementKind::Area;
    areaCenter.result.values = {{QStringLiteral("面积"), QStringLiteral("mm²"), 100}};
    MeasurementRecord volumeCenter = areaCenter; volumeCenter.id = 108; volumeCenter.request.kind = MeasurementKind::Volume;
    volumeCenter.result.values = {{QStringLiteral("体积"), QStringLiteral("mm³"), 1000}};
    view.addMeasurementRecord(areaCenter); view.addMeasurementRecord(volumeCenter);
    int areaLabelY = 0, volumeLabelY = 0; bool haveAreaLabel = false, haveVolumeLabel = false;
    for (const auto& label : labels()) {
        require(label->ZLayer() == Graphic3d_ZLayerId_Topmost, "measurement text inherits occluding model depth");
        const TCollection_AsciiString utf8(label->Text(), '?'); const auto text = QString::fromUtf8(utf8.ToCString());
        int x, y; view.view()->Convert(label->Position().X(), label->Position().Y(), label->Position().Z(), x, y);
        if (text.startsWith("M107 ")) { areaLabelY = y; haveAreaLabel = true; }
        if (text.startsWith("M108 ")) { volumeLabelY = y; haveVolumeLabel = true; }
    }
    require(haveAreaLabel && haveVolumeLabel && std::abs(areaLabelY - volumeLabelY) >= 20. * ratio,
            "coincident area and volume center labels overlap");
    view.removeMeasurementRecord(107); view.removeMeasurementRecord(108);
    AIS_ListOfInteractive all; view.context()->ObjectsInside(all);
    for (const auto& object : all) if (Handle(XCAFPrs_AISObject)::DownCast(object).IsNull()) {
        require(object->IsInfinite(), "measurement overlay changes fit bounds");
        TColStd_ListOfInteger modes; view.context()->ActivatedModes(object, modes);
        require(modes.IsEmpty(), "measurement overlay can be picked");
    }
    view.setMeasurementDecimals(6); require(labels().size() == 2, "decimal refresh duplicated history overlays");
    bool radiusText = false; for (const auto& label : labels()) {
        const TCollection_AsciiString utf8(label->Text(), '?'); radiusText |= QString::fromUtf8(utf8.ToCString()).contains("5.000000");
    }
    require(radiusText, "3D radius label did not follow selected decimal precision");
    view.setExplosion(60, -1);
    require(view.measurementRecordPositionChanged(101), "original-coordinate measurement stayed attached after explosion");
    for (const auto& label : labels()) require(!view.context()->IsDisplayed(label), "displaced history label was not hidden");
    // The application's explosion slider also fits on release. Keep the
    // displaced occurrence inside the viewport before testing its real click.
    view.fitAll();
    performPick(MeasurementSelection::Face, 1);
    view.setMeasurementPicking(false, MeasurementSelection::Part); view.setMeasurementHighlights({}, {});
    MeasurementRecord displayedRecord; displayedRecord.id = 103; displayedRecord.request.kind = MeasurementKind::Distance;
    displayedRecord.request.coordinates = MeasurementCoordinates::Displayed;
    displayedRecord.request.a = {view.measurementPartEntity(leaves[0])};
    displayedRecord.request.b = {view.measurementPartEntity(leaves[1])};
    displayedRecord.result = calculateMeasurement(displayedRecord.request, cancel);
    require(displayedRecord.result.success, "display-coordinate snapshot distance failed");
    view.addMeasurementRecord(displayedRecord);
    require(!view.measurementRecordPositionChanged(103), "display-coordinate record was stale at capture position");
    view.setExplosion(70, -1);
    require(view.measurementRecordPositionChanged(103), "display-coordinate record did not become stale after movement");
    view.removeMeasurementRecord(103);
    view.resetPartPositions(); require(!view.measurementRecordPositionChanged(101), "measurement remained stale after reset");
    for (const auto& label : labels()) require(view.context()->IsDisplayed(label), "measurement did not reappear after position reset");
    view.setMeasurementRecordVisible(102, false);
    int displayed = 0; for (const auto& label : labels()) displayed += view.context()->IsDisplayed(label);
    require(displayed == 1, "per-record annotation visibility is not independent");
    view.setMeasurementPicking(true, MeasurementSelection::Face, 1); quality(view, view.meshQuality() == 0 ? 1 : 0);
    require(labels().size() == 2 && view.measurementPicking(), "mesh replacement lost history or measurement picking");
    performPick(MeasurementSelection::Face, 1);
    view.setGrabMode(true); require(!view.measurementPicking() && view.grabMode(), "grab mode and measurement picking are not exclusive");
    view.setMeasurementPicking(true, MeasurementSelection::Vertex);
    require(!view.grabMode() && view.measurementPicking(), "measurement picking did not disable grab mode");
    view.clearMeasurementRecords(); require(labels().empty(), "clear all left history presentations in AIS context");
    view.addMeasurementRecord(record); view.clearDocument(); require(labels().empty() && !view.measurementPicking(), "new document retained measurement history");
    QObject::disconnect(connection);
    QObject::disconnect(diagnostics);
    std::cout << "MEASUREMENT PASS: vertex/edge/face/solid/part/exact point picks, infinite line/plane guides and references, nested repeated world transforms, perspective/Retina, independent history, precision, inactive infinite overlays, explosion/reset, mesh replacement, grab exclusivity and clear\n";
}
void testDerivedMeasurementPicks(Stp::ViewerWidget& view)
{
    using namespace Stp;
    view.setMeasurementDecimals(3);
    auto doc=std::make_shared<CadDocument>();doc->xde=new TDocStd_Document("BinXCAF");
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc->xde->Main());
    const auto cylinder=shapes->AddShape(BRepPrimAPI_MakeCylinder(5,20).Shape(),false);
    const auto cone=shapes->AddShape(BRepPrimAPI_MakeCone(5,2,20).Shape(),false);
    const auto assembly=[&]{TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);return shapes->AddShape(compound,true);};
    const auto root=assembly(),nested=assembly();
    gp_Trsf local;local.SetTranslation(gp_Vec(7,11,13));shapes->AddComponent(nested,cylinder,TopLoc_Location(local));
    gp_Trsf coneLocal;coneLocal.SetTranslation(gp_Vec(27,11,13));shapes->AddComponent(nested,cone,TopLoc_Location(coneLocal));
    gp_Trsf parent;parent.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,1,0)),.4);parent.SetTranslationPart(gp_Vec(31,-14,8));
    shapes->AddComponent(root,nested,TopLoc_Location(parent));
    gp_Trsf repeated;repeated.SetTranslation(gp_Vec(-45,28,3));shapes->AddComponent(root,nested,TopLoc_Location(repeated));
    shapes->UpdateAssemblies();rebuildIndex(*doc);load(view,doc);
    auto viewport=view.findChild<QWidget*>("cadViewport");const double ratio=viewport->devicePixelRatioF();
    MeasurementEntity picked;bool didPick=false;
    auto connection=QObject::connect(&view,&ViewerWidget::measurementEntityPicked,[&](const MeasurementEntity& e,int){picked=e;didPick=true;});
    int count=0;
    const auto pick=[&](int occurrence,MeasurementSelection mode,GeomAbs_SurfaceType faceType){
        const auto source=view.measurementPartEntity(occurrence);
        view.setMeasurementPicking(false,MeasurementSelection::Part);view.showAll();view.selectOccurrences({occurrence});view.isolateSelection();
        view.standardView(0);view.fitAll();view.setMeasurementPicking(true,mode);view.setMeasurementHighlights({},{});didPick=false;
        const bool edgeSource=mode==MeasurementSelection::FeatureCenter||faceType==GeomAbs_Plane;
        for(TopExp_Explorer it(source.shape,edgeSource?TopAbs_EDGE:TopAbs_FACE);it.More()&&!didPick;it.Next()){
            std::vector<gp_Pnt> samples;
            if(edgeSource){
                BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));if(curve.GetType()!=GeomAbs_Circle)continue;
                for(double fraction:{.1,.3,.5,.7,.9})samples.push_back(curve.Value(curve.FirstParameter()+(curve.LastParameter()-curve.FirstParameter())*fraction));
            }else{
                BRepAdaptor_Surface surface(TopoDS::Face(it.Current()));if(surface.GetType()!=faceType)continue;
                for(double fraction:{.1,.3,.5,.7,.9})samples.push_back(surface.Value(surface.FirstUParameter()+(surface.LastUParameter()-surface.FirstUParameter())*fraction,
                    (surface.FirstVParameter()+surface.LastVParameter())/2.));
            }
            for(auto sample:samples){
                sample.Transform(source.displayTransform);int x,y;view.view()->Convert(sample.X(),sample.Y(),sample.Z(),x,y);
                const QPoint logical(qRound(x/ratio),qRound(y/ratio));if(!viewport->rect().contains(logical))continue;
                QTest::mouseClick(viewport,Qt::LeftButton,Qt::NoModifier,logical);if(didPick)break;
            }
        }
        require(didPick&&picked.occurrence==occurrence&&picked.selection==mode,"derived geometry native pick failed");
        require(!edgeSource||picked.shape.ShapeType()==TopAbs_EDGE,"circular axis was replaced by a face selection");
        TopTools_IndexedMapOfShape canonical;TopExp::MapShapes(source.shape,picked.shape.ShapeType(),canonical);
        require(canonical.Contains(picked.shape)&&same(picked.originalTransform,source.originalTransform),"derived pick lost nested definition identity");
        QString error;const auto reference=measurementFeatureReference(picked,MeasurementCoordinates::Original,&error);
        require(reference.has_value(),"native derived pick lacks analytic reference");
        auto original=picked;original.originalTransform=gp_Trsf();
        const auto localReference=measurementFeatureReference(original,MeasurementCoordinates::Original,&error);
        require(localReference.has_value(),"canonical derived source invalid");
        if(mode==MeasurementSelection::FeatureCenter){
            require(reference->center&&reference->center->Distance(localReference->center->Transformed(source.originalTransform))<1.e-6,"derived center was transformed twice");
            std::atomic_bool cancel(false);MeasurementRecord record;record.id=201+count;
            record.request={MeasurementKind::Point,MeasurementCoordinates::Original,{picked},{}};
            record.result=calculateMeasurement(record.request,cancel);require(record.result.success,"picked center calculation failed");
            view.addMeasurementRecord(record);const double scale=view.view()->Camera()->Scale();view.fitAll();
            require(nearEqual(scale,view.view()->Camera()->Scale()),"derived guides changed fitAll range");
            quality(view,view.meshQuality()==0?1:0);require(view.measurementPicking(),"mesh rebuild lost derived pick mode");
        }else {
            require(reference->axis&&reference->axis->Direction().IsEqual(localReference->axis->Direction().Transformed(source.originalTransform),1.e-6),"derived axis transform wrong");
            view.clearMeasurementRecords();
            view.setMeasurementHighlights({picked},{picked});
            AIS_ListOfInteractive draft;view.context()->DisplayedObjects(draft);
            for(const auto& item:draft) require(Handle(AIS_TextLabel)::DownCast(item).IsNull(),"axis draft shows an auxiliary text badge");
            for(auto kind:{MeasurementKind::Distance,MeasurementKind::Angle}) {
                std::atomic_bool cancel(false);MeasurementRecord record;record.id=300+count;
                record.request={kind,MeasurementCoordinates::Displayed,{picked},{picked}};
                record.result=calculateMeasurement(record.request,cancel);require(record.result.success,"axis label fixture failed");
                view.addMeasurementRecord(record);
                AIS_ListOfInteractive result;view.context()->DisplayedObjects(result);int textCount=0;
                const auto expectedText=kind==MeasurementKind::Distance?QStringLiteral("Min d 0.000 mm"):QStringLiteral("Angle 0.000 deg");
                for(const auto& item:result) {
                    const auto text=Handle(AIS_TextLabel)::DownCast(item);if(text.IsNull())continue;
                    ++textCount;const TCollection_AsciiString label(text->Text(),'?');
                    require(QString::fromUtf8(label.ToCString())==expectedText,"axis result retained a record number or infinity badge");
                }
                require(textCount==1,"axis measurement must show one value label only");
                view.removeMeasurementRecord(record.id);
            }
        }
        AIS_ListOfInteractive all;view.context()->DisplayedObjects(all);
        for(const auto& item:all)if(Handle(XCAFPrs_AISObject)::DownCast(item).IsNull()){
            TColStd_ListOfInteger modes;view.context()->ActivatedModes(item,modes);
            require(modes.IsEmpty()&&item->IsInfinite(),"derived guide is selectable or affects fitAll");
        }
        ++count;
    };
    for(bool perspective:{false,true}){
        view.setPerspective(perspective);
        for(size_t i=0;i<doc->occurrences.size();++i){
            const auto& occurrence=doc->occurrences[i];if(occurrence.partIndex<0)continue;
            bool isCone=false;for(TopExp_Explorer f(doc->parts[size_t(occurrence.partIndex)].shape,TopAbs_FACE);f.More();f.Next())
                isCone=isCone||BRepAdaptor_Surface(TopoDS::Face(f.Current())).GetType()==GeomAbs_Cone;
            if(!isCone){
                pick(int(i),MeasurementSelection::FeatureCenter,GeomAbs_Cylinder);
                pick(int(i),MeasurementSelection::FeatureAxis,GeomAbs_Plane); // Circular rim axis, not a planar face.
            }
            pick(int(i),MeasurementSelection::FeatureAxis,isCone?GeomAbs_Cone:GeomAbs_Cylinder);
        }
    }
    require(count==16,"derived native test did not cover all instances and cameras");
    view.clearMeasurementRecords();view.setMeasurementHighlights({},{});view.setMeasurementPicking(false,MeasurementSelection::Part);
    QObject::disconnect(connection);view.clearDocument();
    std::cout<<"DERIVED MEASUREMENT PASS: 16 real circle-center/circle-axis/cylinder-axis/cone-axis clicks, nested repeated instances, both cameras, Retina, nonselectable guides, mesh replacement and model-only fitAll\n";
}

#endif
}
int main(int argc,char** argv){
    Mayo::QtOpenGlUtils::platformSetup(argc,argv);
#if defined(Q_OS_MACOS)
    auto format=QSurfaceFormat::defaultFormat();format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif
    QApplication app(argc,argv);
    qputenv("CSF_ShadersDirectory",QByteArray(STP_OCCT_RESOURCE_DIR)+"/Shaders");
    try{
        require(app.arguments().size()==2,"pass colored-assembly.step as argument");
        std::atomic_bool cancel(false);auto doc=Stp::loadStep(app.arguments().last(),{},cancel);
        std::vector<gp_Trsf> originals;std::vector<int> leaves;int scope=-1;
        for(size_t i=0;i<doc->occurrences.size();++i){const auto& o=doc->occurrences[i];originals.push_back(o.worldTransform);
            if(o.partIndex>=0)leaves.push_back(int(i));else if(o.parent>=0&&o.children.size()>1)scope=int(i);}
        require(leaves.size()>1&&scope>=0,"fixture must include repeated nested assembly");
        Stp::ViewerWidget view;view.resize(900,650);view.show();QTest::qWait(100);load(view,doc);
        const auto shapes=objects(view);require(shapes.size()==leaves.size(),"one display item per leaf instance");
        std::vector<gp_Trsf> base;for(const auto& shape:shapes)base.push_back(shape->LocalTransformation());
        std::vector<bool> matched(leaves.size(),false);
        for(const auto& shape:shapes){
            const auto transform=shape->LocalTransformation()*XCAFDoc_ShapeTool::GetLocation(shape->GetLabel()).Transformation();
            bool found=false;for(size_t i=0;i<leaves.size();++i)if(!matched[i]&&same(transform,doc->occurrences[leaves[i]].worldTransform)){
                matched[i]=true;found=true;break;}
            require(found,"displayed nested transform differs from original occurrence");
        }
        view.selectOccurrences({leaves.front()});require(view.context()->NbSelected()==1,"scene selection failed");
        view.setTransparency(45,true);int transparent=0;for(const auto& shape:shapes)if(nearEqual(shape->Transparency(),0.45))++transparent;
        require(transparent==1,"selected transparency changed unrelated parts");
        view.setTransparency(0,false);view.isolateSelection();require(objects(view).size()==1,"isolation failed");
        view.showAll();require(objects(view).size()==shapes.size(),"show all did not restore parts");
        view.setOccurrenceVisible(scope,false);require(objects(view).size()<shapes.size(),"assembly hide failed");view.showAll();
        view.setExplosion(70,-1);bool moved=false;for(size_t i=0;i<shapes.size();++i)moved|=!same(base[i],shapes[i]->LocalTransformation());
        require(moved,"assembly explosion moved no parts");
        view.setExplosion(70,leaves.front());for(size_t i=0;i<shapes.size();++i)
            require(same(base[i],shapes[i]->LocalTransformation()),"switching explosion to a leaf did not restore previous transforms");
        view.setExplosion(50,scope);view.setExplosion(0,scope);for(size_t i=0;i<shapes.size();++i)
            require(same(base[i],shapes[i]->LocalTransformation()),"explosion reset changed source position");
        view.setSection(0,50,false,true);const auto clip=view.view()->ClipPlanes()->First();
        require(clip->IsOn()&&clip->IsCapping(),"clipping and capping not enabled");const auto normal=clip->GetEquation();
        view.setSection(0,50,true,false);require(!clip->IsCapping()&&nearEqual(clip->GetEquation().x(),-normal.x()),"clip reverse/cap controls failed");
        view.setSection(-1,50,false,true);require(!clip->IsOn(),"clipping did not turn off");
        view.setPerspective(true);require(view.view()->Camera()->ProjectionType()==Graphic3d_Camera::Projection_Perspective,"perspective switch failed");
        view.setPerspective(false);view.standardView(0);
        auto viewport=view.findChild<QWidget*>("cadViewport");require(viewport,"viewport missing");
        const auto direction=view.view()->Camera()->Direction();drag(viewport,Qt::LeftButton,{400,300},{470,350});
        require(!direction.IsEqual(view.view()->Camera()->Direction(),1.e-6),"mouse rotation did not change camera");
        testRightPan(view,viewport);
        const auto center=view.view()->Camera()->Center();drag(viewport,Qt::MiddleButton,{400,300},{470,350});
        require(center.Distance(view.view()->Camera()->Center())>1.e-6,"mouse pan did not change camera");
        const auto scale=view.view()->Scale();QWheelEvent wheel({400,300},{400,300},{0,0},{0,120},Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(viewport,&wheel);require(view.view()->Scale()>scale,"mouse wheel did not zoom");
        for(size_t i=0;i<originals.size();++i)require(same(originals[i],doc->occurrences[i].worldTransform),"view operation modified source transform");
        QTemporaryDir output;view.fitAll();QTest::qWait(100);require(view.saveScreenshot(output.filePath("view.png")),"OpenGL framebuffer screenshot failed");
        auto single=std::make_shared<Stp::CadDocument>();single->xde=new TDocStd_Document("BinXCAF");
        XCAFDoc_DocumentTool::ShapeTool(single->xde->Main())->AddShape(BRepPrimAPI_MakeBox(10,10,10).Shape(),false);
        Stp::rebuildIndex(*single);load(view,single);auto singleObject=objects(view).front();const auto singleBase=singleObject->LocalTransformation();
        view.setExplosion(100,-1);require(same(singleBase,singleObject->LocalTransformation()),"single part must not explode");
        testAppearanceAndPmiFailure(view);
        testMeshQuality(view);
        testPreparedRotatedMesh(view);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        testPartGrab(view);
        testMeasurementPickingAndHistory(view);
        testDerivedMeasurementPicks(view);
#endif
        std::cout<<"GUI PASS: occurrence transforms, selection, visibility, inherited/face/instance colors and alpha restoration, transparency scope/selection changes, PMI failure reporting, explosion/reset, clip/cap, camera input, mesh precision tiers/cancel and exact geometry/BOM immutability, framebuffer\n";
        return 0;
    }catch(const Standard_Failure& e){std::cerr<<e.GetMessageString()<<'\n';return 1;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
