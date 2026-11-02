#pragma once
#include "section_state.h"
#include <QWidget>
#include <Bnd_Box.hxx>
class QComboBox; class QDoubleSpinBox; class QSlider; class QCheckBox;
class QLabel; class QPushButton;
namespace Stp {
class SectionPanel final : public QWidget {
    Q_OBJECT
public:
    explicit SectionPanel(QWidget* parent=nullptr);
    SectionState state() const;
    void setBounds(const Bnd_Box&);
    void setPickedPlane(const gp_Pln&, const gp_Pnt&, const QString& name);
    void setPicking(bool);
    void reset();
signals:
    void sectionChanged();
    void pickRequested();
    void cancelPickRequested();
private:
    void baseChanged();
    void updateSlider();
    QComboBox *m_base=nullptr,*m_axis=nullptr,*m_centerMode=nullptr;
    QDoubleSpinBox *m_offset=nullptr,*m_angle=nullptr,*m_point[3]{};
    QSlider* m_slider=nullptr;
    QCheckBox *m_reverse=nullptr,*m_cap=nullptr,*m_objectColor=nullptr,*m_show=nullptr;
    QLabel* m_hint=nullptr;
    QPushButton *m_pick=nullptr,*m_cancel=nullptr;
    gp_Pnt m_center;
    gp_Pln m_picked;
    bool m_hasPicked=false,m_sync=false;
    double m_span=100.;
};
}
