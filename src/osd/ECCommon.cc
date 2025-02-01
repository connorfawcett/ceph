// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <iostream>
#include <sstream>
#include <fmt/ostream.h>

#include "ECCommon.h"
#include "ECInject.h"
#include "messages/MOSDECSubOpWrite.h"
#include "messages/MOSDECSubOpRead.h"
#include "ECMsgTypes.h"
#include "PGLog.h"
#include "osd_tracer.h"

#define dout_context cct
#define dout_subsys ceph_subsys_osd
#define DOUT_PREFIX_ARGS this
#undef dout_prefix
#define dout_prefix _prefix(_dout, this)

using std::dec;
using std::hex;
using std::less;
using std::list;
using std::make_pair;
using std::map;
using std::pair;
using std::ostream;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using ceph::bufferhash;
using ceph::bufferlist;
using ceph::bufferptr;
using ceph::ErasureCodeInterfaceRef;
using ceph::Formatter;

static ostream& _prefix(std::ostream *_dout, ECCommon::RMWPipeline *rmw_pipeline) {
  return rmw_pipeline->get_parent()->gen_dbg_prefix(*_dout);
}
static ostream& _prefix(std::ostream *_dout, ECCommon::ReadPipeline *read_pipeline) {
  return read_pipeline->get_parent()->gen_dbg_prefix(*_dout);
}
static ostream& _prefix(std::ostream *_dout,
			ECCommon::UnstableHashInfoRegistry *unstable_hash_info_registry) {
  // TODO: backref to ECListener?
  return *_dout;
}
static ostream& _prefix(std::ostream *_dout, struct ClientReadCompleter *read_completer);

ostream &operator<<(ostream &lhs, const ECCommon::ec_extent_t &rhs)
{
  return lhs << rhs.err << ","
	     << rhs.emap;
}

ostream &operator<<(ostream &lhs, const ECCommon::shard_read_t &rhs)
{
  return lhs << "shard_read_t(extents=[" << rhs.extents << "]"
             << ", zero_pad=" << rhs.zero_pad
	     << ", subchunk=" << rhs.subchunk
	     << ")";
}

ostream &operator<<(ostream &lhs, const ECCommon::read_request_t &rhs)
{
  return lhs << "read_request_t(to_read=[" << rhs.to_read << "]"
             << ", flags=" << rhs.flags
             << ", shard_want_to_read=" << rhs.shard_want_to_read
	     << ", shard_reads=" << rhs.shard_reads
	     << ", want_attrs=" << rhs.want_attrs
	     << ")";
}

ostream &operator<<(ostream &lhs, const ECCommon::read_result_t &rhs)
{
  lhs << "read_result_t(r=" << rhs.r
      << ", errors=" << rhs.errors;
  if (rhs.attrs) {
    lhs << ", attrs=" << *(rhs.attrs);
  } else {
    lhs << ", noattrs";
  }
  return lhs << ", buffers_read=" << rhs.buffers_read << ")";
}

ostream &operator<<(ostream &lhs, const ECCommon::ReadOp &rhs)
{
  lhs << "ReadOp(tid=" << rhs.tid;
#ifndef WITH_SEASTAR
  if (rhs.op && rhs.op->get_req()) {
    lhs << ", op=";
    rhs.op->get_req()->print(lhs);
  }
#endif
  return lhs << ", to_read=" << rhs.to_read
	     << ", complete=" << rhs.complete
	     << ", priority=" << rhs.priority
	     << ", obj_to_source=" << rhs.obj_to_source
	     << ", source_to_obj=" << rhs.source_to_obj
	     << ", in_progress=" << rhs.in_progress
             << ", debug_log=" << rhs.debug_log
             << ")";
}

void ECCommon::ReadOp::dump(Formatter *f) const
{
  f->dump_unsigned("tid", tid);
#ifndef WITH_SEASTAR
  if (op && op->get_req()) {
    f->dump_stream("op") << *(op->get_req());
  }
#endif
  f->dump_stream("to_read") << to_read;
  f->dump_stream("complete") << complete;
  f->dump_int("priority", priority);
  f->dump_stream("obj_to_source") << obj_to_source;
  f->dump_stream("source_to_obj") << source_to_obj;
  f->dump_stream("in_progress") << in_progress;
}

ostream &operator<<(ostream &lhs, const ECCommon::RMWPipeline::Op &rhs)
{
  lhs << "Op(" << rhs.hoid
      << " v=" << rhs.version
      << " tt=" << rhs.trim_to
      << " tid=" << rhs.tid
      << " reqid=" << rhs.reqid;
#ifndef WITH_SEASTAR
  if (rhs.client_op && rhs.client_op->get_req()) {
    lhs << " client_op=";
    rhs.client_op->get_req()->print(lhs);
  }
#endif
  lhs << " pg_committed_to=" << rhs.pg_committed_to
      << " temp_added=" << rhs.temp_added
      << " temp_cleared=" << rhs.temp_cleared
      << " remote_read_result=" << rhs.remote_shard_extent_map
      << " pending_commits=" << rhs.pending_commits
      << " plan.to_read=" << rhs.plan
      << ")";
  return lhs;
}

void ECCommon::ReadPipeline::complete_read_op(ReadOp &&rop)
{
  dout(20) << __func__ << " completing " << rop << dendl;
  map<hobject_t, read_request_t>::iterator req_iter =
    rop.to_read.begin();
  map<hobject_t, read_result_t>::iterator resiter =
    rop.complete.begin();
  ceph_assert(rop.to_read.size() == rop.complete.size());
  for (; req_iter != rop.to_read.end(); ++req_iter, ++resiter) {
    rop.on_complete->finish_single_request(
      req_iter->first,
      std::move(resiter->second),
      req_iter->second);
  }
  ceph_assert(rop.on_complete);
  std::move(*rop.on_complete).finish(rop.priority);
  rop.on_complete = nullptr;
  // if the read op is over. clean all the data of this tid.
  for (set<pg_shard_t>::iterator iter = rop.in_progress.begin();
    iter != rop.in_progress.end();
    iter++) {
    shard_to_read_map[*iter].erase(rop.tid);
  }
  rop.in_progress.clear();
  tid_to_read_map.erase(rop.tid);
}

void ECCommon::ReadPipeline::on_change()
{
  for (map<ceph_tid_t, ReadOp>::iterator i = tid_to_read_map.begin();
       i != tid_to_read_map.end();
       ++i) {
    dout(10) << __func__ << ": cancelling " << i->second << dendl;
  }
  tid_to_read_map.clear();
  shard_to_read_map.clear();
  in_progress_client_reads.clear();
}

void ECCommon::ReadPipeline::get_all_avail_shards(
  const hobject_t &hoid,
  set<int> &have,
  map<shard_id_t, pg_shard_t> &shards,
  bool for_recovery,
  const std::optional<set<pg_shard_t>>& error_shards)
{
  for (auto &&pg_shard : get_parent()->get_acting_shards()) {
    dout(10) << __func__ << ": checking acting " << pg_shard << dendl;
    const pg_missing_t &missing = get_parent()->get_shard_missing(pg_shard);
    if (error_shards && error_shards->contains(pg_shard)) {
      continue;
    }
    const shard_id_t &shard = pg_shard.shard;
    if (cct->_conf->bluestore_debug_inject_read_err &&
        ECInject::test_read_error1(ghobject_t(hoid, ghobject_t::NO_GEN, shard))) {
      dout(0) << __func__ << " Error inject - Missing shard " << shard << dendl;
      continue;
    }
    if (!missing.is_missing(hoid)) {
      ceph_assert(!have.count(shard));
      have.insert(shard);
      ceph_assert(!shards.count(shard));
      shards.insert(make_pair(shard, pg_shard));
    }
  }

  if (for_recovery) {
    for (auto &&pg_shard : get_parent()->get_backfill_shards()) {
      if (error_shards && error_shards->contains(pg_shard))
	continue;
      const shard_id_t &shard = pg_shard.shard;
      if (have.count(shard)) {
	ceph_assert(shards.count(shard));
	continue;
      }
      dout(10) << __func__ << ": checking backfill " << pg_shard << dendl;
      ceph_assert(!shards.count(shard));
      const pg_info_t &info = get_parent()->get_shard_info(pg_shard);
      const pg_missing_t &missing = get_parent()->get_shard_missing(pg_shard);
      if (hoid < info.last_backfill && !missing.is_missing(hoid)) {
	have.insert(shard);
	shards.insert(make_pair(shard, pg_shard));
      }
    }

    map<hobject_t, set<pg_shard_t>>::const_iterator miter =
      get_parent()->get_missing_loc_shards().find(hoid);
    if (miter != get_parent()->get_missing_loc_shards().end()) {
      for (auto &&pg_shard : miter->second) {
	dout(10) << __func__ << ": checking missing_loc " << pg_shard << dendl;
	auto m = get_parent()->maybe_get_shard_missing(pg_shard);
	if (m) {
	  ceph_assert(!(*m).is_missing(hoid));
	}
	if (error_shards && error_shards->contains(pg_shard))
	  continue;
	have.insert(pg_shard.shard);
	shards.insert(make_pair(pg_shard.shard, pg_shard));
      }
    }
  }
}

int ECCommon::ReadPipeline::get_min_avail_to_read_shards(
  const hobject_t &hoid,
  bool for_recovery,
  bool do_redundant_reads,
  read_request_t &read_request,
  const std::optional<set<pg_shard_t>>& error_shards) {
  // Make sure we don't do redundant reads for recovery
  ceph_assert(!for_recovery || !do_redundant_reads);

  if (read_request.object_size == 0) {
    dout(10) << __func__ << " empty read" << dendl;
    return 0;
  }

  set<int> have;
  map<shard_id_t, pg_shard_t> shards;

  get_all_avail_shards(hoid, have, shards, for_recovery, error_shards);

  map<int, vector<pair<int, int>>> need;
  set<int> want;

  for (auto &&[shard, _] : read_request.shard_want_to_read) {
    want.insert(shard);
  }

  int r = ec_impl->minimum_to_decode(want, have, &need);
  if (r < 0) {
    dout(20) << "minimum_to_decode_failed r: " << r <<"want: " << want
      << " have: " << have << " need: " << need << dendl;
    return r;
  }

  if (do_redundant_reads) {
    vector<pair<int, int>> subchunks_list;
    subchunks_list.push_back(make_pair(0, ec_impl->get_sub_chunk_count()));
    for (auto &&i: have) {
      need[i] = subchunks_list;
    }
  }

  extent_set extra_extents;
  ECUtil::shard_extent_set_t read_mask;
  ECUtil::shard_extent_set_t zero_mask;

  sinfo.ro_size_to_read_mask(read_request.object_size, read_mask);
  sinfo.ro_size_to_zero_mask(read_request.object_size, zero_mask);

  /* First deal with missing shards */
  for (auto &&[shard, extent_set] : read_request.shard_want_to_read) {
    /* Work out what extra extents we need to read on each shard. If do
     * redundant reads is set, then we want to have the same reads on
     * every extent. Otherwise, we need to read every shard only if the
     * necessary shard is missing.
     */
    if (!have.contains(shard) || do_redundant_reads) {
      extra_extents.union_of(extent_set);
    }
  }

  for (auto &&[shard, subchunk] : need) {
    if (!have.contains(shard)) {
      continue;
    }
    pg_shard_t pg_shard = shards[shard_id_t(shard)];
    extent_set extents = extra_extents;
    shard_read_t shard_read;
    shard_read.subchunk = subchunk;

    if (read_request.shard_want_to_read.contains(shard)) {
      extents.union_of(read_request.shard_want_to_read.at(shard));
    }

    extents.align(CEPH_PAGE_SIZE);
    if (zero_mask.contains(shard)) {
      shard_read.zero_pad.intersection_of(extents, zero_mask.at(shard));
    }
    if (read_mask.contains(shard)) {
      shard_read.extents.intersection_of(extents, read_mask.at(shard));
    }

    ceph_assert(!shard_read.zero_pad.empty() || !shard_read.extents.empty());
    read_request.shard_reads[pg_shard] = shard_read;
  }

  dout(20) << __func__ << " for_recovery: " << for_recovery
    << " do_redundant_reads: " << do_redundant_reads
    << " read_request: " << read_request
    << " error_shards: " << error_shards
    << dendl;
  return 0;
}


void ECCommon::ReadPipeline::get_min_want_to_read_shards(
  const ec_align_t &to_read,
  ECUtil::shard_extent_set_t &want_shard_reads)
{
  sinfo.ro_range_to_shard_extent_set(to_read.offset, to_read.size, want_shard_reads);;
  dout(20) << __func__ << ": to_read " << to_read
	   << " read_request " << want_shard_reads << dendl;
}

int ECCommon::ReadPipeline::get_remaining_shards(
  const hobject_t &hoid,
  read_result_t &read_result,
  read_request_t &read_request,
  bool for_recovery,
  bool fast_read)
{
  set<int> have;
  map<shard_id_t, pg_shard_t> shards;
  set<pg_shard_t> error_shards;
  for (auto &&[shard, _] : read_result.errors) {
    error_shards.insert(shard);
  }

  int r = get_min_avail_to_read_shards(
    hoid,
    for_recovery,
    fast_read,
    read_request,
    error_shards);

  if (r) {
    dout(0) << __func__ << " not enough shards left to try for " << hoid
	    << " read result was " << read_result << dendl;
    return -EIO;
  }

  // Rather than repeating whole read, we can remove everything we already have.
  for (auto iter = read_request.shard_reads.begin();
      iter != read_request.shard_reads.end();) {
    auto &&[pg_shard, shard_read] = *(iter++);

    // Ignore where shard has not been read at all.
    if (read_result.processed_read_requests.contains(pg_shard.shard)) {

      shard_read.extents.subtract(read_result.processed_read_requests.at(pg_shard.shard));
      shard_read.zero_pad.subtract(read_result.processed_read_requests.at(pg_shard.shard));
      if (shard_read.extents.empty() && shard_read.zero_pad.empty()) {
        read_request.shard_reads.erase(pg_shard);
      }
    }
  }

  return 0;
}

void ECCommon::ReadPipeline::start_read_op(
  int priority,
  map<hobject_t, read_request_t> &to_read,
  OpRequestRef _op,
  bool do_redundant_reads,
  bool for_recovery,
  std::unique_ptr<ECCommon::ReadCompleter> on_complete)
{
  ceph_tid_t tid = get_parent()->get_tid();
  ceph_assert(!tid_to_read_map.count(tid));
  auto &op = tid_to_read_map.emplace(
    tid,
    ReadOp(
      priority,
      tid,
      do_redundant_reads,
      for_recovery,
      std::move(on_complete),
      _op,
      std::move(to_read))).first->second;
  dout(10) << __func__ << ": starting " << op << dendl;
  if (_op) {
#ifndef WITH_SEASTAR
    op.trace = _op->pg_trace;
#endif
    op.trace.event("start ec read");
  }
  do_read_op(op);
}

void ECCommon::ReadPipeline::do_read_op(ReadOp &op)
{
  int priority = op.priority;
  ceph_tid_t tid = op.tid;

  dout(10) << __func__ << ": starting read " << op << dendl;

  map<pg_shard_t, ECSubRead> messages;
  for (auto &&[hoid, read_request] : op.to_read) {
    bool need_attrs = read_request.want_attrs;

    for (auto &&[shard, shard_read] : read_request.shard_reads) {
      if (need_attrs) {
	messages[shard].attrs_to_read.insert(hoid);
	need_attrs = false;
      }
      messages[shard].subchunks[hoid] = shard_read.subchunk;
      op.obj_to_source[hoid].insert(shard);
      op.source_to_obj[shard].insert(hoid);
    }
    for (auto &&[shard, shard_read] : read_request.shard_reads) {
      bool empty = true;
      if (!shard_read.extents.empty()) {
        op.debug_log.emplace_back(ECUtil::READ_REQUEST, shard, shard_read.extents);
        empty = false;
      }
      if (!shard_read.zero_pad.empty()) {
        op.debug_log.emplace_back(ECUtil::ZERO_REQUEST, shard, shard_read.zero_pad);
        empty = false;
      }

      ceph_assert(!empty);
      for (auto extent = shard_read.extents.begin();
      		extent != shard_read.extents.end();
		extent++) {
	messages[shard].to_read[hoid].push_back(boost::make_tuple(extent.get_start(), extent.get_len(), read_request.flags));
      }
    }
    ceph_assert(!need_attrs);
  }

  std::vector<std::pair<int, Message*>> m;
  m.reserve(messages.size());
  for (map<pg_shard_t, ECSubRead>::iterator i = messages.begin();
       i != messages.end();
       ++i) {
    op.in_progress.insert(i->first);
    shard_to_read_map[i->first].insert(op.tid);
    i->second.tid = tid;
    MOSDECSubOpRead *msg = new MOSDECSubOpRead;
    msg->set_priority(priority);
    msg->pgid = spg_t(
      get_info().pgid.pgid,
      i->first.shard);
    msg->map_epoch = get_osdmap_epoch();
    msg->min_epoch = get_parent()->get_interval_start_epoch();
    msg->op = i->second;
    msg->op.from = get_parent()->whoami_shard();
    msg->op.tid = tid;
    if (op.trace) {
      // initialize a child span for this shard
      msg->trace.init("ec sub read", nullptr, &op.trace);
      msg->trace.keyval("shard", i->first.shard.id);
    }
    m.push_back(std::make_pair(i->first.osd, msg));
  }
  if (!m.empty()) {
    get_parent()->send_message_osd_cluster(m, get_osdmap_epoch());
  }

  dout(10) << __func__ << ": started " << op << dendl;
}

void ECCommon::ReadPipeline::get_want_to_read_shards(
  const list<ec_align_t> &to_read,
  ECUtil::shard_extent_set_t &want_shard_reads)
{
  if (sinfo.supports_partial_reads() && cct->_conf->osd_ec_partial_reads) {
    // Optimised.
    for (const auto& single_region : to_read) {
      get_min_want_to_read_shards(single_region, want_shard_reads);
    }
    return;
  }

  // Non-optimised version.
  for (unsigned int raw_shard = 0; raw_shard < sinfo.get_k(); ++raw_shard) {
    int shard = sinfo.get_shard(raw_shard);

    for (auto &&read : to_read) {
      auto offset_len = sinfo.chunk_aligned_ro_range_to_shard_ro_range(read.offset, read.size);
      want_shard_reads[shard].insert(offset_len.first, offset_len.second);
    }
  }
}

struct ClientReadCompleter : ECCommon::ReadCompleter {
  ClientReadCompleter(ECCommon::ReadPipeline &read_pipeline,
                      ECCommon::ClientAsyncReadStatus *status)
    : read_pipeline(read_pipeline),
      status(status) {}

  void finish_single_request(
    const hobject_t &hoid,
    ECCommon::read_result_t &&res,
    ECCommon::read_request_t &req) override
  {
    auto* cct = read_pipeline.cct;
    dout(20) << __func__ << " completing hoid=" << hoid
             << " res=" << res << " req="  << req << dendl;
    extent_map result;
    if (res.r != 0)
      goto out;
    ceph_assert(res.errors.empty());
#if DEBUG_EC_BUFFERS
    dout(20) << __func__ << ": before decode: " << res.buffers_read.debug_string(2048, 8) << dendl;
#endif
    /* Decode any missing buffers */
    ceph_assert(0 == res.buffers_read.decode(read_pipeline.ec_impl, req.shard_want_to_read));

#if DEBUG_EC_BUFFERS
    dout(20) << __func__ << ": after decode: " << res.buffers_read.debug_string(2048, 8) << dendl;
#endif

    for (auto &&read: req.to_read) {
      result.insert(read.offset, read.size,
        res.buffers_read.get_ro_buffer(read.offset, read.size));
    }
out:
    dout(20) << __func__ << " calling complete_object with result="
             << result << dendl;
    status->complete_object(hoid, res.r, std::move(result), std::move(res.buffers_read));
    read_pipeline.kick_reads();
  }

  void finish(int priority) && override
  {
    // NOP
  }

  ECCommon::ReadPipeline &read_pipeline;
  ECCommon::ClientAsyncReadStatus *status;
};
static ostream& _prefix(std::ostream *_dout, ClientReadCompleter *read_completer) {
  return _prefix(_dout, &read_completer->read_pipeline);
}

void ECCommon::ReadPipeline::objects_read_and_reconstruct(
  const map<hobject_t, std::list<ec_align_t>> &reads,
  bool fast_read,
  uint64_t object_size,
  GenContextURef<ECCommon::ec_extents_t &&> &&func)
{
  in_progress_client_reads.emplace_back(
    reads.size(), std::move(func));
  if (!reads.size()) {
    kick_reads();
    return;
  }

  map<hobject_t, read_request_t> for_read_op;
  for (auto &&[hoid, to_read]: reads) {
    ECUtil::shard_extent_set_t want_shard_reads;
    get_want_to_read_shards(to_read, want_shard_reads);

    read_request_t read_request(to_read, want_shard_reads, false, object_size);
    int r = get_min_avail_to_read_shards(
      hoid,
      false,
      fast_read,
      read_request);
    ceph_assert(r == 0);

    int subchunk_size =
      sinfo.get_chunk_size() / ec_impl->get_sub_chunk_count();
    dout(20) << __func__
             << " to_read=" << to_read
             << " subchunk_size=" << subchunk_size
             << " chunk_size=" << sinfo.get_chunk_size() << dendl;

    for_read_op.insert(make_pair(hoid, read_request));
  }

  start_read_op(
    CEPH_MSG_PRIO_DEFAULT,
    for_read_op,
    OpRequestRef(),
    fast_read,
    false,
    std::make_unique<ClientReadCompleter>(*this, &(in_progress_client_reads.back())));
}

void ECCommon::ReadPipeline::objects_read_and_reconstruct_for_rmw(
  map<hobject_t, read_request_t> &&to_read,
  GenContextURef<ECCommon::ec_extents_t &&> &&func)
{
  in_progress_client_reads.emplace_back(to_read.size(), std::move(func));
  if (!to_read.size()) {
    kick_reads();
    return;
  }

  map<hobject_t, set<int>> obj_want_to_read;

  map<hobject_t, read_request_t> for_read_op;
  for (auto &&[hoid, read_request]: to_read) {

    int r = get_min_avail_to_read_shards(
      hoid,
      false,
      false,
      read_request);
    ceph_assert(r == 0);

    int subchunk_size =
      sinfo.get_chunk_size() / ec_impl->get_sub_chunk_count();
    dout(20) << __func__
             << " read_request=" << read_request
             << " subchunk_size=" << subchunk_size
             << " chunk_size=" << sinfo.get_chunk_size() << dendl;

    for_read_op.insert(make_pair(hoid, read_request));
  }

  start_read_op(
    CEPH_MSG_PRIO_DEFAULT,
    for_read_op,
    OpRequestRef(),
    false,
    false,
    std::make_unique<ClientReadCompleter>(*this, &(in_progress_client_reads.back())));
}



int ECCommon::ReadPipeline::send_all_remaining_reads(
  const hobject_t &hoid,
  ReadOp &rop)
{
  // (Note cuixf) If we need to read attrs and we read failed, try to read again.
  bool want_attrs =
    rop.to_read.find(hoid)->second.want_attrs &&
    (!rop.complete.at(hoid).attrs || rop.complete.at(hoid).attrs->empty());
  if (want_attrs) {
    dout(10) << __func__ << " want attrs again" << dendl;
  }

  read_request_t &read_request = rop.to_read.at(hoid);
  // reset the old shard reads, we are going to read them again.
  read_request.shard_reads = std::map<pg_shard_t, shard_read_t>();
  int r = get_remaining_shards(hoid, rop.complete.at(hoid), read_request,
        rop.do_redundant_reads, want_attrs);

  if (r)
    return r;

  return 0;
}

void ECCommon::ReadPipeline::kick_reads()
{
  while (in_progress_client_reads.size() &&
         in_progress_client_reads.front().is_complete()) {
    in_progress_client_reads.front().run();
    in_progress_client_reads.pop_front();
  }
}

bool ec_align_t::operator==(const ec_align_t &other) const {
  return offset == other.offset && size == other.size && flags == other.flags;
}

bool ECCommon::shard_read_t::operator==(const shard_read_t &other) const {
  return extents == other.extents && subchunk == other.subchunk;
}

bool ECCommon::read_request_t::operator==(const read_request_t &other) const {
  return to_read == other.to_read &&
    flags == other.flags &&
    shard_want_to_read == other.shard_want_to_read &&
    shard_reads == other.shard_reads &&
    want_attrs == other.want_attrs;
}

void ECCommon::RMWPipeline::start_rmw(OpRef op)
{
  dout(20) << __func__ << " op=" << *op << dendl;

  ceph_assert(!tid_to_op_map.count(op->tid));
  tid_to_op_map[op->tid] = op;

  op->pending_cache_ops = op->plan.plans.size();
  for (auto &plan : op->plan.plans) {
    ECExtentCache::OpRef cache_op = extent_cache.prepare(plan.hoid,
      plan.to_read,
      plan.will_write,
      plan.orig_size,
      plan.projected_size,
      plan.invalidates_cache,
      [op](ECExtentCache::OpRef &cop)
      {
        op->cache_ready(cop->get_hoid(), cop->get_result());
      });
    op->cache_ops.emplace_back(std::move(cache_op));
  }
  extent_cache.execute(op->cache_ops);
}

void ECCommon::RMWPipeline::cache_ready(Op &op)
{
  get_parent()->apply_stats(
    op.hoid,
    op.delta_stats);

  map<shard_id_t, ObjectStore::Transaction> trans;
  for (auto &&shard : get_parent()->get_acting_recovery_backfill_shards()) {
    trans[shard.shard];
  }

  op.trace.event("start ec write");

  map<hobject_t, ECUtil::shard_extent_map_t> written;
  op.generate_transactions(
    ec_impl,
    get_parent()->get_info().pgid.pgid,
    sinfo,
    &written,
    &trans,
    get_parent()->get_dpp(),
    get_osdmap());

  dout(20) << __func__ << ": written: " << written << ", op: " << op << dendl;

  if (!sinfo.supports_ec_overwrites()) {
    for (auto &&i: op.log_entries) {
      if (i.requires_kraken()) {
        derr << __func__ << ": log entry " << i << " requires kraken"
             << " but overwrites are not enabled!" << dendl;
        ceph_abort();
      }
    }
  }

  op.remote_shard_extent_map.clear();

  ObjectStore::Transaction empty;
  bool should_write_local = false;
  ECSubWrite local_write_op;
  std::vector<std::pair<int, Message*>> messages;
  messages.reserve(get_parent()->get_acting_recovery_backfill_shards().size());
  set<pg_shard_t> backfill_shards = get_parent()->get_backfill_shards();

  if (op.version.version != 0) {
    if (oid_to_version.contains(op.hoid)) {
      ceph_assert(oid_to_version.at(op.hoid) <= op.version);
    }
    oid_to_version[op.hoid] = op.version;
  }
  for (auto &&pg_shard : get_parent()->get_acting_recovery_backfill_shards()) {
    auto iter = trans.find(pg_shard.shard);
    ceph_assert(iter != trans.end());
    if (iter->second.empty()) {
      dout(20) << " BILL: Transaction for osd." << pg_shard.osd << " shard " << iter->first << " is empty" << dendl;
    } else {
      dout(20) << " BILL: Transaction for osd." << pg_shard.osd << " shard " << iter->first << " contents ";
      Formatter *f = Formatter::create("json");
      f->open_object_section("t");
      iter->second.dump(f);
      f->close_section();
      f->flush(*_dout);
      delete f;
      *_dout << dendl;
    }
    if (op.skip_transaction(pending_roll_forward, iter->first, iter->second)) {
      // Must be an empty transaction
      ceph_assert(iter->second.empty());
      dout(20) << " BILL: Skipping transaction for osd." << iter->first << dendl;
      continue;
    }
    op.pending_commits++;
    bool should_send = get_parent()->should_send_op(pg_shard, op.hoid);
    const pg_stat_t &stats =
      (should_send || !backfill_shards.count(pg_shard)) ?
      get_info().stats :
      get_parent()->get_shard_info().find(pg_shard)->second.stats;

    ECSubWrite sop(
      get_parent()->whoami_shard(),
      op.tid,
      op.reqid,
      op.hoid,
      stats,
      should_send ? iter->second : empty,
      op.version,
      op.trim_to,
      op.pg_committed_to,
      op.log_entries,
      op.updated_hit_set_history,
      op.temp_added,
      op.temp_cleared,
      !should_send);

    ZTracer::Trace trace;
    if (op.trace) {
      // initialize a child span for this shard
      trace.init("ec sub write", nullptr, &op.trace);
      trace.keyval("shard", pg_shard.shard.id);
    }

    if (pg_shard == get_parent()->whoami_shard()) {
      should_write_local = true;
      local_write_op.claim(sop);
      } else if (cct->_conf->bluestore_debug_inject_read_err &&
                 ECInject::test_write_error1(ghobject_t(op.hoid,
                   ghobject_t::NO_GEN, pg_shard.shard))) {
        dout(0) << " Error inject - Dropping write message to shard " <<
          pg_shard.shard << dendl;
    } else {
      MOSDECSubOpWrite *r = new MOSDECSubOpWrite(sop);
      r->pgid = spg_t(get_parent()->primary_spg_t().pgid, pg_shard.shard);
      r->map_epoch = get_osdmap_epoch();
      r->min_epoch = get_parent()->get_interval_start_epoch();
      r->trace = trace;
      messages.push_back(std::make_pair(pg_shard.osd, r));
    }
  }

  if (!messages.empty()) {
    get_parent()->send_message_osd_cluster(messages, get_osdmap_epoch());
  }

  if (should_write_local) {
    handle_sub_write(
      get_parent()->whoami_shard(),
      op.client_op,
      local_write_op,
      op.trace);
  }


  for (auto &cop : op.cache_ops) {
    hobject_t &oid = cop->get_hoid();
    if (written.contains(oid)) {
      extent_cache.write_done(cop, std::move(written.at(oid)));
    } else {
      extent_cache.write_done(cop, ECUtil::shard_extent_map_t(&sinfo));
    }
  }
}

struct ECDummyOp : ECCommon::RMWPipeline::Op {
  void generate_transactions(
    ceph::ErasureCodeInterfaceRef &ec_impl,
    pg_t pgid,
    const ECUtil::stripe_info_t &sinfo,
    map<hobject_t, ECUtil::shard_extent_map_t>* written,
    std::map<shard_id_t, ObjectStore::Transaction> *transactions,
    DoutPrefixProvider *dpp,
    const OSDMapRef& osdmap) final
  {
    // NOP, as -- in constrast to ECClassicalOp -- there is no
    // transaction involved
  }
  bool skip_transaction(
      std::set<shard_id_t>& pending_roll_forward,
      shard_id_t shard,
      ceph::os::Transaction& transaction) final
  {
    return !pending_roll_forward.erase(shard);
  }
};

void ECCommon::RMWPipeline::finish_rmw(OpRef &op)
{
  dout(20) << __func__ << " op=" << *op << dendl;
  if (op->pg_committed_to > completed_to)
    completed_to = op->pg_committed_to;
  if (op->version > committed_to)
    committed_to = op->version;

  op->cache_ops.clear();

  if (get_osdmap()->require_osd_release >= ceph_release_t::kraken && extent_cache.idle()) {
    if (op->version > get_parent()->get_log().get_can_rollback_to()) {
      int transactions_since_last_idle = extent_cache.get_and_reset_counter();
      dout(20) << __func__ << " version=" << op->version << " ec_counter=" << transactions_since_last_idle << dendl;
      // submit a dummy, transaction-empty op to kick the rollforward
      auto tid = get_parent()->get_tid();
      auto nop = std::make_shared<ECDummyOp>();
      nop->hoid = op->hoid;
      nop->trim_to = op->trim_to;
      nop->pg_committed_to = op->version;
      nop->tid = tid;
      nop->reqid = op->reqid;
      nop->pending_cache_ops = 1;
      nop->pipeline = this;

      tid_to_op_map[tid] = nop;

      /* This does not need to go through the extent cache at all. The cache
       * is idle (we checked above) and this IO never blocks for the kernel,
       * so its just a waste of effort.
       */
      nop->cache_ready(nop->hoid, ECUtil::shard_extent_map_t(&sinfo));
    }
  }

  tid_to_op_map.erase(op->tid);
}

void ECCommon::RMWPipeline::on_change()
{
  dout(10) << __func__ << dendl;

  completed_to = eversion_t();
  committed_to = eversion_t();
  extent_cache.on_change();
  tid_to_op_map.clear();
  oid_to_version.clear();
}

void ECCommon::RMWPipeline::on_change2() {
  extent_cache.on_change2();
}

void ECCommon::RMWPipeline::call_write_ordered(std::function<void(void)> &&cb) {
  extent_cache.add_on_write(std::move(cb));
}

ECUtil::HashInfoRef ECCommon::UnstableHashInfoRegistry::maybe_put_hash_info(
  const hobject_t &hoid,
  ECUtil::HashInfo &&hinfo)
{
  return registry.lookup_or_create(hoid, hinfo);
}

ECUtil::HashInfoRef ECCommon::UnstableHashInfoRegistry::get_hash_info(
  const hobject_t &hoid,
  bool create,
  const map<string, bufferlist, less<>>& attrs,
  uint64_t size)
{
  dout(10) << __func__ << ": Getting attr on " << hoid << dendl;
  ECUtil::HashInfoRef ref = registry.lookup(hoid);
  if (!ref) {
    dout(10) << __func__ << ": not in cache " << hoid << dendl;
    ECUtil::HashInfo hinfo(ec_impl->get_chunk_count());
    bufferlist bl;
    map<string, bufferlist>::const_iterator k = attrs.find(ECUtil::get_hinfo_key());
    if (k == attrs.end()) {
      dout(5) << __func__ << " " << hoid << " missing hinfo attr" << dendl;
    } else {
      bl = k->second;
    }
    if (bl.length() > 0) {
      auto bp = bl.cbegin();
      try {
        decode(hinfo, bp);
      } catch(...) {
        dout(0) << __func__ << ": Can't decode hinfo for " << hoid << dendl;
        return ECUtil::HashInfoRef();
      }
      if (hinfo.get_total_chunk_size() != size) {
        dout(0) << __func__ << ": Mismatch of total_chunk_size "
      		       << hinfo.get_total_chunk_size() << dendl;
        return ECUtil::HashInfoRef();
      } else {
        create = true;
      }
    } else if (size == 0) { // If empty object and no hinfo, create it
      create = true;
    }
    if (create) {
      ref = registry.lookup_or_create(hoid, hinfo);
    }
  }
  return ref;
}
