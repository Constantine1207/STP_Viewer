#include "bom_export.h"
#include <QSaveFile>
#include <xlsxwriter.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace Stp {
namespace {
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
void checkCancel(std::atomic_bool* cancel) {
    if (cancel && cancel->load()) fail(QStringLiteral("操作已取消"));
}
void checked(lxw_error result) {
    if (result != LXW_NO_ERROR) fail(QStringLiteral("Excel 写入失败：%1").arg(QString::fromUtf8(lxw_strerror(result))));
}
void string(lxw_worksheet* sheet, lxw_row_t row, lxw_col_t col, const QString& text, lxw_format* format=nullptr) {
    if (text.isEmpty()) { checked(worksheet_write_blank(sheet,row,col,format)); return; }
    // Explicit string writes prevent file-controlled names from being interpreted as formulas.
    checked(worksheet_write_string(sheet,row,col,text.left(32767).toUtf8().constData(),format));
}
void number(lxw_worksheet* sheet, lxw_row_t row, lxw_col_t col, double value, lxw_format* format=nullptr) {
    if (std::isfinite(value)) checked(worksheet_write_number(sheet,row,col,value,format));
    else checked(worksheet_write_blank(sheet,row,col,format));
}
void optional(lxw_worksheet* sheet, lxw_row_t row, lxw_col_t col, std::optional<double> value, lxw_format* format) {
    if (value) number(sheet,row,col,*value,format); else checked(worksheet_write_blank(sheet,row,col,format));
}
bool hasManualProperties(const CadDocument& doc) {
    return std::any_of(doc.parts.begin(),doc.parts.end(),[](const PartDefinition& part) {
        return (materialEditable(part) && part.manualMaterial.has_value())
            || (densityEditable(part) && part.manualDensityKgM3.has_value());
    });
}
struct Workbook {
    const char* buffer = nullptr;
    size_t bufferSize = 0;
    lxw_workbook* book = nullptr;
    lxw_format* heading = nullptr;
    lxw_format* decimal = nullptr;
    Workbook() {
        lxw_workbook_options options {};
        options.output_buffer = &buffer;
        options.output_buffer_size = &bufferSize;
        book = workbook_new_opt(nullptr, &options);
        if (!book) fail(QStringLiteral("无法创建 Excel 工作簿"));
        heading = workbook_add_format(book);
        decimal = workbook_add_format(book);
        if (!heading || !decimal) fail(QStringLiteral("无法创建 Excel 格式"));
        format_set_bold(heading);
        format_set_bg_color(heading, 0x154B67);
        format_set_font_color(heading, 0xFFFFFF);
        format_set_text_wrap(heading);
        format_set_num_format(decimal, "0.######");
    }
    ~Workbook() {
        if (book) workbook_close(book);
        std::free(const_cast<char*>(buffer));
    }
    lxw_worksheet* sheet(const char* name, const QStringList& headings) {
        auto* result = workbook_add_worksheet(book, name);
        if (!result) fail(QStringLiteral("无法创建 Excel 工作表"));
        checked(worksheet_set_column(result,0,lxw_col_t(headings.size()-1),20,nullptr));
        checked(worksheet_set_row(result,0,32,nullptr));
        worksheet_freeze_panes(result,1,0);
        for (int i=0; i<headings.size(); ++i) string(result,0,lxw_col_t(i),headings[i],heading);
        return result;
    }
    void finish(const QString& path, std::atomic_bool* cancel = nullptr) {
        checkCancel(cancel);
        lxw_workbook* closing = book;
        book = nullptr;
        checked(workbook_close(closing));
        checkCancel(cancel);
        if (!buffer || !bufferSize) fail(QStringLiteral("Excel 输出为空"));
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) fail(file.errorString());
        try {
            checkCancel(cancel);
            if (file.write(buffer,qint64(bufferSize)) != qint64(bufferSize)) fail(file.errorString());
            checkCancel(cancel);
            if (!file.commit()) fail(file.errorString());
        } catch (...) {
            file.cancelWriting();
            throw;
        }
    }
};

void writePositions(Workbook& workbook, const CadDocument& doc, const QMap<int,QString>* filenames,
                    std::atomic_bool* cancel = nullptr) {
    QStringList columns {QStringLiteral("实例 ID"), QStringLiteral("实例路径"), QStringLiteral("定义 ID"),
                         QStringLiteral("零件名称"),QStringLiteral("文件名"),QStringLiteral("X mm"),
                         QStringLiteral("Y mm"),QStringLiteral("Z mm")};
    for (int row=1;row<=3;++row) for(int col=1;col<=3;++col) columns << QStringLiteral("R%1%2").arg(row).arg(col);
    auto* sheet = workbook.sheet("实例位置",columns);
    lxw_row_t row=1;
    for (const auto& occurrence : doc.occurrences) {
        checkCancel(cancel);
        if (occurrence.partIndex < 0) continue;
        if (filenames && !filenames->contains(occurrence.partIndex)) continue;
        string(sheet,row,0,occurrence.id); string(sheet,row,1,occurrence.path);
        string(sheet,row,2,doc.parts.at(occurrence.partIndex).id); string(sheet,row,3,occurrence.name);
        if (filenames) string(sheet,row,4,filenames->value(occurrence.partIndex));
        for (int axis=1;axis<=3;++axis) number(sheet,row,lxw_col_t(4+axis),occurrence.worldTransform.Value(axis,4),workbook.decimal);
        int column=8;
        for(int r=1;r<=3;++r) for(int c=1;c<=3;++c)
            number(sheet,row,lxw_col_t(column++),occurrence.worldTransform.Value(r,c),workbook.decimal);
        ++row;
    }
    checked(worksheet_set_column(sheet,1,1,60,nullptr));
    if(row>1) checked(worksheet_autofilter(sheet,0,0,row-1,16));
}
void writeDiagnostics(Workbook& workbook, const CadDocument& doc, const QStringList& extra,
                      std::atomic_bool* cancel = nullptr) {
    auto* sheet=workbook.sheet("导入诊断",{QStringLiteral("项目"),QStringLiteral("说明")});
    string(sheet,1,0,QStringLiteral("源文件")); string(sheet,1,1,doc.sourcePath);
    string(sheet,2,0,QStringLiteral("来源单位")); string(sheet,2,1,doc.sourceUnits);
    string(sheet,3,0,QStringLiteral("计量口径"));
    string(sheet,3,1,QStringLiteral("长度 mm；面积 mm²；体积 mm³；密度 kg/m³；质量 kg。空单元格表示未提供或不可计算，不能按 0 解读。"));
    string(sheet,4,0,QStringLiteral("来源与计算"));
    string(sheet,4,1,hasManualProperties(doc)
        ? QStringLiteral("名称/编号来自文件；缺失材料和密度可由人工补充，来源见零件汇总；数量来自当前范围的实际装配实例；包围盒/面积/体积为几何计算；质量仅在有效体积及文件或人工质量密度齐备时计算。人工属性仅供内部计算和 BOM，不写入拆分 STEP。旋转矩阵和位置均为原始全局坐标，未施加爆炸图变换。")
        : QStringLiteral("名称/编号/材料来自文件；数量来自实际装配实例；包围盒/面积/体积为几何计算；质量仅在有效体积及文件质量密度齐备时计算。旋转矩阵和位置均为原始全局坐标，未施加爆炸图变换。"));
    string(sheet,5,0,QStringLiteral("BOM 适用性"));
    string(sheet,5,1,QStringLiteral("几何 BOM 不等于正式 EBOM；同名不同定义分别列出。包围盒尺寸不是图纸尺寸标注。"));
    int row=6;
    for(const auto& message : doc.diagnostics+extra) {
        checkCancel(cancel);
        string(sheet,row,0,QStringLiteral("诊断")); string(sheet,row++,1,message);
    }
    checked(worksheet_set_column(sheet,1,1,110,nullptr));
}
} // namespace

void exportBom(const CadDocument& doc, const QString& path, std::atomic_bool* cancel) {
    checkCancel(cancel);
    Workbook workbook;
    QStringList headings {QStringLiteral("序号"),QStringLiteral("定义 ID"),QStringLiteral("零件名称"),
        QStringLiteral("零件编号"),QStringLiteral("数量"),QStringLiteral("材料"),QStringLiteral("密度 kg/m³"),
        QStringLiteral("包围盒 X mm"),QStringLiteral("包围盒 Y mm"),QStringLiteral("包围盒 Z mm"),
        QStringLiteral("体积 mm³"),QStringLiteral("表面积 mm²"),QStringLiteral("单件质量 kg"),
        QStringLiteral("合计质量 kg"),QStringLiteral("有效封闭实体"),QStringLiteral("实例路径")};
    const bool manualProperties=hasManualProperties(doc);
    if(manualProperties) headings << QStringLiteral("材料来源") << QStringLiteral("密度来源") << QStringLiteral("质量来源");
    auto* summary=workbook.sheet("零件汇总",headings);
    const auto quantities=partQuantities(doc);
    for (size_t index=0;index<doc.parts.size();++index) {
        checkCancel(cancel);
        const auto& part=doc.parts[index]; const lxw_row_t row=lxw_row_t(index+1);
        number(summary,row,0,double(index+1)); string(summary,row,1,part.id); string(summary,row,2,part.name);
        string(summary,row,3,part.number); number(summary,row,4,quantities[index]); string(summary,row,5,effectiveMaterial(part));
        const auto density=effectiveDensity(part), mass=effectiveMass(part);
        optional(summary,row,6,density,workbook.decimal);
        if (part.bboxValid) for(int axis=0;axis<3;++axis) number(summary,row,lxw_col_t(7+axis),part.bboxMm[axis],workbook.decimal);
        optional(summary,row,10,part.volumeMm3,workbook.decimal); optional(summary,row,11,part.areaMm2,workbook.decimal);
        optional(summary,row,12,mass,workbook.decimal);
        if(mass) number(summary,row,13,*mass*quantities[index],workbook.decimal);
        string(summary,row,14,part.validSolid?QStringLiteral("是"):QStringLiteral("否/不可确认"));
        QStringList paths;
        for(const auto& occurrence:doc.occurrences) {
            checkCancel(cancel);
            if(occurrence.partIndex==int(index)) paths<<occurrence.path;
        }
        string(summary,row,15,paths.join(QLatin1Char('\n')));
        if(manualProperties) {
            const bool manualMaterial=materialEditable(part) && part.manualMaterial.has_value();
            const bool manualDensity=densityEditable(part) && density.has_value();
            string(summary,row,16,effectiveMaterial(part).trimmed().isEmpty()?QStringLiteral("未提供")
                : manualMaterial?QStringLiteral("人工输入"):QStringLiteral("文件"));
            string(summary,row,17,!density?QStringLiteral("未提供/不可计算")
                : manualDensity?QStringLiteral("人工输入"):QStringLiteral("文件"));
            string(summary,row,18,!mass?QStringLiteral("不可计算")
                : manualDensity?QStringLiteral("几何计算＋人工密度"):QStringLiteral("几何计算＋文件密度"));
        }
    }
    if (!doc.parts.empty()) checked(worksheet_autofilter(summary,0,0,lxw_row_t(doc.parts.size()),lxw_col_t(headings.size()-1)));
    auto* hierarchy=workbook.sheet("装配层级",{QStringLiteral("实例 ID"),QStringLiteral("父实例 ID"),QStringLiteral("层级"),
        QStringLiteral("名称"),QStringLiteral("类型"),QStringLiteral("定义 ID"),QStringLiteral("实例路径"),QStringLiteral("本行数量")});
    lxw_row_t row=1;
    for(const auto& occurrence:doc.occurrences) {
        checkCancel(cancel);
        int depth=0, parent=occurrence.parent;
        while(parent>=0) { ++depth; parent=doc.occurrences.at(parent).parent; }
        string(hierarchy,row,0,occurrence.id);
        if(occurrence.parent>=0) string(hierarchy,row,1,doc.occurrences.at(occurrence.parent).id);
        number(hierarchy,row,2,depth); string(hierarchy,row,3,occurrence.name);
        string(hierarchy,row,4,occurrence.partIndex<0?QStringLiteral("装配/组"):QStringLiteral("叶级产品"));
        string(hierarchy,row,5,labelEntry(occurrence.definitionLabel)); string(hierarchy,row,6,occurrence.path);
        number(hierarchy,row,7,1); ++row;
    }
    if(row>1) checked(worksheet_autofilter(hierarchy,0,0,row-1,7));
    writePositions(workbook,doc,nullptr,cancel); writeDiagnostics(workbook,doc,{},cancel);
    workbook.finish(path,cancel);
}

void exportPartManifest(const CadDocument& doc, const QString& path,
                        const QMap<int,QString>& filenames,const QStringList& diagnostics) {
    Workbook workbook;
    auto* sheet=workbook.sheet("零件清单",{QStringLiteral("定义 ID"),QStringLiteral("原名称"),QStringLiteral("编号"),
        QStringLiteral("文件名"),doc.occurrenceScopeApplied?QStringLiteral("本次勾选数量"):QStringLiteral("原装配总数量"),
        QStringLiteral("坐标说明")});
    const auto quantities=partQuantities(doc);
    int row=1;
    for(auto it=filenames.cbegin();it!=filenames.cend();++it) {
        const auto& part=doc.parts.at(it.key());
        string(sheet,row,0,part.id); string(sheet,row,1,part.name); string(sheet,row,2,part.number);
        string(sheet,row,3,it.value()); number(sheet,row,4,quantities.at(it.key()));
        string(sheet,row,5,QStringLiteral("零件定义自身坐标；实例位置见实例位置表。")); ++row;
    }
    if(row>1) checked(worksheet_autofilter(sheet,0,0,row-1,5));
    writePositions(workbook,doc,&filenames); writeDiagnostics(workbook,doc,diagnostics);
    workbook.finish(path);
}
} // namespace Stp
