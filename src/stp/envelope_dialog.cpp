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

namespace Stp {
EnvelopeDialog::EnvelopeDialog(std::shared_ptr<CadDocument> doc,PlacementEdits edits,
    std::vector<int> selection,CadJobQueue* jobs,QWidget* parent):QDialog(parent),
    m_source(std::move(doc)),m_edits(std::move(edits)),m_selection(std::move(selection)),m_jobs(jobs){
    static quint64 nextOwner=quint64(1)<<62;m_owner=++nextOwner;
    if(!m_jobs)m_jobs=new CadJobQueue(this);
    setObjectName("customerEnvelopeDialog");setWindowTitle(QStringLiteral("生成客户用实心外包络"));
    setWindowModality(Qt::ApplicationModal);resize(1100,760);setMinimumSize(820,600);
    auto layout=new QVBoxLayout(this);auto settings=new QHBoxLayout;
    settings->addWidget(new QLabel(QStringLiteral("范围")));
    m_scope=new QComboBox;m_scope->setObjectName("envelopeScope");
    m_scope->addItems({QStringLiteral("整个装配"),QStringLiteral("打开窗口时选中的零件/子装配")});
    if(!m_selection.empty())m_scope->setCurrentIndex(1);settings->addWidget(m_scope,1);
    m_fill=new QCheckBox(QStringLiteral("填充封闭空腔"));m_fill->setObjectName("envelopeFill");m_fill->setChecked(true);settings->addWidget(m_fill);
    m_color=new QPushButton(QStringLiteral("结果颜色…"));m_color->setObjectName("envelopeColor");settings->addWidget(m_color);
    m_generate=new QPushButton(QStringLiteral("生成预览"));m_generate->setObjectName("envelopeGenerate");settings->addWidget(m_generate);layout->addLayout(settings);
    auto help=new QLabel(QStringLiteral("合并实体并吸收被填实体完全包住的内部零件；使用已应用的位置。通孔、外部凹槽及对外连通的内腔保留。"));
    help->setWordWrap(true);layout->addWidget(help);
    auto middle=new QHBoxLayout;m_view=new ViewerWidget;m_view->setObjectName("envelopeViewer");m_view->setJobQueue(m_jobs,m_owner);middle->addWidget(m_view,2);
    m_report=new QTextEdit;m_report->setObjectName("envelopeReport");m_report->setReadOnly(true);m_report->setMinimumWidth(230);middle->addWidget(m_report,1);layout->addLayout(middle,1);
    auto preview=new QHBoxLayout;m_mode=new QComboBox;m_mode->setObjectName("envelopePreviewMode");m_mode->addItems({QStringLiteral("外包络结果"),QStringLiteral("处理范围原几何（统一颜色）")});preview->addWidget(m_mode);
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
    connect(m_fill,&QCheckBox::toggled,this,&EnvelopeDialog::invalidate);
    connect(m_color,&QPushButton::clicked,this,[this]{auto c=QColorDialog::getColor(m_options.color,this,QStringLiteral("客户外包络颜色"));if(c.isValid()){m_options.color=c;invalidate();}});
    connect(m_review,&QCheckBox::toggled,this,&EnvelopeDialog::refresh);
    connect(m_mode,&QComboBox::currentIndexChanged,this,&EnvelopeDialog::showPreview);
    connect(fit,&QPushButton::clicked,m_view,&ViewerWidget::fitAll);
    const auto section=[this]{m_view->setSection(m_axis->currentIndex()-1,m_offset->value(),m_reverse->isChecked(),true);};
    connect(m_axis,&QComboBox::currentIndexChanged,this,section);connect(m_offset,&QSlider::valueChanged,this,section);
    connect(m_reverse,&QCheckBox::toggled,this,section);
    connect(m_view,&ViewerWidget::sceneReady,this,[this,section]{m_rendering=false;section();refresh();});
    connect(m_view,&ViewerWidget::sceneBuildCancelled,this,[this]{m_rendering=false;refresh();});
    connect(m_jobs,&CadJobQueue::ownerDrained,this,[this](quint64 owner){if(owner==m_owner)finishClose();});
    connect(m_view,&ViewerWidget::shutdownReady,this,&EnvelopeDialog::finishClose);
    m_report->setPlainText(QStringLiteral("选择范围并生成预览。\n\n本版不自动封闭开口或桥接缝隙，也不自动证明全部内部结构已隐藏。\n\n客户版默认统一名称和颜色，不携带原装配树、材料和 PMI。填实后的体积不能用于计算原产品质量。"));refresh();
}
void EnvelopeDialog::refresh(){
    const bool ready=!m_busy&&!m_rendering&&!m_closing;
    for(auto w:std::initializer_list<QWidget*>{m_scope,m_fill,m_color,m_generate})w->setEnabled(ready);
    m_mode->setEnabled(ready&&bool(m_result));m_review->setEnabled(ready&&bool(m_result));
    m_axis->setEnabled(ready&&bool(m_result));m_offset->setEnabled(ready&&bool(m_result));
    m_reverse->setEnabled(ready&&bool(m_result));
    m_save->setEnabled(ready&&bool(m_result)&&m_review->isChecked());m_cancel->setEnabled(m_busy&&!m_closing);
    m_progress->setVisible(m_busy);
}
void EnvelopeDialog::invalidate(){
    m_result.reset();m_originalPreview.reset();m_resultPreview.reset();m_review->setChecked(false);
    m_view->clearDocument();m_report->setPlainText(QStringLiteral("设置已变化，请重新生成预览。"));refresh();
}
void EnvelopeDialog::generate(){
    if(m_busy||m_rendering||m_closing)return;invalidate();m_options.fillClosedCavities=m_fill->isChecked();
    auto selected=m_selection;if(m_scope->currentIndex()==0){selected.clear();for(size_t i=0;i<m_source->occurrences.size();++i)if(m_source->occurrences[i].parent<0)selected.push_back(int(i));}
    if(selected.empty()){m_report->setPlainText(QStringLiteral("没有选中的零件，请选择整个装配，或关闭窗口后重新选择。"));return;}
    auto result=std::make_shared<EnvelopeResult>();auto original=std::make_shared<std::shared_ptr<CadDocument>>();auto output=std::make_shared<std::shared_ptr<CadDocument>>();
    const auto source=m_source;const auto edits=m_edits;const auto options=m_options;QPointer<EnvelopeDialog> self(this);
    m_busy=true;m_progress->setValue(0);refresh();
    m_jobs->enqueue(m_owner,[source,edits,selected,options,result,original,output](std::atomic_bool& cancel,const ProgressCallback& progress){
        *result=buildCustomerEnvelope(*source,edits,selected,options,cancel,progress);
        *original=envelopeDocument(result->original,QColor("#b0b0b0"));*output=envelopeDocument(result->shape,options.color);
        prepareDisplayMeshes(**original,0,cancel);prepareDisplayMeshes(**output,0,cancel);
    },[self](int n,const QString& text){if(self&&!self->m_closing){self->m_progress->setValue(n);self->m_report->setPlainText(text);}},
    [self,result,original,output](const QString& error){
        if(!self)return;self->m_busy=false;if(self->m_closing){self->finishClose();return;}
        if(!error.isEmpty()){self->m_report->setPlainText(error);self->refresh();return;}
        self->m_result=result;self->m_originalPreview=*original;self->m_resultPreview=*output;
        self->m_report->setPlainText(QStringLiteral("几何处理完成，待交付检查\n\n输入：%1 个零件实例 / %2 个实体\n输出：%3 个实体\n填充封闭空腔：%4 个\n合并后原体积：%5 mm³\n结果体积：%6 mm³\n新增体积：%7 mm³\n\n%8")
            .arg(result->inputOccurrences).arg(result->inputSolids).arg(result->outputSolids).arg(result->filledCavities)
            .arg(result->inputVolume,0,'g',10).arg(result->outputVolume,0,'g',10).arg(result->outputVolume-result->inputVolume,0,'g',10).arg(result->warnings.join("\n\n")));
        self->showPreview();
    });
}
void EnvelopeDialog::showPreview(){
    if(!m_result||m_busy||m_closing)return;m_rendering=true;refresh();
    m_view->setDocument(m_mode->currentIndex()==0?m_resultPreview:m_originalPreview,{},true);
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
