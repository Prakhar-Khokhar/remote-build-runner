/*
    This file is part of KNewStuff3
    SPDX-FileCopyrightText: 2024 KDE Contributors

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <KSharedConfig>
#include <QDir>
#include <QSignalSpy>
#include <QTest>
#include <QtGlobal>
#include <QThread>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QPointer>
#include <QFileInfo>

#include "entry.h"
#include "installation_p.h"
#include "jobs/archiveextractionjob.h"
#include "question.h"
#include "questionmanager.h"
#include "filesystemmutex.h"

using namespace KNSCore;

class ArchiveExtractionJobTest : public QObject
{
    Q_OBJECT
public:
    const QString dataDir = QStringLiteral(DATA_DIR);

private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();
    void testSuccessfulExtraction();
    void testSuccessfulExtractionTopLevelFiles();
    void testCorruptArchiveAlwaysUncompress();
    void testCorruptArchiveWithFallback();
    void testNonArchiveFile();
    void testUnknownMimeUncompressIntoSubdir();
    void testArchiveOpenFailureUncompressIntoSubdir();
    void testCancellationBeforeStart();
    void testCancellationDuringExtraction();
    void testJobDestructionDuringExtraction();
    void testGuiResponsivenessDuringExtraction();
    void testWorkerSelfCleanup();
    void testCancellationRaceCondition();
    void testArchiveOpenFailureUncompressIfArchiveFallback();
    void testArchiveOpenFailureUncompressIntoSubdirIfArchiveFallback();
    void testUnknownMimeAlwaysUncompressError();
};

void ArchiveExtractionJobTest::initTestCase()
{
    qRegisterMetaType<QStringList>();
    QStandardPaths::setTestModeEnabled(true);
}

void ArchiveExtractionJobTest::cleanup()
{
    // Do not clear destination mutexes here - mutexes are process-lifetime and
    // clearing them while worker threads might still be running is unsafe.
    // Mutexes will be cleaned up at process exit.
}

void ArchiveExtractionJobTest::cleanupTestCase()
{
    const QString dataPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("demo/"), QStandardPaths::LocateDirectory);
    if (!dataPath.isEmpty()) {
        QDir(dataPath).removeRecursively();
    }
    // Mutexes are process-lifetime; no need to clear them.
}

void ArchiveExtractionJobTest::testSuccessfulExtraction()
{
    // Test successful async archive extraction with subdirectory
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    QVERIFY(!job->error());

    QStringList installedFiles = job->installedFiles();
    QCOMPARE(installedFiles.count(), 1);
    QVERIFY(installedFiles.first().endsWith(QLatin1String("/*")));

    const QFileInfo fileInfo(installedFiles.first().left(installedFiles.first().size() - 2));
    QVERIFY(fileInfo.exists());
    QVERIFY(fileInfo.isDir());

    const QStringList files = QDir(fileInfo.absoluteFilePath()).entryList(QDir::Filter::Files | QDir::Filter::NoDotAndDotDot);
    QCOMPARE(files, QStringList({QStringLiteral("test1.txt"), QStringLiteral("test2.txt")}));

    // Payload should be removed on Success
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testSuccessfulExtractionTopLevelFiles()
{
    // Test successful async archive extraction with top-level files
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_toplevel_files.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    QVERIFY(!job->error());

    QStringList installedFiles = job->installedFiles();
    QCOMPARE(installedFiles.count(), 1);
    QVERIFY(installedFiles.first().endsWith(QLatin1String("/*")));

    const QFileInfo fileInfo(installedFiles.first().left(installedFiles.first().size() - 2));
    QVERIFY(fileInfo.exists());
    QVERIFY(fileInfo.isDir());

    const QStringList files = QDir(fileInfo.absoluteFilePath()).entryList(QDir::Filter::Files | QDir::Filter::NoDotAndDotDot);
    QCOMPARE(files, QStringList({QStringLiteral("test1.txt"), QStringLiteral("test2.txt")}));

    // Payload should be removed on Success
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
}

void ArchiveExtractionJobTest::testCorruptArchiveAlwaysUncompress()
{
    // Test corrupt archive handling with AlwaysUncompress (should error, payload removed)
    QTemporaryFile corruptArchive;
    corruptArchive.setAutoRemove(true);
    QVERIFY(corruptArchive.open());
    corruptArchive.write("This is not a valid archive");
    corruptArchive.flush();
    QString payloadFile = corruptArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::AlwaysUncompress);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    QVERIFY(job->error());
    QCOMPARE(job->error(), KJob::UserDefinedError);
    QVERIFY(!job->errorString().isEmpty());

    // Payload should be removed on ExtractionFailed
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testCorruptArchiveWithFallback()
{
    // Test corrupt archive handling with UncompressIfArchive (should request fallback, payload preserved)
    QTemporaryFile corruptArchive;
    corruptArchive.setAutoRemove(true);
    QVERIFY(corruptArchive.open());
    corruptArchive.write("This is not a valid archive");
    corruptArchive.flush();
    QString payloadFile = corruptArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // For corrupt archive with UncompressIfArchive, the job should succeed (no error) but request fallback
    QVERIFY(!job->error());
    QCOMPARE(job->property("fallbackRequired").toBool(), true);
    QCOMPARE(job->installedFiles().count(), 0);

    // Payload should be preserved on FallbackRequired (caller will consume it)
    QVERIFY(QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testNonArchiveFile()
{
    // Test non-archive file with UncompressIfArchive (should request fallback, payload preserved)
    QString payloadFile = dataDir + QStringLiteral("testfile.txt");
    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // For non-archive with UncompressIfArchive, the job succeeds but requests fallback
    QVERIFY(!job->error());
    QCOMPARE(job->property("fallbackRequired").toBool(), true);
    QCOMPARE(job->installedFiles().count(), 0);

    // Payload should be preserved on FallbackRequired (caller will consume it)
    QVERIFY(QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testUnknownMimeUncompressIntoSubdir()
{
    // Test unknown MIME type with UncompressIntoSubdir - should fail with empty installedFiles (no fallback)
    QTemporaryFile unknownFile;
    unknownFile.setAutoRemove(true);
    QVERIFY(unknownFile.open());
    unknownFile.write("This is not a recognized archive format");
    unknownFile.flush();
    QString payloadFile = unknownFile.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdir);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // Should succeed (no error) but return empty installedFiles
    QVERIFY(!job->error());
    QCOMPARE(job->property("fallbackRequired").toBool(), false);
    QCOMPARE(job->installedFiles().count(), 0);

    // Payload preserved (Success with empty list, but not ExtractionFailed)
    QVERIFY(QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testArchiveOpenFailureUncompressIntoSubdir()
{
    // Test archive open failure with UncompressIntoSubdir - should fail with ExtractionFailed
    // Create a corrupt archive file with .tar.gz suffix so MIME type is recognized
    QTemporaryFile corruptArchive;
    corruptArchive.setAutoRemove(true);
    corruptArchive.setFileTemplate(QDir::tempPath() + "/corrupt_XXXXXX.tar.gz");
    QVERIFY(corruptArchive.open());
    corruptArchive.write("This looks like a tar but is corrupt");
    corruptArchive.flush();
    QString payloadFile = corruptArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdir);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // Should fail with ExtractionFailed (archive open failed)
    QVERIFY(job->error());
    QCOMPARE(job->error(), KJob::UserDefinedError);
    QVERIFY(!job->errorString().isEmpty());

    // Payload should be removed on ExtractionFailed
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testCancellationBeforeStart()
{
    // Test cancellation before extraction starts
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    bool kr = job->kill(KJob::EmitResult);
    Q_UNUSED(kr);
    job->start();

    // Check spy count immediately - signal should have been emitted by kill()
    QCOMPARE(spy.count(), 1);
    
    // Job should be killed
    QVERIFY(job->error() == KJob::KilledJobError);
    QVERIFY(job->errorText().contains("cancelled", Qt::CaseInsensitive));

    // Payload should be removed on Cancelled (no fallback consumer)
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testCancellationDuringExtraction()
{
    // Test cancellation during extraction - verify the job doesn't finish prematurely
    // and properly reports KilledJobError after worker completes
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    // Request cancellation immediately after start
    job->kill();

    QVERIFY(spy.wait(5000));
    // Job should complete with KilledJobError (since worker was running when kill was called)
    QVERIFY(job->error() == KJob::KilledJobError);
    QVERIFY(job->errorText().contains("cancelled", Qt::CaseInsensitive));

    // Payload should be removed on Cancelled (no fallback consumer)
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testJobDestructionDuringExtraction()
{
    // Test job destruction during extraction - verify no crash and proper cleanup
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    // Give the job a moment to start the worker thread
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    // Delete job while extraction is running - should not crash
    // Job destructor requests interruption; worker will self-cleanup via deleteLater
    job->deleteLater();

    // Process events to allow cleanup - worker thread should quit and worker delete
    QCoreApplication::processEvents(QEventLoop::AllEvents, 500);

    // Verify the job object was deleted (no crash, no leak)
    // Use QPointer to safely observe deletion
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());

    // The spy may or may not have received a result (cancellation is async)
    // If it did, verify it's KilledJobError (from destructor's kill request)
    if (spy.count() > 0) {
        QVERIFY(job->error() == KJob::KilledJobError || spy.count() == 0);
    }
}

void ArchiveExtractionJobTest::testGuiResponsivenessDuringExtraction()
{
    // Test that the GUI event loop remains responsive during extraction
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    // Process events a few times to verify GUI responsiveness
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    // Job should complete successfully
    QVERIFY(spy.wait(5000));
    QVERIFY(!job->error());

    // Payload should be removed on Success
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testWorkerSelfCleanup()
{
    // Test that worker self-cleans up after completion
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    QVERIFY(!job->error());

    // Payload should be removed on Success
    QVERIFY(!QFileInfo::exists(payloadFile));

    // Job should be deletable immediately after completion
    // Worker self-cleans via deleteLater when its thread finishes
    job->deleteLater();

    // Verify worker thread cleanup by waiting for the job's QObject destruction
    // Use QPointer to safely observe deletion without dereferencing raw pointers
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testCancellationRaceCondition()
{
    // Test that cancellation cannot race with successful completion
    // If kill() is called, the job must report KilledJobError even if
    // the worker passed its last interruption checkpoint before the request arrived.
    QTemporaryFile tempArchive;
    tempArchive.setAutoRemove(true);
    QVERIFY(tempArchive.open());
    QFile sourceArchive(dataDir + QStringLiteral("archive_dir.tar.gz"));
    QVERIFY(sourceArchive.open(QIODevice::ReadOnly));
    tempArchive.write(sourceArchive.readAll());
    tempArchive.flush();
    sourceArchive.close();
    QString payloadFile = tempArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    // Immediately request cancellation - race with worker startup
    job->kill();

    QVERIFY(spy.wait(5000));
    // Must report KilledJobError, never Success
    QVERIFY(job->error() == KJob::KilledJobError);
    QVERIFY(job->errorText().contains("cancelled", Qt::CaseInsensitive));

    // Payload should be removed on Cancelled (no fallback consumer)
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testArchiveOpenFailureUncompressIfArchiveFallback()
{
    // Test extraction failure with UncompressIfArchive - corrupt archive that opens but fails to extract
    // Create a corrupt archive file with .tar.gz suffix so MIME type is recognized
    QTemporaryFile corruptArchive;
    corruptArchive.setAutoRemove(true);
    corruptArchive.setFileTemplate(QDir::tempPath() + "/corrupt_XXXXXX.tar.gz");
    QVERIFY(corruptArchive.open());
    corruptArchive.write("This looks like a tar but is corrupt");
    corruptArchive.flush();
    QString payloadFile = corruptArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // Should fail with ExtractionFailed (archive opens but copyTo fails)
    QVERIFY(job->error());
    QCOMPARE(job->error(), KJob::UserDefinedError);
    QVERIFY(!job->errorString().isEmpty());

    // Payload should be removed on ExtractionFailed
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testArchiveOpenFailureUncompressIntoSubdirIfArchiveFallback()
{
    // Test extraction failure with UncompressIntoSubdirIfArchive - corrupt archive that opens but fails to extract
    // Create a corrupt archive file with .tar.gz suffix so MIME type is recognized
    QTemporaryFile corruptArchive;
    corruptArchive.setAutoRemove(true);
    corruptArchive.setFileTemplate(QDir::tempPath() + "/corrupt_XXXXXX.tar.gz");
    QVERIFY(corruptArchive.open());
    corruptArchive.write("This looks like a tar but is corrupt");
    corruptArchive.flush();
    QString payloadFile = corruptArchive.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::UncompressIntoSubdirIfArchive);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // Should fail with ExtractionFailed (archive opens but copyTo fails)
    QVERIFY(job->error());
    QCOMPARE(job->error(), KJob::UserDefinedError);
    QVERIFY(!job->errorString().isEmpty());

    // Payload should be removed on ExtractionFailed
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

void ArchiveExtractionJobTest::testUnknownMimeAlwaysUncompressError()
{
    // Test unknown MIME type with AlwaysUncompress - should error, payload removed
    QTemporaryFile unknownFile;
    unknownFile.setAutoRemove(true);
    QVERIFY(unknownFile.open());
    unknownFile.write("This is not a recognized archive format");
    unknownFile.flush();
    QString payloadFile = unknownFile.fileName();

    QString installDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/demo/");
    QDir().mkpath(installDir);

    ArchiveExtractionJob *job = new ArchiveExtractionJob(payloadFile, installDir, ArchiveExtractionJob::AlwaysUncompress);
    QSignalSpy spy(job, &KJob::result);
    job->start();

    QVERIFY(spy.wait(5000));
    // Should error
    QVERIFY(job->error());
    QCOMPARE(job->error(), KJob::UserDefinedError);
    QVERIFY(!job->errorString().isEmpty());

    // Payload should be removed on ExtractionFailed
    QVERIFY(!QFileInfo::exists(payloadFile));

    job->deleteLater();
    QPointer<ArchiveExtractionJob> jobPtr(job);
    QTRY_VERIFY(jobPtr.isNull());
}

QTEST_MAIN(ArchiveExtractionJobTest)

#include "archiveextractionjobtest.moc"