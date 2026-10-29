#include "assembly_export.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopExp_Explorer.hxx>
#include <Quantity_Color.hxx>
#include <TDataStd_Name.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool b,const char* text){if(!b)throw std::runtime_error(text);}
void name(const TDF_Label& l,const char* n){TDataStd_Name::Set(l,TCollection_ExtendedString(n));}
gp_Trsf translation(double x,double y,double z){gp_Trsf t;t.SetTranslation(gp_Vec(x,y,z));return t;}
Stp::CadDocument fixture(){
    Stp::CadDocument d;d.xde=new TDocStd_Document("BinXCAF");d.sourcePath="fixture.stp";
    XCAFDoc_DocumentTool::Set(d.xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d.xde,0.001);
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(d.xde->Main());
    auto assembly=[&](const char* n){TopoDS_Compound c;BRep_Builder().MakeCompound(c);auto l=shapes->AddShape(c,false);name(l,n);return l;};
    auto part=shapes->AddShape(BRepPrimAPI_MakeBox(2,3,5).Shape(),false);name(part,"Box");
    XCAFDoc_DocumentTool::ColorTool(d.xde->Main())->SetColor(part,Quantity_Color(0.2,0.4,0.8,Quantity_TOC_RGB),XCAFDoc_ColorGen);
    const auto face=TopExp_Explorer(shapes->GetShape(part),TopAbs_FACE).Current();
    XCAFDoc_DocumentTool::ColorTool(d.xde->Main())->SetColor(shapes->AddSubShape(part,face),Quantity_Color(0.9,0.1,0.2,Quantity_TOC_RGB),XCAFDoc_ColorSurf);
    XCAFDoc_DocumentTool::MaterialTool(d.xde->Main())->SetMaterial(part,
        new TCollection_HAsciiString("Steel"),new TCollection_HAsciiString("fixture material"),7.85,
        new TCollection_HAsciiString("density"),new TCollection_HAsciiString("MASS_DENSITY_MEASURE"));
    auto sub=assembly("Sub");auto a=shapes->AddComponent(sub,part,TopLoc_Location(translation(7,3,2)));name(a,"A");
    auto b=shapes->AddComponent(sub,part,TopLoc_Location(translation(-8,1,4)));name(b,"B");
    auto root=assembly("Root");gp_Trsf rotate;rotate.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,0,1)),std::acos(-1.)/2);
    name(shapes->AddComponent(root,sub,TopLoc_Location(translation(100,20,30)*rotate)),"Left");
    name(shapes->AddComponent(root,sub,TopLoc_Location(translation(-40,70,0))),"Right");
    shapes->UpdateAssemblies();Stp::rebuildIndex(d);return d;
}
int leaf(const Stp::CadDocument& d,const QString& suffix){for(size_t i=0;i<d.occurrences.size();++i)if(d.occurrences[i].partIndex>=0&&d.occurrences[i].path.endsWith(suffix))return int(i);throw std::runtime_error("leaf missing");}
int leafCount(const Stp::CadDocument& d){int n=0;for(const auto& o:d.occurrences)if(o.partIndex>=0)++n;return n;}
void compare(const Stp::CadDocument& source,const Stp::PlacementEdits& edits,const Stp::CadDocument& result,bool serialized=true){
    require(leafCount(source)==leafCount(result),"instance count changed");
    for(size_t i=0;i<source.occurrences.size();++i){const auto& o=source.occurrences[i];if(o.partIndex<0)continue;
        const int j=leaf(result,o.path);const auto& p=source.parts[o.partIndex];const auto& q=result.parts[result.occurrences[j].partIndex];
        require(Stp::samePlacement(Stp::placedWorldTransform(source,edits,int(i)),result.occurrences[j].worldTransform,1.e-7),"world placement mismatch after export");
        require(p.volumeMm3&&q.volumeMm3&&std::abs(*p.volumeMm3-*q.volumeMm3)<1.e-6,"volume changed");
        require(p.areaMm2&&q.areaMm2&&std::abs(*p.areaMm2-*q.areaMm2)<1.e-6,"area changed");
        require(Stp::placedPartCenter(source,edits,int(i)).Distance(Stp::placedPartCenter(result,{},j))<1.e-6,"transformed center mismatch");
        Quantity_Color c;require(XCAFDoc_DocumentTool::ColorTool(result.xde->Main())->GetColor(q.label,XCAFDoc_ColorGen,c)
            ||XCAFDoc_DocumentTool::ColorTool(result.xde->Main())->GetColor(q.label,XCAFDoc_ColorSurf,c),"part color lost");
        require(std::abs(c.Red()-.2)<1.e-5&&std::abs(c.Blue()-.8)<1.e-5,"part color changed");
        require(q.material==p.material,"material name changed");
        if(serialized)require(p.densityKgM3&&q.densityKgM3&&std::abs(*p.densityKgM3-*q.densityKgM3)<1.e-5,"density changed");
        TDF_LabelSequence faces;XCAFDoc_DocumentTool::ShapeTool(result.xde->Main())->GetSubShapes(q.label,faces);
        bool redFace=false;
        for(int k=1;k<=faces.Length();++k)if(XCAFDoc_DocumentTool::ColorTool(result.xde->Main())->GetColor(faces(k),XCAFDoc_ColorSurf,c))
            redFace|=std::abs(c.Red()-.9)<1.e-5&&std::abs(c.Green()-.1)<1.e-5;
        require(redFace,"per-face style lost");
    }
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    auto source=fixture();require(leafCount(source)==4&&source.parts.size()==1,"fixture must repeat shared definitions");
    const int moved=leaf(source,"Left/A"),other=leaf(source,"Right/A");const auto before=source.occurrences[moved].worldTransform;
    Stp::PlacementEdits edits;edits[moved]=translation(20,-5,7);gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(9,-3,8),gp_Dir(0,1,0)),-std::acos(-1.)/2);edits[moved]=rotation*edits[moved];
    edits[leaf(source,"Right/B")]=translation(0,0,12);
    std::atomic_bool cancel(false);Stp::CadDocument staged;staged.xde=Stp::buildPlacedAssembly(source,edits,cancel);Stp::rebuildIndex(staged);compare(source,edits,staged,false);
    require(Stp::samePlacement(source.occurrences[moved].worldTransform,before),"source document mutated");
    require(Stp::samePlacement(source.occurrences[other].worldTransform,Stp::placedWorldTransform(source,edits,other)),"sibling instance changed");
    QTemporaryDir dir;require(dir.isValid(),"temp directory");auto file=dir.filePath(QStringLiteral("装配 modified.stp"));
    Stp::exportPlacedAssembly(source,edits,file,{},cancel);auto loaded=Stp::loadStep(file,{},cancel);compare(source,edits,*loaded);
    // Empty edits must also preserve all original placements.
    auto original=dir.filePath("original.step");Stp::exportPlacedAssembly(source,{},original,{},cancel);compare(source,{},*Stp::loadStep(original,{},cancel));
    QFile output(file);require(output.open(QIODevice::ReadOnly),"read output");const auto bytes=output.readAll();output.close();
    bool cancelled=false;try{Stp::exportPlacedAssembly(source,edits,file,[&](int p,const QString&){if(p>=80)cancel=true;},cancel);}catch(const std::exception&){cancelled=true;}
    cancel=false;require(cancelled,"cancel not propagated");output.open(QIODevice::ReadOnly);require(output.readAll()==bytes,"cancel corrupted existing output");output.close();
    auto protectedSource=source;protectedSource.sourcePath=file;bool rejected=false;
    try{Stp::exportPlacedAssembly(protectedSource,edits,file,{},cancel);}catch(const std::exception&){rejected=true;}require(rejected,"source overwrite allowed");
    rejected=false;try{Stp::validatePlacementEdits(source,{{999,gp_Trsf()}});}catch(const std::exception&){rejected=true;}require(rejected,"invalid occurrence accepted");
    // A single free part with an intrinsic location must not lose or double that location.
    Stp::CadDocument single;single.xde=new TDocStd_Document("BinXCAF");single.sourcePath="single.stp";XCAFDoc_DocumentTool::Set(single.xde->Main());
    auto shape=BRepPrimAPI_MakeBox(2,3,5).Shape().Moved(TopLoc_Location(translation(13,17,19)));
    auto l=XCAFDoc_DocumentTool::ShapeTool(single.xde->Main())->AddShape(shape,false);name(l,"Single");Stp::rebuildIndex(single);
    Stp::PlacementEdits one{{0,translation(20,0,0)}};auto f=dir.filePath("single.stp");Stp::exportPlacedAssembly(single,one,f,{},cancel);
    auto back=Stp::loadStep(f,{},cancel);require(leafCount(*back)==1,"single part duplicated");
    require(Stp::placedPartCenter(single,one,0).Distance(Stp::placedPartCenter(*back,{},leaf(*back,"Single")))<1.e-6,"located free part changed");
    std::cout<<"PASS: nested/repeated instances, rotation about non-origin pivot, independent edits, original source, exact STEP roundtrip, names/colors, volume/area/center, Unicode path, cancellation, overwrite protection, located free part\n";return 0;
}catch(const Standard_Failure& e){std::cerr<<"OCCT: "<<e.GetMessageString()<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
