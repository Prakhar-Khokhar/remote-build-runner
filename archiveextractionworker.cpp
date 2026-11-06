/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "archiveextractionworker.h"

#include "filesystemmutex.h"

#include <KArchive>
#include <KZip>
#include <KTar>
#include <QMimeDatabase>
#include <QMimeType>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>

#include <KLocalizedString>

#include "knewstuffcore_debug.h"

using namespace KNSCore;

class KNSCore::ArchiveExtractionWorker::Private
{
public:
    QString payloadFile;
    QString installDir;
    UncompressionOption uncompressionOption = NeverUncompress;
};

ArchiveExtractionWorker::ArchiveExtractionWorker(const QString &payloadFile, const QString &installDir, UncompressionOption uncompressionOption)
    : QObject(nullptr)
    , d(new Private)
{
    d->payloadFile = payloadFile;
    d->installDir = installDir;
    d->uncompressionOption = uncompressionOption;
}

ArchiveExtractionWorker::~ArchiveExtractionWorker() = default;

void ArchiveExtractionWorker::start()
{
    QMetaObject::invokeMethod(this, "doWork", Qt::QueuedConnection);
}

void ArchiveExtractionWorker::doWork()
{
    QStringList installedFiles;
    ResultType result = Success;
    QString errorMessage;
    bool isArchive = true;

// Check for cancellation before starting
    if (QThread::currentThread()->isInterruptionRequested()) {
        QFile::remove(d->payloadFile); // Cancelled: no fallback, clean up payload.
        Q_EMIT finished(Cancelled, QStringList(), QString());
        return;
    }

    ArchiveExtractionWorker::UncompressionOption uncompressionOpt = d->uncompressionOption;

    // Respect the uncompress flag
    if (uncompressionOpt == AlwaysUncompress || uncompressionOpt == UncompressIntoSubdirIfArchive
        || uncompressionOpt == UncompressIfArchive || uncompressionOpt == UncompressIntoSubdir) {

        // Check MIME type to determine if it's an archive
        QMimeDatabase db;
        QMimeType mimeType = db.mimeTypeForFile(d->payloadFile);

        QScopedPointer<KArchive> archive;

        if (mimeType.inherits(QStringLiteral("application/zip"))) {
            archive.reset(new KZip(d->payloadFile));
        } else if (mimeType.inherits(QStringLiteral("application/tar"))
                || mimeType.inherits(QStringLiteral("application/x-tar"))
                || mimeType.inherits(QStringLiteral("application/x-gzip"))
                || mimeType.inherits(QStringLiteral("application/x-bzip"))
                || mimeType.inherits(QStringLiteral("application/x-lzma"))
                || mimeType.inherits(QStringLiteral("application/x-xz"))
                || mimeType.inherits(QStringLiteral("application/x-bzip-compressed-tar"))
                || mimeType.inherits(QStringLiteral("application/x-compressed-tar"))) {
            archive.reset(new KTar(d->payloadFile));
        } else {
            // Unknown MIME type
            if (uncompressionOpt == AlwaysUncompress) {
                errorMessage = i18n("Could not determine the type of archive of the downloaded file %1", d->payloadFile);
                result = ExtractionFailed;
                QFile::remove(d->payloadFile); // No consumer for verified paths; clean up.
                Q_EMIT finished(result, QStringList(), errorMessage);
                return;
            }
            // For UncompressIfArchive, UncompressIntoSubdirIfArchive, UncompressIntoSubdir:
            // treat as non-archive (will emit FallbackRequired or Success with empty list)
            isArchive = false;
        }

        if (isArchive) {
            // Check for cancellation before opening archive
            if (QThread::currentThread()->isInterruptionRequested()) {
                QFile::remove(d->payloadFile); // Cancelled: no fallback, clean up payload.
                Q_EMIT finished(Cancelled, QStringList(), QString());
                return;
            }

            bool success = archive->open(QIODevice::ReadOnly);
            if (!success) {
                // Archive open failure
                if (uncompressionOpt == AlwaysUncompress) {
                    errorMessage = i18n("Failed to open the archive file %1. The reported error was: %2", d->payloadFile, archive->errorString());
                    result = ExtractionFailed;
                    QFile::remove(d->payloadFile); // No consumer for verified paths; clean up.
                    Q_EMIT finished(result, QStringList(), errorMessage);
                    return;
                }
                // For UncompressIfArchive and UncompressIntoSubdirIfArchive: fall back to non-archive handling
                if (uncompressionOpt == UncompressIfArchive || uncompressionOpt == UncompressIntoSubdirIfArchive) {
                    result = FallbackRequired;
                    Q_EMIT finished(result, QStringList(), QString());
                    return;
                }
                // UncompressIntoSubdir: archive open failure is an extraction failure
                errorMessage = i18n("Failed to open the archive file %1. The reported error was: %2", d->payloadFile, archive->errorString());
                result = ExtractionFailed;
                QFile::remove(d->payloadFile); // No consumer for verified paths; clean up.
                Q_EMIT finished(result, QStringList(), errorMessage);
                return;
            }
        }

        if (isArchive) {
            const KArchiveDirectory *dir = archive->directory();
            if (!dir) {
                // Defensive: archive opened but has no root directory
                errorMessage = i18n("Archive %1 has no valid root directory", d->payloadFile);
                result = ExtractionFailed;
                QFile::remove(d->payloadFile);
                archive->close();
                Q_EMIT finished(result, QStringList(), errorMessage);
                return;
            }
            // If there is more than an item in the file, and we are requested to do so
            // put contents in a subdirectory with the same name as the file
            QString installPath;
            const bool isSubdir =
                (uncompressionOpt == UncompressIntoSubdir || uncompressionOpt == UncompressIntoSubdirIfArchive) && dir->entries().count() > 1;
            if (isSubdir) {
                installPath = d->installDir + QLatin1Char('/') + QFileInfo(d->payloadFile).baseName();
            } else {
                installPath = d->installDir;
            }

            // Protect destination with per-destination mutex
            QMutex* destMutex = KNSCore::getDestinationMutex(installPath);
            QMutexLocker locker(destMutex);

            if (QThread::currentThread()->isInterruptionRequested()) {
                archive->close(); // Archive was opened, close it before removing payload
                QFile::remove(d->payloadFile); // Cancelled: no fallback, clean up payload.
                Q_EMIT finished(Cancelled, QStringList(), QString());
                return;
            }

            // copyTo() is not interruptible; if cancellation is requested during extraction,
            // it will only take effect after copyTo() completes. The worker checks for
            // interruption after copyTo() returns and will then clean up and emit Cancelled.
            // The destination directory may be partially populated; no rollback is performed.
            if (dir->copyTo(installPath)) {
                if (QThread::currentThread()->isInterruptionRequested()) {
                    archive->close(); // Archive was opened, close it before removing payload
                    QFile::remove(d->payloadFile); // Cancelled: no fallback, clean up payload.
                    Q_EMIT finished(Cancelled, QStringList(), QString());
                    return;
                }
                // If we extracted the subdir we want to save it using the /* notation
                if (isSubdir) {
                    installedFiles << QDir(installPath).absolutePath() + QLatin1String("/*");
                } else {
                    // Collect all installed files
                    const auto lst = dir->entries();
                    for (const QString &entry : lst) {
                        const auto currentEntry = dir->entry(entry);
                        const QString childPath = QDir(installPath).filePath(entry);
                        if (currentEntry->isFile()) {
                            installedFiles << childPath;
                        } else if (currentEntry->isDirectory()) {
                            installedFiles << childPath + QStringLiteral("/*");
                        }
                    }
                }
                // If copyTo succeeded but no files were extracted, treat as extraction failure
                // (corrupt/empty archive) rather than success with empty list.
                if (installedFiles.isEmpty()) {
                    errorMessage = i18n("Archive %1 is empty or corrupt", d->payloadFile);
                    result = ExtractionFailed;
                    archive->close();
                    QFile::remove(d->payloadFile);
                    Q_EMIT finished(result, QStringList(), errorMessage);
                    return;
                }
            } else {
                // copyTo() failure: no installed files can be produced, and no caller can retry
                // or consume the payload (installNonArchiveFile is not invoked for ExtractionFailed).
                // Treat as extraction failure and clean up the payload.
                qCWarning(KNEWSTUFFCORE) << "could not install" << d->payloadFile << "to" << installPath;
                errorMessage = i18n("Failed to extract archive %1 to %2", d->payloadFile, installPath);
                result = ExtractionFailed;
                archive->close();
                QFile::remove(d->payloadFile);
                Q_EMIT finished(result, QStringList(), errorMessage);
                return;
            }

            archive->close();
            QFile::remove(d->payloadFile);
        }
    }

    // If not an archive, determine the result type
    if (!isArchive) {
        // For UncompressIntoSubdir with unknown MIME or archive open failure: empty list = failure
        // For UncompressIfArchive/UncompressIntoSubdirIfArchive: fallback required
        if (uncompressionOpt == UncompressIfArchive || uncompressionOpt == UncompressIntoSubdirIfArchive) {
            result = FallbackRequired;
        } else {
            // UncompressIntoSubdir, AlwaysUncompress (already handled above), NeverUncompress
            result = Success; // empty installedFiles will be treated as failure by caller
        }
    }

    Q_EMIT finished(result, installedFiles, errorMessage);
}

#include "moc_archiveextractionworker.cpp"