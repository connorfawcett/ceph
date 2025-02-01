// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#include <gtest/gtest.h>
#include "osd/ECExtentCache.h"

using namespace std;
using namespace ECExtentCache;
using namespace ECUtil;

shard_extent_map_t imap_from_vector(vector<vector<pair<uint64_t, uint64_t>>> &&in, stripe_info_t const *sinfo)
{
  shard_extent_map_t out(sinfo);
  for (int shard = 0; shard < (int)in.size(); shard++) {
    for (auto &&tup: in[shard]) {
      bufferlist bl;
      bl.append_zero(tup.second);
      out.insert_in_shard(shard, tup.first, bl);
    }
  }
  return out;
}

shard_extent_map_t imap_from_iset(const shard_extent_set_t &sset, stripe_info_t *sinfo)
{
  shard_extent_map_t out(sinfo);

  for (auto &&[shard, set]: sset) {
    for (auto &&iter: set) {
      bufferlist bl;
      bl.append_zero(iter.second);
      out.insert_in_shard(shard, iter.first, bl);
    }
  }
  return out;
}

shard_extent_set_t iset_from_vector(vector<vector<pair<uint64_t, uint64_t>>> &&in)
{
  shard_extent_set_t out;
  for (int shard = 0; shard < (int)in.size(); shard++) {
    for (auto &&tup: in[shard]) {
      out[shard].insert(tup.first, tup.second);
    }
  }
  return out;
}

struct Client : public BackendRead
{
  hobject_t oid;
  stripe_info_t sinfo;
  LRU lru;
  PG pg;
  optional<shard_extent_set_t> active_reads;
  optional<shard_extent_map_t> result;

  Client(uint64_t chunk_size, int k, int m, uint64_t cache_size) :
    sinfo(k, chunk_size * k, m, vector<int>(0)),
    lru(cache_size), pg(*this, lru, sinfo) {};

  void backend_read(hobject_t _oid, const shard_extent_set_t& request,
    uint64_t object_size) override  {
    ceph_assert(oid == _oid);
    active_reads = request;
  }

  void cache_ready(hobject_t& _oid, const optional<shard_extent_map_t>& _result)
  {
    ceph_assert(oid == _oid);

    result = _result;
  }

  void complete_read()
  {
    auto reads_done = imap_from_iset(*active_reads, &sinfo);
    active_reads.reset(); // set before done, as may be called back.
    pg.read_done(oid, std::move(reads_done));
  }

  void complete_write(OpRef &op)
  {
    shard_extent_map_t emap = imap_from_iset(op->get_writes(), &sinfo);
    //Fill in the parity. Parity correctness does not matter to the cache.
    emap.insert_parity_buffers();
    result.reset();
    pg.write_done(op, std::move(emap));
  }

  void commit_write(OpRef &op)
  {
    pg.complete(op);
  }

  void kick_cache() {
    pg.read_done(oid, shard_extent_map_t(&sinfo));
  }
};

TEST(ECExtentCache, simple_write)
{
  Client cl(32, 2, 1, 64);
  {
    auto to_read = iset_from_vector( {{{0, 2}}, {{0, 2}}});
    auto to_write = iset_from_vector({{{0, 10}}, {{0, 10}}});

    /*    OpRef request(hobject_t const &oid,
      std::optional<std::shard_extent_set_t> const &to_read,
      std::shard_extent_set_t const &write,
      uint64_t orig_size,
      uint64_t projected_size,
      CacheReadyCb &&ready_cb)
      */

    OpRef op = cl.pg.prepare(cl.oid, to_read, to_write, 10, 10,
      [&cl](OpRef &cache_op)
      {
        cl.cache_ready(cl.oid, cache_op->get_result());
      });
    cl.pg.execute(op);
    ASSERT_EQ(to_read, cl.active_reads);
    ASSERT_FALSE(cl.result);
    cl.complete_read();

    ASSERT_FALSE(cl.active_reads);
    ASSERT_EQ(to_read, cl.result->get_extent_set());
    cl.complete_write(op);

    ASSERT_FALSE(cl.active_reads);
    ASSERT_FALSE(cl.result);
    cl.commit_write(op);
  }

  // Repeating the same read should complete without a backend read..
  // NOTE: This test is broken because the LRU is currently disabled.
  {
    auto to_read = iset_from_vector( {{{0, 2}}, {{0, 2}}});
    auto to_write = iset_from_vector({{{0, 10}}, {{0, 10}}});
    OpRef op = cl.pg.prepare(cl.oid, to_read, to_write, 10, 10,
      [&cl](OpRef &cache_op)
      {
        cl.cache_ready(cl.oid, cache_op->get_result());
      });
    cl.pg.execute(op);
    // FIXME: LRU Cache Disabled.
    ASSERT_TRUE(cl.active_reads);
    cl.complete_read();

    ASSERT_TRUE(cl.result);
    ASSERT_EQ(to_read, cl.result->get_extent_set());
    cl.complete_write(op);
    cl.commit_write(op);
  }

  // Perform a read overlapping with the previous write, but not hte previous read.
  // This should not result in any backend reads, since the cache can be honoured
  // from the previous write.
  {
    auto to_read = iset_from_vector( {{{2, 2}}, {{2, 2}}});
    auto to_write = iset_from_vector({{{0, 10}}, {{0, 10}}});
    OpRef op = cl.pg.prepare(cl.oid, to_read, to_write, 10, 10,
      [&cl](OpRef &cache_op)
      {
        cl.cache_ready(cl.oid, cache_op->get_result());
      });
    cl.pg.execute(op);
    // FIXME: LRU Cache Disabled.
    ASSERT_TRUE(cl.active_reads);
    cl.complete_read();

    ASSERT_EQ(to_read, cl.result->get_extent_set());
    cl.complete_write(op);
    cl.commit_write(op);
  }
}

TEST(ECExtentCache, sequential_appends) {
  Client cl(32, 2, 1, 32);

  auto to_write1 = iset_from_vector({{{0, 10}}});

  // The first write...
  OpRef op1 = cl.pg.prepare(cl.oid, nullopt, to_write1, 0, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op1);

  // Write should have been honoured immediately.
  ASSERT_TRUE(cl.result);
  auto to_write2 = iset_from_vector({{{10, 10}}});
  cl.complete_write(op1);
  ASSERT_FALSE(cl.result);

  // The first write...
  OpRef op2 = cl.pg.prepare(cl.oid, nullopt, to_write1, 10, 20,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op2);

  ASSERT_TRUE(cl.result);
  cl.complete_write(op2);

}

TEST(ECExtentCache, multiple_writes)
{
  Client cl(32, 2, 1, 32);

  auto to_read1 = iset_from_vector( {{{0, 2}}});
  auto to_write1 = iset_from_vector({{{0, 10}}});

  // This should drive a request for this IO, which we do not yet honour.
  OpRef op1 = cl.pg.prepare(cl.oid, to_read1, to_write1, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op1);
  ASSERT_EQ(to_read1, cl.active_reads);
  ASSERT_FALSE(cl.result);

  // Perform another request. We should not see any change in the read requests.
  auto to_read2 = iset_from_vector( {{{8, 4}}});
  auto to_write2 = iset_from_vector({{{10, 10}}});
  OpRef op2 = cl.pg.prepare(cl.oid, to_read2, to_write2, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op2);
  ASSERT_EQ(to_read1, cl.active_reads);
  ASSERT_FALSE(cl.result);

  // Perform another request, this to check that reads are coalesced.
  auto to_read3 = iset_from_vector( {{{32, 6}}});
  auto to_write3 = iset_from_vector({{}, {{40, 0}}});
  OpRef op3 = cl.pg.prepare(cl.oid, to_read3, to_write3, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op3);
  ASSERT_EQ(to_read1, cl.active_reads);
  ASSERT_FALSE(cl.result);

  // Finally op4, with no reads.
  auto to_write4 = iset_from_vector({{{20, 10}}});
  OpRef op4 = cl.pg.prepare(cl.oid, nullopt, to_write4, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op4);
  ASSERT_EQ(to_read1, cl.active_reads);
  ASSERT_FALSE(cl.result);

  // Completing the first read will allow the first write and start a batched read.
  // Note that the cache must not read what was written in op 1.
  cl.complete_read();
  auto expected_read = iset_from_vector({{{10,2}, {32,6}}});
  ASSERT_EQ(expected_read, cl.active_reads);
  ASSERT_EQ(to_read1, cl.result->get_extent_set());
  cl.complete_write(op1);

  // The next write requires some more reads, so should not occur.
  ASSERT_FALSE(cl.result);

  // All reads complete, this should allow for op2 to be ready.
  cl.complete_read();
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read2, cl.result->get_extent_set());
  cl.complete_write(op2);
  // In the real code, this happens inside the complete read callback. Here
  // we need to kick the statemachine.
  cl.kick_cache();

  // Since no further reads are required op3 and op4 should occur immediately.
  ASSERT_TRUE(cl.result);
  ASSERT_EQ(to_read3, cl.result->get_extent_set());
  cl.complete_write(op3);

  // No write data for op 4.
  ASSERT_FALSE(cl.result);
  cl.complete_write(op4);

  cl.commit_write(op1);
  cl.commit_write(op2);
  cl.commit_write(op3);
  cl.commit_write(op4);
}

TEST(ECExtentCache, multiple_lru_frees)
{
  // LRU is currently disabled.
  GTEST_SKIP();
  Client cl(32, 2, 1, 64);

  /* Perform two writes which fill up the cache (over-fill) */
  auto to_read = iset_from_vector({{{0, 32}}});
  auto to_write = iset_from_vector({{{0, 32}}});
  OpRef op1 = cl.pg.prepare(cl.oid, to_read, to_write, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });  cl.complete_read();
  cl.pg.execute(op1);
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read, cl.result->get_extent_set());
  cl.complete_write(op1);


  auto to_read2 = iset_from_vector({{{32, 32}}});
  auto to_write2 = iset_from_vector({{{32, 32}}});
  OpRef op2 = cl.pg.prepare(cl.oid, to_read2, to_write2, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });  cl.complete_read();
  cl.pg.execute(op2);
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read2, cl.result->get_extent_set());
  cl.complete_write(op2);

  // Since we have not commited the op, both cache lines are cached.
  auto to_read3 = iset_from_vector({{{10, 2}, {40,2}}});
  auto to_write3 = iset_from_vector({{{0, 64}}});
  OpRef op3 = cl.pg.prepare(cl.oid, to_read3, to_write3, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op3);
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read3, cl.result->get_extent_set());
  cl.complete_write(op3);

  // commit the first two writes... The third IO should still be
  // locking the cache lines.
  cl.commit_write(op1);
  cl.commit_write(op2);

  // Since we have not commited the op, both cache lines are cached.
  auto to_read4 = iset_from_vector({{{12, 2}, {42,2}}});
  auto to_write4 = iset_from_vector({{{0, 64}}});
  OpRef op4 = cl.pg.prepare(cl.oid, to_read4, to_write4, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });
  cl.pg.execute(op4);
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read4, cl.result->get_extent_set());
  cl.complete_write(op4);

  // commit everything, this should release the first cache line.
  cl.commit_write(op3);
  cl.commit_write(op4);

  // Now the same read as before will have to request the first cache line.

  op4 = cl.pg.prepare(cl.oid, to_read4, to_write4, 10, 10,
    [&cl](OpRef &cache_op)
    {
      cl.cache_ready(cl.oid, cache_op->get_result());
    });  auto ref = iset_from_vector({{{12, 2}}});
  cl.pg.execute(op4);
  ASSERT_EQ(ref, cl.active_reads);
  ASSERT_FALSE(cl.result);
  cl.complete_read();
  ASSERT_FALSE(cl.active_reads);
  ASSERT_EQ(to_read4, cl.result->get_extent_set());
  cl.complete_write(op4);
  cl.commit_write(op4);
}