#include "cad_document.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPConstruct_Tool.hxx>
#include <Standard_Failure.hxx>
#include <StepBasic_ConversionBasedUnit.hxx>
#include <StepBasic_DerivedUnit.hxx>
#include <StepBasic_DerivedUnitElement.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepBasic_MeasureValueMember.hxx>
#include <StepBasic_Product.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepBasic_ProductDefinitionFormation.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepData_StepModel.hxx>
#include <StepRepr_MeasureRepresentationItem.hxx>
#include <StepRepr_NextAssemblyUsageOccurrence.hxx>
#include <StepRepr_PropertyDefinition.hxx>
#include <StepRepr_PropertyDefinitionRepresentation.hxx>
#include <StepRepr_Representation.hxx>
#include <TransferBRep.hxx>
#include <Transfer_TransientProcess.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TColStd_SequenceOfHAsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDataStd_NamedData.hxx>
#include <TDataStd_TreeNode.hxx>
#include <TDF_ChildIterator.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <XCAFDoc.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_MaterialTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace Stp {
namespace {
void checkCancel(std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("操作已取消");
}
QString text(const TCollection_ExtendedString& value) {
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(value.ToExtString()), value.Length());
}
QString text(const Handle(TCollection_HAsciiString)& value) {
    return value.IsNull() ? QString() : QString::fromUtf8(value->ToCString());
}

// OCCT 7.9 tests STEP names with byte-oriented isgraph(). In a C locale
// a pure Unicode UTF-8 name can be treated as empty and replaced by an ID.
// Use the source entities and the existing transfer map; never change locale
// or infer names from geometry, order, or another occurrence.
void restoreUnicodeNames(CadDocument& doc,const STEPCAFControl_Reader& reader,std::atomic_bool& cancel) {
    const auto model=reader.Reader().StepModel();
    if(model.IsNull()) return;
    const auto shapes=XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    const auto session=reader.Reader().WS();
    const auto process=session->TransferReader()->TransientProcess();
    const STEPConstruct_Tool tool(session);
    auto decoded=[&](const Handle(TCollection_HAsciiString)& value) {
        if(value.IsNull()) return QString();
        return model->SourceCodePage()==Resource_FormatType_NoConversion
            ? QString::fromLatin1(value->ToCString()) : QString::fromUtf8(value->ToCString());
    };
    for(int i=1;i<=model->NbEntities();++i) {
        checkCancel(&cancel);
        const auto entity=model->Value(i);
        QString name;
        TDF_Label label;
        if(const auto occurrence=Handle(StepRepr_NextAssemblyUsageOccurrence)::DownCast(entity); !occurrence.IsNull()) {
            if(occurrence->HasDescription()) name=decoded(occurrence->Description()).trimmed();
            if(name.isEmpty()) name=decoded(occurrence->Name()).trimmed();
            if(name.isEmpty()) continue; // Retain the kernel's ID/default fallback.
            label=STEPCAFControl_Reader::FindInstance(occurrence,shapes,tool,reader.GetShapeLabelMap());
        } else if(const auto product=Handle(StepBasic_ProductDefinition)::DownCast(entity); !product.IsNull()) {
            const auto formation=product->Formation();
            if(formation.IsNull() || formation->OfProduct().IsNull()) continue;
            name=decoded(formation->OfProduct()->Name()).trimmed();
            if(name.isEmpty()) continue;
            const auto binder=process->Find(product);
            if(binder.IsNull() || !binder->HasResult()) continue;
            const auto shape=TransferBRep::ShapeResult(process,binder);
            if(shape.IsNull()) continue;
            if(!reader.GetShapeLabelMap().Find(shape,label))
                shapes->Search(shape,label,true,true,false);
        }
        if(!label.IsNull() && !name.isEmpty()) {
            const auto utf8=name.toUtf8();
            TDataStd_Name::Set(label,TCollection_ExtendedString(utf8.constData(),true));
        }
    }
}

class ImportProgress final : public Message_ProgressIndicator {
public:
    ImportProgress(const ProgressCallback& callback, std::atomic_bool& cancel)
        : callback_(callback), cancel_(cancel) {}
    Standard_Boolean UserBreak() override { return cancel_.load(); }
protected:
    void Show(const Message_ProgressScope&, Standard_Boolean) override {
        const int percent = 10 + static_cast<int>(GetPosition() * 65);
        if (percent != last_ && callback_) {
            last_ = percent;
            callback_(percent, QStringLiteral("解析精确几何、装配与属性"));
        }
    }
private:
    ProgressCallback callback_;
    std::atomic_bool& cancel_;
    int last_ = -1;
};

struct UnitFactor { double factor=1, length=0, mass=0; bool valid=false; };
UnitFactor densityUnit(const Handle(StepBasic_NamedUnit)& unit,int depth=0) {
    if(unit.IsNull() || depth>16) return {};
    if(auto si=Handle(StepBasic_SiUnit)::DownCast(unit); !si.IsNull()) {
        UnitFactor result;
        if(si->Name()==StepBasic_sunMetre) { result.length=1; result.valid=true; }
        else if(si->Name()==StepBasic_sunGram) { result.mass=1; result.factor=0.001; result.valid=true; }
        if(si->HasPrefix()) {
            const double powers[] {18,15,12,9,6,3,2,1,-1,-2,-3,-6,-9,-12,-15,-18};
            const int prefix=int(si->Prefix());
            if(prefix<0 || prefix>=16) return {};
            result.factor*=std::pow(10.,powers[prefix]);
        }
        return result;
    }
    if(auto converted=Handle(StepBasic_ConversionBasedUnit)::DownCast(unit); !converted.IsNull()) {
        const auto measure=converted->ConversionFactor();
        if(measure.IsNull()) return {};
        auto result=densityUnit(measure->UnitComponent().NamedUnit(),depth+1);
        result.factor*=measure->ValueComponent();
        result.valid=result.valid && std::isfinite(result.factor) && result.factor>0;
        return result;
    }
    return {};
}

// Resolve each source density through its product relation. A value belonging
// to another part cannot validate this part, even when the numbers happen to match.
void verifyDensityUnits(CadDocument& doc,const STEPCAFControl_Reader& reader) {
    const auto model=reader.Reader().StepModel();
    if(model.IsNull()) return;
    const auto process=reader.Reader().WS()->TransferReader()->TransientProcess();
    QHash<QString,std::vector<double>> verified;
    QSet<QString> unsupported;
    for(int i=1;i<=model->NbEntities();++i) {
        const auto relation=Handle(StepRepr_PropertyDefinitionRepresentation)::DownCast(model->Value(i));
        if(relation.IsNull()) continue;
        const auto property=relation->Definition().PropertyDefinition();
        const auto representation=relation->UsedRepresentation();
        if(property.IsNull() || representation.IsNull()) continue;
        const auto product=property->Definition().ProductDefinition();
        if(product.IsNull()) continue;
        const auto binder=process->Find(product);
        if(binder.IsNull()) continue;
        const auto shape=TransferBRep::ShapeResult(process,binder);
        TDF_Label label;
        if(shape.IsNull() || !reader.GetShapeLabelMap().Find(shape,label)) continue;
        const QString key=labelEntry(label);
        for(int j=1;j<=representation->NbItems();++j) {
            const auto item=Handle(StepRepr_MeasureRepresentationItem)::DownCast(representation->ItemsValue(j));
            if(item.IsNull() || item->Measure().IsNull()) continue;
            const auto measure=item->Measure();
            const auto derived=measure->UnitComponent().DerivedUnit();
            UnitFactor total; total.valid=!derived.IsNull();
            if(!derived.IsNull()) for(int k=1;k<=derived->NbElements();++k) {
                const auto element=derived->ElementsValue(k);
                if(element.IsNull()) { total.valid=false; break; }
                const auto base=densityUnit(element->Unit());
                total.valid=total.valid && base.valid;
                total.factor*=std::pow(base.factor,element->Exponent());
                total.length+=base.length*element->Exponent(); total.mass+=base.mass*element->Exponent();
            }
            const bool densityDimensions=std::abs(total.length+3)<1.e-9 && std::abs(total.mass-1)<1.e-9;
            const QString description=text(item->Name())+QLatin1Char(' ')+text(representation->Name())
                                      +QLatin1Char(' ')+text(property->Description());
            if(!densityDimensions && !description.contains(QStringLiteral("density"),Qt::CaseInsensitive)) continue;
            const double value=measure->ValueComponent()*total.factor;
            if(total.valid && densityDimensions && std::isfinite(value) && value>0) verified[key].push_back(value);
            else unsupported.insert(key);
        }
    }
    for(auto& part:doc.parts) {
        const bool hadDensity=part.densityKgM3.has_value();
        part.densityKgM3.reset(); part.massKg.reset();
        const auto values=verified.value(part.id);
        const bool valid=!unsupported.contains(part.id) && !values.empty()
            && std::all_of(values.begin(),values.end(),[&](double value) {
                return std::abs(value-values.front())<=1.e-6*std::max(value,values.front());
            });
        if(valid) {
            part.densityKgM3=values.front();
            if(part.validSolid && part.volumeMm3) part.massKg=*part.volumeMm3**part.densityKgM3*1.e-9;
        } else if(hadDensity || unsupported.contains(part.id) || !values.empty()) {
            doc.diagnostics << QStringLiteral("%1：源文件密度单位、零件关联或多值一致性未通过核验；密度、质量留空。").arg(part.name);
        }
    }
}

void readMaterial(const TDF_Label& label, PartDefinition& part, QStringList& diagnostics) {
    Handle(TDataStd_TreeNode) link;
    if (!label.FindAttribute(XCAFDoc::MaterialRefGUID(), link) || !link->HasFather()) return;
    Handle(TCollection_HAsciiString) name, description, densityName, densityType;
    double density = 0;
    if (!XCAFDoc_MaterialTool::GetMaterial(link->Father()->Label(), name, description,
                                         density, densityName, densityType)) return;
    part.material = text(name);
    // STEPCAF normalizes material densities to gram/cm^3 (OCCT 7.9.3 ReadMaterials).
    // A density value without explicit mass-density semantics is never guessed.
    if (std::isfinite(density) && density > 0
        && text(densityType).compare(QStringLiteral("MASS_DENSITY_MEASURE"), Qt::CaseInsensitive) == 0)
        part.densityKgM3 = density * 1000.;
    else if (!std::isfinite(density) || density < 0)
        diagnostics << QStringLiteral("%1：密度不是有效正数，质量不计算。").arg(part.name);
}

void calculateProperties(PartDefinition& part, QStringList& diagnostics) {
    if (part.shape.IsNull()) return;
    try {
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(part.shape, bounds, false, false);
        if (!bounds.IsVoid() && !bounds.IsOpen()) {
            double x1, y1, z1, x2, y2, z2;
            bounds.Get(x1,y1,z1,x2,y2,z2);
            part.bboxMm = {{x2-x1, y2-y1, z2-z1}};
            part.bboxValid = std::all_of(part.bboxMm.begin(),part.bboxMm.end(),[](double value) { return std::isfinite(value) && value>=0; });
        }
        GProp_GProps area;
        BRepGProp::SurfaceProperties(part.shape, area);
        if (std::isfinite(area.Mass()) && area.Mass() > 0) part.areaMm2 = area.Mass();
        TopTools_IndexedMapOfShape solids, allFaces, solidFaces;
        TopExp::MapShapes(part.shape, TopAbs_SOLID, solids);
        TopExp::MapShapes(part.shape, TopAbs_FACE, allFaces);
        for (int i=1; i<=solids.Extent(); ++i)
            TopExp::MapShapes(solids(i), TopAbs_FACE, solidFaces);
        part.validSolid = !solids.IsEmpty() && allFaces.Extent() == solidFaces.Extent()
                          && BRepCheck_Analyzer(part.shape, true).IsValid();
        if (part.validSolid) {
            GProp_GProps volume;
            BRepGProp::VolumeProperties(part.shape, volume, true);
            if (std::isfinite(volume.Mass()) && volume.Mass() > 0) {
                part.volumeMm3 = volume.Mass();
                if (part.densityKgM3) part.massKg = *part.volumeMm3 * *part.densityKgM3 * 1.e-9;
            } else part.validSolid = false;
        }
        if (!part.validSolid)
            diagnostics << QStringLiteral("%1：几何不是已验证的封闭有效实体，体积和质量留空。").arg(part.name);
        if (solids.Extent() > 1)
            diagnostics << QStringLiteral("%1：一个产品定义包含 %2 个实体；未虚构独立零件或装配数量。")
                           .arg(part.name).arg(solids.Extent());
    } catch (const Standard_Failure& e) {
        part.validSolid = false;
        part.volumeMm3.reset(); part.massKg.reset();
        diagnostics << QStringLiteral("%1：几何属性计算失败：%2").arg(part.name, QString::fromUtf8(e.GetMessageString()));
    }
}
} // namespace

QString labelEntry(const TDF_Label& label) {
    if (label.IsNull()) return {};
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return QString::fromLatin1(entry.ToCString());
}

QString labelName(const TDF_Label& label) {
    Handle(TDataStd_Name) name;
    return !label.IsNull() && label.FindAttribute(TDataStd_Name::GetID(), name) ? text(name->Get()) : QString();
}

void rebuildIndex(CadDocument& doc, const ProgressCallback& progress, std::atomic_bool* cancel) {
    if (doc.xde.IsNull()) throw std::runtime_error("文档为空");
    doc.parts.clear(); doc.occurrences.clear();
    const auto shapes = XCAFDoc_DocumentTool::ShapeTool(doc.xde->Main());
    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    QHash<QString, int> definitions;
    QSet<QString> ancestors;
    bool hasAssembly = false;
    std::function<void(const TDF_Label&,int,int)> visit;
    visit = [&](const TDF_Label& label, int parent, int depth) {
        checkCancel(cancel);
        if (depth > 256) throw std::runtime_error("装配层级超过安全上限 256");
        TDF_Label definition = label;
        const bool reference = shapes->IsReference(label);
        if (reference && !shapes->GetReferredShape(label, definition)) {
            doc.diagnostics << QStringLiteral("引用未解析：%1").arg(labelEntry(label)); return;
        }
        const QString key = labelEntry(definition);
        if (ancestors.contains(key)) {
            doc.diagnostics << QStringLiteral("检测到循环装配引用：%1，已停止该分支。").arg(key); return;
        }
        Occurrence occurrence;
        occurrence.id = parent < 0 ? labelEntry(label) : doc.occurrences[parent].id + QLatin1Char('/') + labelEntry(label);
        occurrence.parent = parent;
        occurrence.label = label;
        occurrence.definitionLabel = definition;
        occurrence.name = labelName(label);
        // XDE automatically names unnamed references with an internal label link.
        // Do not expose that implementation name in the assembly tree or BOM.
        static const QRegularExpression automaticReferenceName(QStringLiteral("^=>\\[[0-9]+(?::[0-9]+)*\\]$"));
        if (reference && automaticReferenceName.match(occurrence.name).hasMatch()) occurrence.name.clear();
        if (occurrence.name.isEmpty()) occurrence.name = labelName(definition);
        if (occurrence.name.isEmpty()) occurrence.name = QStringLiteral("未命名_%1").arg(key);
        occurrence.path = parent < 0 ? occurrence.name : doc.occurrences[parent].path + QLatin1Char('/') + occurrence.name;
        if (reference) occurrence.localTransform = shapes->GetLocation(label).Transformation();
        occurrence.worldTransform = parent < 0 ? occurrence.localTransform
                                              : doc.occurrences[parent].worldTransform * occurrence.localTransform;
        const int index = static_cast<int>(doc.occurrences.size());
        doc.occurrences.push_back(occurrence);
        if (parent >= 0) doc.occurrences[parent].children.push_back(index);
        TColStd_SequenceOfHAsciiString external;
        // GetExternRefs merely reads child names; it does not check the external
        // marker itself. SetExternRefs may put that marker on a child container.
        if (shapes->IsExternRef(definition)) shapes->GetExternRefs(definition, external);
        for (TDF_ChildIterator child(definition, false); child.More(); child.Next())
            if (shapes->IsExternRef(child.Value())) shapes->GetExternRefs(child.Value(), external);
        for (int i=1; i<=external.Length(); ++i)
            doc.diagnostics << QStringLiteral("外部引用：%1。请核对引用文件与装配完整性。").arg(text(external(i)));
        if (shapes->IsAssembly(definition)) {
            hasAssembly = true;
            ancestors.insert(key);
            TDF_LabelSequence children;
            shapes->GetComponents(definition, children, false);
            for (int i=1; i<=children.Length(); ++i) visit(children(i), index, depth+1);
            ancestors.remove(key);
        } else {
            if (!definitions.contains(key)) {
                PartDefinition part;
                part.id = key; part.label = definition; part.shape = shapes->GetShape(definition);
                part.name = labelName(definition);
                if (part.name.isEmpty()) part.name = occurrence.name;
                const auto data = shapes->GetNamedProperties(definition, false);
                if (!data.IsNull() && data->HasString("ProductID")) part.number = text(data->GetString("ProductID"));
                readMaterial(definition, part, doc.diagnostics);
                if (progress) progress(80, QStringLiteral("计算几何属性：%1").arg(part.name));
                calculateProperties(part, doc.diagnostics);
                definitions.insert(key, static_cast<int>(doc.parts.size()));
                doc.parts.push_back(std::move(part));
            }
            doc.occurrences[index].partIndex = definitions.value(key);
        }
    };
    for (int i=1; i<=roots.Length(); ++i) visit(roots(i), -1, 0);
    if (!hasAssembly)
        doc.diagnostics << QStringLiteral("文件未提供装配引用结构；当前清单为文件产品/几何定义，不代表企业正式 BOM。");
    doc.diagnostics.removeDuplicates();
}

std::shared_ptr<CadDocument> loadStep(const QString& path, const ProgressCallback& progress,
                                     std::atomic_bool& cancel) {
    checkCancel(&cancel);
    if (!QFileInfo(path).isFile()) throw std::runtime_error("STEP 文件不存在");
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QStringLiteral("step") && suffix != QStringLiteral("stp"))
        throw std::runtime_error("请选择 .stp 或 .step 文件");
    auto doc = std::make_shared<CadDocument>();
    doc->sourcePath = QFileInfo(path).absoluteFilePath();
    doc->xde = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(doc->xde->Main());
    try {
        STEPCAFControl_Reader reader;
        reader.SetColorMode(true); reader.SetNameMode(true); reader.SetLayerMode(true);
        reader.SetPropsMode(true); reader.SetGDTMode(true); reader.SetMatMode(true);
        reader.SetSHUOMode(true); reader.SetViewMode(true); reader.SetMetaMode(true);
        reader.SetProductMetaMode(true);
        if (progress) progress(2, QStringLiteral("读取 STEP 文件（读取阶段结束后响应取消）"));
        const QByteArray filename = doc->sourcePath.toUtf8(); // OCCT 7.9 uses OSD UTF-8 paths on both platforms.
        if (reader.ReadFile(filename.constData()) != IFSelect_RetDone)
            throw std::runtime_error("STEP 解析失败：文件可能损坏或格式不支持");
        checkCancel(&cancel);
        TColStd_SequenceOfAsciiString lengths, angles, solidAngles;
        reader.ChangeReader().FileUnits(lengths, angles, solidAngles);
        QStringList units;
        for (int i=1; i<=lengths.Length(); ++i) units << QString::fromUtf8(lengths(i).ToCString());
        units.removeDuplicates();
        doc->sourceUnits = units.isEmpty() ? QStringLiteral("未声明/无法识别") : units.join(QStringLiteral("、"));
        // Explicitly use mm for the XDE geometry, independent of source units or machine defaults.
        reader.ChangeReader().SetSystemLengthUnit(1.0);
        XCAFDoc_DocumentTool::SetLengthUnit(doc->xde, 0.001);
        Handle(ImportProgress) indicator = new ImportProgress(progress, cancel);
        if (!reader.Transfer(doc->xde, indicator->Start())) {
            checkCancel(&cancel);
            throw std::runtime_error("STEP 几何传输失败");
        }
        checkCancel(&cancel);
        restoreUnicodeNames(*doc,reader,cancel);
        doc->diagnostics << QStringLiteral("来源长度单位：%1；几何统一为 mm。名称、编号、材料来自文件；包围盒、面积、体积为几何计算。")
                            .arg(doc->sourceUnits);
        rebuildIndex(*doc, progress, &cancel);
        verifyDensityUnits(*doc,reader);
        std::ostringstream kernelMessages;
        reader.Reader().PrintCheckLoad(kernelMessages,false,IFSelect_CountByItem);
        reader.Reader().PrintCheckTransfer(kernelMessages,false,IFSelect_CountByItem);
        const QString checks=QString::fromStdString(kernelMessages.str()).trimmed();
        if(!checks.isEmpty()) doc->diagnostics << QStringLiteral("OCCT 导入检查：\n%1").arg(checks.left(48000));
        if (doc->parts.empty()) throw std::runtime_error("文件未包含可显示的产品几何");
        if (progress) progress(90, QStringLiteral("精确几何已就绪，准备三维显示"));
        return doc;
    } catch (const Standard_Failure& e) {
        throw std::runtime_error(QStringLiteral("CAD 内核错误：%1").arg(QString::fromUtf8(e.GetMessageString())).toUtf8().constData());
    }
}

std::vector<int> partQuantities(const CadDocument& doc) {
    std::vector<int> quantities(doc.parts.size(), 0);
    for (const auto& occurrence : doc.occurrences)
        if (occurrence.partIndex >= 0 && occurrence.partIndex < int(quantities.size())) ++quantities[occurrence.partIndex];
    return quantities;
}

std::vector<int> selectedPartIndices(const CadDocument& doc, const std::vector<int>& selection) {
    QSet<int> selected;
    std::function<void(int)> visit = [&](int i) {
        if (i < 0 || i >= int(doc.occurrences.size())) return;
        const auto& item = doc.occurrences[i];
        if (item.partIndex >= 0) selected.insert(item.partIndex);
        for (int child : item.children) visit(child);
    };
    if (selection.empty()) for (const auto& o : doc.occurrences) { if (o.partIndex >= 0) selected.insert(o.partIndex); }
    else for (int index : selection) visit(index);
    auto list = selected.values();
    std::sort(list.begin(), list.end());
    return std::vector<int>(list.begin(), list.end());
}

bool materialEditable(const PartDefinition& part) { return part.material.trimmed().isEmpty(); }
bool densityEditable(const PartDefinition& part) { return !part.densityKgM3.has_value(); }

QString effectiveMaterial(const PartDefinition& part) {
    return materialEditable(part) && part.manualMaterial ? *part.manualMaterial : part.material;
}

std::optional<double> effectiveDensity(const PartDefinition& part) {
    const auto value = part.densityKgM3 ? part.densityKgM3 : part.manualDensityKgM3;
    return value && std::isfinite(*value) && *value > 0 ? value : std::nullopt;
}

std::optional<double> effectiveMass(const PartDefinition& part) {
    const auto density = effectiveDensity(part);
    if (!part.validSolid || !part.volumeMm3 || !std::isfinite(*part.volumeMm3)
        || *part.volumeMm3 <= 0 || !density) return std::nullopt;
    // Geometry is normalized to mm; one cubic millimetre is 1e-9 m³.
    const double mass = *part.volumeMm3 * *density * 1.e-9;
    return std::isfinite(mass) && mass > 0 ? std::optional<double>(mass) : std::nullopt;
}

bool setManualMaterial(PartDefinition& part, const QString& value) {
    if (!materialEditable(part)) return false;
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) part.manualMaterial.reset();
    else part.manualMaterial = trimmed;
    return true;
}

bool setManualDensity(PartDefinition& part, const QString& value, QString* error) {
    if (error) error->clear();
    if (!densityEditable(part)) {
        if (error) *error = QStringLiteral("密度已由 STEP 文件提供，不能覆盖。");
        return false;
    }
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        part.manualDensityKgM3.reset();
        return true;
    }
    bool ok = false;
    const double density = trimmed.toDouble(&ok);
    if (!ok || !std::isfinite(density) || density <= 0) {
        if (error) *error = QStringLiteral("请输入大于 0 的有限密度数值，单位为 kg/m³；清空可移除人工输入。");
        return false;
    }
    part.manualDensityKgM3 = density;
    return true;
}

CadDocument scopedBomDocument(const CadDocument& document, const std::vector<int>& includedLeafOccurrences) {
    CadDocument result;
    result.xde = document.xde;
    result.sourcePath = document.sourcePath;
    result.sourceUnits = document.sourceUnits;
    result.diagnostics = document.diagnostics;
    result.occurrenceScopeApplied = true;
    result.sourceAssemblyContext = document.sourceAssemblyContext
        ? document.sourceAssemblyContext : std::make_shared<CadDocument>(document);
    std::vector<bool> included(document.occurrences.size(), false);
    int leafCount = 0;
    for (int index : includedLeafOccurrences) {
        if (index < 0 || index >= int(document.occurrences.size())) continue;
        const auto& leaf = document.occurrences[index];
        if (leaf.partIndex < 0 || leaf.partIndex >= int(document.parts.size()) || !leaf.children.empty()) continue;
        if (included[index]) continue;
        ++leafCount;
        // Already included ancestors terminate traversal and also guard malformed
        // cycles. Only actual supplied leaves establish the processing scope.
        while (index >= 0 && index < int(included.size()) && !included[index]) {
            included[index] = true;
            index = document.occurrences[index].parent;
        }
    }
    std::vector<int> partMap(document.parts.size(), -1);
    for (size_t i = 0; i < included.size(); ++i)
        if (included[i] && document.occurrences[i].partIndex >= 0)
            partMap.at(document.occurrences[i].partIndex) = 0;
    for (size_t i = 0; i < partMap.size(); ++i) if (partMap[i] == 0) {
        partMap[i] = int(result.parts.size());
        result.parts.push_back(document.parts[i]);
    }
    std::vector<int> occurrenceMap(document.occurrences.size(), -1);
    for (size_t i = 0; i < included.size(); ++i) if (included[i]) {
        occurrenceMap[i] = int(result.occurrences.size());
        result.occurrences.push_back(document.occurrences[i]);
    }
    for (auto& occurrence : result.occurrences) {
        if (occurrence.partIndex >= 0) occurrence.partIndex = partMap.at(occurrence.partIndex);
        occurrence.parent = occurrence.parent >= 0 && occurrence.parent < int(occurrenceMap.size())
            ? occurrenceMap[occurrence.parent] : -1;
        std::vector<int> children;
        for (int child : occurrence.children)
            if (child >= 0 && child < int(occurrenceMap.size()) && occurrenceMap[child] >= 0)
                children.push_back(occurrenceMap[child]);
        occurrence.children = std::move(children);
    }
    result.diagnostics << QStringLiteral("当前处理范围：%1 个勾选的叶级实例，%2 个零件定义；未勾选实例不参与数量和导出。")
        .arg(leafCount).arg(result.parts.size());
    return result;
}

} // namespace Stp
