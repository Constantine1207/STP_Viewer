#include "part_transform_panel.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QUndoCommand>
#include <QVBoxLayout>
#include <Standard_Failure.hxx>
#include <gp_Ax1.hxx>
#include <cmath>

namespace Stp {
namespace {
class PlacementCommand final:public QUndoCommand {
public:
    PlacementCommand(std::function<void(const PlacementEdits&)> set,PlacementEdits before,
                     PlacementEdits after,const QString& name):QUndoCommand(name),m_set(std::move(set)),
                     m_before(std::move(before)),m_after(std::move(after)){}
    void undo() override{m_set(m_before);}void redo() override{m_set(m_after);}
private:std::function<void(const PlacementEdits&)> m_set;PlacementEdits m_before,m_after;
};
bool sameEdits(const PlacementEdits& a,const PlacementEdits& b) {
    for(const auto& e:a)if(!samePlacement(e.second,placementDelta(b,e.first)))return false;
    for(const auto& e:b)if(!samePlacement(e.second,placementDelta(a,e.first)))return false;
    return true;
}
}
PartTransformPanel::PartTransformPanel(QWidget* parent):QWidget(parent),m_history(this) {
    setObjectName("partTransformPanel");auto outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    auto scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    auto content=new QWidget;scroll->setWidget(content);outer->addWidget(scroll);
    auto layout=new QVBoxLayout(content);
    m_selection=new QLabel(QStringLiteral("请选择一个零件实例"));m_selection->setWordWrap(true);layout->addWidget(m_selection);
    auto hint=new QLabel(QStringLiteral("全局坐标系；输入值为相对当前位置的增量。\n抓手及爆炸位置不写入另存 STEP。"));hint->setWordWrap(true);layout->addWidget(hint);
    m_form=new QWidget;auto form=new QVBoxLayout(m_form);form->setContentsMargins(0,0,0,0);layout->addWidget(m_form);
    m_mode=new QComboBox;m_mode->setObjectName("placementMode");m_mode->addItems({QStringLiteral("平移"),QStringLiteral("旋转")});form->addWidget(m_mode);
    auto spin=[this](const QString& name){auto s=new QDoubleSpinBox;s->setObjectName(name);s->setRange(-1.e7,1.e7);s->setDecimals(6);s->setSingleStep(1);s->setKeyboardTracking(false);
        connect(s,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{cancelPreview();});return s;};
    m_moveFields=new QWidget;auto move=new QFormLayout(m_moveFields);
    for(int i=0;i<3;++i){m_translation[i]=spin(QStringLiteral("placementTranslation%1").arg(i));move->addRow(QStringLiteral("Δ%1（mm）").arg(QChar('X'+i)),m_translation[i]);}form->addWidget(m_moveFields);
    m_rotateFields=new QWidget;auto rotate=new QFormLayout(m_rotateFields);
    m_axis=new QComboBox;m_axis->setObjectName("placementAxis");m_axis->addItems({"X","Y","Z"});rotate->addRow(QStringLiteral("旋转轴"),m_axis);
    m_angle=spin("placementAngle");m_angle->setRange(-36000,36000);rotate->addRow(QStringLiteral("角度（°）"),m_angle);
    m_pivot=new QComboBox;m_pivot->setObjectName("placementPivot");m_pivot->addItems({QStringLiteral("零件包围盒中心"),QStringLiteral("全局原点"),QStringLiteral("指定坐标")});rotate->addRow(QStringLiteral("旋转中心"),m_pivot);
    m_pointFields=new QWidget;auto point=new QFormLayout(m_pointFields);
    for(int i=0;i<3;++i){m_point[i]=spin(QStringLiteral("placementPivot%1").arg(i));point->addRow(QStringLiteral("%1（mm）").arg(QChar('X'+i)),m_point[i]);}rotate->addRow(m_pointFields);form->addWidget(m_rotateFields);
    auto buttons=new QHBoxLayout;form->addLayout(buttons);
    auto add=[this](QLayout* l,const QString& text,const char* name,auto fn){auto b=new QPushButton(text);b->setObjectName(name);l->addWidget(b);connect(b,&QPushButton::clicked,this,fn);return b;};
    add(buttons,QStringLiteral("预览"),"previewPlacement",&PartTransformPanel::preview);
    add(buttons,QStringLiteral("应用"),"applyPlacement",&PartTransformPanel::apply);
    add(buttons,QStringLiteral("取消"),"cancelPlacement",&PartTransformPanel::cancelPreview);
    add(form,QStringLiteral("恢复选中零件导入位置"),"resetPlacement",&PartTransformPanel::resetSelected);
    auto historyButtons=new QHBoxLayout;layout->addLayout(historyButtons);
    auto undo=add(historyButtons,QStringLiteral("撤销位置编辑"),"undoPlacement",[this]{cancelPreview();m_history.undo();});
    auto redo=add(historyButtons,QStringLiteral("重做位置编辑"),"redoPlacement",[this]{cancelPreview();m_history.redo();});
    undo->setEnabled(false);redo->setEnabled(false);
    connect(&m_history,&QUndoStack::canUndoChanged,undo,&QPushButton::setEnabled);connect(&m_history,&QUndoStack::canRedoChanged,redo,&QPushButton::setEnabled);
    connect(&m_history,&QUndoStack::cleanChanged,this,&PartTransformPanel::modifiedChanged);
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{cancelPreview();refresh();});
    connect(m_pivot,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{cancelPreview();refresh();});
    connect(m_axis,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{cancelPreview();});
    m_message=new QLabel;m_message->setWordWrap(true);m_message->setObjectName("placementMessage");layout->addWidget(m_message);layout->addStretch();refresh();
}
void PartTransformPanel::setDocument(std::shared_ptr<CadDocument> doc) {
    m_history.clear();m_edits.clear();m_preview.reset();m_occurrence=-1;m_document=std::move(doc);clearInputs();refresh();emit placementsChanged();
}
void PartTransformPanel::setSelection(int index){if(index!=m_occurrence){cancelPreview();clearInputs();}m_occurrence=index;refresh();}
void PartTransformPanel::setSceneEnabled(bool enabled){m_sceneEnabled=enabled;setEnabled(enabled);refresh();}
void PartTransformPanel::refresh(){
    const bool valid=m_sceneEnabled&&m_document&&m_occurrence>=0&&size_t(m_occurrence)<m_document->occurrences.size()&&m_document->occurrences[m_occurrence].partIndex>=0;
    m_form->setEnabled(valid);m_selection->setText(valid?m_document->occurrences[m_occurrence].path:QStringLiteral("请选择一个叶级零件实例"));
    m_moveFields->setVisible(m_mode->currentIndex()==0);m_rotateFields->setVisible(m_mode->currentIndex()==1);m_pointFields->setVisible(m_pivot->currentIndex()==2);
}
void PartTransformPanel::clearInputs(){for(auto s:m_translation)s->setValue(0);m_angle->setValue(0);}
gp_Trsf PartTransformPanel::requestedDelta() const {
    gp_Trsf action;
    if(m_mode->currentIndex()==0)action.SetTranslation(gp_Vec(m_translation[0]->value(),m_translation[1]->value(),m_translation[2]->value()));
    else {
        gp_Pnt center;
        if(m_pivot->currentIndex()==0)center=placedPartCenter(*m_document,m_edits,m_occurrence);
        else if(m_pivot->currentIndex()==2)center=gp_Pnt(m_point[0]->value(),m_point[1]->value(),m_point[2]->value());
        const int axis=m_axis->currentIndex();
        action.SetRotation(gp_Ax1(center,gp_Dir(axis==0,axis==1,axis==2)),m_angle->value()*std::acos(-1.)/180.);
    }
    return action*placementDelta(m_edits,m_occurrence);
}
void PartTransformPanel::preview(){
    if(!m_form->isEnabled())return;
    try{auto next=m_edits;const auto delta=requestedDelta();
        if(samePlacement(delta,gp_Trsf()))next.erase(m_occurrence);else next[m_occurrence]=delta;
        validatePlacementEdits(*m_document,next);m_preview=next;
        emit placementsChanged();m_message->setText(QStringLiteral("预览中；点击应用后才会进入 STEP 导出。"));}
    catch(const Standard_Failure& e){m_message->setText(QString::fromUtf8(e.GetMessageString()));}
    catch(const std::exception& e){m_message->setText(QString::fromUtf8(e.what()));}
}
void PartTransformPanel::cancelPreview(){if(m_preview){m_preview.reset();emit placementsChanged();}if(m_message)m_message->clear();}
void PartTransformPanel::commit(const PlacementEdits& after,const QString& text){
    cancelPreview();if(sameEdits(m_edits,after))return;
    m_history.push(new PlacementCommand([this](const PlacementEdits& edits){m_preview.reset();m_edits=edits;emit placementsChanged();},m_edits,after,text));
}
void PartTransformPanel::apply(){if(!m_form->isEnabled())return;cancelPreview();preview();if(!m_preview)return;
    const auto next=*m_preview;commit(next,m_mode->currentText());clearInputs();m_message->setText(QStringLiteral("位置已应用；可另存为 STEP。"));}
void PartTransformPanel::resetSelected(){if(!m_form->isEnabled())return;auto next=m_edits;next.erase(m_occurrence);commit(next,QStringLiteral("恢复导入位置"));clearInputs();}
} // namespace Stp
