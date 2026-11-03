#pragma once
#include "customer_envelope.h"
#include <QDialog>
class QComboBox;class QCheckBox;class QPushButton;class QTextEdit;class QProgressBar;class QSlider;
namespace Stp {
class CadJobQueue;class ViewerWidget;
class EnvelopeDialog final:public QDialog {
    Q_OBJECT
public:
    EnvelopeDialog(std::shared_ptr<CadDocument>,PlacementEdits,std::vector<int>,CadJobQueue*,QWidget* parent=nullptr);
    const std::shared_ptr<EnvelopeResult>& result()const{return m_result;}
    void reject() override;
protected: void closeEvent(QCloseEvent*) override;
private:
    void generate();void save();void invalidate();void refresh();void showPreview();void finishClose();
    std::shared_ptr<CadDocument> m_source,m_originalPreview,m_resultPreview;
    PlacementEdits m_edits;std::vector<int> m_selection;
    std::shared_ptr<EnvelopeResult> m_result;
    CadJobQueue* m_jobs;quint64 m_owner;
    ViewerWidget* m_view;
    QComboBox *m_scope,*m_mode,*m_axis;
    QCheckBox *m_fill,*m_review;
    QPushButton *m_generate,*m_save,*m_cancel,*m_color;
    QTextEdit* m_report;QProgressBar* m_progress;QSlider* m_offset;
    EnvelopeOptions m_options;
    bool m_busy=false,m_rendering=false,m_closing=false;
};
}
