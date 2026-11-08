/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "archiveextractionmanager.h"

#include "archiveextractionworker.h"
#include "archiveextractionjob.h"

#include <KArchive>
#include <KZip>
#include <KTar>
#include <KArchiveDirectory>
#include <QMimeDatabase>
#include <QMimeType>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <KLocalizedString>
#include <QElapsedTimer>

#include "knewstuffcore_debug.h"

using namespace KNSCore;

struct ArchiveExtractionManager::ExtractionEntry
{
    enum class State { Starting, Running, Finishing, Reaping, Done };
    QString payloadFile;
    QString installDir;
    ArchiveExtractionJob::UncompressionOption uncompressionOption = ArchiveExtractionJob::NeverUncompress;
    QPointer<ArchiveExtractionJob> job = nullptr;
    std::shared_ptr<std::atomic<bool>> cancellation;
    int handle = -1;
    std::atomic<State> state{State::Starting};
    QThread *thread = nullptr;
    ArchiveExtractionWorker *worker = nullptr;
};

ArchiveExtractionManager &ArchiveExtractionManager::instance()
{
    // Intentionally leaked singleton for process lifetime.
    // Never destroyed during static teardown; no destructor runs.
    static auto *manager = new ArchiveExtractionManager();
    return *manager;
}

ArchiveExtractionManager::ArchiveExtractionManager(QObject *parent)
    : QObject(parent)
{
    // Manager lives on the main thread (QApplication thread).
    // All entry mutations must happen on this thread.
    moveToThread(QCoreApplication::instance()->thread());
}

int ArchiveExtractionManager::startExtraction(const QString &payloadFile,
                                              const QString &installDir,
                                              ArchiveExtractionJob::UncompressionOption uncompressionOption,
                                              ArchiveExtractionJob *job)
{
    // Must be called from manager's thread (main thread)
    Q_ASSERT(QThread::currentThread() == thread());

    const int handle = m_nextHandle.fetch_add(1, std::memory_order_relaxed);

    auto entry = std::make_shared<ExtractionEntry>();
    entry->payloadFile = payloadFile;
    entry->installDir = installDir;
    entry->uncompressionOption = uncompressionOption;
    entry->job = job;
    entry->handle = handle;
    entry->state.store(ExtractionEntry::State::Starting, std::memory_order_release);

    // Share the job's cancellation flag with the manager entry
    if (job) {
        entry->cancellation = job->cancellationFlag();
    } else {
        entry->cancellation = std::make_shared<std::atomic<bool>>(false);
    }

    // Apply test seams if job has them set
    bool testMode = job && job->testModeNeverRunDoWork();

    if (testMode) {
        // In test mode, run synchronously on manager's thread without a separate thread
        entry->thread = nullptr;
        entry->worker = new ArchiveExtractionWorker(payloadFile, installDir,
            static_cast<ArchiveExtractionWorker::UncompressionOption>(uncompressionOption),
            entry->cancellation);
        entry->worker->moveToThread(thread());

if (job) {
            if (auto b = job->testBarrierAfterStarted()) {
                entry->worker->setBarrierAfterStarted(b);
            }
            if (auto b = job->testBarrierBeforeCopyTo()) {
                entry->worker->setBarrierBeforeCopyTo(b);
            }
            if (job->testWorkerStartedCallback()) {
                entry->worker->setTestStartedCallback(job->testWorkerStartedCallback());
            }
            if (job->testBeforeCopyToCallback()) {
                entry->worker->setTestBeforeCopyToCallback(job->testBeforeCopyToCallback());
            }
        }

        // Store entry
        {
            QMutexLocker locker(&m_entriesMutex);
            if (static_cast<size_t>(handle) >= m_entries.size()) {
                m_entries.resize(handle + 1);
            }
            m_entries[handle] = entry;
        }

        ExtractionEntry *storedEntry = entry.get();
        Q_ASSERT(storedEntry);

        // In test mode, connect finished to handleWorkerFinished first (direct connection on same thread),
        // then schedule reapEntry (queued to run after handleWorkerFinished)
        QObject::connect(storedEntry->worker, &ArchiveExtractionWorker::finished,
                         this, [this, handle](ArchiveExtractionWorker::ResultType result,
                                              const QStringList &files,
                                              const QString &errorMessage) {
            handleWorkerFinished(handle, result, files, errorMessage);
        }, Qt::DirectConnection);

        QObject::connect(storedEntry->worker, &ArchiveExtractionWorker::finished,
                         this, [this, handle]() {
            QMetaObject::invokeMethod(this, [this, handle]() {
                ExtractionEntry *e = findEntry(handle);
                if (e) {
                    reapEntry(e);
                }
            }, Qt::QueuedConnection);
        }, Qt::DirectConnection);

        // Run worker synchronously
        storedEntry->state.store(ExtractionEntry::State::Running, std::memory_order_release);
        // Call start directly - it will emit finished synchronously in test mode
        storedEntry->worker->start();
        return handle;
    }

    // Normal mode: create thread and worker
    QThread *newThread = new QThread();
    entry->thread = newThread;
    entry->worker = new ArchiveExtractionWorker(payloadFile, installDir,
        static_cast<ArchiveExtractionWorker::UncompressionOption>(uncompressionOption),
        entry->cancellation);

if (job) {
            if (auto b = job->testBarrierAfterStarted()) {
                entry->worker->setBarrierAfterStarted(b);
            }
            if (auto b = job->testBarrierBeforeCopyTo()) {
                entry->worker->setBarrierBeforeCopyTo(b);
            }
            if (job->testWorkerStartedCallback()) {
                entry->worker->setTestStartedCallback(job->testWorkerStartedCallback());
            }
            if (job->testBeforeCopyToCallback()) {
                entry->worker->setTestBeforeCopyToCallback(job->testBeforeCopyToCallback());
            }
        }

    entry->worker->moveToThread(newThread);

    // Store entry before starting thread
    {
        QMutexLocker locker(&m_entriesMutex);
        if (static_cast<size_t>(handle) >= m_entries.size()) {
            m_entries.resize(handle + 1);
        }
        m_entries[handle] = entry;
    }

    ExtractionEntry *storedEntry = entry.get();
    Q_ASSERT(storedEntry);

    // Connections:
    // 1. Worker finished -> thread quit (DirectConnection, thread-safe)
    // 2. Worker finished -> manager.handleWorkerFinished (QueuedConnection, crosses to main thread)
    // 3. Thread finished -> worker.deleteLater (DirectConnection, runs during thread shutdown)
    // 4. Thread finished -> manager.reapEntry (QueuedConnection, crosses to main thread)

    QObject::connect(storedEntry->worker, &ArchiveExtractionWorker::finished,
                     newThread, &QThread::quit, Qt::DirectConnection);

    QObject::connect(storedEntry->worker, &ArchiveExtractionWorker::finished,
                     this, [this, handle](ArchiveExtractionWorker::ResultType result,
                                          const QStringList &files,
                                          const QString &errorMessage) {
        // This lambda runs in worker's thread (DirectConnection from worker.finished)
        // Re-post to manager's thread via QueuedConnection
        QMetaObject::invokeMethod(this, [this, handle, result, files, errorMessage]() {
            handleWorkerFinished(handle, result, files, errorMessage);
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);

    // Canonical Qt pattern: worker.deleteLater() from thread's event loop during shutdown
    QObject::connect(newThread, &QThread::finished,
                     storedEntry->worker, &QObject::deleteLater, Qt::DirectConnection);

    QObject::connect(newThread, &QThread::finished,
                     this, [this, handle]() {
        QMetaObject::invokeMethod(this, [this, handle]() {
            ExtractionEntry *entry = findEntry(handle);
            if (entry) {
                reapEntry(entry);
            }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);

    storedEntry->state.store(ExtractionEntry::State::Running, std::memory_order_release);
    newThread->start();
    QMetaObject::invokeMethod(storedEntry->worker, "start", Qt::QueuedConnection);

    return handle;
}

void ArchiveExtractionManager::requestCancellation(int handle)
{
    // Can be called from any thread (e.g., job destructor, doKill())
    // Step 1: Set the cancellation flag immediately (thread-safe atomic)
    std::shared_ptr<ExtractionEntry> entry;
    {
        QMutexLocker locker(&m_entriesMutex);
        if (handle >= 0 && handle < static_cast<int>(m_entries.size())) {
            entry = m_entries[handle];
        }
    }

    if (!entry) {
        return; // Entry already reaped
    }

    entry->cancellation->store(true, std::memory_order_release);

    // Step 2: Marshal thread-specific operation to manager thread via queued invocation.
    // We capture only the handle (not the raw QThread pointer) to avoid cross-thread
    // access to the thread object which could be concurrently deleted by the reaper.
    QMetaObject::invokeMethod(this, [this, handle]() {
        // This runs on the manager's thread (main thread)
        Q_ASSERT(QThread::currentThread() == thread());

        std::shared_ptr<ExtractionEntry> e;
        {
            QMutexLocker locker(&m_entriesMutex);
            if (handle >= 0 && handle < static_cast<int>(m_entries.size())) {
                e = m_entries[handle];
            }
        }

        if (!e) {
            return; // Entry already reaped - nothing to do
        }

        // Now safely access the thread pointer on the manager thread
        QThread *workerThread = e->thread;
        if (workerThread && workerThread->isRunning()) {
            workerThread->requestInterruption();
        }
    }, Qt::QueuedConnection);
}

void ArchiveExtractionManager::handleWorkerFinished(int handle,
                                                    ArchiveExtractionWorker::ResultType result,
                                                    const QStringList &files,
                                                    const QString &errorMessage)
{
    // Must be called from manager's thread (main thread)
    Q_ASSERT(QThread::currentThread() == thread());

    ExtractionEntry *entry = findEntry(handle);
    if (!entry) {
        return; // Already reaped
    }

    // State should be Running or Finishing
    auto currentState = entry->state.load(std::memory_order_acquire);
    if (currentState == ExtractionEntry::State::Done || currentState == ExtractionEntry::State::Reaping) {
        return; // Already processed
    }

    entry->state.store(ExtractionEntry::State::Finishing, std::memory_order_release);

    // The worker's emitted result is authoritative. Do NOT override Success/FallbackRequired
    // with Cancelled based on a late-arriving cancellation flag. The worker checks
    // interruption at well-defined checkpoints; if it emitted Success/FallbackRequired,
    // it passed its final checkpoint uncancelled.
    // Cancellation arriving after the worker committed its result must not retroactively
    // change the outcome.

    // Notify job if still alive
    qCDebug(KNEWSTUFFCORE) << "handleWorkerFinished for handle" << handle << "result" << result << "job valid:" << (bool)entry->job;
    if (entry->job) {
        switch (result) {
            case ArchiveExtractionWorker::Success:
                qCDebug(KNEWSTUFFCORE) << "Calling handleSuccess for handle" << handle;
                entry->job->handleSuccess(files);
                break;
            case ArchiveExtractionWorker::FallbackRequired:
                qCDebug(KNEWSTUFFCORE) << "Calling handleFallbackRequired for handle" << handle;
                entry->job->handleFallbackRequired();
                break;
            case ArchiveExtractionWorker::ExtractionFailed:
                qCDebug(KNEWSTUFFCORE) << "Calling handleExtractionFailed for handle" << handle;
                entry->job->handleExtractionFailed(errorMessage);
                break;
            case ArchiveExtractionWorker::Cancelled:
                qCDebug(KNEWSTUFFCORE) << "Calling handleCancelled for handle" << handle;
                entry->job->handleCancelled();
                break;
        }
    } else {
        qCDebug(KNEWSTUFFCORE) << "Job pointer is null for handle" << handle;
    }

    // Thread will quit via worker.finished -> thread.quit (DirectConnection)
    // Thread.finished will trigger worker.deleteLater and reapEntry via queued connection
    // We don't delete worker or thread here - that happens during thread shutdown
}

void ArchiveExtractionManager::reapEntry(ExtractionEntry *entry)
{
    // Must be called from manager's thread (main thread)
    Q_ASSERT(QThread::currentThread() == thread());

    if (!entry) {
        return;
    }

    auto currentState = entry->state.load(std::memory_order_acquire);
    if (currentState == ExtractionEntry::State::Done) {
        return; // Already reaped
    }
    if (currentState == ExtractionEntry::State::Reaping) {
        return; // Reaping in progress, avoid reentrancy
    }

    // Thread should have already finished (worker.finished -> thread.quit -> thread.finished).
    // The worker has been deleted via thread.finished -> worker.deleteLater (DirectConnection).
    // We need to synchronize with thread completion using wait() before deleting the QThread.
    if (entry->thread) {
        // Thread should already be finished, but verify using wait() to ensure
        // all thread-local cleanup has completed. Qt documents that finished() can
        // be emitted before all thread-local cleanup is done.
        QThread *workerThread = entry->thread;
        if (workerThread->isRunning()) {
            // Should not happen if worker.finished -> thread.quit worked, but be safe
            qCWarning(KNEWSTUFFCORE) << "Thread still running during reapEntry; requesting quit.";
            workerThread->quit();
        }

        // Wait for the thread to finish completely. Use a short timeout and retry
        // via event loop to avoid blocking the manager/GUI thread.
        constexpr int waitTimeoutMs = 50;
        if (!workerThread->wait(waitTimeoutMs)) {
            qCWarning(KNEWSTUFFCORE) << "Thread did not finish within" << waitTimeoutMs << "ms; deferring reap.";
            // Do not delete thread or mark entry Done. Preserve entry for eventual reaping.
            // Re-post to try again on next event loop iteration.
            QMetaObject::invokeMethod(this, [this, handle = entry->handle]() {
                ExtractionEntry *e = findEntry(handle);
                if (e) {
                    reapEntry(e);
                }
            }, Qt::QueuedConnection);
            return;
        }

        // Delete the QThread object (now safe, thread is fully stopped)
        delete workerThread;
        entry->thread = nullptr;
    } else {
        // Test mode: no QThread was created. Worker runs on manager's thread.
        // Worker must be deleted here since there's no thread.finished -> worker.deleteLater.
        if (entry->worker) {
            delete entry->worker;
            entry->worker = nullptr;
        }
    }

    // Worker was deleted via thread.finished -> worker.deleteLater (normal mode)
    // or deleted above (test mode)
    entry->worker = nullptr;

    entry->state.store(ExtractionEntry::State::Reaping, std::memory_order_release);
    entry->state.store(ExtractionEntry::State::Done, std::memory_order_release);

    // Remove entry from vector
    {
        QMutexLocker locker(&m_entriesMutex);
        if (entry->handle >= 0 && entry->handle < static_cast<int>(m_entries.size())) {
            m_entries[entry->handle].reset();
        }
    }
}

bool ArchiveExtractionManager::waitForAllFinished(int timeoutMs)
{
    Q_ASSERT(QThread::currentThread() == thread());

    QElapsedTimer timer;
    timer.start();

    while (true) {
        int active = 0;
        {
            QMutexLocker locker(&m_entriesMutex);
            for (const auto &entry : m_entries) {
                if (entry && entry->state.load(std::memory_order_acquire) != ExtractionEntry::State::Done) {
                    active++;
                }
            }
        }

        if (active == 0) {
            return true;
        }

        if (timer.elapsed() >= timeoutMs) {
            return false;
        }

        // Process events to allow queued reapEntry calls to execute
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

int ArchiveExtractionManager::activeExtractionsCount() const
{
    QMutexLocker locker(&m_entriesMutex);
    int count = 0;
    for (const auto &entry : m_entries) {
        if (entry && entry->state.load(std::memory_order_acquire) != ExtractionEntry::State::Done) {
            count++;
        }
    }
    return count;
}

ArchiveExtractionManager::ExtractionEntry* ArchiveExtractionManager::findEntry(int handle)
{
    QMutexLocker locker(&m_entriesMutex);
    if (handle >= 0 && handle < static_cast<int>(m_entries.size())) {
        return m_entries[handle].get();
    }
    return nullptr;
}

// Test-only seams
void ArchiveExtractionManager::setTestBarrierForHandle(int handle, std::shared_ptr<QSemaphore> barrier)
{
    // Must be called from manager's thread (main thread)
    Q_ASSERT(QThread::currentThread() == thread());

    ExtractionEntry *entry = findEntry(handle);
    if (entry && entry->worker) {
        entry->worker->setBarrierAfterStarted(barrier);
    }
}

void ArchiveExtractionManager::setTestModeNeverRunDoWorkForHandle(int handle, bool enabled)
{
    // Must be called from manager's thread (main thread)
    Q_ASSERT(QThread::currentThread() == thread());

    ExtractionEntry *entry = findEntry(handle);
    if (entry && entry->worker) {
        entry->worker->setTestModeNeverRunDoWork(enabled);
    }
}