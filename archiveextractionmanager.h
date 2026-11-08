/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef ARCHIVEEXTRACTIONMANAGER_H
#define ARCHIVEEXTRACTIONMANAGER_H

#include <QObject>
#include <QThread>
#include <QPointer>
#include <QMutex>
#include <QSemaphore>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>

#include "archiveextractionworker.h"
#include "archiveextractionjob.h"

namespace KNSCore
{
class ArchiveExtractionManager : public QObject
{
    Q_OBJECT
public:
    static ArchiveExtractionManager &instance();

    // Non-copyable, non-movable
    ArchiveExtractionManager(const ArchiveExtractionManager&) = delete;
    ArchiveExtractionManager& operator=(const ArchiveExtractionManager&) = delete;
    ArchiveExtractionManager(ArchiveExtractionManager&&) = delete;
    ArchiveExtractionManager& operator=(ArchiveExtractionManager&&) = delete;

    // Start an extraction; returns a handle (index) for cancellation/querying
    int startExtraction(const QString &payloadFile,
                        const QString &installDir,
                        ArchiveExtractionJob::UncompressionOption uncompressionOption,
                        ArchiveExtractionJob *job);

    // Request cooperative cancellation for a specific extraction
    void requestCancellation(int handle);

    // Called by worker when it finishes (queued from worker thread)
    void handleWorkerFinished(int handle,
                              ArchiveExtractionWorker::ResultType result,
                              const QStringList &files,
                              const QString &errorMessage);

    // Test-only: wait for all active extractions to complete (bounded)
    bool waitForAllFinished(int timeoutMs = 5000);

    // Test-only: get count of active extractions
    int activeExtractionsCount() const;

    // Test-only seams
    void setTestBarrierForHandle(int handle, std::shared_ptr<QSemaphore> barrier);
    void setTestModeNeverRunDoWorkForHandle(int handle, bool enabled);

private:
    struct ExtractionEntry; // Forward declaration - definition in .cpp to avoid moc issues

    std::atomic<int> m_nextHandle{0};
    mutable QMutex m_entriesMutex;
    std::vector<std::shared_ptr<ExtractionEntry>> m_entries;

    ArchiveExtractionManager(QObject *parent = nullptr);

    ExtractionEntry* findEntry(int handle);
    void reapEntry(ExtractionEntry *entry);
};

} // namespace KNSCore

#endif // ARCHIVEEXTRACTIONMANAGER_H