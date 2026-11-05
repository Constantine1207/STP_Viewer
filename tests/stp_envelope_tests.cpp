#include "envelope_fixture.h"
#include "stp/color_edits.h"
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <TopExp_Explorer.hxx>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f,const char* why){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,why);}
bool contains(const TopoDS_Shape& s,const gp_Pnt& p){for(TopExp_Explorer it(s,TopAbs_SOLID);it.More();it.Next()){
    BRepClass3d_SolidClassifier c(TopoDS::Solid(it.Current()),p,1.e-7);if(c.State()==TopAbs_IN||c.State()==TopAbs_ON)return true;}return false;}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    std::atomic_bool cancel(false);Stp::EnvelopeOptions options;auto doc=envelopeAssemblyFixture();
    auto result=Stp::buildCustomerEnvelope(doc,{},envelopeRoots(doc),options,cancel);
    require(result.inputOccurrences==3&&result.outputSolids==1&&result.filledCavities==1,"closed cavity or internal PCB not removed");
    require(std::abs(result.outputVolume-8000)<1.e-5&&contains(result.shape,gp_Pnt(10,10,10)),"filled geometry is not solid");
    require(!contains(doc.parts[0].shape,gp_Pnt(10,10,10))&&doc.parts.size()==3,"source changed");
    options.fillClosedCavities=false;auto unfilled=Stp::buildCustomerEnvelope(doc,{},envelopeRoots(doc),options,cancel);
    require(!contains(unfilled.shape,gp_Pnt(10,10,10))&&unfilled.outputSolids==3,"fill-off option ignored");options.fillClosedCavities=true;
    auto through=envelopeFixture({BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(20,20,20).Shape(),
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(10,10,-1),gp_Dir(0,0,1)),2,22).Shape()).Shape()});
    auto hole=Stp::buildCustomerEnvelope(through,{},envelopeRoots(through),options,cancel);
    require(hole.filledCavities==0&&!contains(hole.shape,gp_Pnt(10,10,10)),"mounting through-hole filled");
    require(std::abs(hole.outputVolume-(8000-80*std::acos(-1.)))<1.e-4,"through hole volume changed");
    auto open=envelopeFixture({BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(20,20,20).Shape(),
        BRepPrimAPI_MakeBox(gp_Pnt(2,2,2),16,16,20).Shape()).Shape()});
    auto openResult=Stp::buildCustomerEnvelope(open,{},envelopeRoots(open),options,cancel);
    require(openResult.filledCavities==0&&!contains(openResult.shape,gp_Pnt(10,10,10))&&!openResult.warnings.empty(),"open cavity silently filled or certified");
    auto separated=envelopeFixture({BRepPrimAPI_MakeBox(10,10,10).Shape(),BRepPrimAPI_MakeBox(gp_Pnt(10.1,0,0),10,10,10).Shape()});
    auto gap=Stp::buildCustomerEnvelope(separated,{},envelopeRoots(separated),options,cancel);
    require(gap.outputSolids==2&&std::abs(gap.outputVolume-2000)<1.e-5&&!contains(gap.shape,gp_Pnt(10.05,5,5)),"gap bridged");
    auto overlap=envelopeFixture({BRepPrimAPI_MakeBox(10,10,10).Shape(),BRepPrimAPI_MakeBox(gp_Pnt(5,0,0),10,10,10).Shape()});
    auto united=Stp::buildCustomerEnvelope(overlap,{},envelopeRoots(overlap),options,cancel);
    require(united.outputSolids==1&&std::abs(united.outputVolume-1500)<1.e-5,"overlap double counted");
    // The source shell is made from two contacting halves, not pre-closed as one solid.
    auto lower=BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(20,20,10).Shape(),BRepPrimAPI_MakeBox(gp_Pnt(2,2,2),16,16,10).Shape()).Shape();
    auto upper=BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(gp_Pnt(0,0,10),20,20,10).Shape(),BRepPrimAPI_MakeBox(gp_Pnt(2,2,8),16,16,10).Shape()).Shape();
    auto halves=envelopeFixture({lower,upper});auto closed=Stp::buildCustomerEnvelope(halves,{},envelopeRoots(halves),options,cancel);
    require(closed.filledCavities==1&&closed.outputSolids==1&&std::abs(closed.outputVolume-8000)<1.e-5,"touching housing halves failed");
    auto partial=envelopeFixture({envelopeClosedShell(),BRepPrimAPI_MakeBox(gp_Pnt(10,8,8),15,4,4).Shape()});
    auto protruding=Stp::buildCustomerEnvelope(partial,{},envelopeRoots(partial),options,cancel);
    require(contains(protruding.shape,gp_Pnt(24,10,10))&&std::abs(protruding.bounds[3]-25)<1.e-6,"partially protruding part deleted");
    auto moveDoc=envelopeFixture({BRepPrimAPI_MakeBox(2,3,4).Shape()});int leaf=-1;
    for(size_t i=0;i<moveDoc.occurrences.size();++i)if(moveDoc.occurrences[i].partIndex>=0)leaf=int(i);
    gp_Trsf rotation;rotation.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,0,1)),std::acos(-1.)/2);rotation.SetTranslationPart(gp_Vec(10,20,30));
    auto moved=Stp::buildCustomerEnvelope(moveDoc,{{leaf,rotation}},{leaf,leaf},options,cancel);
    require(moved.inputOccurrences==1&&std::abs(moved.bounds[0]-7)<1.e-6&&std::abs(moved.bounds[5]-34)<1.e-6,"applied transform or scope deduplication wrong");
    rejects([&]{Stp::buildCustomerEnvelope(doc,{}, {},options,cancel);},"empty scope accepted");
    auto surface=envelopeFixture({BRepBuilderAPI_MakeFace(gp_Pln(),0,10,0,10).Shape()});
    rejects([&]{Stp::buildCustomerEnvelope(surface,{},envelopeRoots(surface),options,cancel);},"open face accepted");
    rejects([&]{Stp::buildCustomerEnvelope(doc,{},envelopeRoots(doc),options,cancel,[&](int n,const QString&){if(n>=45)cancel=true;});},"mid-operation cancel ignored");cancel=false;
    QTemporaryDir dir;const auto path=dir.filePath(QStringLiteral("客户 外包络.step"));
    Stp::exportCustomerEnvelope(result,options,path,doc.sourcePath,cancel);
    const auto imported=Stp::loadStep(path,{},cancel);require(imported->parts.size()==1,"customer export still has internal parts");
    require(imported->parts[0].name=="CustomerEnvelope"&&imported->parts[0].material.isEmpty()&&!imported->parts[0].densityKgM3,"private metadata retained");
    for(size_t i=0;i<imported->occurrences.size();++i)if(imported->occurrences[i].partIndex>=0){
        auto color=Stp::occurrenceBaseColor(*imported,int(i));require(std::abs(color.red()-options.color.red())<=1,"uniform export color lost");}
    QFile f(path);require(f.open(QIODevice::ReadOnly),"cannot inspect STEP");const auto bytes=f.readAll();f.close();
    require(!bytes.contains("SecretPart")&&!bytes.contains("Confidential"),"source names leaked");
    rejects([&]{Stp::exportCustomerEnvelope(result,options,path,path,cancel);},"source overwrite allowed");
    rejects([&]{Stp::exportCustomerEnvelope(result,options,path,doc.sourcePath,cancel,[&](int n,const QString&){if(n>=60)cancel=true;});},"export cancellation ignored");cancel=false;
    require(f.open(QIODevice::ReadOnly)&&f.readAll()==bytes,"failed export replaced destination");f.close();
    Stp::exportCustomerEnvelope(gap,options,dir.filePath("multi.step"),doc.sourcePath,cancel);
    Stp::exportCustomerEnvelope(moved,options,dir.filePath("moved.step"),doc.sourcePath,cancel);
    // Located definitions lose STEP instance styles unless the location is on
    // the assembly component. Verify all channels, not just the first channel.
    const auto movedRead=Stp::loadStep(dir.filePath("moved.step"),{},cancel);
    int movedCount=0;for(size_t i=0;i<movedRead->occurrences.size();++i){const auto& o=movedRead->occurrences[i];if(o.partIndex<0)continue;
        ++movedCount;const auto c=Stp::occurrenceBaseColor(*movedRead,int(i));
        require(std::abs(c.red()-options.color.red())<=1&&std::abs(c.green()-options.color.green())<=1&&std::abs(c.blue()-options.color.blue())<=1,"moved STEP lost RGB color");
        require(contains(movedRead->parts[o.partIndex].shape.Moved(TopLoc_Location(o.worldTransform)),gp_Pnt(8,21,32)),"moved STEP geometry placement lost");
    }require(movedCount==1,"moved STEP duplicate instance");
    // Per-instance preservation must not fill the connector or touch the source definition.
    Stp::EnvelopeOptions keep;const auto leaves=Stp::envelopeScope(doc,envelopeRoots(doc));
    keep.preservedOccurrences={leaves[1]};keep.preservedColors[leaves[1]]=QColor("#e04030");
    auto retained=Stp::buildCustomerEnvelope(doc,{},envelopeRoots(doc),keep,cancel);
    require(retained.preservedOccurrences==1&&retained.processedOccurrences==2&&retained.pieces.size()==2,"preservation scope wrong");
    require(std::abs(retained.overlapVolume-100)<1.e-5,"retained overlap not reported");
    require(retained.pieces.back().sourceOccurrence==leaves[1]&&retained.pieces.back().color==QColor("#e04030"),"instance identity/color lost");
    const auto keepPath=dir.filePath("retained.step");Stp::exportCustomerEnvelope(retained,keep,keepPath,doc.sourcePath,cancel);
    auto keepRead=Stp::loadStep(keepPath,{},cancel);require(keepRead->parts.size()==2,"preserved part fused at export");
    bool red=false;for(size_t i=0;i<keepRead->occurrences.size();++i){const auto& o=keepRead->occurrences[i];if(o.partIndex<0)continue;
        std::cout<<"Retained round-trip part: "<<keepRead->parts[o.partIndex].name.toStdString()<<"\n";
        require(keepRead->parts[o.partIndex].name=="CustomerEnvelope"||keepRead->parts[o.partIndex].name=="RetainedPart1","original name leaked");
        if(keepRead->parts[o.partIndex].name=="RetainedPart1")red=std::abs(Stp::occurrenceBaseColor(*keepRead,int(i)).red()-224)<=1;}
    require(red,"retained color lost in STEP");
    keep.preservedOccurrences=envelopeRoots(doc);auto allKept=Stp::buildCustomerEnvelope(doc,{},envelopeRoots(doc),keep,cancel);
    require(allKept.preservedOccurrences==3&&allKept.processedOccurrences==0&&allKept.filledCavities==0&&allKept.outputSolids==3,"all preserved or group expansion failed");
    require(!contains(allKept.shape,gp_Pnt(10,10,10)),"all-preserved cavity filled");
    keep.preservedOccurrences={leaves[1]};auto outside=Stp::buildCustomerEnvelope(doc,{}, {leaves[0]},keep,cancel);
    require(outside.preservedOccurrences==0&&outside.processedOccurrences==1,"out-of-scope rule affected result");
    Stp::EnvelopeOptions movedKeep;movedKeep.preservedOccurrences={leaf};movedKeep.preservedColors[leaf]=QColor("#d02f8a");
    auto movedRetained=Stp::buildCustomerEnvelope(moveDoc,{{leaf,rotation}},{leaf},movedKeep,cancel);
    require(std::abs(movedRetained.bounds[0]-7)<1.e-6&&std::abs(movedRetained.bounds[5]-34)<1.e-6,"retained placement lost");
    Stp::exportCustomerEnvelope(movedRetained,movedKeep,dir.filePath("moved-retained.step"),moveDoc.sourcePath,cancel);
    // Connector is a through socket entirely inside the closed housing.
    auto socket=BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(gp_Pnt(8,8,8),4,4,4).Shape(),
        BRepPrimAPI_MakeBox(gp_Pnt(9.5,9.5,7),1,1,6).Shape()).Shape();
    auto socketDoc=envelopeFixture({envelopeClosedShell(),socket});const auto socketLeaves=Stp::envelopeScope(socketDoc,envelopeRoots(socketDoc));
    Stp::EnvelopeOptions socketOptions;socketOptions.preservedOccurrences={socketLeaves[1]};
    auto blocked=Stp::buildCustomerEnvelope(socketDoc,{},envelopeRoots(socketDoc),socketOptions,cancel);
    require(contains(blocked.shape,gp_Pnt(10,10,10)),"fixture does not demonstrate blocked socket");
    gp_Trsf toolPlace;toolPlace.SetTranslation(gp_Vec(9.5,9.5,8));
    socketOptions.protections.push_back({Stp::envelopeProtectionPrimitive(false,1,1,4,toolPlace),"socket clearance",false});
    auto protectedSocket=Stp::buildCustomerEnvelope(socketDoc,{},envelopeRoots(socketDoc),socketOptions,cancel);
    require(!contains(protectedSocket.shape,gp_Pnt(10,10,10))&&std::abs(protectedSocket.removedVolume-4)<1.e-5,"socket clearance blocked after fill");
    require(protectedSocket.removedOriginalVolume<1.e-6&&protectedSocket.preservedOccurrences==1,"protection cut original material");
    Stp::exportCustomerEnvelope(protectedSocket,socketOptions,dir.filePath("protected-socket.step"),socketDoc.sourcePath,cancel);
    socketOptions.protections[0].shape=BRepPrimAPI_MakeBox(gp_Pnt(7,7,7),6,6,6).Shape();
    rejects([&]{Stp::buildCustomerEnvelope(socketDoc,{},envelopeRoots(socketDoc),socketOptions,cancel);},"tool cut through preserved socket accepted");
    auto shellDoc=envelopeFixture({envelopeClosedShell()});Stp::EnvelopeOptions cutting;
    cutting.protections.push_back({BRepPrimAPI_MakeBox(gp_Pnt(9,9,-1),2,2,22).Shape(),"mounting clearance",false});
    rejects([&]{Stp::buildCustomerEnvelope(shellDoc,{},envelopeRoots(shellDoc),cutting,cancel);},"original material removed without permission");
    cutting.protections[0].allowOriginalCut=true;auto cut=Stp::buildCustomerEnvelope(shellDoc,{},envelopeRoots(shellDoc),cutting,cancel);
    require(cut.removedOriginalVolume>0&&!contains(cut.shape,gp_Pnt(10,10,1)),"explicit original cut ignored");
    cutting.protections[0].shape=BRepPrimAPI_MakeBox(gp_Pnt(-1,-1,-1),22,22,22).Shape();
    rejects([&]{Stp::buildCustomerEnvelope(shellDoc,{},envelopeRoots(shellDoc),cutting,cancel);},"empty body accepted");
    cutting.protections[0].shape=BRepBuilderAPI_MakeFace(gp_Pln(),0,10,0,10).Shape();
    rejects([&]{Stp::buildCustomerEnvelope(shellDoc,{},envelopeRoots(shellDoc),cutting,cancel);},"open protection tool accepted");
    rejects([&]{Stp::envelopeProtectionPrimitive(false,0,1,1,gp_Trsf());},"zero-size tool accepted");
    auto cylinder=Stp::envelopeProtectionPrimitive(true,2,1,8,rotation);
    require(contains(cylinder,gp_Pnt(10,20,31)),"transformed cylinder basis wrong");
    auto named=doc;named.occurrences[leaves[1]].name="USB_CONNECTOR_A";
    const auto candidates=Stp::envelopePreservationCandidates(named,{"connector","连接器",""});
    require(candidates.size()==1&&candidates.count(leaves[1])&&options.preservedOccurrences.empty(),"recommendation changed rules or matched empty keyword");
    // Shared definitions remain independent occurrence rules.
    auto shared=envelopeFixture({BRepPrimAPI_MakeBox(2,3,4).Shape()});
    auto shapeTool=XCAFDoc_DocumentTool::ShapeTool(shared.xde->Main());
    const auto sharedRoot=shared.occurrences.front().label;gp_Trsf offset;offset.SetTranslation(gp_Vec(10,0,0));
    shapeTool->AddComponent(sharedRoot,shared.parts.front().label,TopLoc_Location(offset));shapeTool->UpdateAssemblies();Stp::rebuildIndex(shared);
    const auto sharedLeaves=Stp::envelopeScope(shared,envelopeRoots(shared));Stp::EnvelopeOptions one;one.preservedOccurrences={sharedLeaves.back()};
    auto sharedResult=Stp::buildCustomerEnvelope(shared,{},envelopeRoots(shared),one,cancel);
    require(sharedResult.preservedOccurrences==1&&sharedResult.processedOccurrences==1,"shared definition rule leaked to all instances");
    std::cout<<"Envelope: closed cavities, PCB removal, holes, gaps, overlaps, contacting halves, protrusions, transforms, scope, rejection, cancellation and STEP round-trip passed\n";
    return 0;
}catch(const Standard_Failure& e){std::cerr<<e.GetMessageString();return 1;}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
