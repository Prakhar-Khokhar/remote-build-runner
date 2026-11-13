#!/usr/bin/env python3
"""Assemble /tmp/kilo/review_bundle_5_implementations_and_state.txt from live sources."""
import datetime
from pathlib import Path

ROOT = Path("/home/prakhar/Github Projects/knewstuff")
OUT = Path("/tmp/kilo/review_bundle_5_implementations_and_state.txt")

MGR_CPP = ROOT / "src/core/jobs/archiveextractionmanager.cpp"
MGR_H = ROOT / "src/core/jobs/archiveextractionmanager.h"
JOB_CPP = ROOT / "src/core/jobs/archiveextractionjob.cpp"
JOB_H = ROOT / "src/core/jobs/archiveextractionjob.h"
WRK_H = ROOT / "src/core/jobs/archiveextractionworker.h"
WRK_CPP = ROOT / "src/core/jobs/archiveextractionworker.cpp"


def lines(path):
    return Path(path).read_text().splitlines()


def excerpt(path, lo, hi, label=""):
    ls = lines(path)
    out = [f"===== {label}  ({path.name}:{lo}-{hi}) ====="]
    width = len(str(hi))
    for i in range(lo, hi + 1):
        out.append(f"{i:>{width}}: {ls[i - 1]}")
    out.append(f"===== end {path.name}:{lo}-{hi} =====")
    return "\n".join(out)


def whole(path, label=""):
    ls = lines(path)
    out = [f"===== {label}  ({path.name}:1-{len(ls)}) COMPLETE FILE ====="]
    width = len(str(len(ls)))
    for i, s in enumerate(ls, 1):
        out.append(f"{i:>{width}}: {s}")
    out.append(f"===== end COMPLETE FILE {path.name} =====")
    return "\n".join(out)


parts = []
now = datetime.datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S UTC")

parts.append(
    f"""=================================================================================
REVIEW BUNDLE 5 of 7 - STATE-FIELD DECLARATIONS + LIFECYCLE IMPLEMENTATION INDEX
=================================================================================
Generated (UTC): {now}
Sources: /home/prakhar/Github Projects/knewstuff/src/core/jobs/*
All excerpts below are VERBATIM from the working-tree files (no source was
modified while producing this bundle). Line numbers refer to the current file
contents on disk.

CONTENTS
--------
PART A : State-field declarations (ExtractionEntry struct, manager members,
         job Private struct, worker state fields) + summary inventory table.
PART B : Complete verbatim implementations of every lifecycle function,
         grouped by responsibility.
PART C : Ordering / invariant analysis (entry lifecycle state machine).
"""
)

# ---------------- PART A ----------------
parts.append(
    """
-------------------------------------------------------------------------------
PART A : STATE-FIELD DECLARATIONS
-------------------------------------------------------------------------------
"""
)
parts.append(excerpt(MGR_CPP, 33, 74, "ArchiveExtractionManager::ExtractionEntry struct (manager.cpp:33-74)"))
parts.append(excerpt(MGR_H, 75, 101, "ArchiveExtractionManager class members (manager.h:75-101)"))
parts.append(excerpt(JOB_CPP, 20, 49, "ArchiveExtractionJobPrivate state fields (job.cpp:20-49)"))
parts.append(excerpt(WRK_H, 69, 90, "ArchiveExtractionWorker state fields (worker.h:69-90)"))
parts.append(excerpt(WRK_CPP, 31, 37, "ArchiveExtractionWorker::Private fields (worker.cpp:31-37)"))

parts.append(
    """
-------------------------------------------------------------------------------
A5 : SUMMARY INVENTORY TABLE  (field | type | mutating thread | purpose)
-------------------------------------------------------------------------------
ExtractionEntry.state            | std::atomic<State> (Starting|Running|Finishing|Reaping|Done) | manager thread (mostly)  | lifecycle state of one extraction
ExtractionEntry.resultClaimed    | std::atomic<bool>            | manager thread           | worker result accepted for processing (reentrancy guard)
ExtractionEntry.resultDisposed   | std::atomic<bool>            | manager thread           | job notification / payload disposition complete
ExtractionEntry.threadReaped     | std::atomic<bool>            | manager thread           | wait(0) succeeded and QThread deleted
ExtractionEntry.reapRetryScheduled| bool (mutex-guarded)         | manager thread           | only one reap retry outstanding per entry
ExtractionEntry.payloadCleanupRetryScheduled | bool (mutex-guarded) | manager thread | only one payload-cleanup retry outstanding
ExtractionEntry.payloadCleanupPending | std::atomic<bool>        | manager thread           | payload removal failed; entry not released until removed/absent
ExtractionEntry.jobDead          | std::atomic<bool>            | manager thread           | handleWorkerFinished observed a dead job (QPointer null)
ExtractionEntry.testWorkerResultCallback | std::function           | manager thread           | captures worker terminal result even after job destruction
ExtractionEntry.testSuppressInterruptionFallback | bool           | manager thread           | suppress queued interruption fallback in requestCancellation()
ExtractionEntry.cancellation     | shared_ptr<atomic<bool>>     | any (thread-safe atomic) | shared cancellation flag with job
ExtractionEntry.thread           | QThread*                     | manager thread           | worker's QThread
ExtractionEntry.worker           | ArchiveExtractionWorker*     | manager thread           | worker (deleteLater'd on thread.finished)
ExtractionEntry.job              | QPointer<ArchiveExtractionJob>| manager thread           | owning job (auto-nulls on destruction)
ExtractionEntry.handle / payloadFile / installDir / uncompressionOption | plain members | manager thread | entry identity and payload description

ArchiveExtractionManager::m_nextHandle  | std::atomic<int>  | manager thread  | monotonic handle allocator
ArchiveExtractionManager::m_entriesMutex | QMutex             | manager thread  | guards m_entries vector
ArchiveExtractionManager::m_entries     | vector<shared_ptr<ExtractionEntry>> | manager thread | active entry tracking
ArchiveExtractionManager::m_testForceStartFailure | bool        | manager thread  | test seam: simulate failed thread start
ArchiveExtractionManager::m_testPayloadRemoveFailure | bool       | manager thread  | test seam: simulate payload removal failure

ArchiveExtractionJobPrivate::finished      | bool  | job thread (main)  | emitResult already called
ArchiveExtractionJobPrivate::killRequested | bool  | job thread (main)  | kill() invoked before start
ArchiveExtractionJobPrivate::startCalled   | bool  | job thread (main)  | start() already invoked
ArchiveExtractionJobPrivate::extractionHandle | int | manager thread (set before result delivery) | handle assigned by manager
ArchiveExtractionJobPrivate::cancellation  | shared_ptr<atomic<bool>> | shared | cancellation flag shared with manager entry

ArchiveExtractionWorker::m_cancellation | shared_ptr<atomic<bool>> | any thread-safe | shared cancellation flag
ArchiveExtractionWorker::m_testBarrier*/m_test*Callback | test seams | worker thread | synchronization test seams
"""
)

# ---------------- PART B ----------------
parts.append(
    """
-------------------------------------------------------------------------------
PART B : LIFECYCLE FUNCTION IMPLEMENTATIONS (VERBATIM WITH LINE NUMBERS)
-------------------------------------------------------------------------------
"""
)
parts.append(excerpt(MGR_CPP, 95, 302, "B1 : startExtraction  -- entry creation + handle assignment + thread/worker start"))
parts.append(excerpt(MGR_CPP, 132, 158, "B2 : job-destruction observation block (inside startExtraction) -- DirectConnection destroyed() -> queued invokeMethod -> jobDead"))
parts.append(excerpt(MGR_CPP, 347, 437, "B3 : handleWorkerFinished -- worker terminal result delivery + resultDisposed + jobDead + direct-reap attempt + tryFinalize"))
parts.append(excerpt(MGR_CPP, 439, 456, "B4 : deliverSyntheticFailure -- synthetic terminal failure for a thread that failed to start"))
parts.append(excerpt(MGR_CPP, 458, 490, "B5 : cleanupPayloadIfNeeded -- remove unconsumed payload only when resultDisposed && threadReaped"))
parts.append(excerpt(MGR_CPP, 612, 638, "B6 : schedulePayloadCleanupRetry -- single-outstanding 15ms timer retry"))
parts.append(excerpt(MGR_CPP, 640, 682, "B7 : onPayloadCleanupTimer -- retry removal, unbounded reschedule, tryFinalize on success"))
parts.append(excerpt(MGR_CPP, 684, 731, "B8 : tryFinalize -- finalization invariant resultDisposed && threadReaped; entry release gating"))
parts.append(excerpt(MGR_CPP, 806, 812, "B9 : releaseEntryFromVector -- mutex-guarded m_entries[handle].reset()"))
parts.append(excerpt(MGR_CPP, 492, 561, "B10 : reapEntry -- physical reaping of the QThread"))
parts.append(excerpt(MGR_CPP, 563, 610, "B11 : scheduleReapRetry + onReapTimer -- non-blocking reap retry"))
parts.append(excerpt(MGR_CPP, 733, 761, "B12 : waitForAllFinished -- bounded wait for all entries to reach Done"))
parts.append(excerpt(MGR_CPP, 763, 773, "B13 : activeExtractionsCount"))
parts.append(excerpt(JOB_CPP, 61, 75, "B14 : ArchiveExtractionJob::~ArchiveExtractionJob -- destructor (skips requestCancellation when finished)"))
parts.append(excerpt(JOB_CPP, 77, 104, "B15 : ArchiveExtractionJob::start -- post-C4: does NOT touch d after startExtraction() returns"))
parts.append(excerpt(JOB_CPP, 221, 224, "B16 : ArchiveExtractionJob::setExtractionHandle -- manager-assigned handle (C4)"))
parts.append(excerpt(JOB_CPP, 226, 246, "B17 : ArchiveExtractionJob::doKill -- cooperative cancellation"))
parts.append(excerpt(JOB_CPP, 248, 302, "B18 : ArchiveExtractionJob::handle* -- d->finished guard, emitResult()"))
parts.append(excerpt(WRK_CPP, 68, 331, "B19 : ArchiveExtractionWorker::doWork -- emits started(), finished(ResultType,...) at every terminal checkpoint"))

OUT.write_text("\n".join(parts) + "\n")
print(f"Wrote {OUT} ({len('\n'.join(parts))} chars)")
