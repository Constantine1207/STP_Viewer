#pragma once
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QSet>

class QAction;
class QMenu;
class QMenuBar;
class QStackedWidget;
class QTabWidget;

namespace Stp {
class CadJobQueue;
class MainWindow;

// The shell owns the native menu; each tab owns an entire document UI/session.
class DocumentWorkspace final : public QMainWindow {
    Q_OBJECT
public:
    explicit DocumentWorkspace(QWidget* parent = nullptr);
    ~DocumentWorkspace() override;
    void openFiles(const QStringList& paths);
    void closeCurrent();
    void closeAll();
    void closeDocument(MainWindow* page);
    int documentCount() const;
    MainWindow* currentDocument() const;
    CadJobQueue* jobQueue() const { return m_jobs; }
    QMenuBar* nativeMenuBar() const { return m_nativeMenu; }
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    void chooseFiles();
    void activateDocument(MainWindow*);
    void updateActiveDocument();
    void updateActions();
    void updateLabels();
    void finishQuit();
    CadJobQueue* m_jobs = nullptr;
    QStackedWidget* m_pages = nullptr;
    QTabWidget* m_tabs = nullptr;
    QMenu *m_viewMenu = nullptr, *m_helpMenu = nullptr;
    QMenuBar* m_nativeMenu = nullptr;
    QAction *m_open = nullptr, *m_closeCurrent = nullptr, *m_closeAll = nullptr;
    QAction *m_bom = nullptr, *m_parts = nullptr;
    QHash<QString, QPointer<MainWindow>> m_documents;
    QHash<MainWindow*, QString> m_paths;
    QSet<MainWindow*> m_closing;
    QPointer<MainWindow> m_previous;
    quint64 m_nextOwner = 0;
    bool m_quitting = false, m_quitScheduled = false;
};
} // namespace Stp
