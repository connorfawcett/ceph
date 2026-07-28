// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

/**
 * ceph_test_ec_snap_migration_redirect
 *
 * Reproduces the scenario where a snap read on a pool undergoing migration
 * is incorrectly redirected to the (empty) target pool, returning ENOENT.
 *
 * Setup (run once):
 *  1. Create head object "test" in pool "ec"
 *  2. Create 4 selfmanaged snaps (snap1..snap4), then update the write context
 *     once to include all 4 with the highest snap id as seq. A single
 *     subsequent write_full materialises one clone that covers all 4 snapshots.
 *     The repeated reads are issued to one of the lower snap ids, not to the
 *     highest snap id that names the clone object.
 *
 * Loop (repeated kNumIterations times):
 *  3. Start a pool migration: <src> -> <tgt>
 *     On iteration 0:  src="ec",            tgt="ec_target_0"
 *     On iteration N:  src="ec_target_N-1", tgt="ec_target_N"
 *     The pool migration watermark is chosen by the OSD from PG object order;
 *     this test does not set it explicitly.
 *  4. Repeatedly read "test" at one of the lower snap ids from the source
 *     pool. Reads request 1 byte so they exercise a real data read path. Any
 *     read failure is a bug; ENOENT specifically means the OSD incorrectly
 *     redirected the snap read to the (empty) target pool.
 */

#include "include/rados/librados.hpp"
#include "include/stringify.h"
#include "global/global_context.h"
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "common/errno.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>

using namespace std;
using namespace librados;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static int check(const char *what, int r, int expected = 0)
{
  if (r != expected) {
    cerr << what << " failed: " << cpp_strerror(r)
         << " (got " << r << ", expected " << expected << ")" << endl;
  } else {
    cout << what << " ok (r=" << r << ")" << endl;
  }
  return r;
}

/**
 * Issue a mon command and print the result string on failure.
 */
static int mon_cmd(Rados &rados, string cmd)
{
  bufferlist outbl;
  string outstr;
  int r = rados.mon_command(std::move(cmd), {}, &outbl, &outstr);
  if (r != 0) {
    cerr << "mon_command(" << cmd << ") failed: "
         << (outstr.empty() ? cpp_strerror(r) : outstr) << endl;
  }
  return r;
}

struct PendingRead {
  bufferlist bl;
  AioCompletion *completion = nullptr;

  PendingRead() = default;
  PendingRead(const PendingRead&) = delete;
  PendingRead& operator=(const PendingRead&) = delete;

  ~PendingRead()
  {
    if (completion) {
      completion->release();
    }
  }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
  // ---- initialise librados / global context --------------------------------
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(nullptr, args,
                         CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  // ---- connect to cluster --------------------------------------------------
  Rados rados;
  int r = rados.init_with_context(g_ceph_context);
  if (check("rados.init_with_context", r) < 0) return 1;

  r = rados.connect();
  if (check("rados.connect", r) < 0) return 1;

  // ---- use the pre-existing 'ec' pool --------------------------------------
  const string pool_name  = "ec";
  const string tgt_prefix = "ec_target_";
  const string oid        = "test";

  static const int kNumIterations = 50;

  // Open ioctx for the source pool (used for setup writes only)
  IoCtx ioctx;
  r = rados.ioctx_create(pool_name.c_str(), ioctx);
  if (check("ioctx_create(ec)", r) < 0) {
    rados.shutdown();
    return 1;
  }

  // ---- Step 1: create head object -----------------------------------------
  //
  // Write the largest object the OSD will accept (osd_max_write_size, which
  // defaults to 90 MiB) so that migration takes long enough for the snap read
  // stress loop to overlap with an in-progress migration.
  //
  // osd_max_write_size is expressed in MiB; we stay 1 byte under the hard
  // limit to avoid EMSGSIZE.
  //
  const uint64_t max_write_mib =
    g_ceph_context->_conf.get_val<Option::size_t>("osd_max_write_size");
  const size_t   kChunkSize = 1 * 1024 * 1024;                  // 1 MiB
  const size_t   kWriteSize = max_write_mib * 1024 * 1024 - 1;  // max - 1 byte
  const size_t   kNumChunks = kWriteSize / kChunkSize;           // full MiB chunks
  const string   chunk(kChunkSize, 'A');

  cout << "\n--- Step 1: write head object '" << oid
       << "' (" << kWriteSize << " bytes, osd_max_write_size="
       << max_write_mib << " MiB) ---" << endl;
  {
    bufferlist bl;
    for (size_t i = 0; i < kNumChunks; ++i) {
      bl.append(chunk);
    }
    r = ioctx.write_full(oid, bl);
    if (check("write_full(test)", r) < 0) {
      rados.shutdown();
      return 1;
    }
  }

  // ---- Step 2: create 4 selfmanaged snaps and one clone covering all 4 -----
  //
  // All 4 snap ids are allocated first. The write context is then updated once
  // to the full snapset (descending order, seq = highest) and a single write
  // materialises one clone object named by the highest snap id. Reads below use
  // one of the lower snap ids that maps to that same clone.
  //
  static const int kNumSnaps = 4;
  vector<snap_t> snap_ids;

  cout << "\n--- Step 2: create " << kNumSnaps << " selfmanaged snaps ---" << endl;
  for (int s = 0; s < kNumSnaps; ++s) {
    snap_t sid = 0;
    r = ioctx.selfmanaged_snap_create(&sid);
    if (check(("selfmanaged_snap_create snap" + to_string(s + 1)).c_str(), r) < 0) {
      rados.shutdown();
      return 1;
    }
    cout << "snap" << (s + 1) << " id = " << sid << endl;
    snap_ids.push_back(sid);
  }

  vector<snap_t> snapset(snap_ids.rbegin(), snap_ids.rend());
  r = ioctx.selfmanaged_snap_set_write_ctx(snapset[0], snapset);
  if (check("selfmanaged_snap_set_write_ctx (all snaps)", r) < 0) {
    rados.shutdown();
    return 1;
  }

  cout << "\n--- Step 2b: write head to create single clone covering snaps 1-"
       << kNumSnaps << " ---" << endl;
  {
    bufferlist bl;
    for (size_t i = 0; i < kNumChunks; ++i) {
      bl.append(chunk);
    }
    r = ioctx.write_full(oid, bl);
    if (check("write_full(test) single clone", r) < 0) {
      rados.shutdown();
      return 1;
    }
  }

  vector<snap_t> head_snapset;
  r = ioctx.selfmanaged_snap_set_write_ctx(0, head_snapset);
  if (check("selfmanaged_snap_set_write_ctx (head)", r) < 0) {
    rados.shutdown();
    return 1;
  }

  // ---- Steps 3 & 4: migration + snap-read loop ----------------------------
  //
  // Each iteration creates a new target pool migrating from the pool that was
  // the target of the previous iteration, then repeatedly reads one older snap
  // while migration is in progress. Head writes are intentionally omitted: the
  // bug is about redirect ordering during migration, not concurrent mutation.
  //
  static const int kMaxAttempts  = 15;
  static const int kReadsPerWave = 4;
  static const int kRetryDelayMs = 500;
  static const uint64_t kReadLen = 1;
  bool test_passed = false;

  string src_pool = pool_name;  // start from "ec"

  for (int iter = 0; iter < kNumIterations; ++iter) {
    const string tgt_pool = tgt_prefix + to_string(iter);

    // -- Step 3 ---------------------------------------------------------------
    cout << "\n=== Iteration " << iter << " ===" << endl;
    cout << "\n--- Step 3: start pool migration "
         << src_pool << " -> " << tgt_pool << " ---" << endl;

    r = mon_cmd(rados,
      "{\"prefix\":\"osd pool create\","
      " \"pool\":\"" + tgt_pool + "\","
      " \"pg_num\":8,"
      " \"migrate_from_pool\":\"" + src_pool + "\","
      " \"yes_i_really_mean_it\":true}");
    if (check(("start migration " + src_pool + " -> " + tgt_pool).c_str(), r) < 0) {
      rados.shutdown();
      return 1;
    }

    // -- Step 4 ---------------------------------------------------------------
    //
    // Read one of the lower snap ids repeatedly from the source pool while
    // migration is in progress. All snap ids map to the single clone created
    // above, but the repeated read must target a lower snap id rather than the
    // highest snap id that names the clone object. Submit a small wave of async
    // reads together so several ops can be in flight when the migration
    // watermark advances.
    //
    const int read_snap_index = 0;
    const snap_t read_snap_id = snap_ids[read_snap_index];
    cout << "\n--- Step 4: repeatedly read '" << oid
         << "' at snap" << (read_snap_index + 1) << " id=" << read_snap_id
         << " from pool '" << src_pool << "' ---" << endl;
    cout << "Instrumentation: expecting redirect only if this snap's hobject sorts"
         << " before the current migration watermark; this test cannot set that"
         << " watermark directly. Using async reads to widen the redirect/retry"
         << " window." << endl;

    IoCtx snap_ioctx;
    r = rados.ioctx_create(src_pool.c_str(), snap_ioctx);
    if (check(("ioctx_create(" + src_pool + ") read snap").c_str(), r) < 0) {
      rados.shutdown();
      return 1;
    }
    snap_ioctx.snap_set_read(read_snap_id);

    test_passed = false;
    bool saw_short_read = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      vector<unique_ptr<PendingRead>> pending_reads;
      pending_reads.reserve(kReadsPerWave);

      for (int i = 0; i < kReadsPerWave; ++i) {
        auto pending = make_unique<PendingRead>();
        pending->completion = rados.aio_create_completion();
        int submit_r = snap_ioctx.aio_read(oid, pending->completion,
                                           &pending->bl, kReadLen, 0);
        if (submit_r < 0) {
          cerr << "\nTest FAILED on read submit at iteration " << iter
               << " (attempt " << attempt << "/" << kMaxAttempts
               << ", wave index " << i << ", snap" << (read_snap_index + 1)
               << " id=" << read_snap_id << "): aio_read failed: "
               << cpp_strerror(submit_r) << " (r=" << submit_r << ")" << endl;
          test_passed = false;
          pending_reads.clear();
          goto done_attempts;
        }
        pending_reads.push_back(std::move(pending));
      }

      test_passed = true;
      for (int i = 0; i < kReadsPerWave; ++i) {
        auto& pending = pending_reads[i];
        pending->completion->wait_for_complete();
        int read_r = pending->completion->get_return_value();

        if (read_r == -ENOENT) {
          cerr << "\nTest FAILED on async read at iteration " << iter
               << " (attempt " << attempt << "/" << kMaxAttempts
               << ", wave index " << i << ", snap" << (read_snap_index + 1)
               << " id=" << read_snap_id
               << "): snap read returned -ENOENT.  The OSD incorrectly redirected"
               << " the snap read to the (empty) target pool." << endl;
          test_passed = false;
          break;
        } else if (read_r < 0) {
          cerr << "\nTest FAILED on async read at iteration " << iter
               << " (attempt " << attempt << "/" << kMaxAttempts
               << ", wave index " << i << ", snap" << (read_snap_index + 1)
               << " id=" << read_snap_id << "): snap read failed: "
               << cpp_strerror(read_r) << " (r=" << read_r << ")" << endl;
          test_passed = false;
          break;
        } else if (static_cast<uint64_t>(read_r) < kReadLen) {
          saw_short_read = true;
          cerr << "\nInstrumentation: short async snap read at iteration " << iter
               << " (attempt " << attempt << "/" << kMaxAttempts
               << ", wave index " << i << ", snap" << (read_snap_index + 1)
               << " id=" << read_snap_id << "): got " << read_r
               << " byte(s), expected at least " << kReadLen
               << ". Migration may have already moved this object family away from"
               << " the interesting redirect window." << endl;
        }
      }

      if (!test_passed) {
        break;
      }

      if (attempt < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
      }
    }

done_attempts:

    if (!test_passed) {
      // Abort the entire test on first failure.
      rados.shutdown();
      return 1;
    }

    cout << "passed " << (kMaxAttempts * kReadsPerWave) << " async reads of snap"
         << (read_snap_index + 1) << endl;
    if (saw_short_read) {
      cout << "Instrumentation: observed short reads; no redirect-induced ENOENT"
           << " was triggered during this migration window." << endl;
    } else {
      cout << "Instrumentation: all reads returned at least " << kReadLen
           << " byte(s); no redirect-induced ENOENT was triggered during this"
           << " migration window." << endl;
    }
    cout << "\nIteration " << iter << " PASSED." << endl;

    // The target of this iteration becomes the source of the next.
    src_pool = tgt_pool;

    // Wait for the migration to complete before starting the next iteration.
    // A 5-second sleep is sufficient for the small number of objects involved.
    if (iter + 1 < kNumIterations) {
      cout << "Waiting 20s for migration to complete..." << endl;
      std::this_thread::sleep_for(std::chrono::seconds(20));
    }
  }

  cout << "\nTest PASSED: snap read succeeded across all "
       << kNumIterations << " migration iteration(s)." << endl;

  rados.shutdown();
  return 0;
}
