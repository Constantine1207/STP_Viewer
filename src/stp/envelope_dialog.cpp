#include "envelope_dialog.h"
#include "cad_job_queue.h"
#include "viewer_widget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTextEdit>
#include <QProgressBar>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QCloseEvent>
#include <QPointer>
#include <QTimer>
#include <QJsonDocument>
#include <QTabWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QSignalBlocker>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <cmath>
#include <algorithm>

namespace Stp {
EnvelopeDialog::EnvelopeDialog(std::shared_ptr<CadDocument> doc,PlacementEdits edits,
    std::vector<int> selection,CadJobQueue* jobs,QWidget* parent,EnvelopeOptions options):QDialog(parent),
    m_source(std::move(doc)),m_edits(std::move(edits)),m_selection(std::move(selection)),m_jobs(jobs){
    m_options=std::move(options);
    static quint64 nextOwner=quint64(1)<<62;m_owner=++nextOwner;
    if(!m_jobs)m_jobs=new CadJobQueue(this);
    setObjectName("customerEnvelopeDialog");setWindowTitle(QStringLiteral("生成客户用实心外包络"));
    setWindowModality(Qt::ApplicationModal);resize(1100,760);setMinimumSize(820,600);
    auto layout=new QVBoxLayout(this);auto settings=new QHBoxLayout;
    settings->addWidget(new QLabel(QStringLiteral("范围")));
    m_scope=new QComboBox;m_scope->setObjectName("envelopeScope");
    m_scope->addItems({QStringLiteral("整个装配"),QStringLiteral("打开窗口时选中的零件/子装配")});
    if(!m_selection.empty())m_scope->setCurrentIndex(1);settings->addWidget(m_scope,1);
    m_fill=new QCheckBox(QStringLiteral("填充封闭空腔"));m_fill->setObjectName("envelopeFill");m_fill->setChecked(m_options.fillClosedCavities);settings->addWidget(m_fill);
    m_color=new QPushButton(QStringLiteral("结果颜色…"));m_color->setObjectName("envelopeColor");settings->addWidget(m_color);
    m_generate=new QPushButton(QStringLiteral("生成预览"));m_generate->setObjectName("envelopeGenerate");settings->addWidget(m_generate);layout->addLayout(settings);
    auto help=new QLabel(QStringLiteral("合并实体并吸收被填实体完全包住的内部零件；使用已应用的位置。通孔、外部凹槽及对外连通的内腔保留。"));
    help->setWordWrap(true);layout->addWidget(help);
    auto middle=new QHBoxLayout;m_view=new ViewerWidget;m_view->setObjectName("envelopeViewer");m_view->setJobQueue(m_jobs,m_owner);middle->addWidget(m_view,2);
    m_report=new QTextEdit;m_report->setObjectName("envelopeReport");m_report->setReadOnly(true);m_report->setMinimumWidth(230);auto tabs=new QTabWidget;tabs->setMinimumWidth(330);tabs->addTab(m_report,QStringLiteral("检查报告"));
    buildRules(tabs);middle->addWidget(tabs,1);layout->addLayout(middle,1);
    auto preview=new QHBoxLayout;m_mode=new QComboBox;m_mode->setObjectName("envelopePreviewMode");m_mode->addItems({QStringLiteral("外包络结果"),QStringLiteral("处理范围原几何（统一颜色）"),QStringLiteral("原几何与保护空间")});preview->addWidget(m_mode);
    auto fit=new QPushButton(QStringLiteral("适配窗口"));preview->addWidget(fit);
    m_axis=new QComboBox;m_axis->setObjectName("envelopeSectionAxis");m_axis->addItems({QStringLiteral("关闭剖切"),QStringLiteral("X 剖切"),QStringLiteral("Y 剖切"),QStringLiteral("Z 剖切")});preview->addWidget(m_axis);
    m_reverse=new QCheckBox(QStringLiteral("反向"));m_reverse->setObjectName("envelopeSectionReverse");m_reverse->setChecked(true);preview->addWidget(m_reverse);
    m_offset=new QSlider(Qt::Horizontal);m_offset->setObjectName("envelopeSectionOffset");m_offset->setRange(0,100);m_offset->setValue(50);preview->addWidget(m_offset,1);layout->addLayout(preview);
    m_review=new QCheckBox(QStringLiteral("我已通过剖切检查内部空间及安装接口，确认此结果可用于客户交付"));m_review->setObjectName("envelopeReviewed");layout->addWidget(m_review);
    m_progress=new QProgressBar;m_progress->setObjectName("envelopeProgress");m_progress->setRange(0,100);layout->addWidget(m_progress);
    auto actions=new QHBoxLayout;m_cancel=new QPushButton(QStringLiteral("取消任务"));m_cancel->setObjectName("envelopeCancel");actions->addWidget(m_cancel);actions->addStretch();
    m_save=new QPushButton(QStringLiteral("导出客户版 STEP…"));m_save->setObjectName("envelopeExport");actions->addWidget(m_save);
    auto close=new QPushButton(QStringLiteral("关闭"));close->setObjectName("envelopeClose");actions->addWidget(close);layout->addLayout(actions);
    connect(close,&QPushButton::clicked,this,&EnvelopeDialog::reject);
    connect(m_generate,&QPushButton::clicked,this,&EnvelopeDialog::generate);
    connect(m_save,&QPushButton::clicked,this,&EnvelopeDialog::save);
    connect(m_cancel,&QPushButton::clicked,this,[this]{m_jobs->cancelOwner(m_owner);m_report->append(QStringLiteral("正在取消，请等待当前几何步骤退出…"));});
    connect(m_scope,&QComboBox::currentIndexChanged,this,&EnvelopeDialog::invalidate);
    connect(m_fill,&QCheckBox::toggled,this,[this](bool v){m_options.fillClosedCavities=v;invalidate();});
    connect(m_color,&QPushButton::clicked,this,[this]{auto c=QColorDialog::getColor(m_options.color,this,QStringLiteral("客户外包络颜色"));if(c.isValid()){m_options.color=c;invalidate();}});
    connect(m_review,&QCheckBox::toggled,this,&EnvelopeDialog::refresh);
    connect(m_mode,&QComboBox::currentIndexChanged,this,&EnvelopeDialog::showPreview);
    connect(fit,&QPushButton::clicked,m_view,&ViewerWidget::fitAll);
    const auto section=[this]{m_view->setSection(m_axis->currentIndex()-1,m_offset->value(),m_reverse->isChecked(),true);};
    connect(m_axis,&QComboBox::currentIndexChanged,this,section);connect(m_offset,&QSlider::valueChanged,this,section);
    connect(m_reverse,&QCheckBox::toggled,this,section);
    connect(m_view,&ViewerWidget::sceneReady,this,[this,section]{m_rendering=false;section();
        if(m_mode->currentIndex()==2)for(size_t i=0;i<m_resultPreviewTools->occurrences.size();++i){
            const auto& o=m_resultPreviewTools->occurrences[i];if(o.partIndex>=0)m_view->setOccurrenceTemporaryTransparency(int(i),65);}
        locateRule();refresh();});
    connect(m_view,&ViewerWidget::sceneBuildCancelled,this,[this]{m_rendering=false;refresh();});
    connect(m_jobs,&CadJobQueue::ownerDrained,this,[this](quint64 owner){if(owner==m_owner)finishClose();});
    connect(m_view,&ViewerWidget::shutdownReady,this,&EnvelopeDialog::finishClose);
    connect(m_view,&ViewerWidget::sectionPlanePicked,this,[this](const gp_Pln& plane,const gp_Pnt&,const QString&){
        m_hasProtectionBasis=true;m_protectionBasis.SetDisplacement(gp_Ax3(),plane.Position());
        m_report->append(QStringLiteral("已记录所选平面坐标系，可在添加保护空间时选择“拾取平面”。"));
    });
    m_report->setPlainText(QStringLiteral("选择范围并生成预览。\n\n本版不自动封闭开口或桥接缝隙，也不自动证明全部内部结构已隐藏。\n\n客户版默认统一名称和颜色，不携带原装配树、材料和 PMI。填实后的体积不能用于计算原产品质量。"));refresh();
}
void EnvelopeDialog::buildRules(QTabWidget* tabs){
    m_rulesPage=new QWidget;auto layout=new QVBoxLayout(m_rulesPage);
    auto hint=new QLabel(QStringLiteral("勾选的实例保留原几何和实例颜色，不参与填实。子装配可在主窗口右键批量设置。"));hint->setWordWrap(true);layout->addWidget(hint);
    m_preserveList=new QListWidget;m_preserveList->setObjectName("envelopePreserveList");layout->addWidget(m_preserveList,1);
    const auto preserved=envelopeScope(*m_source,m_options.preservedOccurrences);
    for(size_t i=0;i<m_source->occurrences.size();++i){const auto& o=m_source->occurrences[i];if(o.partIndex<0)continue;
        auto item=new QListWidgetItem(QStringLiteral("[%1] %2").arg(i).arg(o.name),m_preserveList);item->setData(Qt::UserRole,int(i));
        item->setToolTip(o.path);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);
        item->setCheckState(std::binary_search(preserved.begin(),preserved.end(),int(i))?Qt::Checked:Qt::Unchecked);
    }
    auto locate=new QPushButton(QStringLiteral("定位选中项 / 预览原装配"));locate->setObjectName("envelopeLocateRule");layout->addWidget(locate);
    connect(locate,&QPushButton::clicked,this,&EnvelopeDialog::previewRules);
    m_keywords=new QLineEdit(QStringLiteral("Connector,Conn,接插件,连接器"));m_keywords->setObjectName("envelopeKeywords");layout->addWidget(m_keywords);
    auto recommend=new QPushButton(QStringLiteral("按名称推荐（确认后生效）"));recommend->setObjectName("envelopeRecommend");layout->addWidget(recommend);
    connect(recommend,&QPushButton::clicked,this,[this]{
        const auto candidates=envelopePreservationCandidates(*m_source,m_keywords->text().split(','));
        QDialog dialog(this);dialog.setWindowTitle(QStringLiteral("确认建议保留的实例"));auto box=new QVBoxLayout(&dialog);
        auto list=new QListWidget;list->setObjectName("envelopeCandidates");box->addWidget(list);
        for(const auto& candidate:candidates){auto item=new QListWidgetItem(QStringLiteral("[%1] %2 — %3").arg(candidate.first).arg(m_source->occurrences[candidate.first].name,candidate.second),list);
            item->setData(Qt::UserRole,candidate.first);item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(Qt::Unchecked);}
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);box->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        if(dialog.exec()!=QDialog::Accepted)return;
        {QSignalBlocker block(m_preserveList);for(int i=0;i<list->count();++i)if(list->item(i)->checkState()==Qt::Checked)
            for(int j=0;j<m_preserveList->count();++j)if(m_preserveList->item(j)->data(Qt::UserRole)==list->item(i)->data(Qt::UserRole))m_preserveList->item(j)->setCheckState(Qt::Checked);}
        updateRules();invalidate();
    });
    m_ruleCount=new QLabel;layout->addWidget(m_ruleCount);tabs->addTab(m_rulesPage,QStringLiteral("保留零件"));
    connect(m_preserveList,&QListWidget::itemChanged,this,[this]{updateRules();invalidate();});
    connect(m_scope,&QComboBox::currentIndexChanged,this,&EnvelopeDialog::updateRules);updateRules();
    m_protectionPage=new QWidget;auto protectionLayout=new QVBoxLayout(m_protectionPage);
    auto notice=new QLabel(QStringLiteral("保护空间为世界坐标中的空腔工具，不导出。绿色透明预览；与保留零件材料冲突会拒绝生成。移动零件后需重新检查工具位置。"));notice->setWordWrap(true);protectionLayout->addWidget(notice);
    m_protectionList=new QListWidget;m_protectionList->setObjectName("envelopeProtections");protectionLayout->addWidget(m_protectionList,1);
    auto addButton=[&](const QString& title,const char* name,auto callback){auto b=new QPushButton(title);b->setObjectName(name);protectionLayout->addWidget(b);connect(b,&QPushButton::clicked,this,callback);};
    addButton(QStringLiteral("添加长方体…"),"envelopeAddBox",[this]{addProtection(false);});
    addButton(QStringLiteral("添加圆柱体…"),"envelopeAddCylinder",[this]{addProtection(true);});
    addButton(QStringLiteral("导入 STEP 工具体…"),"envelopeImportTool",[this]{importProtection();});
    addButton(QStringLiteral("从预览拾取基准平面"),"envelopePickBasis",[this]{if(m_originalPreview&&!m_rendering)m_view->beginSectionPlanePick();else m_report->setPlainText(QStringLiteral("请先在保留零件页点击预览原装配，再拾取平面。"));});
    addButton(QStringLiteral("移除选中保护空间"),"envelopeRemoveTool",[this]{const int row=m_protectionList->currentRow();if(row<0)return;m_options.protections.erase(m_options.protections.begin()+row);updateProtectionList();invalidate();});
    addButton(QStringLiteral("预览原装配与保护空间"),"envelopePreviewTools",[this]{previewRules();});
    connect(m_protectionList,&QListWidget::itemChanged,this,[this](QListWidgetItem* item){const auto row=m_protectionList->row(item);if(row>=0){m_options.protections[row].allowOriginalCut=item->checkState()==Qt::Checked;invalidate();}});
    tabs->addTab(m_protectionPage,QStringLiteral("保护空间"));updateProtectionList();
}
void EnvelopeDialog::updateRules(){
    m_options.preservedOccurrences.clear();int scoped=0,total=0;auto selected=m_selection;
    if(m_scope->currentIndex()==0){selected.clear();for(size_t i=0;i<m_source->occurrences.size();++i)if(m_source->occurrences[i].parent<0)selected.push_back(int(i));}
    const auto scope=envelopeScope(*m_source,selected);total=int(scope.size());
    for(int j=0;j<m_preserveList->count();++j){auto item=m_preserveList->item(j);if(item->checkState()!=Qt::Checked)continue;
        const int i=item->data(Qt::UserRole).toInt();m_options.preservedOccurrences.push_back(i);if(std::binary_search(scope.begin(),scope.end(),i))++scoped;}
    m_ruleCount->setText(QStringLiteral("当前范围：参与填实 %1 / 保留 %2；范围外规则不参与处理").arg(total-scoped).arg(scoped));
}
void EnvelopeDialog::updateProtectionList(){
    QSignalBlocker block(m_protectionList);m_protectionList->clear();
    for(const auto& tool:m_options.protections){auto item=new QListWidgetItem(tool.label+QStringLiteral("（勾选＝允许切除原材料）"),m_protectionList);
        item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(tool.allowOriginalCut?Qt::Checked:Qt::Unchecked);}
}
void EnvelopeDialog::addProtection(bool cylinder){
    QDialog dialog(this);dialog.setObjectName("envelopeToolEditor");dialog.setWindowTitle(cylinder?QStringLiteral("圆柱保护空间"):QStringLiteral("长方体保护空间"));auto form=new QFormLayout(&dialog);
    auto basis=new QComboBox;basis->setObjectName("toolBasis");basis->addItems({"XY","YZ","ZX",QStringLiteral("拾取平面")});form->addRow(QStringLiteral("坐标基准"),basis);
    std::array<QDoubleSpinBox*,9> values{};const QStringList labels={cylinder?QStringLiteral("半径"):QStringLiteral("X 长度"),QStringLiteral("Y 长度（圆柱忽略）"),QStringLiteral("Z 高度"),"X","Y","Z","Rx°","Ry°","Rz°"};
    for(int i=0;i<9;++i){auto v=new QDoubleSpinBox;v->setObjectName(QString("toolValue%1").arg(i));v->setDecimals(4);v->setRange(i<3?.0001:-1000000,1000000);v->setValue(i<3?10:0);values[i]=v;form->addRow(labels[i],v);}
    values[1]->setEnabled(!cylinder);
    auto hint=new QLabel(QStringLiteral("尺寸/位置单位 mm。原点为长方体最小角或圆柱底面中心，旋转依次 Rx、Ry、Rz。"));hint->setWordWrap(true);form->addRow(hint);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);form->addRow(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    gp_Trsf base,rx,ry,rz;const double deg=std::acos(-1.)/180.;
    if(basis->currentIndex()==1)base.SetDisplacement(gp_Ax3(),gp_Ax3(gp_Pnt(),gp_Dir(1,0,0),gp_Dir(0,1,0)));
    if(basis->currentIndex()==2)base.SetDisplacement(gp_Ax3(),gp_Ax3(gp_Pnt(),gp_Dir(0,1,0),gp_Dir(0,0,1)));
    if(basis->currentIndex()==3){if(!m_hasProtectionBasis){m_report->setPlainText(QStringLiteral("尚未拾取基准平面，请先预览并拾取。"));return;}base=m_protectionBasis;}
    rx.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(1,0,0)),values[6]->value()*deg);ry.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,1,0)),values[7]->value()*deg);rz.SetRotation(gp_Ax1(gp_Pnt(),gp_Dir(0,0,1)),values[8]->value()*deg);
    auto local=rz*ry*rx;local.SetTranslationPart(gp_Vec(values[3]->value(),values[4]->value(),values[5]->value()));
    m_options.protections.push_back({envelopeProtectionPrimitive(cylinder,values[0]->value(),values[1]->value(),values[2]->value(),base*local),cylinder?QStringLiteral("圆柱空间"):QStringLiteral("长方体空间"),false});
    updateProtectionList();invalidate();
}
void EnvelopeDialog::previewRules(){
    if(m_busy||m_rendering||m_closing)return;
    auto document=std::make_shared<std::shared_ptr<CadDocument>>();const auto source=m_source;const auto edits=m_edits;const auto options=m_options;QPointer<EnvelopeDialog> self(this);
    m_busy=true;refresh();m_jobs->enqueue(m_owner,[document,source,edits,options](std::atomic_bool& cancel,const ProgressCallback&){
        EnvelopeResult preview;for(size_t i=0;i<source->occurrences.size();++i){const auto& o=source->occurrences[i];if(o.partIndex<0)continue;
            BRepBuilderAPI_Copy copy(source->parts[o.partIndex].shape,true,false);
            preview.pieces.push_back({copy.Shape().Moved(TopLoc_Location(placedWorldTransform(*source,edits,int(i)))),QColor("#b0b0b0"),int(i)});}
        for(const auto& tool:options.protections)preview.pieces.push_back({tool.shape,QColor("#38db80"),int(preview.pieces.size())});
        *document=envelopeDocument(preview,QColor("#b0b0b0"));prepareDisplayMeshes(**document,0,cancel);
    },{},[self,document](const QString& error){if(!self)return;self->m_busy=false;if(self->m_closing){self->finishClose();return;}
        if(!error.isEmpty()){self->m_report->setPlainText(error);self->refresh();return;}
        // A rule preview is not a generated/approved output. Keep export disabled until regenerate.
        self->m_result.reset();self->m_review->setChecked(false);self->m_originalPreview=*document;self->m_resultPreviewTools=*document;
        {QSignalBlocker block(self->m_mode);self->m_mode->setCurrentIndex(2);}self->m_rendering=true;self->refresh();
        self->m_view->setDocument(*document,{},true);
    });
}
void EnvelopeDialog::locateRule(){
    if(!m_preserveList->currentItem()||!m_originalPreview||m_mode->currentIndex()==0)return;
    const int target=m_preserveList->currentItem()->data(Qt::UserRole).toInt();int ordinal=0;
    if(m_result){for(const auto& piece:m_result->originalPieces){++ordinal;if(piece.sourceOccurrence==target)break;}}
    else for(int i=0;i<=target;++i)if(m_source->occurrences[i].partIndex>=0)++ordinal;
    const auto doc=m_mode->currentIndex()==2?m_resultPreviewTools:m_originalPreview;
    for(size_t i=0;i<doc->occurrences.size();++i){const auto& o=doc->occurrences[i];if(o.partIndex>=0&&doc->parts[o.partIndex].name==QString("RetainedPart%1").arg(ordinal))m_view->selectOccurrences({int(i)});}
}
void EnvelopeDialog::importProtection(){
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("导入保护空间工具体（世界坐标）"),{},QStringLiteral("STEP (*.step *.stp)"));if(path.isEmpty())return;
    auto shape=std::make_shared<TopoDS_Shape>();QPointer<EnvelopeDialog> self(this);m_busy=true;refresh();
    m_jobs->enqueue(m_owner,[shape,path](std::atomic_bool& cancel,const ProgressCallback& progress){auto doc=loadStep(path,progress,cancel);
        TopoDS_Compound c;BRep_Builder builder;builder.MakeCompound(c);for(const auto& o:doc->occurrences)if(o.partIndex>=0){BRepBuilderAPI_Copy copy(doc->parts[o.partIndex].shape,true,false);builder.Add(c,copy.Shape().Moved(TopLoc_Location(o.worldTransform)));}*shape=c;
    },{},[self,shape](const QString& error){if(!self)return;self->m_busy=false;if(self->m_closing){self->finishClose();return;}if(!error.isEmpty()){self->m_report->setPlainText(error);self->refresh();return;}
        self->m_options.protections.push_back({*shape,QStringLiteral("导入工具实体"),false});self->updateProtectionList();self->invalidate();});
}
void EnvelopeDialog::refresh(){
    const bool ready=!m_busy&&!m_rendering&&!m_closing;
    for(auto w:std::initializer_list<QWidget*>{m_scope,m_fill,m_color,m_generate,m_rulesPage,m_protectionPage})w->setEnabled(ready);
    m_mode->setEnabled(ready&&bool(m_result));m_review->setEnabled(ready&&bool(m_result));
    m_axis->setEnabled(ready&&bool(m_originalPreview));m_offset->setEnabled(ready&&bool(m_originalPreview));
    m_reverse->setEnabled(ready&&bool(m_originalPreview));
    m_save->setEnabled(ready&&bool(m_result)&&m_review->isChecked());m_cancel->setEnabled(m_busy&&!m_closing);
    m_progress->setVisible(m_busy);
}
void EnvelopeDialog::invalidate(){
    m_view->cancelSectionPlanePick();
    m_result.reset();m_originalPreview.reset();m_resultPreview.reset();m_resultPreviewTools.reset();m_review->setChecked(false);
    m_view->clearDocument();m_report->setPlainText(QStringLiteral("设置已变化，请重新生成预览。"));refresh();
}
void EnvelopeDialog::generate(){
    if(m_busy||m_rendering||m_closing)return;invalidate();m_options.fillClosedCavities=m_fill->isChecked();
    auto selected=m_selection;if(m_scope->currentIndex()==0){selected.clear();for(size_t i=0;i<m_source->occurrences.size();++i)if(m_source->occurrences[i].parent<0)selected.push_back(int(i));}
    if(selected.empty()){m_report->setPlainText(QStringLiteral("没有选中的零件，请选择整个装配，或关闭窗口后重新选择。"));return;}
    updateRules();
    auto result=std::make_shared<EnvelopeResult>();auto original=std::make_shared<std::shared_ptr<CadDocument>>();auto output=std::make_shared<std::shared_ptr<CadDocument>>();
    auto tools=std::make_shared<std::shared_ptr<CadDocument>>();
    const auto source=m_source;const auto edits=m_edits;const auto options=m_options;QPointer<EnvelopeDialog> self(this);
    m_busy=true;m_progress->setValue(0);refresh();
    m_jobs->enqueue(m_owner,[source,edits,selected,options,result,original,output,tools](std::atomic_bool& cancel,const ProgressCallback& progress){
        *result=buildCustomerEnvelope(*source,edits,selected,options,cancel,progress);
        EnvelopeResult sourceResult;sourceResult.pieces=result->originalPieces;
        *original=envelopeDocument(sourceResult,QColor("#b0b0b0"));*output=envelopeDocument(*result,options.color);
        for(const auto& tool:options.protections)sourceResult.pieces.push_back({tool.shape,QColor("#38db80"),int(sourceResult.pieces.size())});
        *tools=envelopeDocument(sourceResult,QColor("#b0b0b0"));
        prepareDisplayMeshes(**original,0,cancel);prepareDisplayMeshes(**output,0,cancel);prepareDisplayMeshes(**tools,0,cancel);
    },[self](int n,const QString& text){if(self&&!self->m_closing){self->m_progress->setValue(n);self->m_report->setPlainText(text);}},
    [self,result,original,output,tools](const QString& error){
        if(!self)return;self->m_busy=false;if(self->m_closing){self->finishClose();return;}
        if(!error.isEmpty()){self->m_report->setPlainText(error);self->refresh();return;}
        self->m_result=result;self->m_originalPreview=*original;self->m_resultPreview=*output;self->m_resultPreviewTools=*tools;
        self->m_report->setPlainText(QStringLiteral("几何处理完成，待交付检查\n\n输入：%1 个零件实例 / %2 个实体\n输出：%3 个实体\n填充封闭空腔：%4 个\n合并后原体积：%5 mm³\n结果体积：%6 mm³\n新增体积：%7 mm³\n\n%8")
            .arg(result->inputOccurrences).arg(result->inputSolids).arg(result->outputSolids).arg(result->filledCavities)
            .arg(result->inputVolume,0,'g',10).arg(result->outputVolume,0,'g',10).arg(result->outputVolume-result->inputVolume,0,'g',10).arg(result->warnings.join("\n\n")));
        self->m_report->append(QStringLiteral("\n参与填实：%1 个实例；保留：%2 个实例\n重叠材料：%3 mm³\n保护空间切除：%4 mm³，其中原有材料：%5 mm³")
            .arg(result->processedOccurrences).arg(result->preservedOccurrences).arg(result->overlapVolume).arg(result->removedVolume).arg(result->removedOriginalVolume));
        self->showPreview();
    });
}
void EnvelopeDialog::showPreview(){
    if(!m_result||m_busy||m_closing)return;m_rendering=true;refresh();
    m_view->setDocument(m_mode->currentIndex()==0?m_resultPreview:(m_mode->currentIndex()==1?m_originalPreview:m_resultPreviewTools),{},true);
}
void EnvelopeDialog::save(){
    if(!m_result||m_busy||m_rendering||!m_review->isChecked())return;
    auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出客户版 STEP"),QStringLiteral("CustomerEnvelope.step"),QStringLiteral("STEP (*.step *.stp)"));
    if(path.isEmpty())return;if(!path.endsWith(".step",Qt::CaseInsensitive)&&!path.endsWith(".stp",Qt::CaseInsensitive))path+=".step";
    const auto result=m_result;const auto options=m_options;const auto source=m_source->sourcePath;QPointer<EnvelopeDialog> self(this);
    m_busy=true;m_progress->setValue(0);refresh();
    m_jobs->enqueue(m_owner,[result,options,path,source](std::atomic_bool& cancel,const ProgressCallback& progress){exportCustomerEnvelope(*result,options,path,source,cancel,progress);},
        [self](int n,const QString&){if(self)self->m_progress->setValue(n);},[self,path](const QString& error){
            if(!self)return;self->m_busy=false;if(self->m_closing){self->finishClose();return;}
            self->m_report->append(error.isEmpty()?QStringLiteral("\n已保存并通过回读检查：%1").arg(path):error);self->refresh();
        });
}
void EnvelopeDialog::reject(){
    if(!m_closing){m_closing=true;m_jobs->cancelOwner(m_owner);m_view->requestSessionClose();refresh();}
    finishClose();
}
void EnvelopeDialog::finishClose(){if(m_closing&&!m_jobs->hasJobs(m_owner)&&!m_view->hasPendingWork())QDialog::reject();}
void EnvelopeDialog::closeEvent(QCloseEvent* event){event->ignore();reject();}
}
