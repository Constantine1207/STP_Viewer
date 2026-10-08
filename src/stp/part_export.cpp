#include "part_export.h"
#include "bom_export.h"
#include "pmi.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <DESTEP_Parameters.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <StepBasic_DerivedUnit.hxx>
#include <StepBasic_DerivedUnitElement.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepData_StepModel.hxx>
#include <StepRepr_MeasureRepresentationItem.hxx>
#include <StepRepr_Representation.hxx>
#include <TDataStd_TreeNode.hxx>
#include <TDF_LabelSequence.hxx>
#include <TCollection_HAsciiString.hxx>
#include <XCAFDoc.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_Editor.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <ostream>
#include <streambuf>
#include <stdexcept>
#ifdef Q_OS_MACOS
#include <sys/stat.h>
#include <sys/xattr.h>
#include <cerrno>
#include <cstring>
#endif

namespace Stp {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
void checkCancel(std::atomic_bool& cancel) { if(cancel.load()) fail(QStringLiteral("操作已取消")); }

#ifdef Q_OS_MACOS
// FileProvider can mark a dot-prefixed staging tree invisible. Renaming it
// does not clear that metadata; QFileInfo still sees files that Finder hides.
// Change only the invisibility bits on newly committed export artifacts.
void makeExportVisible(const QString& path) {
    const auto native=QFile::encodeName(path);
    const auto visibilityError=[&] {
        return QStringLiteral("文件已写入，但无法恢复 Finder 可见性：%1\n%2")
            .arg(path,QString::fromLocal8Bit(std::strerror(errno)));
    };
    struct stat metadata{};
    if(::stat(native.constData(),&metadata)!=0) fail(visibilityError());
    if((metadata.st_flags&UF_HIDDEN)!=0
        &&::chflags(native.constData(),metadata.st_flags&~UF_HIDDEN)!=0) fail(visibilityError());
    unsigned char finderInfo[32];
    const auto size=::getxattr(native.constData(),"com.apple.FinderInfo",finderInfo,sizeof(finderInfo),0,0);
    if(size<0) {
        if(errno==ENOATTR||errno==ENOTSUP)return;
        fail(visibilityError());
    }
    // FileInfo and FolderInfo both store their big-endian flags at byte 8.
    // kIsInvisible is 0x4000. Preserve labels, icons and all other metadata.
    if(size>=10&&(finderInfo[8]&0x40)!=0) {
        finderInfo[8]&=~0x40;
        if(::setxattr(native.constData(),"com.apple.FinderInfo",finderInfo,size,0,0)!=0)
            fail(visibilityError());
    }
}
#endif

class ExportProgress final : public Message_ProgressIndicator {
public:
    ExportProgress(std::atomic_bool& cancel) : cancel_(cancel) {}
    Standard_Boolean UserBreak() override { return cancel_.load(); }
protected:
    void Show(const Message_ProgressScope&, Standard_Boolean) override {}
private:
    std::atomic_bool& cancel_;
};

// QSaveFile handles Unicode paths and commits only complete files; OCCT writes a stream.
class FileBuffer final : public std::streambuf {
public:
    FileBuffer(QSaveFile& file, std::atomic_bool& cancel) : file_(file), cancel_(cancel) {}
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if(cancel_.load()) return 0;
        return std::streamsize(file_.write(data,qint64(size)));
    }
    int_type overflow(int_type character) override {
        if(traits_type::eq_int_type(character,traits_type::eof())) return traits_type::not_eof(character);
        const char byte=traits_type::to_char_type(character);
        return xsputn(&byte,1)==1?character:traits_type::eof();
    }
private:
    QSaveFile& file_;
    std::atomic_bool& cancel_;
};

QString pmiKindCounts(const PmiCatalog& catalog) {
    QStringList counts;
    for(const auto kind:{PmiKind::Dimension,PmiKind::Datum,PmiKind::GeometricTolerance,PmiKind::Graphical,PmiKind::Legacy}) {
        int count=0;
        for(const auto& record:catalog.records) if(record.kind==kind) ++count;
        counts << QStringLiteral("%1 %2").arg(pmiKindText(kind)).arg(count);
    }
    return counts.join(QStringLiteral("，"));
}

// Label entries change during STEP serialization. Compare datum semantics in
// place of document-local label IDs, retaining multiplicity and link order.
QString pmiSemanticSignature(const PmiRecord& record,const PmiCatalog& catalog) {
    QString semantics=record.semanticText;
    static const QRegularExpression datumLink(QStringLiteral("；基准关联 ([0-9]+(?::[0-9]+)+)"));
    auto matches=datumLink.globalMatch(semantics);
    struct Replacement { qsizetype position,length; QString text; };
    std::vector<Replacement> replacements;
    while(matches.hasNext()) {
        const auto match=matches.next();
        QString datum=QStringLiteral("未解析基准");
        for(const auto& candidate:catalog.records)
            if(candidate.sourceLabelEntry==match.captured(1) && candidate.kind==PmiKind::Datum) {
                datum=candidate.semanticText; break;
            }
        replacements.push_back({match.capturedStart(1),match.capturedLength(1),datum});
    }
    for(auto it=replacements.rbegin();it!=replacements.rend();++it)
        semantics.replace(it->position,it->length,it->text);
    return QString::number(int(record.kind))+QLatin1Char(':')+semantics;
}

void auditPmiRoundTrip(const QString& partName,const PmiCatalog& before,const PmiCatalog& after,
                      QStringList& diagnostics) {
    diagnostics << QStringLiteral("%1：PMI 写入回读核验：临时 XDE %2 项（%3）；STEP 回读 %4 项（%5）。")
        .arg(partName).arg(before.records.size()).arg(pmiKindCounts(before))
        .arg(after.records.size()).arg(pmiKindCounts(after));
    QHash<QString,std::vector<int>> available;
    for(size_t i=0;i<after.records.size();++i)
        available[pmiSemanticSignature(after.records[i],after)].push_back(int(i));
    int matched=0;
    for(const auto& record:before.records) {
        auto& candidates=available[pmiSemanticSignature(record,before)];
        if(candidates.empty()) {
            diagnostics << QStringLiteral("%1 / 标注 %2：STEP 回读未找到同类型且语义摘要相同的对应项，可能丢失或改变；写入前为【%3】。")
                .arg(partName,record.name,record.semanticText);
            continue;
        }
        const auto& readBack=after.records[candidates.back()]; candidates.pop_back(); ++matched;
        if(record.associationComplete && !readBack.associationComplete)
            diagnostics << QStringLiteral("%1 / 标注 %2：STEP 回读的几何关联不完整。").arg(partName,record.name);
        if(!record.graphicalPresentation.IsNull() && readBack.graphicalPresentation.IsNull())
            diagnostics << QStringLiteral("%1 / 标注 %2：STEP 回读未保留源图形表现。").arg(partName,record.name);
    }
    for(auto it=available.cbegin();it!=available.cend();++it) for(const auto index:it.value()) {
        const auto& record=after.records[index];
        diagnostics << QStringLiteral("%1 / 回读标注 %2：新增或改变的语义摘要【%3】，无法与写入前标注可靠一一对应。")
            .arg(partName,record.name,record.semanticText);
    }
    diagnostics << QStringLiteral("%1：仅 %2 项在类型及语义摘要层面匹配。源实体编号未保留；即使数量与摘要相同，也不证明全部修饰符、几何关联和图形表现等价。")
        .arg(partName).arg(matched);
}
}

QString safePartFilename(const QString& proposed, int fallbackIndex) {
    QString name=proposed.normalized(QString::NormalizationForm_C).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")),QStringLiteral("_"));
    name=name.left(90); // Leaves room for collision suffixes and the file extension.
    while(name.toUtf8().size()>180) name.chop(1);
    if(!name.isEmpty() && name.back().isHighSurrogate()) name.chop(1);
    while(name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    if(name.isEmpty() || name==QStringLiteral(".") || name==QStringLiteral(".."))
        name=QStringLiteral("Part_%1").arg(fallbackIndex,4,10,QLatin1Char('0'));
    const QString base=name.section(QLatin1Char('.'),0,0).toUpper();
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|CLOCK\\$|COM[1-9¹²³]|LPT[1-9¹²³])$"));
    if(reserved.match(base).hasMatch()) name.prepend(QLatin1Char('_'));
    return name;
}

void prepareStepMaterialExport(STEPCAFControl_Writer& writer) {
    const auto model=writer.ChangeWriter().Model();
    for(int i=1;i<=model->NbEntities();++i) {
        const auto representation=Handle(StepRepr_Representation)::DownCast(model->Value(i));
        if(representation.IsNull() || representation->Name().IsNull()
           || representation->Name()->String()!="density") continue;
        for(int j=1;j<=representation->NbItems();++j) {
            const auto item=Handle(StepRepr_MeasureRepresentationItem)::DownCast(representation->ItemsValue(j));
            if(item.IsNull() || item->Measure().IsNull()) continue;
            const auto measure=item->Measure();
            const auto unit=measure->UnitComponent().DerivedUnit();
            if(unit.IsNull() || unit->NbElements()!=2) continue;
            Handle(StepBasic_DerivedUnitElement) mass,length;
            for(int k=1;k<=2;++k) {
                const auto element=unit->ElementsValue(k);
                if(element.IsNull()) continue;
                const auto si=Handle(StepBasic_SiUnit)::DownCast(element->Unit());
                if(si.IsNull()) continue;
                if(si->Name()==StepBasic_sunGram && !si->HasPrefix()) mass=element;
                if(si->Name()==StepBasic_sunMetre && si->HasPrefix() && si->Prefix()==StepBasic_spCenti) length=element;
            }
            // Match the exact known writer defect, never repair arbitrary input.
            if(mass.IsNull() || length.IsNull() || mass->Exponent()!=3 || length->Exponent()!=2) continue;
            mass->SetExponent(1); length->SetExponent(-3);
            measure->ValueComponentMember()->SetName("NUMERIC_MEASURE");
        }
    }
}

ExportResult exportParts(const CadDocument& doc, const std::vector<int>& selectedOccurrences,
                         const QString& parentDirectory,const ProgressCallback& progress,
                         std::atomic_bool& cancel) {
    checkCancel(cancel);
    const auto indices=selectedPartIndices(doc,selectedOccurrences);
    if(indices.empty()) fail(QStringLiteral("没有可导出的叶级零件"));
    const QDir parent(parentDirectory);
    if(!parent.exists()) fail(QStringLiteral("输出目录不存在"));
    // A visible staging name also avoids inherited FileProvider visibility flags
    // on macOS; publication still renames atomically into a fresh result folder.
    QTemporaryDir staging(parent.filePath(QStringLiteral("STP_exporting-XXXXXX")));
    if(!staging.isValid()) fail(QStringLiteral("无法在所选位置创建导出目录"));
    ExportResult result;
    QMap<int,QString> filenames;
    QSet<QString> used;
    for(size_t item=0;item<indices.size();++item) {
        checkCancel(cancel);
        const int partIndex=indices[item]; const auto& part=doc.parts.at(partIndex);
        if(part.shape.IsNull()) fail(QStringLiteral("%1：没有可导出的精确几何").arg(part.name));
        const QString base=safePartFilename(part.number.isEmpty()?part.name:part.number,int(item+1));
        QString filename=base+QStringLiteral(".step");
        for(int suffix=2;used.contains(filename.toCaseFolded());++suffix)
            filename=base+QStringLiteral("_%1.step").arg(suffix);
        used.insert(filename.toCaseFolded());
        if(progress) progress(int(item*95/indices.size()),QStringLiteral("导出 %1/%2：%3").arg(item+1).arg(indices.size()).arg(part.name));
        try {
            Handle(TDocStd_Document) target=new TDocStd_Document("BinXCAF");
            XCAFDoc_DocumentTool::Set(target->Main());
            XCAFDoc_DocumentTool::SetLengthUnit(target,0.001);
            if(!XCAFDoc_Editor::Extract(part.label,target->Main()))
                fail(QStringLiteral("无法复制零件定义：%1").arg(part.name));
            TDF_LabelSequence copiedParts;
            XCAFDoc_DocumentTool::ShapeTool(target->Main())->GetFreeShapes(copiedParts);
            for(int i=1;i<=copiedParts.Length();++i) {
                Handle(TDataStd_TreeNode) link;
                if(!copiedParts(i).FindAttribute(XCAFDoc::MaterialRefGUID(),link) || !link->HasFather()) continue;
                Handle(TCollection_HAsciiString) name,description,densityName,densityType;
                double density=0;
                if(!XCAFDoc_MaterialTool::GetMaterial(link->Father()->Label(),name,description,density,densityName,densityType)) continue;
                if(!part.densityKgM3 && density>0)
                    result.diagnostics << QStringLiteral("%1：材料名保留；未经单位核验的密度不写入拆分文件。").arg(part.name);
                XCAFDoc_DocumentTool::MaterialTool(target->Main())->SetMaterial(copiedParts(i),name,description,
                    part.densityKgM3 ? *part.densityKgM3/1000. : 0.,new TCollection_HAsciiString("density"),
                    new TCollection_HAsciiString("NUMERIC_MEASURE"));
            }
            // Copies only fully resolved same-part PMI. Cross-part/missing associations are reported.
            copyIndependentPmi(doc,partIndex,target,result.diagnostics);
            CadDocument stagedDocument; stagedDocument.xde=target;
            rebuildIndex(stagedDocument,{},&cancel);
            const auto stagedPmi=buildPmiCatalog(stagedDocument);
            STEPCAFControl_Writer writer;
            writer.SetColorMode(true); writer.SetNameMode(true); writer.SetLayerMode(true);
            writer.SetPropsMode(true); writer.SetMaterialMode(true); writer.SetDimTolMode(true);
            DESTEP_Parameters parameters;
            parameters.WriteSchema=DESTEP_Parameters::WriteMode_StepSchema_AP242DIS;
            parameters.WriteUnit=UnitsMethods_LengthUnit_Millimeter;
            Handle(ExportProgress) indicator=new ExportProgress(cancel);
            if(!writer.Transfer(target,parameters,STEPControl_AsIs,nullptr,indicator->Start())) {
                checkCancel(cancel); fail(QStringLiteral("STEP 转换失败：%1").arg(part.name));
            }
            prepareStepMaterialExport(writer);
            checkCancel(cancel);
            QSaveFile output(QDir(staging.path()).filePath(filename));
            if(!output.open(QIODevice::WriteOnly)) fail(output.errorString());
            FileBuffer buffer(output,cancel);
            std::ostream stream(&buffer);
            const auto status=writer.WriteStream(stream);
            checkCancel(cancel);
            if(status!=IFSelect_RetDone || !stream.good()) fail(QStringLiteral("STEP 写入失败：%1").arg(filename));
            if(!output.commit()) fail(output.errorString());
            if(!stagedPmi.records.empty()) {
                if(progress) progress(int((item+0.8)*95/indices.size()),QStringLiteral("核验 %1 的 PMI 写入结果").arg(part.name));
                const auto readBack=loadStep(QDir(staging.path()).filePath(filename),{},cancel);
                auditPmiRoundTrip(part.name,stagedPmi,buildPmiCatalog(*readBack),result.diagnostics);
            }
            filenames.insert(partIndex,filename);
        } catch(const Standard_Failure& e) {
            fail(QStringLiteral("导出 %1 失败：%2").arg(part.name,QString::fromUtf8(e.GetMessageString())));
        }
    }
    checkCancel(cancel);
    result.diagnostics << QStringLiteral("文件按零件定义去重；全部保持定义自身坐标、原始颜色和文件材料。实例颜色覆盖属于装配实例，单一定义文件不能表示多实例不同外观。")
                       << QStringLiteral("导出已启用 AP242 PMI；含 PMI 的临时文档均已写入后重新导入核验，缺失或改变的摘要列于上方。源 STEP 标注完整性与全部图形等价性仍未经证明。");
    result.diagnostics.removeDuplicates();
    exportPartManifest(doc,QDir(staging.path()).filePath(QStringLiteral("manifest.xlsx")),filenames,result.diagnostics);
    checkCancel(cancel);
    QString base=QStringLiteral("STP_parts_%1_%2").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
                                                     QUuid::createUuid().toString(QUuid::Id128).left(8));
    result.directory=parent.filePath(base);
    if(QFileInfo::exists(result.directory) || !QDir().rename(staging.path(),result.directory))
        fail(QStringLiteral("无法完成导出目录：%1").arg(result.directory));
    staging.setAutoRemove(false);
    result.manifestPath=QDir(result.directory).filePath(QStringLiteral("manifest.xlsx"));
    for(auto it=filenames.cbegin();it!=filenames.cend();++it) result.files<<QDir(result.directory).filePath(it.value());
#ifdef Q_OS_MACOS
    for(const auto& file:result.files) makeExportVisible(file);
    makeExportVisible(result.manifestPath);
    makeExportVisible(result.directory);
#endif
    if(progress) progress(100,QStringLiteral("零件与导出清单已保存"));
    return result;
}
} // namespace Stp
