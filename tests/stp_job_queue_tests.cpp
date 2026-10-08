#include "cad_job_queue.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void until(Predicate ready, const char* message)
{
    QElapsedTimer deadline; deadline.start();
    while (!ready() && deadline.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(ready(), message);
}

void testFifoAndOwnerCounts()
{
    QSemaphore release;
    std::atomic_bool firstStarted{false};
    std::atomic_int active{0}, peak{0};
    std::mutex orderMutex;
    std::vector<int> order;
    std::vector<QThread*> workerThreads;
    int completions = 0, owner10Drains = 0, owner20Drains = 0;
    bool callbackState = true, progressState = true;
    std::vector<bool> idleSignals;
    Stp::CadJobQueue queue;
    QObject::connect(&queue, &Stp::CadJobQueue::ownerDrained, &queue, [&](quint64 owner) {
        callbackState &= !queue.hasJobs(owner);
        if (owner == 10) ++owner10Drains;
        if (owner == 20) ++owner20Drains;
    });
    QObject::connect(&queue, &Stp::CadJobQueue::idleChanged, &queue, [&](bool idle) { idleSignals.push_back(idle); });
    const auto work = [&](int index) {
        return [&, index](std::atomic_bool& cancel, const Stp::ProgressCallback& progress) {
            const int concurrent = ++active;
            peak.store(std::max(peak.load(), concurrent));
            {
                std::lock_guard<std::mutex> lock(orderMutex);
                order.push_back(index); workerThreads.push_back(QThread::currentThread());
            }
            if (index == 1) {
                firstStarted.store(true);
                while (!cancel.load() && !release.tryAcquire(1, 5)) {}
            }
            progress(index * 10, QString::number(index));
            --active;
        };
    };
    const auto progress = [&](int value, const QString& stage) {
        progressState &= QThread::currentThread() == QCoreApplication::instance()->thread()
            && stage.toInt() * 10 == value;
    };
    queue.enqueue(10, work(1), progress, [&](const QString& error) {
        callbackState &= error.isEmpty() && queue.hasJobs(10); ++completions;
    });
    queue.enqueue(10, work(2), progress, [&](const QString& error) {
        callbackState &= error.isEmpty() && !queue.hasJobs(10); ++completions;
    });
    queue.enqueue(20, work(3), progress, [&](const QString& error) {
        callbackState &= error.isEmpty() && !queue.hasJobs(20); ++completions;
    });
    until([&] { return firstStarted.load(); }, "first worker did not start");
    require(!queue.idle() && queue.hasJobs(10) && queue.hasJobs(20), "pending owner accounting lost queued jobs");
    release.release();
    until([&] { return queue.idle(); }, "FIFO queue did not drain");
    require(order == std::vector<int>({1, 2, 3}) && peak.load() == 1, "CAD jobs did not execute serially in FIFO order");
    require(workerThreads.size() == 3 && workerThreads[0] == workerThreads[1] && workerThreads[1] == workerThreads[2]
            && workerThreads[0] != QCoreApplication::instance()->thread(), "jobs did not use one persistent background thread");
    require(completions == 3 && owner10Drains == 1 && owner20Drains == 1 && callbackState && progressState,
            "owner completion counts, GUI callbacks or progress are inconsistent");
    require(idleSignals == std::vector<bool>({false, true}), "idleChanged did not report queue transitions once");
}

void testQueuedAndRunningCancellation()
{
    std::atomic_bool started{false}, queuedRan{false}, survivorRan{false};
    bool queuedDone = false, runningDone = false, survivorDone = false, callbackState = true, lateProgress = false;
    Stp::CadJobQueue queue;
    const auto runningFlag = queue.enqueue(1, [&](std::atomic_bool& cancel, const Stp::ProgressCallback& progress) {
        started.store(true);
        while (!cancel.load()) QThread::msleep(1);
        progress(90, QStringLiteral("late"));
    }, [&](int, const QString&) { lateProgress = true; }, [&](const QString& error) {
        callbackState &= error == QStringLiteral("操作已取消") && !queue.hasJobs(1); runningDone = true;
    });
    const auto queuedFlag = queue.enqueue(2, [&](std::atomic_bool&, const Stp::ProgressCallback&) { queuedRan.store(true); }, {},
        [&](const QString& error) {
            callbackState &= error == QStringLiteral("操作已取消") && !queue.hasJobs(2) && queue.hasJobs(1); queuedDone = true;
        });
    queue.enqueue(3, [&](std::atomic_bool&, const Stp::ProgressCallback&) { survivorRan.store(true); }, {},
        [&](const QString& error) { callbackState &= error.isEmpty() && !queue.hasJobs(3); survivorDone = true; });
    until([&] { return started.load(); }, "cancellation fixture worker did not start");
    queue.cancelOwner(2);
    until([&] { return queuedDone; }, "queued cancellation waited for a different owner's running job");
    require(queuedFlag->load() && !queuedRan.load() && !runningDone && !queue.idle(), "queued cancel executed work or cancelled an unrelated owner");
    queue.cancelOwner(1);
    until([&] { return queue.idle(); }, "running cancellation did not drain");
    require(runningFlag->load() && runningDone && survivorDone && survivorRan.load() && callbackState && !lateProgress,
            "running cancel result, surviving owner or late-progress filtering failed");
}

void testExceptionsAndContinuation()
{
    std::vector<QString> errors;
    int completions = 0, continuationDrains = 0;
    bool state = true;
    Stp::CadJobQueue queue;
    const auto completion = [&](const QString& error) {
        errors.push_back(error); ++completions;
        state &= QThread::currentThread() == QCoreApplication::instance()->thread();
    };
    queue.enqueue(40, [](std::atomic_bool&, const Stp::ProgressCallback&) { throw std::runtime_error("ordinary failure"); }, {}, completion);
    queue.enqueue(41, [](std::atomic_bool&, const Stp::ProgressCallback&) { throw Standard_Failure("kernel failure"); }, {}, completion);
    queue.enqueue(42, [](std::atomic_bool&, const Stp::ProgressCallback&) { throw 7; }, {}, completion);
    queue.enqueue(43, [](std::atomic_bool&, const Stp::ProgressCallback&) {}, {}, [&](const QString& error) {
        completion(error); state &= !queue.hasJobs(43);
        throw std::runtime_error("consumer failure");
    });
    QObject::connect(&queue, &Stp::CadJobQueue::ownerDrained, &queue, [&](quint64 owner) {
        if (owner == 50) ++continuationDrains;
    });
    queue.enqueue(50, [](std::atomic_bool&, const Stp::ProgressCallback&) {}, {}, [&](const QString& error) {
        completion(error); state &= !queue.hasJobs(50);
        queue.enqueue(50, [](std::atomic_bool&, const Stp::ProgressCallback&) {}, {}, [&](const QString& nextError) {
            completion(nextError); state &= !queue.hasJobs(50);
        });
    });
    until([&] { return queue.idle(); }, "exception or continuation stopped queue processing");
    require(completions == 6 && errors.size() == 6 && errors[0] == QStringLiteral("ordinary failure")
            && errors[1] == QStringLiteral("kernel failure") && !errors[2].isEmpty()
            && errors[3].isEmpty() && errors[4].isEmpty() && errors[5].isEmpty() && state && continuationDrains == 1,
            "exception reporting, callback isolation or reentrant owner drain failed");
}

void testEmptyExceptionMessages()
{
    std::vector<QString> errors;
    bool ownersDrained = true;
    Stp::CadJobQueue queue;
    const auto completion = [&](quint64 owner) {
        return [&, owner](const QString& error) {
            errors.push_back(error);
            ownersDrained &= !queue.hasJobs(owner);
        };
    };
    queue.enqueue(90, [](std::atomic_bool&, const Stp::ProgressCallback&) {
        throw Standard_Failure();
    }, {}, completion(90));
    queue.enqueue(91, [](std::atomic_bool&, const Stp::ProgressCallback&) {
        throw std::runtime_error("");
    }, {}, completion(91));
    until([&] { return queue.idle(); }, "empty-message exceptions did not drain");
    // Empty error denotes success to import/export consumers. Even kernels that
    // omit a diagnostic must deliver failure instead of an unset import result.
    require(errors.size() == 2 && !errors[0].isEmpty() && !errors[1].isEmpty() && ownersDrained,
            "an exception without a message was reported as a successful CAD job");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        testFifoAndOwnerCounts();
        testQueuedAndRunningCancellation();
        testExceptionsAndContinuation();
        testEmptyExceptionMessages();
        std::cout << "QUEUE PASS: persistent FIFO worker, GUI progress/completion, owner counts/drain, immediate queued cancel, cooperative running cancel, late-progress filtering, exceptions and continuation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
