// Empirical probe for the C4 ordering question:
// "Is the job actually destroyed before startExtraction() returns?"
//
// This probe does NOT use KNewStuff sources; it reproduces the exact shape of
// the C4 synthetic-failure path using KF6 KJob only:
//   manager->startExtraction()  -->  job->handleExtractionFailed()  -->  emitResult()
//   -->  KJob::result emitted synchronously -->  user handler runs
//
// Modes:
//   later  - handler calls job->deleteLater()   (what the C4 regression test does)
//   delete - handler calls delete job            (true synchronous destruction)
//   auto   - default autoDelete(true), no handler action
//
// It reports whether the job object is still alive (QPointer non-null) right
// after the call that emitted the result returns.

#include <KJob>
#include <QCoreApplication>
#include <QPointer>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QTest>
#include <cstdio>
#include <cstring>

extern const char *g_mode;

class ProbeJob : public KJob
{
public:
    explicit ProbeJob(QObject *parent = nullptr)
        : KJob(parent)
    {
    }

    void start() override
    {
        // Mimic ArchiveExtractionJob::handleExtractionFailed(): sets error then emits.
        setError(KJob::UserDefinedError);
        setErrorText(QStringLiteral("synthetic failure"));
        emitResult();
        // Mimic the PRE-C4 code in ArchiveExtractionJob::start():
        //     d->extractionHandle = manager.startExtraction(...);
        // i.e. touch member state AFTER the result-emitting call returns.
        if (!strcmp(g_mode, "delete-noread")) {
            // POST-C4 pattern: do not touch member state after the call.
            printf("  [probe] returned from emitResult() WITHOUT touching member state (post-C4 pattern)\n");
            return;
        }
        m_touchedAfterEmit = true;
        printf("  [probe] member state touched after emitResult() returned (pre-C4 pattern)\n");
    }

    bool m_touchedAfterEmit = false;
};

const char *g_mode = "later";

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const char *mode = argc > 1 ? argv[1] : "later";
    g_mode = mode;

    printf("mode=%s\n", mode);

    ProbeJob *job = new ProbeJob(nullptr);
    job->setAutoDelete(strcmp(mode, "auto") != 0); // "auto" keeps the KF6 default (true)

    QPointer<ProbeJob> ptr(job);
    bool handlerRan = false;

    QObject::connect(job, &KJob::result, job, [mode, &handlerRan, &ptr](KJob *finishedJob) {
        handlerRan = true;
        printf("  [probe] result handler entered; job=%p alive=%d\n",
               static_cast<void *>(finishedJob), ptr.isNull() ? 0 : 1);
        if (strcmp(mode, "later") == 0) {
            finishedJob->deleteLater();
            printf("  [probe] handler called deleteLater(); job alive right after deleteLater()=%d\n",
                   ptr.isNull() ? 0 : 1);
        } else if (strcmp(mode, "delete") == 0 || strcmp(mode, "delete-noread") == 0) {
            printf("  [probe] handler calling delete job (synchronous destruction)\n");
            delete finishedJob;
            printf("  [probe] handler returned; job alive=%d\n", ptr.isNull() ? 0 : 1);
        }
    });

    job->start();

    printf("RESULT after start() returned: handlerRan=%d jobAlive=%d\n",
           handlerRan ? 1 : 0, ptr.isNull() ? 0 : 1);

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    printf("RESULT after processEvents(): jobAlive=%d\n", ptr.isNull() ? 0 : 1);

    if (strcmp(mode, "later") == 0) {
        // QTest's QTRY_VERIFY/qWait spin the event loop with
        // QCoreApplication::processEvents() (no nested QEventLoop::exec()),
        // which keeps the event-loop level at 0 and therefore lets Qt deliver
        // the deferred-delete event. Reproduce that here.
        QElapsedTimer timer;
        timer.start();
        while (ptr && timer.elapsed() < 500) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        printf("RESULT after QTRY_VERIFY-like processEvents spin: jobAlive=%d\n",
               ptr.isNull() ? 0 : 1);
    }

    if (strcmp(mode, "qwait") == 0) {
        // Same mechanism the real KNewStuff regression test uses
        // (QTRY_VERIFY -> QTest::qWait), reproduced with the real QtTest API.
        QTest::qWait(300);
        printf("RESULT after QTest::qWait(300): jobAlive=%d\n", ptr.isNull() ? 0 : 1);
    }

    if (ptr.isNull()) {
        printf("CONCLUSION: job was destroyed\n");
    } else {
        printf("CONCLUSION: job was NOT destroyed\n");
    }
    return 0;
}
