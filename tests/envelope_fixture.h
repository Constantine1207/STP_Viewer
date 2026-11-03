#pragma once
#include "stp/customer_envelope.h"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <TDataStd_Name.hxx>

inline Stp::CadDocument envelopeFixture(const std::vector<TopoDS_Shape>& parts){
    Stp::CadDocument d;d.sourcePath="Confidential-Source.step";d.xde=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(d.xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d.xde,.001);
    auto tool=XCAFDoc_DocumentTool::ShapeTool(d.xde->Main());
    TopoDS_Compound c;BRep_Builder().MakeCompound(c);auto root=tool->AddShape(c,false);
    TDataStd_Name::Set(root,TCollection_ExtendedString("Confidential-Assembly"));
    int n=0;for(const auto& shape:parts){
        auto p=tool->AddShape(shape,false);const auto name=QString("SecretPart%1").arg(++n).toUtf8();
        TDataStd_Name::Set(p,TCollection_ExtendedString(name.constData()));
        tool->AddComponent(root,p,TopLoc_Location());
    }
    tool->UpdateAssemblies();Stp::rebuildIndex(d);return d;
}
inline TopoDS_Shape envelopeClosedShell(){
    return BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(20,20,20).Shape(),
        BRepPrimAPI_MakeBox(gp_Pnt(2,2,2),16,16,16).Shape()).Shape();
}
inline Stp::CadDocument envelopeAssemblyFixture(){return envelopeFixture({envelopeClosedShell(),
    BRepPrimAPI_MakeBox(gp_Pnt(5,5,5),10,10,1).Shape(),BRepPrimAPI_MakeBox(gp_Pnt(6,6,8),2,2,2).Shape()});}
inline std::vector<int> envelopeRoots(const Stp::CadDocument& d){std::vector<int> r;
    for(size_t i=0;i<d.occurrences.size();++i)if(d.occurrences[i].parent<0)r.push_back(int(i));return r;}
