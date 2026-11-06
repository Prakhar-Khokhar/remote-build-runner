/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef ARCHIVEEXTRACTIONJOB_H
#define ARCHIVEEXTRACTIONJOB_H

#include "jobbase.h"

#include <QStringList>
#include <QPointer>
#include <QThread>
#include <memory>

#include "archiveextractionworker.h"

namespace KNSCore
{
class ArchiveExtractionJobPrivate;
class ArchiveExtractionJob : public KJob
{
    Q_OBJECT
public:
    enum UncompressionOption {
        AlwaysUncompress,
        UncompressIfArchive,
        UncompressIntoSubdirIfArchive,
        UncompressIntoSubdir,
        NeverUncompress,
    };
    Q_ENUM(UncompressionOption)

    explicit ArchiveExtractionJob(const QString &payloadFile, const QString &installDir, UncompressionOption uncompressionOption, QObject *parent = nullptr);
    ~ArchiveExtractionJob() override;

    Q_SCRIPTABLE void start() override;

    QStringList installedFiles() const;

protected:
    bool doKill() override;

private Q_SLOTS:
    void handleWorkerFinished(ArchiveExtractionWorker::ResultType result, const QStringList &files, const QString &errorMessage);

private:
    const std::unique_ptr<ArchiveExtractionJobPrivate> d;
};

}

#endif // ARCHIVEEXTRACTIONJOB_H