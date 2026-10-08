#pragma once

#include "cad_document.h"
#include <QObject>
#include <QString>
#include <memory>
#include <unordered_map>

class QThread;

namespace Stp {

// One application-owned queue serializes CAD work across document windows.
// All public methods and callbacks belong to this object's GUI thread. Work
// runs on one persistent worker; cancellation is cooperative once it starts.
class CadJobQueue final : public QObject {
    Q_OBJECT
public:
    using Work = std::function<void(std::atomic_bool&, const ProgressCallback&)>;
    using Completion = std::function<void(const QString&)>;

    explicit CadJobQueue(QObject* parent = nullptr);
    ~CadJobQueue() override;

    std::shared_ptr<std::atomic_bool> enqueue(quint64 owner, Work work,
                                            ProgressCallback progress, Completion done);
    void cancelOwner(quint64 owner);
    bool hasJobs(quint64 owner) const;
    bool idle() const;

signals:
    void ownerDrained(quint64 owner);
    void idleChanged(bool idle);

private:
    struct Job;
    struct State;
    void runWorker();
    void finishJob(const std::shared_ptr<Job>& job, const QString& error);
    void updateIdleSignal();
    void assertGuiThread() const;

    std::shared_ptr<State> m_state;
    QThread* m_worker = nullptr;
    std::unordered_map<quint64, std::shared_ptr<Job>> m_jobs;
    std::unordered_map<quint64, size_t> m_ownerPending;
    quint64 m_nextJob = 1;
    bool m_reportedIdle = true;
};

} // namespace Stp
