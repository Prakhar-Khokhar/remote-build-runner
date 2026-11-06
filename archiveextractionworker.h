/*
    SPDX-FileCopyrightText: 2024 KDE Contributors
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef ARCHIVEEXTRACTIONWORKER_H
#define ARCHIVEEXTRACTIONWORKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

namespace KNSCore
{
class ArchiveExtractionWorker : public QObject
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

    enum ResultType {
        Success,
        FallbackRequired,
        ExtractionFailed,
        Cancelled
    };
    Q_ENUM(ResultType)

    explicit ArchiveExtractionWorker(const QString &payloadFile, const QString &installDir, UncompressionOption uncompressionOption);
    ~ArchiveExtractionWorker() override;

    void start();

    Q_SIGNAL void finished(ResultType result, const QStringList &installedFiles, const QString &errorMessage);

private:
    class Private;
    const std::unique_ptr<Private> d;

private Q_SLOTS:
    void doWork();
};

}

#endif // ARCHIVEEXTRACTIONWORKER_H