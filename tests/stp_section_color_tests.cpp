#include "section_state.h"
#include "assembly_export.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <stdexcept>
#include <cmath>

namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void name(const TDF_Label& label,const char* value){TDataStd_Name::Set(label,TCollection_ExtendedString(value));}
gp_Trsf shift(double x,double y,double z){gp_Trsf t;t.SetTranslation(gp_Vec(x,y,z));return t;}
bool sameColor(QColor a,QColor b){return std::abs(a.red()-b.red())<=1&&std::abs(a.green()-b.green())<=1&&std::abs(a.blue()-b.blue())<=1;}
Stp::CadDocument fixture(){
    Stp::CadDocument d;d.sourcePath="source.step";d.xde=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(d.xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d.xde,.001);
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(d.xde->Main());auto colors=XCAFDoc_DocumentTool::ColorTool(d.xde->Main());
    auto part=shapes->AddShape(BRepPrimAPI_MakeBox(2,3,5).Shape(),false);name(part,"Shared");
    const QColor blue("#2366c4"),red("#dd2222");
    colors->SetColor(part,Quantity_Color(blue.redF(),blue.greenF(),blue.blueF(),Quantity_TOC_sRGB),XCAFDoc_ColorGen);
    const auto f=TopExp_Explorer(shapes->GetShape(part),TopAbs_FACE).Current();
    colors->SetColor(shapes->AddSubShape(part,f),Quantity_Color(red.redF(),red.greenF(),red.blueF(),Quantity_TOC_sRGB),XCAFDoc_ColorSurf);
    XCAFDoc_DocumentTool::MaterialTool(d.xde->Main())->SetMaterial(part,new TCollection_HAsciiString("Steel"),
        new TCollection_HAsciiString(""),7.85,new TCollection_HAsciiString("density"),new TCollection_HAsciiString("MASS_DENSITY_MEASURE"));
    auto assembly=[&](const char* n){TopoDS_Compound compound;BRep_Builder().MakeCompound(compound);auto l=shapes->AddShape(compound,false);name(l,n);return l;};
    auto sub=assembly("Sub");name(shapes->AddComponent(sub,part,TopLoc_Location(shift(7,3,2))),"A");
    name(shapes->AddComponent(sub,part,TopLoc_Location(shift(20,3,2))),"B");
    auto root=assembly("Root");name(shapes->AddComponent(root,sub,TopLoc_Location(shift(100,20,30))),"Left");
    name(shapes->AddComponent(root,sub,TopLoc_Location(shift(-40,70,0))),"Right");
    shapes->UpdateAssemblies();Stp::rebuildIndex(d);return d;
}
int leaf(const Stp::CadDocument& d,const QString& suffix){for(size_t i=0;i<d.occurrences.size();++i)
    if(d.occurrences[i].partIndex>=0&&d.occurrences[i].path.endsWith(suffix))return int(i);throw std::runtime_error("missing instance");}
void compare(const Stp::CadDocument& a,const Stp::CadDocument& b,const Stp::PlacementEdits& moves,const Stp::ColorEdits& colors){
    size_t count=0;for(const auto& o:b.occurrences)if(o.partIndex>=0)++count;require(count==4,"extra free part or missing instance");
    for(size_t i=0;i<a.occurrences.size();++i){const auto& o=a.occurrences[i];if(o.partIndex<0)continue;
        const int j=leaf(b,o.path);const auto& original=a.parts[o.partIndex];const auto& result=b.parts[b.occurrences[j].partIndex];
        require(Stp::samePlacement(Stp::placedWorldTransform(a,moves,int(i)),b.occurrences[j].worldTransform,1.e-7),"placement changed");
        require(original.volumeMm3&&result.volumeMm3&&std::abs(*original.volumeMm3-*result.volumeMm3)<1.e-6,"geometry volume changed");
        require(original.areaMm2&&result.areaMm2&&std::abs(*original.areaMm2-*result.areaMm2)<1.e-6,"geometry area changed");
        require(Stp::placedPartCenter(a,moves,int(i)).Distance(Stp::placedPartCenter(b,{},j))<1.e-6,"geometry center changed");
        require(original.material==result.material,"physical material changed");
        require(original.densityKgM3&&result.densityKgM3&&std::abs(*original.densityKgM3-*result.densityKgM3)<1.e-5,"density changed");
        const auto c=colors.find(int(i));const QColor expected=c==colors.end()?Stp::occurrenceBaseColor(a,int(i)):c->second;
        require(sameColor(Stp::occurrenceBaseColor(b,j),expected),"instance color lost or leaked");
        auto ct=XCAFDoc_DocumentTool::ColorTool(b.xde->Main());TDF_LabelSequence faces;
        XCAFDoc_DocumentTool::ShapeTool(b.xde->Main())->GetSubShapes(result.label,faces);
        bool originalFace=false;
        for(int k=1;k<=faces.Length();++k){Quantity_Color q;if(!ct->GetColor(faces(k),XCAFDoc_ColorSurf,q))continue;
            double r,g,bl;q.Values(r,g,bl,Quantity_TOC_sRGB);const auto faceColor=QColor::fromRgbF(r,g,bl);
            if(c!=colors.end())require(sameColor(faceColor,expected),"original face color overrides edited color");
            else originalFace|=sameColor(faceColor,QColor("#dd2222"));}
        if(c==colors.end())require(originalFace,"original face color lost on unedited sibling");
    }
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    Stp::SectionState state;state.enabled=true;state.base=gp_Pln(gp_Pnt(10,20,30),gp_Dir(0,0,1));state.pivot=gp_Pnt(10,20,30);
    state.rotationAxis=1;state.angleDegrees=90;state.offsetMm=5;
    auto plane=Stp::sectionPlane(state);require(plane.Axis().Direction().Dot(gp_Dir(1,0,0))>1.-1.e-10,"rotated normal");
    require(plane.Location().Distance(gp_Pnt(15,20,30))<1.e-8,"offset after rotation");
    state.reverse=true;require(Stp::sectionPlane(state).Axis().Direction().Dot(plane.Axis().Direction())<-.999,"reverse normal");
    state.reverse=false;state.pivot=gp_Pnt();state.offsetMm=0;plane=Stp::sectionPlane(state);
    require(plane.Location().Distance(gp_Pnt(30,20,-10))<1.e-8,"custom rotation pivot");
    const auto face=BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0,0,2),gp_Dir(0,0,1)),-5,5,-5,5).Face();
    const auto pick=gp_Pnt(7,9,12);const auto picked=Stp::sectionPlaneFromFace(face,shift(7,9,10),pick);
    require(picked.Distance(pick)<1.e-8&&picked.Location().Distance(pick)<1.e-8,"picked plane placement");
    bool rejected=false;const auto cylinder=BRepPrimAPI_MakeCylinder(2,5).Shape();
    for(TopExp_Explorer e(cylinder,TopAbs_FACE);e.More();e.Next()){try{Stp::sectionPlaneFromFace(TopoDS::Face(e.Current()),gp_Trsf(),gp_Pnt());}catch(const std::invalid_argument&){rejected=true;}}
    require(rejected,"curved face accepted as plane");
    auto source=fixture();const auto sourceColor=Stp::occurrenceBaseColor(source,leaf(source,"Left/A"));
    const int a=leaf(source,"Left/A"),b=leaf(source,"Right/A");
    Stp::ColorEdits colors{{a,QColor("#f09b16")},{b,QColor("#10a86b")}};
    Stp::PlacementEdits moves{{a,shift(20,-3,7)}};std::atomic_bool cancel(false);QTemporaryDir directory;
    const auto path=directory.filePath(QStringLiteral("改色 装配.stp"));
    Stp::exportPlacedAssembly(source,moves,path,{},cancel,colors);compare(source,*Stp::loadStep(path,{},cancel),moves,colors);
    require(sameColor(Stp::occurrenceBaseColor(source,a),sourceColor),"source XDE mutated");
    // All occurrences colored must not leave unused source definitions as extra roots.
    for(size_t i=0;i<source.occurrences.size();++i)if(source.occurrences[i].partIndex>=0)colors[int(i)]=QColor("#8030b0");
    Stp::exportPlacedAssembly(source,moves,path,{},cancel,colors);compare(source,*Stp::loadStep(path,{},cancel),moves,colors);
    QFile output(path);require(output.open(QIODevice::ReadOnly),"read existing output");auto bytes=output.readAll();output.close();
    rejected=false;try{Stp::exportPlacedAssembly(source,moves,path,[&](int p,const QString&){if(p>=80)cancel=true;},cancel,colors);}catch(const std::exception&){rejected=true;}
    cancel=false;output.open(QIODevice::ReadOnly);require(rejected&&output.readAll()==bytes,"canceled color export damaged output");output.close();
    Stp::exportPlacedAssembly(source,{},path,{},cancel,{});compare(source,*Stp::loadStep(path,{},cancel),{},{});
    rejected=false;try{Stp::validateColorEdits(source,{{-1,QColor("red")}});}catch(const std::exception&){rejected=true;}require(rejected,"invalid color instance");
    std::cout<<"PASS: exact planes/rotation/pivots/picking, repeated-instance colors and placements, all-colored roots, original face styles/material/density, full geometry, STEP roundtrip, Unicode, cancellation and source immutability\n";return 0;
}catch(const Standard_Failure& e){std::cerr<<e.GetMessageString()<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
