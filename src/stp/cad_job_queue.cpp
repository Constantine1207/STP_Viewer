#include "cad_job_queue.h"

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <Standard_Failure.hxx>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace Stp {
namespace {
QString cancelledText() { return QStringLiteral("操作已取消"); }
void callbackWarning(const char* kind) {
    qWarning("CadJobQueue: %s callback threw; job bookkeeping has been preserved", kind);
}
}

struct CadJobQueue::Job {
    quint64 id = 0, owner = 0;
    Work work;
    ProgressCallback progress;
    Completion done;
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
    std::atomic_bool completed{false};
};

struct CadJobQueue::State {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::shared_ptr<Job>> queued;
    std::shared_ptr<Job> running;
    bool stopping = false;
};

CadJobQueue::CadJobQueue(QObject* parent) : QObject(parent), m_state(std::make_shared<State>())
{
    m_worker = QThread::create([this] { runWorker(); });
    m_worker->setObjectName(QStringLiteral("serialCadWorker"));
    m_worker->start();
}

CadJobQueue::~CadJobQueue()
{
    // Normal application shutdown first cancels owners and waits asynchronously
    // for idle(). This is only a safety net for exceptional teardown.
    for (const auto& entry : m_jobs) entry.second->cancel->store(true);
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stopping = true;
        if (m_state->running) m_state->running->cancel->store(true);
        m_state->queued.clear();
    }
    m_state->ready.notify_one();
    m_worker->wait();
    delete m_worker;
}

void CadJobQueue::assertGuiThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
}

std::shared_ptr<std::atomic_bool> CadJobQueue::enqueue(quint64 owner, Work work,
                                                      ProgressCallback progress, Completion done)
{
    assertGuiThread();
    auto job = std::make_shared<Job>();
    job->id = m_nextJob++; job->owner = owner;
    job->work = std::move(work); job->progress = std::move(progress); job->done = std::move(done);
    m_jobs.emplace(job->id, job); ++m_ownerPending[owner];
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->queued.push_back(job);
    }
    m_state->ready.notify_one();
    updateIdleSignal();
    return job->cancel;
}

void CadJobQueue::cancelOwner(quint64 owner)
{
    assertGuiThread();
    // Include jobs whose worker result is already queued to the GUI thread.
    for (const auto& entry : m_jobs)
        if (entry.second->owner == owner) entry.second->cancel->store(true);
    std::vector<std::shared_ptr<Job>> removed;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        for (auto it = m_state->queued.begin(); it != m_state->queued.end();) {
            if ((*it)->owner == owner) { removed.push_back(*it); it = m_state->queued.erase(it); }
            else ++it;
        }
    }
    // Removal is immediate; completions are asynchronous so callers can finish
    // marking a window as closing before its busy/drained callbacks run.
    for (const auto& job : removed)
        QMetaObject::invokeMethod(this, [this, job] { finishJob(job, cancelledText()); }, Qt::QueuedConnection);
}

bool CadJobQueue::hasJobs(quint64 owner) const
{
    assertGuiThread();
    const auto found = m_ownerPending.find(owner);
    return found != m_ownerPending.end() && found->second != 0;
}

bool CadJobQueue::idle() const
{
    assertGuiThread();
    return m_jobs.empty();
}

void CadJobQueue::updateIdleSignal()
{
    const bool nowIdle = idle();
    if (nowIdle == m_reportedIdle) return;
    m_reportedIdle = nowIdle;
    emit idleChanged(nowIdle);
}

void CadJobQueue::runWorker()
{
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(m_state->mutex);
            m_state->ready.wait(lock, [this] { return m_state->stopping || !m_state->queued.empty(); });
            if (m_state->stopping) return;
            job = m_state->queued.front(); m_state->queued.pop_front(); m_state->running = job;
        }
        QString error;
        const ProgressCallback progress = [this, job](int percent, const QString& stage) {
            QMetaObject::invokeMethod(this, [job, percent, stage] {
                if (job->completed.load() || job->cancel->load() || !job->progress) return;
                try { job->progress(percent, stage); }
                catch (...) { callbackWarning("progress"); }
            }, Qt::QueuedConnection);
        };
        try {
            if (!job->cancel->load()) {
                if (!job->work) throw std::runtime_error("未提供 CAD 作业");
                job->work(*job->cancel, progress);
            }
        } catch (const Standard_Failure& failure) {
            error = QString::fromUtf8(failure.GetMessageString());
            if (error.isEmpty()) error = QStringLiteral("CAD 操作失败（几何内核未提供错误说明）。");
        } catch (const std::exception& failure) {
            error = QString::fromUtf8(failure.what());
            if (error.isEmpty()) error = QStringLiteral("CAD 操作失败（未提供错误说明）。");
        } catch (...) {
            error = QStringLiteral("发生未识别的 CAD 错误。");
        }
        QMetaObject::invokeMethod(this, [this, job, error] { finishJob(job, error); }, Qt::QueuedConnection);
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->running.reset();
        }
    }
}

void CadJobQueue::finishJob(const std::shared_ptr<Job>& job, const QString& error)
{
    assertGuiThread();
    if (job->completed.exchange(true)) return;
    m_jobs.erase(job->id);
    auto owner = m_ownerPending.find(job->owner);
    if (owner != m_ownerPending.end() && --owner->second == 0) m_ownerPending.erase(owner);
    const QPointer<CadJobQueue> guard(this);
    if (job->done) {
        try { job->done(job->cancel->load() ? cancelledText() : error); }
        catch (...) { callbackWarning("completion"); }
    }
    if (!guard) return;
    // Completion may enqueue a continuation for this owner. Only announce a
    // genuine drain, and keep close handlers out of the completion callback.
    if (!hasJobs(job->owner)) emit ownerDrained(job->owner);
    if (guard) updateIdleSignal();
}

} // namespace Stp
