#!/usr/bin/env python3
"""Assemble /tmp/kilo/review_bundle_7_c4_ordering_analysis.txt."""
import datetime
from pathlib import Path

ROOT = Path("/home/prakhar/Github Projects/knewstuff")
OUT = Path("/tmp/kilo/review_bundle_7_c4_ordering_analysis.txt")
PROBE = Path("/tmp/kilo/c4_order_probe.cpp")
PROBE_OUT = {m: Path(f"/tmp/kilo/_probe_{m}.txt") for m in ["later", "qwait", "auto", "delete", "deletenoread"]}

TEST = ROOT / "autotests/core/archiveextractionjobtest.cpp"
MGR_CPP = ROOT / "src/core/jobs/archiveextractionmanager.cpp"
JOB_CPP = ROOT / "src/core/jobs/archiveextractionjob.cpp"

now = datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d %H:%M:%S UTC")


def excerpt(path, lo, hi, label=""):
    ls = Path(path).read_text().splitlines()
    out = [f"===== {label}  ({Path(path).name}:{lo}-{hi}) ====="]
    width = len(str(hi))
    for i in range(lo, hi + 1):
        out.append(f"{i:>{width}}: {ls[i - 1]}")
    out.append(f"===== end =====")
    return "\n".join(out)


def whole_file_labelled(path, label):
    ls = Path(path).read_text().splitlines()
    out = [f"===== {label}  ({Path(path).name}:1-{len(ls)}) ====="]
    width = len(str(len(ls)))
    for i, s in enumerate(ls, 1):
        out.append(f"{i:>{width}}: {s}")
    out.append(f"===== end =====")
    return "\n".join(out)


def probe_section(mode):
    p = PROBE_OUT[mode]
    if not p.exists():
        return f"===== PROBE OUTPUT [{mode}] : (missing file) =====\n"
    data = p.read_text()
    tail = ""
    if data and not data.endswith("\n"):
        data += "\n"
    if "Segmentation fault" in data or data.strip().endswith("exit_code=139"):
        tail = "EXIT CODE: 139 (segmentation fault)\n"
    elif data.strip().endswith("exit_code=0"):
        tail = "EXIT CODE: 0\n"
    return f"===== PROBE OUTPUT [{mode}] (verbatim) =====\n{data}{tail}===== end probe [{mode}] =====\n"


parts = []
parts.append(f"""=================================================================================
REVIEW BUNDLE 7 of 7 - C4 ORDERING INVESTIGATION + testCompletedEntryReleased SEMANTICS
=================================================================================
Generated (UTC): {now}

SCOPE
-----
(7.1) Investigation (test NOT modified): is the job actually destroyed before
      ArchiveExtractionManager::startExtraction() returns on the C4
      synthetic-failure path? Answered with a standalone Qt/KF6 probe
      (source in PART D) that mirrors the exact shape of the C4 path.
(7.2) Semantics of testCompletedEntryReleased (what it proves / does not prove).
(7.3) Relevant source excerpts (verbatim) for cross-reference.
""")

# ---------- 7.1 ----------
parts.append("""
-------------------------------------------------------------------------------
7.1 : IS THE JOB DESTROYED BEFORE startExtraction() RETURNS?  (C4 ordering)
-------------------------------------------------------------------------------

SHORT ANSWER
------------
No. On the C4 synthetic-failure path, ArchiveExtractionJob::start() ->
ArchiveExtractionManager::startExtraction() returns while the job object is
still alive. The job is destroyed LATER, asynchronously, when the Qt event
loop delivers the QDeferredDeleteEvent posted by deleteLater().

This is demonstrated empirically with a standalone probe (no KNewStuff code,
Qt 6.11.2 + KF6CoreAddons only) in PART D:

  MODE "later"   = exact shape of the C4 regression test
                   (result handler calls job->deleteLater()).
  RESULT: after start() returned -> jobAlive=1 (job NOT destroyed).

  MODE "qwait"   = QTest::qWait(300), i.e. the mechanism QTRY_VERIFY uses.
  RESULT: after QTest::qWait(300) -> jobAlive=0 (job destroyed later,
          asynchronously, by the event loop).

  MODE "delete" / "delete-noread" = synchronous delete job inside the
                   result handler (the kind of destruction that WOULD be
                   required to catch the pre-C4 UAF).
  RESULT: segmentation fault (exit 139) INSIDE Qt/KF6 signal-activation
          cleanup, before any KNewStuff member state is touched. Synchronous
          destruction of a job inside its own result handler is not supported
          by KF6/Qt in this configuration.

WHY
---
Qt's QObject::deleteLater() posts a QDeferredDeleteEvent to the object.
Deferred-delete events are delivered when the event loop processes them
asynchronously -- not synchronously within the signal handler that called
deleteLater(). Since ArchiveExtractionManager::startExtraction() runs with
no event loop active at its level, the deferred delete is not processed
before startExtraction() returns to ArchiveExtractionJob::start(), and
ArchiveExtractionJob::start() (post-C4) does not touch d after the call.

WHAT THE C4 REGRESSION TEST PROVES
----------------------------------
testSyntheticFailureSynchronousJobDestruction
(archiveextractionjobtest.cpp:1739-1811):

  - handlerRanDuringStartExtraction == true   -> the KJob::result handler
    runs SYNCHRONOUSLY inside emitResult(), i.e. inside startExtraction()
    (before it returns to ArchiveExtractionJob::start()).
  - job->extractionHandle() != -1             -> the manager assigned the
    handle via setExtractionHandle() BEFORE result delivery (C4 handle
    assignment ordering).
  - jobErrorCode == KJob::UserDefinedError    -> exactly one failure result
    delivered (spy.count() == 1).
  - No crash when a result handler schedules destruction during the call.

WHAT THE C4 TEST DOES NOT PROVE
-------------------------------
It does NOT prove that the job is destroyed before startExtraction() returns,
because the handler uses deleteLater() (asynchronous). It also does NOT
discriminate the pre-C4 bug: with the pre-C4 code
    d->extractionHandle = manager.startExtraction(...);
the assignment is safe because the job is still alive at return
(probe mode "later" touches member state after emitResult() and does not
crash). Hence the test passes with pre-C4 code as well -- it is not a
regression test that would have caught the C4 use-after-free.

DISCRIMINATING THE UAF WOULD REQUIRE SYNCHRONOUS DESTRUCTION
------------------------------------------------------------
A handler that calls `delete job;` synchronously would destroy the job
before startExtraction() returns, exposing the pre-C4 `d->extractionHandle =`
use-after-free. However the probe shows this path crashes inside Qt/KF6
signal-activation cleanup (exit 139) regardless of KNewStuff code, so such a
test cannot be written trivially within KF6's current signal semantics.

C4 FIX POSTURE (source cross-reference)
---------------------------------------
  src/core/jobs/archiveextractionjob.cpp:77-104  -- ArchiveExtractionJob::start()
      Post-C4: after ArchiveExtractionManager::instance().startExtraction(...)
      returns, start() does NOT access d (no d->extractionHandle assignment).
  src/core/jobs/archiveextractionjob.cpp:221-224  -- setExtractionHandle(int)
      The manager's assignment is authoritative; called before any result
      delivery (manager.cpp:176-178).
  src/core/jobs/archiveextractionmanager.cpp:95-302  -- startExtraction()
      Sequence: entry created/stored -> destroyed() observation installed ->
      setExtractionHandle(handle) -> thread start -> (failure path) ...
      destroy path: delete newThread; state=Done; threadReaped=true;
      payload removal attempt -> deliverSyntheticFailure() -> return handle.
  src/core/jobs/archiveextractionmanager.cpp:439-456  -- deliverSyntheticFailure()
      resultClaimed=true; job->handleExtractionFailed(...) [d->finished=true,
      emitResult() -> result handler runs synchronously]; resultDisposed=true;
      cleanupPayloadIfNeeded if jobDead || job.isNull(); tryFinalize().
  src/core/jobs/archiveextractionmanager.cpp:684-731  -- tryFinalize()
      Requires resultDisposed && threadReaped. Done branch releases only if
      !payloadCleanupPending; first-transition branch releases only if
      (jobDead || job.isNull()) && !payloadCleanupPending.
""")

# ---------- 7.2 ----------
parts.append("""
-------------------------------------------------------------------------------
7.2 : testCompletedEntryReleased SEMANTICS
-------------------------------------------------------------------------------

Location: archiveextractionjobtest.cpp:1479-1533

Purpose (per the test's own comment): C1 -- verify that completed entries are
released from the manager even while the job is still alive, using
testTotalEntryCount() as the oracle (activeExtractionsCount() ignores Done
entries).

BODY SUMMARY
------------
  - waitForAllFinished(5000) + processEvents; assert testTotalEntryCount()==0
    (clears leftover Done entries from prior tests).
  - Loop 3x: start a job on a real tar.gz payload; spy.wait(5000);
    assert !job->error(); handle = job->extractionHandle() != -1;
    assert testTotalEntryCount()==1            (entry present while job alive)
    job->deleteLater(); QTRY_VERIFY(jobPtr.isNull());
    QTRY_VERIFY(testTotalEntryCount()==0);     (entry released)
    assert !QFileInfo::exists(payloadFile);    (payload removed)
  - Final: waitForAllFinished(5000); assert testTotalEntryCount()==0.

WHAT IT PROVES
--------------
  - While the job is alive and finished, exactly one entry is tracked
    (testTotalEntryCount()==1 at line 1518).
  - After job->deleteLater() plus QTRY_VERIFY(jobPtr.isNull()), the entry is
    released (testTotalEntryCount()==0 at line 1525).
  - The payload is removed after completion.
  - Repeated 3x with no accumulation (no entry leak across iterations).

WHAT IT DOES NOT PROVE
----------------------
  - It does NOT prove the entry is released while the job is still alive: the
    zero-entry assertion (line 1525) runs AFTER QTRY_VERIFY(jobPtr.isNull()),
    i.e. after the job has been destroyed. The release path observed is:
      job destroyed -> queued destroyed callback sets jobDead ->
      tryFinalize (Done branch) -> cleanupPayloadIfNeeded -> releaseEntryFromVector.
  - The pre-loop assertion at line 1490 (testTotalEntryCount()==0 before any
    new job) only confirms leftovers from prior tests were drained; it is not
    part of the per-iteration release check.

CONCLUSION
----------
The test validates that a finished job's entry is eventually released after
job destruction, and that the payload is removed. It does not, by itself,
establish release-before-destruction.
""")

# ---------- 7.3 ----------
parts.append("""
-------------------------------------------------------------------------------
7.3 : VERBATIM SOURCE EXCERPTS FOR CROSS-REFERENCE
-------------------------------------------------------------------------------
""")
parts.append(excerpt(JOB_CPP, 77, 104, "ArchiveExtractionJob::start (post-C4)"))
parts.append(excerpt(MGR_CPP, 132, 158, "job-destruction observation block (manager.cpp:132-158)"))
parts.append(excerpt(MGR_CPP, 160, 222, "startExtraction: handle assignment + failed-start path (manager.cpp:160-222)"))
parts.append(excerpt(MGR_CPP, 439, 456, "deliverSyntheticFailure (manager.cpp:439-456)"))
parts.append(excerpt(MGR_CPP, 684, 731, "tryFinalize (manager.cpp:684-731)"))

# ---------- PART D probe source ----------
parts.append("""
-------------------------------------------------------------------------------
PART D : STANDALONE PROBE SOURCE (VERBATIM)  /tmp/kilo/c4_order_probe.cpp
-------------------------------------------------------------------------------
""")
parts.append(whole_file_labelled(PROBE, "standalone C4-ordering probe (no KNewStuff code)"))
parts.append("""
-------------------------------------------------------------------------------
PART E : PROBE EXECUTION OUTPUTS (VERBATIM)
-------------------------------------------------------------------------------
""")
for m in ["later", "qwait", "auto", "delete", "deletenoread"]:
    parts.append(probe_section(m))

parts.append("""
-------------------------------------------------------------------------------
END OF REVIEW BUNDLE 7
-------------------------------------------------------------------------------
""")

OUT.write_text("\n".join(parts) + "\n")
print(f"Wrote {OUT} ({len('\n'.join(parts))} chars)")
