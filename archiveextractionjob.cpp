/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "archiveextractionjob.h"

#include "archiveextractionworker.h"

#include "knewstuffcore_debug.h"

#include <QThread>
#include <QVariant>
#include <QFile>
#include <KLocalizedString>

using namespace KNSCore;

class KNSCore::ArchiveExtractionJobPrivate
{
public:
    QString payloadFile;
    QString installDir;
    ArchiveExtractionJob::UncompressionOption uncompressionOption = ArchiveExtractionJob::NeverUncompress;
    // QPointer is used to observe worker/thread lifetime; becomes null when object is deleted.
    // The thread self-deletes via finished() → deleteLater().
    QPointer<ArchiveExtractionWorker> worker;
    QPointer<QThread> thread;
    QStringList installedFiles;
    bool finished = false;
    bool killRequested = false;
    bool startCalled = false;
    bool threadWaited = false;
};

ArchiveExtractionJob::ArchiveExtractionJob(const QString &payloadFile, const QString &installDir, UncompressionOption uncompressionOption, QObject *parent)
    : KJob(parent)
    , d(new ArchiveExtractionJobPrivate)
{
    d->payloadFile = payloadFile;
    d->installDir = installDir;
    d->uncompressionOption = uncompressionOption;
}

ArchiveExtractionJob::~ArchiveExtractionJob()
{
    // If the worker thread is running, request interruption and wait for it to finish.
    // We must wait to avoid destroying the job (and its private data) while the worker
    // might still call back into handleWorkerFinished via the queued connection.
    // The thread self-deletes via finished() → deleteLater() when its event loop exits.
    // Use QPointer to safely observe thread lifetime.
    if (d->thread && d->thread->isRunning() && !d->threadWaited) {
        d->killRequested = true;
        d->thread->requestInterruption();
        d->thread->quit();
        // Wait up to 3 seconds for the thread to finish. This is a bounded timeout
        // suitable for a GUI-facing destructor. If the thread doesn't finish in time
        // (e.g., stuck in uninterruptible copyTo()), we log a warning and return.
        // The thread will self-delete via finished() → deleteLater() when it eventually exits.
        if (!d->thread->wait(3000)) {
            qCWarning(KNEWSTUFFCORE) << "Archive extraction thread did not finish in time; it will self-delete when finished.";
        }
        d->threadWaited = true;
    }
}

void ArchiveExtractionJob::start()
{
    if (d->startCalled) {
        return;
    }
    d->startCalled = true;

    // If kill was requested before start, finish immediately with KilledJobError
    // and clean up the payload since no worker will be created to handle it.
    if (d->killRequested) {
        QFile::remove(d->payloadFile);
        // If kill(KJob::EmitResult) was called before start(), KJob already emitted
        // the result (KilledJobError). Avoid duplicate emitResult().
        // However, the error text may not be set yet, so set it if missing.
        if (!error()) {
            setError(KJob::KilledJobError);
            setErrorText(i18n("Archive extraction was cancelled"));
            emitResult();
        } else if (error() == KJob::KilledJobError && errorText().isEmpty()) {
            setErrorText(i18n("Archive extraction was cancelled"));
        }
        return;
    }

    // Convert Job enum to Worker enum (they have the same values)
    ArchiveExtractionWorker::UncompressionOption workerOption;
    switch (d->uncompressionOption) {
        case AlwaysUncompress:
            workerOption = ArchiveExtractionWorker::AlwaysUncompress;
            break;
        case UncompressIfArchive:
            workerOption = ArchiveExtractionWorker::UncompressIfArchive;
            break;
        case UncompressIntoSubdirIfArchive:
            workerOption = ArchiveExtractionWorker::UncompressIntoSubdirIfArchive;
            break;
        case UncompressIntoSubdir:
            workerOption = ArchiveExtractionWorker::UncompressIntoSubdir;
            break;
        default:
            workerOption = ArchiveExtractionWorker::NeverUncompress;
            break;
    }

    // Job creates the thread; worker will be moved to it
    d->thread = new QThread();
    d->worker = new ArchiveExtractionWorker(d->payloadFile, d->installDir, workerOption);
    d->worker->moveToThread(d->thread);

    // Worker lifetime management:
    // - When worker emits finished(), it schedules its own deletion via deleteLater()
    //   (runs in worker thread's event loop)
    // - The thread quits when worker finishes (via QueuedConnection to quit())
    // - The thread self-deletes when its event loop finishes (via finished() → deleteLater())
    // Using QueuedConnection for quit() ensures the quit event is processed in the
    // worker thread's event loop after the current slot returns, allowing deleteLater
    // to be processed first.
    // Note: QThread object lives in the creator thread; the queued connection to quit()
    // posts a QMetaCallEvent to the creator thread's event loop. If the creator thread
    // is blocked in wait(), the quit() won't be processed until wait() returns.
    // This is acceptable because wait() returns when the native thread finishes.
    connect(d->worker, &ArchiveExtractionWorker::finished, d->thread, &QThread::quit, Qt::QueuedConnection);
    connect(d->worker, &ArchiveExtractionWorker::finished, d->worker, &QObject::deleteLater);
    connect(d->thread, &QThread::finished, d->thread, &QObject::deleteLater);
    // Handle worker result - use QueuedConnection to ensure this runs in the job's thread
    connect(d->worker, &ArchiveExtractionWorker::finished, this, &ArchiveExtractionJob::handleWorkerFinished, Qt::QueuedConnection);
    // Start thread and work
    d->thread->start();
    QMetaObject::invokeMethod(d->worker, "doWork", Qt::QueuedConnection);
}

QStringList ArchiveExtractionJob::installedFiles() const
{
    return d->installedFiles;
}

bool ArchiveExtractionJob::doKill()
{
    d->killRequested = true;

    // If already finished, nothing to do
    if (d->finished) {
        return true;
    }

    // If worker hasn't started yet or is already done, we can finish immediately
    // QPointer automatically handles the case where thread/worker was deleted
    if (!d->worker || !d->thread || !d->thread->isRunning()) {
        return true;
    }

    // Worker is running - request interruption on the worker's thread
    // The worker will check for interruption at safe points
    d->thread->requestInterruption();

    // We cannot interrupt copyTo(), so we must wait for it to finish
    // Return false to indicate the job is not yet finished
    // The worker will complete and the job will finish when the worker emits finished
    return false;
}

void ArchiveExtractionJob::handleWorkerFinished(ArchiveExtractionWorker::ResultType result, const QStringList &files, const QString &errorMessage)
{
    // Guard against duplicate completion and use-after-free:
    // - d->finished is set true here before any emitResult()
    // - isFinished() check handles the case where kill(EmitResult) already completed the KJob
    // - The job destructor waits for the thread before destroying private data
    if (d->finished || isFinished()) {
        return; // Prevent duplicate completion
    }
    d->finished = true;

    // If kill was requested, override success/fallback with cancellation
    if (d->killRequested && (result == ArchiveExtractionWorker::Success || result == ArchiveExtractionWorker::FallbackRequired)) {
        result = ArchiveExtractionWorker::Cancelled;
    }

    switch (result) {
        case ArchiveExtractionWorker::Success:
            d->installedFiles = files;
            emitResult();
            break;
        case ArchiveExtractionWorker::FallbackRequired:
            // Mark that fallback is needed, but complete successfully
            // Installation will check the fallbackRequired property
            setProperty("fallbackRequired", QVariant(true));
            emitResult();
            break;
        case ArchiveExtractionWorker::ExtractionFailed:
            setError(KJob::UserDefinedError);
            setErrorText(errorMessage);
            emitResult();
            break;
        case ArchiveExtractionWorker::Cancelled:
            setError(KJob::KilledJobError);
            setErrorText(i18n("Archive extraction was cancelled"));
            emitResult();
            break;
    }
}

#include "moc_archiveextractionjob.cpp"