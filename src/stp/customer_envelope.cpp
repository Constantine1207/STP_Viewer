#include "customer_envelope.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
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
#include <Quantity_Color.hxx>
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
template<class Operation> TopoDS_Shape booleanResult(const TopoDS_Shape& a,const TopoDS_Shape& b,std::atomic_bool& cancel){
    check(cancel);TopTools_ListOfShape args,tools;args.Append(a);tools.Append(b);
    Operation op;op.SetArguments(args);op.SetTools(tools);op.SetNonDestructive(true);op.SetRunParallel(false);
    Handle(Progress) p=new Progress(cancel);op.Build(p->Start());check(cancel);
    if(!op.IsDone()||op.HasErrors()||op.HasWarnings()||op.Shape().IsNull()||!BRepCheck_Analyzer(op.Shape()).IsValid())
        throw std::runtime_error("保护空间或重叠检查失败，请检查工具实体及接触边界");
    return op.Shape();
}
double commonVolume(const TopoDS_Shape& a,const TopoDS_Shape& b,std::atomic_bool& cancel){
    return volume(booleanResult<BRepAlgoAPI_Common>(a,b,cancel));
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
TopoDS_Shape envelopeProtectionPrimitive(bool cylinder,double x,double y,double z,const gp_Trsf& placement){
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||x<=0||y<=0||z<=0)
        throw std::runtime_error("保护空间尺寸必须为有限正数");
    TopoDS_Shape shape=cylinder?BRepPrimAPI_MakeCylinder(x,z).Shape():BRepPrimAPI_MakeBox(x,y,z).Shape();
    return shape.Moved(TopLoc_Location(placement));
}
std::map<int,QString> envelopePreservationCandidates(const CadDocument& doc,const QStringList& keywords){
    std::map<int,QString> result;
    for(size_t i=0;i<doc.occurrences.size();++i){const auto& o=doc.occurrences[i];if(o.partIndex<0)continue;
        const auto text=o.name+" "+doc.parts.at(o.partIndex).name;
        for(const auto& raw:keywords){const auto key=raw.trimmed();if(!key.isEmpty()&&text.contains(key,Qt::CaseInsensitive)){
            result[int(i)]=QStringLiteral("名称包含：%1").arg(key);break;}}
    }return result;
}
EnvelopeResult buildCustomerEnvelope(const CadDocument& doc,const PlacementEdits& edits,
    const std::vector<int>& selected,const EnvelopeOptions& options,std::atomic_bool& cancel,const ProgressCallback& progress){
    check(cancel);validatePlacementEdits(doc,edits);
    const auto scope=envelopeScope(doc,selected);if(scope.empty())throw std::runtime_error("请先选择至少一个实体零件");
    EnvelopeResult r;r.inputOccurrences=int(scope.size());std::vector<TopoDS_Shape> solids,allSolids;
    const auto preserve=envelopeScope(doc,options.preservedOccurrences);
    std::vector<EnvelopePiece> retained;
    validateColorEdits(doc,options.preservedColors);
    for(int i:scope){
        check(cancel);const auto& o=doc.occurrences[i];
        if(size_t(o.partIndex)>=doc.parts.size())throw std::runtime_error("零件索引无效");
        // Deep-copy geometry so Boolean processing and preview meshes cannot alter source caches.
        BRepBuilderAPI_Copy copy(doc.parts[o.partIndex].shape,true,false);
        auto placed=copy.Shape().Moved(TopLoc_Location(placedWorldTransform(doc,edits,i)));
        collect(placed,allSolids);r.originalPieces.push_back({placed,QColor("#b0b0b0"),i});
        if(std::binary_search(preserve.begin(),preserve.end(),i)){
            const auto color=options.preservedColors.find(i);
            retained.push_back({placed,color==options.preservedColors.end()?occurrenceBaseColor(doc,i):color->second,i});
            ++r.preservedOccurrences;
        }else{collect(placed,solids);++r.processedOccurrences;}
    }
    r.inputSolids=int(allSolids.size());r.original=compound(allSolids);r.originalBounds=bounds(r.original);
    r.inputVolume=volume(fuse(allSolids,cancel));
    TopoDS_Shape body;
    if(!solids.empty()){
    if(progress)progress(20,QStringLiteral("合并精确实体…"));
    const auto merged=fuse(solids,cancel);
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
    body=fuse(mergedSolids,cancel);
    const double bodyBefore=volume(body);
    if(bodyBefore<volume(merged)-std::max(1.e-6,volume(merged)*1.e-8))throw std::runtime_error("填实后体积异常");
    for(const auto& protection:options.protections){
        check(cancel);std::vector<TopoDS_Shape> toolSolids;collect(protection.shape,toolSolids);
        const auto tool=fuse(toolSolids,cancel);
        for(const auto& item:retained)if(commonVolume(item.shape,tool,cancel)>1.e-6)
            throw std::runtime_error("保护空间与保留零件材料相交，请调整保护空间；保留零件不会被切削");
        const double originalCut=commonVolume(merged,tool,cancel);
        if(originalCut>1.e-6&&!protection.allowOriginalCut)
            throw std::runtime_error("保护空间将切除原有材料；请调整空间，或明确勾选允许切除原材料后重新生成");
        const double before=volume(body);body=booleanResult<BRepAlgoAPI_Cut>(body,tool,cancel);
        std::vector<TopoDS_Shape> valid;collect(body,valid);
        const double removed=before-volume(body);r.removedVolume+=removed;
        if(removed<1.e-6)r.warnings<<QStringLiteral("一个保护空间未切除主体材料，请检查其位置。");
    }
    if(!options.protections.empty()){
        r.removedOriginalVolume=volume(merged)-commonVolume(merged,body,cancel);
        if(r.removedOriginalVolume>1.e-6)r.warnings<<QStringLiteral("保护空间切除了 %1 mm³ 原有主体材料，请检查外形和安装接口。").arg(r.removedOriginalVolume,0,'g',9);
    }
    r.pieces.push_back({body,options.color,-1});
    }else{
        for(const auto& protection:options.protections){std::vector<TopoDS_Shape> valid;collect(protection.shape,valid);
            for(const auto& item:retained)if(commonVolume(item.shape,protection.shape,cancel)>1.e-6)
                throw std::runtime_error("保护空间与保留零件材料相交；全部保留时不执行切削");}
        r.warnings<<QStringLiteral("全部零件均保留，未进行合并填实。");
    }
    r.pieces.insert(r.pieces.end(),retained.begin(),retained.end());
    std::vector<TopoDS_Shape> outputShapes;for(const auto& item:r.pieces)outputShapes.push_back(item.shape);
    r.shape=compound(outputShapes);std::vector<TopoDS_Shape> output;collect(r.shape,output);
    r.outputSolids=int(output.size());r.outputVolume=volume(r.shape);r.bounds=bounds(r.shape);
    // Kept pieces remain independent. Report duplicate material instead of hiding it in a final fuse.
    if(!retained.empty())r.overlapVolume=std::max(0.,r.outputVolume-volume(fuse(output,cancel)));
    if(r.overlapVolume>1.e-6)r.warnings<<QStringLiteral("主体与保留零件或保留零件之间有 %1 mm³ 重叠材料；仅保留零件不能保证插口畅通，请设置保护空间并剖切检查。").arg(r.overlapVolume,0,'g',9);
    double scale=1;for(int k=0;k<3;++k)scale=std::max(scale,r.originalBounds[k+3]-r.originalBounds[k]);
    const double tolerance=std::max(1.e-5,scale*1.e-8);
    for(int k=0;k<6;++k){
        if(options.protections.empty()&&std::abs(r.bounds[k]-r.originalBounds[k])>tolerance)
            throw std::runtime_error("处理改变了外形边界，已停止生成");
        if((k<3&&r.bounds[k]<r.originalBounds[k]-tolerance)||(k>=3&&r.bounds[k]>r.originalBounds[k]+tolerance))
            throw std::runtime_error("处理超出了原始外形边界");
    }
    if(!std::isfinite(r.outputVolume)||r.outputVolume<=0)throw std::runtime_error("处理后体积异常");
    if(!retained.empty())r.warnings<<QStringLiteral("保留零件按原几何和实例颜色独立导出；内部细节也会保留，请确认适合客户交付。体积为实体体积之和，可能包含重叠。");
    if(r.outputSolids>1)r.warnings<<QStringLiteral("结果仍包含 %1 个独立实体；没有跨越装配缝隙自动增加材料。").arg(r.outputSolids);
    if(!options.fillClosedCavities)r.warnings<<QStringLiteral("未启用空腔填实，结果可能保留内部空腔。");
    if(options.fillClosedCavities&&!r.filledCavities)r.warnings<<QStringLiteral("未发现独立的封闭空腔；不代表模型没有对外连通的内部空间。");
    r.warnings<<QStringLiteral("本版不能自动区分安装孔与对外连通的内腔；这些区域保留，交付前必须剖切检查。")
        <<QStringLiteral("填实后的体积不能用于计算原产品真实质量。客户版仅清理元数据，不自动认定全部内部结构已隐藏。");
    check(cancel);if(progress)progress(80,QStringLiteral("几何处理完成，准备预览…"));return r;
}
QJsonObject EnvelopeResult::report() const{
    QJsonArray box,originalBox,w;for(double x:bounds)box.append(x);for(double x:originalBounds)originalBox.append(x);for(const auto& s:warnings)w.append(s);
    return {{"inputOccurrences",inputOccurrences},{"inputSolids",inputSolids},{"outputSolids",outputSolids},
        {"preservedOccurrences",preservedOccurrences},{"processedOccurrences",processedOccurrences},
        {"overlapVolumeMm3",overlapVolume},{"removedVolumeMm3",removedVolume},{"removedOriginalVolumeMm3",removedOriginalVolume},
        {"filledClosedCavities",filledCavities},{"inputUnionVolumeMm3",inputVolume},{"outputVolumeMm3",outputVolume},
        {"addedVolumeMm3",outputVolume-inputVolume},{"boundsMm",box},{"originalBoundsMm",originalBox},{"reviewRequired",true},{"warnings",w}};
}
std::shared_ptr<CadDocument> envelopeDocument(const TopoDS_Shape& shape,const QColor& color){
    if(shape.IsNull()||!color.isValid())throw std::runtime_error("外包络或颜色无效");
    auto d=std::make_shared<CadDocument>();d->xde=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(d->xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d->xde,.001);
    // No label/attribute cloning: source names, materials, layers, and PMI never enter this document.
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(d->xde->Main());
    const auto label=shapes->NewShape();shapes->SetShape(label,shape);
    TDataStd_Name::Set(label,TCollection_ExtendedString("CustomerEnvelope"));
    XCAFDoc_DocumentTool::ColorTool(d->xde->Main())->SetColor(label,
        Quantity_Color(color.redF(),color.greenF(),color.blueF(),Quantity_TOC_sRGB),XCAFDoc_ColorGen);
    rebuildIndex(*d);return d;
}
std::shared_ptr<CadDocument> envelopeDocument(const EnvelopeResult& result,const QColor& color){
    if(result.pieces.empty())return envelopeDocument(result.shape,color);
    auto d=std::make_shared<CadDocument>();d->xde=new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(d->xde->Main());XCAFDoc_DocumentTool::SetLengthUnit(d->xde,.001);
    auto shapes=XCAFDoc_DocumentTool::ShapeTool(d->xde->Main());auto colors=XCAFDoc_DocumentTool::ColorTool(d->xde->Main());
    TDF_Label root;
    if(result.pieces.size()>1){TopoDS_Compound c;BRep_Builder().MakeCompound(c);root=shapes->AddShape(c,false);
        TDataStd_Name::Set(root,TCollection_ExtendedString("CustomerAssembly"));}
    int n=0;for(const auto& piece:result.pieces){
        BRepBuilderAPI_Copy copy(piece.shape,true,false);
        // AddShape may create a reference for located geometry and name only the
        // reference, leaving the definition with an automatic SOLID name.
        const auto label=shapes->NewShape();shapes->SetShape(label,copy.Shape());
        const auto name=piece.sourceOccurrence<0?QStringLiteral("CustomerEnvelope"):QStringLiteral("RetainedPart%1").arg(++n);
        TDataStd_Name::Set(label,TCollection_ExtendedString(name.toUtf8().constData()));
        const auto c=piece.color;const Quantity_Color rgb(c.redF(),c.greenF(),c.blueF(),Quantity_TOC_sRGB);
        colors->SetColor(label,rgb,XCAFDoc_ColorGen);
        if(!root.IsNull()){const auto component=shapes->AddComponent(root,label,TopLoc_Location());
            TDataStd_Name::Set(component,TCollection_ExtendedString(name.toUtf8().constData()));colors->SetColor(component,rgb,XCAFDoc_ColorGen);}
    }shapes->UpdateAssemblies();rebuildIndex(*d);return d;
}
void exportCustomerEnvelope(const EnvelopeResult& r,const EnvelopeOptions& options,const QString& path,
    const QString& sourcePath,std::atomic_bool& cancel,const ProgressCallback& progress){
    check(cancel);if(!sourcePath.isEmpty()&&normalizedPath(path)==normalizedPath(sourcePath))
        throw std::runtime_error("请另存为新文件，不能覆盖原装配");
    if(r.shape.IsNull()||!BRepCheck_Analyzer(r.shape).IsValid())throw std::runtime_error("没有有效的外包络结果");
    const auto doc=envelopeDocument(r,options.color);
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
    if(!r.pieces.empty()){
        if(read->parts.size()!=r.pieces.size())throw std::runtime_error("STEP 回读的独立形体数量不一致，未保存");
        int n=0;for(const auto& piece:r.pieces){check(cancel);
            const auto name=piece.sourceOccurrence<0?QStringLiteral("CustomerEnvelope"):QStringLiteral("RetainedPart%1").arg(++n);
            bool found=false;for(size_t i=0;i<read->occurrences.size();++i){const auto& o=read->occurrences[i];if(o.partIndex<0)continue;
                const auto& part=read->parts[o.partIndex];if(part.name!=name)continue;
                if(found)throw std::runtime_error("STEP 回读的形体标识不唯一，未保存");found=true;
                const auto placed=part.shape.Moved(TopLoc_Location(o.worldTransform));const auto before=bounds(piece.shape),after=bounds(placed);
                if(std::abs(volume(placed)-volume(piece.shape))>std::max(1.e-5,volume(piece.shape)*1.e-7))throw std::runtime_error("STEP 回读的独立形体体积不一致，未保存");
                for(int k=0;k<6;++k)if(std::abs(before[k]-after[k])>1.e-4)throw std::runtime_error("STEP 回读的独立形体位置不一致，未保存");
                const auto color=occurrenceBaseColor(*read,int(i));
                if(std::abs(color.red()-piece.color.red())>1||std::abs(color.green()-piece.color.green())>1||std::abs(color.blue()-piece.color.blue())>1)
                    throw std::runtime_error("STEP 回读的实例颜色不一致，未保存");
            }if(!found)throw std::runtime_error("STEP 回读缺少独立形体，未保存");
        }
    }
    for(const auto& tool:options.protections)if(commonVolume(roundtrip,tool.shape,cancel)>1.e-5)
        throw std::runtime_error("STEP 回读的保护空间仍被材料占据，未保存");
    check(cancel);QSaveFile out(path);if(!out.open(QIODevice::WriteOnly))throw std::runtime_error(out.errorString().toStdString());
    for(size_t i=0;i<bytes.size();i+=1048576){check(cancel);const auto n=std::min(size_t(1048576),bytes.size()-i);
        if(out.write(bytes.data()+i,qint64(n))!=qint64(n))throw std::runtime_error("STEP 保存失败");}
    check(cancel);if(!out.commit())throw std::runtime_error(out.errorString().toStdString());
    if(progress)progress(100,QStringLiteral("客户版 STEP 已保存并通过回读检查"));
}
} // namespace Stp
