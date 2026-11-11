#pragma once
#include "customer_envelope.h"
#include <QDialog>
class QListWidget;class QLineEdit;class QLabel;class QTabWidget;
class QComboBox;class QCheckBox;class QPushButton;class QTextEdit;class QProgressBar;class QSlider;
namespace Stp {
class CadJobQueue;class ViewerWidget;
class EnvelopeDialog final:public QDialog {
    Q_OBJECT
public:
    EnvelopeDialog(std::shared_ptr<CadDocument>,PlacementEdits,std::vector<int>,CadJobQueue*,QWidget* parent=nullptr,EnvelopeOptions options={});
    const std::shared_ptr<EnvelopeResult>& result()const{return m_result;}
    void reject() override;
    const EnvelopeOptions& options()const{return m_options;}
protected: void closeEvent(QCloseEvent*) override;
private:
    void buildRules(QTabWidget*);void updateRules();void addProtection(bool cylinder);void importProtection();
    void previewRules();void locateRule();void updateProtectionList();
    QListWidget *m_preserveList=nullptr,*m_protectionList=nullptr,*m_inputIssues=nullptr;
    QTabWidget* m_tabs=nullptr;
    EnvelopeInspection m_inspection;
    QLineEdit* m_keywords=nullptr;QLabel* m_ruleCount=nullptr;
    QWidget* m_rulesPage=nullptr;QWidget* m_protectionPage=nullptr;
    gp_Trsf m_protectionBasis;bool m_hasProtectionBasis=false;
    void generate();void run(bool inspectOnly);void updateInspection();void locateIssue();void saveInspection();void save();void invalidate();void refresh();void showPreview();void finishClose();
    std::shared_ptr<CadDocument> m_source,m_originalPreview,m_resultPreview,m_resultPreviewTools;
    PlacementEdits m_edits;std::vector<int> m_selection;
    std::shared_ptr<EnvelopeResult> m_result;
    CadJobQueue* m_jobs;quint64 m_owner;
    ViewerWidget* m_view;
    QComboBox *m_scope,*m_mode,*m_axis;
    QCheckBox *m_fill,*m_review,*m_reverse,*m_repair;
    QPushButton *m_generate,*m_save,*m_cancel,*m_color,*m_inspect,*m_saveInspection;
    QTextEdit* m_report;QProgressBar* m_progress;QSlider* m_offset;
    EnvelopeOptions m_options;
    bool m_busy=false,m_rendering=false,m_closing=false,m_cancelRequested=false;
};
}
