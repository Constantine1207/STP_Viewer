#pragma once
#include "part_placement.h"
#include <QWidget>
#include <QUndoStack>
class QLabel;class QComboBox;class QDoubleSpinBox;class QPushButton;
namespace Stp {
class PartTransformPanel final:public QWidget {
    Q_OBJECT
public:
    explicit PartTransformPanel(QWidget* parent=nullptr);
    void setDocument(std::shared_ptr<CadDocument> document);
    void setSelection(int occurrence);
    void setSceneEnabled(bool enabled);
    void cancelPreview();
    bool hasPreview() const{return m_preview.has_value();}
    bool isModified() const{return !m_history.isClean();}
    void markSaved(){m_history.setClean();}
    const PlacementEdits& committedEdits() const{return m_edits;}
    const PlacementEdits& displayEdits() const{return m_preview?*m_preview:m_edits;}
    QUndoStack* history(){return &m_history;}
signals:
    void placementsChanged();
    void modifiedChanged();
private:
    void preview();void apply();void resetSelected();void refresh();void clearInputs();
    gp_Trsf requestedDelta() const;
    void commit(const PlacementEdits&,const QString&);
    std::shared_ptr<CadDocument> m_document;
    PlacementEdits m_edits;
    std::optional<PlacementEdits> m_preview;
    QUndoStack m_history;
    int m_occurrence=-1;bool m_sceneEnabled=false;
    QLabel *m_selection=nullptr,*m_message=nullptr;
    QComboBox *m_mode=nullptr,*m_axis=nullptr,*m_pivot=nullptr;
    QDoubleSpinBox *m_translation[3]{},*m_point[3]{},*m_angle=nullptr;
    QWidget *m_moveFields=nullptr,*m_rotateFields=nullptr,*m_pointFields=nullptr,*m_form=nullptr;
};
} // namespace Stp
