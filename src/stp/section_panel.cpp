#include "section_panel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace Stp {
SectionPanel::SectionPanel(QWidget* parent):QWidget(parent) {
    setObjectName("sectionPanel");
    auto outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    auto scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumSize(0,0);scroll->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
    auto content=new QWidget;scroll->setWidget(content);outer->addWidget(scroll);
    auto layout=new QVBoxLayout(content); auto form=new QFormLayout;
    m_base=new QComboBox; m_base->setObjectName("sectionAxis");
    m_base->addItems({QStringLiteral("关闭"),QStringLiteral("YZ 平面（法向 X）"),
        QStringLiteral("ZX 平面（法向 Y）"),QStringLiteral("XY 平面（法向 Z）"),QStringLiteral("拾取的零件平面")});
    form->addRow(QStringLiteral("基准"),m_base);
    const auto number=[&](const QString& name,double limit,const QString& suffix){
        auto box=new QDoubleSpinBox;box->setObjectName(name);box->setRange(-limit,limit);
        box->setDecimals(3);box->setSuffix(suffix);box->setKeyboardTracking(false);
        connect(box,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{if(!m_sync)emit sectionChanged();});return box;
    };
    m_offset=number("sectionOffset",1.e8,QStringLiteral(" mm"));form->addRow(QStringLiteral("法向偏移"),m_offset);
    m_slider=new QSlider(Qt::Horizontal);m_slider->setRange(0,100);m_slider->setValue(50);m_slider->setObjectName("sectionSlider");
    form->addRow(QStringLiteral("快速偏移"),m_slider);
    m_axis=new QComboBox;m_axis->setObjectName("sectionRotationAxis");m_axis->addItems({"X","Y","Z"});form->addRow(QStringLiteral("旋转轴（全局）"),m_axis);
    m_angle=number("sectionAngle",360.,QStringLiteral(" °"));form->addRow(QStringLiteral("旋转角度"),m_angle);
    m_centerMode=new QComboBox;m_centerMode->setObjectName("sectionCenterMode");
    m_centerMode->addItems({QStringLiteral("基准点 / 拾取点"),QStringLiteral("自定义坐标")});form->addRow(QStringLiteral("旋转中心"),m_centerMode);
    for(int i=0;i<3;++i){m_point[i]=number(QString("sectionCenter%1").arg(i),1.e8,QStringLiteral(" mm"));
        m_point[i]->setEnabled(false);form->addRow(QString(QChar("XYZ"[i])),m_point[i]);}
    layout->addLayout(form);
    auto pickRow=new QHBoxLayout;m_pick=new QPushButton(QStringLiteral("拾取零件平面…"));m_pick->setObjectName("pickSectionPlane");
    m_cancel=new QPushButton(QStringLiteral("取消拾取"));m_cancel->setObjectName("cancelSectionPick");m_cancel->hide();
    pickRow->addWidget(m_pick);pickRow->addWidget(m_cancel);layout->addLayout(pickRow);
    auto row=new QHBoxLayout;
    const auto check=[&](const QString& text,const char* name,bool initial){auto b=new QCheckBox(text);b->setObjectName(name);b->setChecked(initial);
        connect(b,&QCheckBox::toggled,this,[this]{if(!m_sync)emit sectionChanged();});return b;};
    m_reverse=check(QStringLiteral("反向"),"sectionReverse",false);m_cap=check(QStringLiteral("封盖"),"sectionCap",true);
    m_objectColor=check(QStringLiteral("封盖随零件颜色"),"sectionObjectColor",true);
    row->addWidget(m_reverse);row->addWidget(m_cap);layout->addLayout(row);layout->addWidget(m_objectColor);
    m_show=check(QStringLiteral("显示剖切平面轮廓"),"sectionShowPlane",false);layout->addWidget(m_show);
    auto resetButton=new QPushButton(QStringLiteral("复位并关闭剖切"));resetButton->setObjectName("resetSection");layout->addWidget(resetButton);
    m_hint=new QLabel(QStringLiteral("基准平面穿过模型中心；先旋转，再沿新法向偏移。剖切和封盖仅影响显示。"));
    m_hint->setWordWrap(true);layout->addWidget(m_hint);layout->addStretch();
    connect(m_base,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{if(!m_sync)baseChanged();});
    connect(m_axis,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{if(!m_sync)emit sectionChanged();});
    connect(m_centerMode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){for(auto p:m_point)p->setEnabled(i==1);if(!m_sync)emit sectionChanged();});
    connect(m_slider,&QSlider::valueChanged,this,[this](int v){if(m_sync)return;m_offset->setValue((v-50)*m_span/100.);});
    connect(m_offset,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{updateSlider();});
    connect(m_pick,&QPushButton::clicked,this,&SectionPanel::pickRequested);
    connect(m_cancel,&QPushButton::clicked,this,&SectionPanel::cancelPickRequested);
    connect(resetButton,&QPushButton::clicked,this,&SectionPanel::reset);
}
SectionState SectionPanel::state() const {
    SectionState s;const int base=m_base->currentIndex();s.enabled=base>0&&(base<4||m_hasPicked);
    const gp_Dir normals[]{gp_Dir(1,0,0),gp_Dir(0,1,0),gp_Dir(0,0,1)};
    s.base=base==4?m_picked:gp_Pln(m_center,normals[std::clamp(base-1,0,2)]);
    s.pivot=m_centerMode->currentIndex()==1?gp_Pnt(m_point[0]->value(),m_point[1]->value(),m_point[2]->value()):s.base.Location();
    s.rotationAxis=m_axis->currentIndex();s.angleDegrees=m_angle->value();s.offsetMm=m_offset->value();
    s.reverse=m_reverse->isChecked();s.cap=m_cap->isChecked();s.objectColor=m_objectColor->isChecked();s.showPlane=m_show->isChecked();return s;
}
void SectionPanel::updateSlider(){if(m_sync)return;m_sync=true;m_slider->setValue(std::clamp(qRound(50.+m_offset->value()*100./m_span),0,100));m_sync=false;}
void SectionPanel::baseChanged(){
    m_sync=true;m_offset->setValue(0);m_angle->setValue(0);m_slider->setValue(50);m_sync=false;
    if(m_base->currentIndex()==4&&!m_hasPicked)m_hint->setText(QStringLiteral("请点击“拾取零件平面”，再点击模型上的平面。"));
    emit cancelPickRequested();emit sectionChanged();
}
void SectionPanel::setBounds(const Bnd_Box& box){
    if(!box.IsVoid()&&!box.IsOpen()){double a,b,c,d,e,f;box.Get(a,b,c,d,e,f);m_center=gp_Pnt((a+d)/2,(b+e)/2,(c+f)/2);m_span=std::max(1.,std::sqrt(box.SquareExtent()));}
    updateSlider();emit sectionChanged();
}
void SectionPanel::setPickedPlane(const gp_Pln& plane,const gp_Pnt&,const QString& name){
    m_picked=plane;m_hasPicked=true;m_sync=true;m_base->setCurrentIndex(4);m_angle->setValue(0);m_offset->setValue(0);m_slider->setValue(50);m_centerMode->setCurrentIndex(0);m_sync=false;
    setPicking(false);m_hint->setText(QStringLiteral("基准：%1。平面固定于拾取时的世界坐标。Esc 取消拾取。").arg(name));emit sectionChanged();
}
void SectionPanel::setPicking(bool active){m_cancel->setVisible(active);m_pick->setEnabled(!active);
    if(active)m_hint->setText(QStringLiteral("请点击零件上的平面；Esc 取消。拾取期间暂时关闭剖切。"));}
void SectionPanel::reset(){
    m_sync=true;m_base->setCurrentIndex(0);m_axis->setCurrentIndex(0);m_centerMode->setCurrentIndex(0);
    m_angle->setValue(0);m_offset->setValue(0);m_slider->setValue(50);for(auto p:m_point)p->setValue(0);
    m_reverse->setChecked(false);m_cap->setChecked(true);m_objectColor->setChecked(true);m_show->setChecked(false);m_hasPicked=false;m_sync=false;
    setPicking(false);m_hint->setText(QStringLiteral("基准平面穿过模型中心；先旋转，再沿新法向偏移。剖切和封盖仅影响显示。"));
    emit cancelPickRequested();emit sectionChanged();
}
}
