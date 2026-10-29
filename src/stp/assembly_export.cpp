#include "assembly_export.h"
#include "part_export.h"
#include <BRep_Builder.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_LabelDataMap.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_Editor.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopoDS_Compound.hxx>
#include <QFileInfo>
#include <QSaveFile>
#include <ostream>
#include <streambuf>
#include <stdexcept>

namespace Stp {
namespace {
void check(std::atomic_bool& cancel) {if(cancel.load())throw std::runtime_error("导出已取消");}
void nameLabel(const TDF_Label& label,const QString& text) {
    TDataStd_Name::Set(label,TCollection_ExtendedString(reinterpret_cast<const Standard_ExtCharacter*>(text.utf16())));
}
class Progress final:public Message_ProgressIndicator {
public: explicit Progress(std::atomic_bool& c):cancel(c){}
    Standard_Boolean UserBreak() override{return cancel.load();}
protected: void Show(const Message_ProgressScope&,Standard_Boolean) override{}
private: std::atomic_bool& cancel;
};
class Buffer final:public std::streambuf {
public: Buffer(QSaveFile& f,std::atomic_bool& c):file(f),cancel(c){}
protected:
    std::streamsize xsputn(const char* p,std::streamsize n) override{return cancel.load()?0:file.write(p,n);}
    int_type overflow(int_type c) override {
        if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
        const char b=traits_type::to_char_type(c);return xsputn(&b,1)==1?c:traits_type::eof();
    }
private: QSaveFile& file;std::atomic_bool& cancel;
};
}
Handle(TDocStd_Document) buildPlacedAssembly(const CadDocument& doc,const PlacementEdits& edits,
                                            std::atomic_bool& cancel) {
    validatePlacementEdits(doc,edits);check(cancel);
    if(doc.xde.IsNull()||doc.occurrences.empty())throw std::runtime_error("没有可导出的装配");
    Handle(TDocStd_Document) target=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(target->Main());XCAFDoc_DocumentTool::SetLengthUnit(target,0.001);
    const auto src=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    const auto dst=XCAFDoc_DocumentTool::ShapeTool(target->Main());
    TDF_LabelDataMap labels;
    std::vector<TDF_Label> parts;
    for(const auto& part:doc.parts) {
        check(cancel);
        if(part.shape.IsNull())throw std::runtime_error("零件缺少精确几何，无法完整导出");
        parts.push_back(XCAFDoc_Editor::CloneShapeLabel(part.label,src,dst,labels));
    }
    NCollection_DataMap<Handle(XCAFDoc_VisMaterial),Handle(XCAFDoc_VisMaterial)> materials;
    // Includes per-face styles and original part properties, never session supplements.
    for(TDF_LabelDataMap::Iterator it(labels);it.More();it.Next())
        XCAFDoc_Editor::CloneMetaData(it.Key(),it.Value(),&materials);
    for(size_t i=0;i<parts.size();++i) {
        const auto& p=doc.parts[i];
        if(!p.material.isEmpty()||p.densityKgM3)
            XCAFDoc_DocumentTool::MaterialTool(target->Main())->SetMaterial(parts[i],
                new TCollection_HAsciiString(p.material.toUtf8().constData()),new TCollection_HAsciiString(""),
                p.densityKgM3?*p.densityKgM3/1000.:0.,new TCollection_HAsciiString("density"),
                new TCollection_HAsciiString("NUMERIC_MEASURE"));
    }
    auto newAssembly=[&](){TopoDS_Compound shape;BRep_Builder().MakeCompound(shape);return dst->AddShape(shape,false);};
    const auto root=newAssembly();nameLabel(root,QFileInfo(doc.sourcePath).completeBaseName()+QStringLiteral("_edited"));
    std::function<void(int,const TDF_Label&)> append=[&](int index,const TDF_Label& parent){
        check(cancel);const auto& o=doc.occurrences.at(index);
        // Every assembly occurrence gets its own definition. Shared leaf definitions remain shared.
        // This prevents changing a nested part in one repeated subassembly from moving its siblings.
        const auto definition=o.partIndex>=0?parts.at(o.partIndex):newAssembly();
        if(o.partIndex<0)XCAFDoc_Editor::CloneMetaData(o.definitionLabel,definition,&materials);
        const gp_Trsf parentWorld=o.parent<0?gp_Trsf():doc.occurrences.at(o.parent).worldTransform;
        const gp_Trsf local=parentWorld.Inverted()*placedWorldTransform(doc,edits,index);
        const auto component=dst->AddComponent(parent,definition,TopLoc_Location(local));
        XCAFDoc_Editor::CloneMetaData(o.label,component,&materials);nameLabel(component,o.name);
        for(int child:o.children)append(child,definition);
    };
    for(size_t i=0;i<doc.occurrences.size();++i)if(doc.occurrences[i].parent<0)append(int(i),root);
    dst->UpdateAssemblies();check(cancel);return target;
}
void exportPlacedAssembly(const CadDocument& doc,const PlacementEdits& edits,const QString& path,
                          const ProgressCallback& progress,std::atomic_bool& cancel) {
    const QFileInfo outputInfo(path),sourceInfo(doc.sourcePath);
    if(!doc.sourcePath.isEmpty()&&(outputInfo.absoluteFilePath()==sourceInfo.absoluteFilePath()
       ||(outputInfo.exists()&&outputInfo.canonicalFilePath()==sourceInfo.canonicalFilePath())))
        throw std::runtime_error("请另存为新文件，不能覆盖当前导入的 STEP");
    if(progress)progress(10,QStringLiteral("建立调整后的装配…"));
    const auto target=buildPlacedAssembly(doc,edits,cancel);
    STEPCAFControl_Writer writer;writer.SetColorMode(true);writer.SetNameMode(true);
    writer.SetLayerMode(true);writer.SetPropsMode(true);writer.SetMaterialMode(true);
    // Assembly/PMI associations need a separate remapping implementation. Never claim full PMI fidelity.
    writer.SetDimTolMode(false);
    DESTEP_Parameters params;params.WriteSchema=DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
    params.WriteUnit=UnitsMethods_LengthUnit_Millimeter;
    Handle(Progress) indicator=new Progress(cancel);
    if(progress)progress(40,QStringLiteral("转换精确 STEP 几何…"));
    if(!writer.Transfer(target,params,STEPControl_AsIs,nullptr,indicator->Start())) {
        check(cancel);throw std::runtime_error("装配 STEP 转换失败");
    }
    prepareStepMaterialExport(writer);check(cancel);
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))throw std::runtime_error(file.errorString().toStdString());
    Buffer buffer(file,cancel);std::ostream stream(&buffer);
    if(progress)progress(80,QStringLiteral("写入 STEP…"));
    const auto status=writer.WriteStream(stream);check(cancel);
    if(status!=IFSelect_RetDone||!stream.good())throw std::runtime_error("STEP 写入失败");
    if(!file.commit())throw std::runtime_error(file.errorString().toStdString());
    if(progress)progress(100,QStringLiteral("已保存调整后的装配"));
}
} // namespace Stp
