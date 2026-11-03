#include "customer_envelope.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d.hxx>
#include <BRepGProp.hxx>
#include <BRepBndLib.hxx>
#include <BRepLib.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Compound.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <TDataStd_Name.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryFile>
#include <QSaveFile>
#include <QJsonArray>
#include <fstream>
#include <sstream>
#include <set>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace Stp { namespace {
void check(std::atomic_bool& c){if(c.load())throw std::runtime_error("外包络处理已取消");}
class Progress final:public Message_ProgressIndicator {
public: explicit Progress(std::atomic_bool& c):cancel(c){}
    Standard_Boolean UserBreak() override{return cancel.load();}
protected: void Show(const Message_ProgressScope&,Standard_Boolean) override{}
private: std::atomic_bool& cancel;
};
double volume(const TopoDS_Shape& s){GProp_GProps p;BRepGProp::VolumeProperties(s,p);return p.Mass();}
std::array<double,6> bounds(const TopoDS_Shape& s){
    Bnd_Box b;BRepBndLib::AddOptimal(s,b,false,false);
    if(b.IsVoid()||b.IsOpen())throw std::runtime_error("模型边界无效");
    std::array<double,6> a{};b.Get(a[0],a[1],a[2],a[3],a[4],a[5]);return a;
}
void collect(const TopoDS_Shape& s,std::vector<TopoDS_Shape>& solids){
    if(s.IsNull())throw std::runtime_error("输入包含空几何");
    if(s.ShapeType()==TopAbs_SOLID){
        if(!BRepCheck_Analyzer(s).IsValid()||!std::isfinite(volume(s))||volume(s)<=0)
            throw std::runtime_error("输入包含无效、反向或零体积实体，请先修复模型");
        solids.push_back(s);return;
    }
    if(s.ShapeType()!=TopAbs_COMPOUND&&s.ShapeType()!=TopAbs_COMPSOLID)
        throw std::runtime_error("输入包含开放曲面、线或非实体几何，请仅选择有效实体");
    const auto before=solids.size();
    for(TopoDS_Iterator it(s);it.More();it.Next())collect(it.Value(),solids);
    if(before==solids.size())throw std::runtime_error("输入包含空的几何组");
}
TopoDS_Shape compound(const std::vector<TopoDS_Shape>& shapes){
    TopoDS_Compound c;BRep_Builder b;b.MakeCompound(c);for(const auto& s:shapes)b.Add(c,s);return c;
}
TopoDS_Shape fuse(const std::vector<TopoDS_Shape>& shapes,std::atomic_bool& cancel){
    check(cancel);if(shapes.empty())throw std::runtime_error("没有可处理的实体");
    if(shapes.size()==1)return shapes.front();
    TopTools_ListOfShape args,tools;args.Append(shapes.front());
    for(size_t i=1;i<shapes.size();++i)tools.Append(shapes[i]);
    BRepAlgoAPI_Fuse op;op.SetArguments(args);op.SetTools(tools);
    op.SetNonDestructive(true);op.SetRunParallel(false);
    Handle(Progress) p=new Progress(cancel);op.Build(p->Start());check(cancel);
    if(!op.IsDone()||op.HasErrors())throw std::runtime_error("实体合并失败；请检查重叠面、微小缝隙或异常几何");
    if(op.HasWarnings())throw std::runtime_error("几何内核报告合并警告；为避免遗漏几何，本次停止处理");
    auto s=op.Shape();if(s.IsNull()||!BRepCheck_Analyzer(s).IsValid())throw std::runtime_error("合并结果无效");
    return s;
}
QString normalizedPath(const QString& p){QFileInfo i(p);auto s=i.exists()?i.canonicalFilePath():i.absoluteFilePath();
#ifdef Q_OS_WIN
    s=s.toCaseFolded();
#endif
    return QDir::cleanPath(s);
}
}
std::vector<int> envelopeScope(const CadDocument& doc,const std::vector<int>& selected){
    std::set<int> leaves,visited;
    std::function<void(int)> add=[&](int i){
        if(i<0||size_t(i)>=doc.occurrences.size())throw std::runtime_error("选择已失效");
        if(!visited.insert(i).second)return;
        const auto& o=doc.occurrences[i];if(o.partIndex>=0)leaves.insert(i);
        for(int child:o.children)add(child);
    };
    for(int i:selected)add(i);return {leaves.begin(),leaves.end()};
}
EnvelopeResult buildCustomerEnvelope(const CadDocument& doc,const PlacementEdits& edits,
    const std::vector<int>& selected,const EnvelopeOptions& options,std::atomic_bool& cancel,const ProgressCallback& progress){
    check(cancel);validatePlacementEdits(doc,edits);
    const auto scope=envelopeScope(doc,selected);if(scope.empty())throw std::runtime_error("请先选择至少一个实体零件");
    EnvelopeResult r;r.inputOccurrences=int(scope.size());std::vector<TopoDS_Shape> solids;
    for(int i:scope){
        check(cancel);const auto& o=doc.occurrences[i];
        if(size_t(o.partIndex)>=doc.parts.size())throw std::runtime_error("零件索引无效");
        // Deep-copy geometry so Boolean processing and preview meshes cannot alter source caches.
        BRepBuilderAPI_Copy copy(doc.parts[o.partIndex].shape,true,false);
        auto placed=copy.Shape().Moved(TopLoc_Location(placedWorldTransform(doc,edits,i)));
        collect(placed,solids);
    }
    r.inputSolids=int(solids.size());r.original=compound(solids);r.bounds=bounds(r.original);
    if(progress)progress(20,QStringLiteral("合并精确实体…"));
    const auto merged=fuse(solids,cancel);r.inputVolume=volume(merged);
    std::vector<TopoDS_Shape> mergedSolids;collect(merged,mergedSolids);
    if(options.fillClosedCavities){
        if(progress)progress(45,QStringLiteral("填充完全封闭的空腔…"));
        for(auto& s:mergedSolids){
            check(cancel);auto solid=TopoDS::Solid(s);const auto outer=BRepClass3d::OuterShell(solid);
            if(outer.IsNull())throw std::runtime_error("无法确定实体外边界");
            int cavities=0;for(TopoDS_Iterator it(solid);it.More();it.Next()){
                if(it.Value().ShapeType()!=TopAbs_SHELL)throw std::runtime_error("实体含有无法处理的内部拓扑");
                if(!it.Value().IsSame(outer))++cavities;
            }
            if(cavities){
                auto filled=BRepBuilderAPI_MakeSolid(outer).Solid();
                if(!BRepLib::OrientClosedSolid(filled)||!BRepCheck_Analyzer(filled).IsValid()||volume(filled)<volume(s)-1.e-6)
                    throw std::runtime_error("空腔填实后的实体无效");
                s=filled;r.filledCavities+=cavities;
            }
        }
    }
    if(progress)progress(65,QStringLiteral("去除填实体内的冗余结构…"));
    // A second union absorbs enclosed PCB/fasteners, and retains protruding material.
    r.shape=fuse(mergedSolids,cancel);std::vector<TopoDS_Shape> output;collect(r.shape,output);
    r.outputSolids=int(output.size());r.outputVolume=volume(r.shape);
    const auto after=bounds(r.shape);double scale=1;
    for(int k=0;k<3;++k)scale=std::max(scale,r.bounds[k+3]-r.bounds[k]);
    for(int k=0;k<6;++k)if(std::abs(after[k]-r.bounds[k])>std::max(1.e-5,scale*1.e-8))
        throw std::runtime_error("处理改变了外形边界，已停止生成");
    if(!std::isfinite(r.outputVolume)||r.outputVolume<r.inputVolume-std::max(1.e-6,r.inputVolume*1.e-8))
        throw std::runtime_error("处理后体积异常");
    if(r.outputSolids>1)r.warnings<<QStringLiteral("结果仍包含 %1 个独立实体；没有跨越装配缝隙自动增加材料。").arg(r.outputSolids);
    if(!options.fillClosedCavities)r.warnings<<QStringLiteral("未启用空腔填实，结果可能保留内部空腔。");
    if(options.fillClosedCavities&&!r.filledCavities)r.warnings<<QStringLiteral("未发现独立的封闭空腔；不代表模型没有对外连通的内部空间。");
    r.warnings<<QStringLiteral("本版不能自动区分安装孔与对外连通的内腔；这些区域保留，交付前必须剖切检查。")
        <<QStringLiteral("填实后的体积不能用于计算原产品真实质量。客户版仅清理元数据，不自动认定全部内部结构已隐藏。");
    check(cancel);if(progress)progress(80,QStringLiteral("几何处理完成，准备预览…"));return r;
}
QJsonObject EnvelopeResult::report() const{
    QJsonArray box,w;for(double x:bounds)box.append(x);for(const auto& s:warnings)w.append(s);
    return {{"inputOccurrences",inputOccurrences},{"inputSolids",inputSolids},{"outputSolids",outputSolids},
        {"filledClosedCavities",filledCavities},{"inputUnionVolumeMm3",inputVolume},{"outputVolumeMm3",outputVolume},
        {"addedVolumeMm3",outputVolume-inputVolume},{"boundsMm",box},{"reviewRequired",true},{"warnings",w}};
}
std::shared_ptr<CadDocument> envelopeDocument(const TopoDS_Shape& shape,const QColor& color){
    if(shape.IsNull()||!color.isValid())throw std::runtime_error("外包络或颜色无效");
    auto d=std::make_shared<CadDocument>();d->xde=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(d->xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d->xde,.001);
    // No label/attribute cloning: source names, materials, layers, and PMI never enter this document.
    const auto label=XCAFDoc_DocumentTool::ShapeTool(d->xde->Main())->AddShape(shape,false);
    TDataStd_Name::Set(label,TCollection_ExtendedString("CustomerEnvelope"));
    XCAFDoc_DocumentTool::ColorTool(d->xde->Main())->SetColor(label,
        Quantity_Color(color.redF(),color.greenF(),color.blueF(),Quantity_TOC_sRGB),XCAFDoc_ColorGen);
    rebuildIndex(*d);return d;
}
void exportCustomerEnvelope(const EnvelopeResult& r,const EnvelopeOptions& options,const QString& path,
    const QString& sourcePath,std::atomic_bool& cancel,const ProgressCallback& progress){
    check(cancel);if(!sourcePath.isEmpty()&&normalizedPath(path)==normalizedPath(sourcePath))
        throw std::runtime_error("请另存为新文件，不能覆盖原装配");
    if(r.shape.IsNull()||!BRepCheck_Analyzer(r.shape).IsValid())throw std::runtime_error("没有有效的外包络结果");
    const auto doc=envelopeDocument(r.shape,options.color);
    STEPCAFControl_Writer writer;writer.SetColorMode(true);writer.SetNameMode(true);
    writer.SetLayerMode(false);writer.SetPropsMode(false);writer.SetMaterialMode(false);writer.SetDimTolMode(false);
    DESTEP_Parameters params;params.WriteSchema=DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
    params.WriteUnit=UnitsMethods_LengthUnit_Millimeter;
    Handle(Progress) indicator=new Progress(cancel);
    if(progress)progress(10,QStringLiteral("转换客户版 STEP…"));
    if(!writer.Transfer(doc->xde,params,STEPControl_AsIs,nullptr,indicator->Start())){check(cancel);throw std::runtime_error("外包络 STEP 转换失败");}
    // Temporary STEP is validated before an existing destination can be replaced.
    QTemporaryFile temp(QFileInfo(path).absolutePath()+"/CustomerEnvelope-XXXXXX.step");
    if(!temp.open())throw std::runtime_error(temp.errorString().toStdString());
    std::ostringstream stream;if(writer.WriteStream(stream)!=IFSelect_RetDone)throw std::runtime_error("STEP 写入失败");
    check(cancel);const auto bytes=stream.str();if(temp.write(bytes.data(),qint64(bytes.size()))!=qint64(bytes.size())||!temp.flush())throw std::runtime_error("临时 STEP 写入失败");
    temp.close();if(progress)progress(60,QStringLiteral("重新读取并核对 STEP…"));
    auto read=loadStep(temp.fileName(),{},cancel);std::vector<TopoDS_Shape> shapes;
    for(const auto& o:read->occurrences)if(o.partIndex>=0)collect(read->parts[o.partIndex].shape.Moved(TopLoc_Location(o.worldTransform)),shapes);
    const auto roundtrip=compound(shapes);const auto box=bounds(roundtrip);
    if(int(shapes.size())!=r.outputSolids||std::abs(volume(roundtrip)-r.outputVolume)>std::max(1.e-5,r.outputVolume*1.e-7))
        throw std::runtime_error("STEP 回读的实体数量或体积不一致，未保存");
    for(int k=0;k<6;++k)if(std::abs(box[k]-r.bounds[k])>1.e-4)throw std::runtime_error("STEP 回读外形边界不一致，未保存");
    check(cancel);QSaveFile out(path);if(!out.open(QIODevice::WriteOnly))throw std::runtime_error(out.errorString().toStdString());
    for(size_t i=0;i<bytes.size();i+=1048576){check(cancel);const auto n=std::min(size_t(1048576),bytes.size()-i);
        if(out.write(bytes.data()+i,qint64(n))!=qint64(n))throw std::runtime_error("STEP 保存失败");}
    check(cancel);if(!out.commit())throw std::runtime_error(out.errorString().toStdString());
    if(progress)progress(100,QStringLiteral("客户版 STEP 已保存并通过回读检查"));
}
} // namespace Stp
