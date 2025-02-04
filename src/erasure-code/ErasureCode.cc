// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph distributed storage system
 *
 * Copyright (C) 2014 Cloudwatt <libre.licensing@cloudwatt.com>
 * Copyright (C) 2014 Red Hat <contact@redhat.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 * 
 */

#include <algorithm>
#include <cerrno>

#include "ErasureCode.h"

#include "common/strtol.h"
#include "include/buffer.h"
#include "crush/CrushWrapper.h"
#include "osd/osd_types.h"

#define DEFAULT_RULE_ROOT "default"
#define DEFAULT_RULE_FAILURE_DOMAIN "host"

using std::make_pair;
using std::map;
using std::ostream;
using std::pair;
using std::set;
using std::string;
using std::vector;

using ceph::bufferlist;

namespace ceph {
  const unsigned ErasureCode::SIMD_ALIGN = 64;

  int ErasureCode::init(
    ErasureCodeProfile &profile,
    std::ostream *ss)
  {
    int err = 0;
    err |= to_string("crush-root", profile,
                     &rule_root,
                     DEFAULT_RULE_ROOT, ss);
    err |= to_string("crush-failure-domain", profile,
                     &rule_failure_domain,
                     DEFAULT_RULE_FAILURE_DOMAIN, ss);
    err |= to_int("crush-osds-per-failure-domain", profile,
                  &rule_osds_per_failure_domain,
                  "0", ss);
    err |= to_int("crush-num-failure-domains", profile,
                  &rule_num_failure_domains,
                  "0", ss);
    err |= to_string("crush-device-class", profile,
                     &rule_device_class,
                     "", ss);
    if (err)
      return err;
    _profile = profile;
    return 0;
  }

  int ErasureCode::create_rule(
    const std::string &name,
    CrushWrapper &crush,
    std::ostream *ss) const
  {
    if (rule_osds_per_failure_domain <= 1) {
      return crush.add_simple_rule(
        name,
        rule_root,
        rule_failure_domain,
        rule_num_failure_domains,
        rule_device_class,
        "indep",
        pg_pool_t::TYPE_ERASURE,
        ss);
    } else {
      if (rule_num_failure_domains < 1)  {
        if (ss) {
          *ss << "crush-num-failure-domains " << rule_num_failure_domains
              << " must be >= 1 if crush-osds-per-failure-domain specified";
          return -EINVAL;
        }
      }
      return crush.add_indep_multi_osd_per_failure_domain_rule(
        name,
        rule_root,
        rule_failure_domain,
        rule_num_failure_domains,
        rule_osds_per_failure_domain,
        rule_device_class,
        ss);
    }
  }

  int ErasureCode::sanity_check_k_m(int k, int m, ostream *ss)
  {
    if (k < 2) {
      *ss << "k=" << k << " must be >= 2" << std::endl;
      return -EINVAL;
    }
    if (m < 1) {
      *ss << "m=" << m << " must be >= 1" << std::endl;
      return -EINVAL;
    }
    return 0;
  }

  int ErasureCode::chunk_index(unsigned int i) const
  {
    return chunk_mapping.size() > i ? chunk_mapping[i] : i;
  }

  int ErasureCode::_minimum_to_decode(const shard_id_set &want_to_read,
                                     const shard_id_set &available_chunks,
                                     shard_id_set *minimum)
  {
    if (available_chunks.includes(want_to_read)) {
      *minimum = want_to_read;
                 } else {
                   unsigned int k = get_data_chunk_count();
                   if (available_chunks.size() < (unsigned)k)
                     return -EIO;
                   shard_id_set::const_iterator i;
                   unsigned j;
                   for (i = available_chunks.begin(), j = 0; j < (unsigned)k; ++i, j++)
                     minimum->insert(*i);
                 }
    return 0;
  }

  int ErasureCode::minimum_to_decode(const shard_id_set &want_to_read,
                                     const shard_id_set &available_chunks,
                                     shard_id_map<vector<pair<int, int>>> *minimum)
  {
    shard_id_set minimum_shard_ids;
    int r = _minimum_to_decode(want_to_read, available_chunks, &minimum_shard_ids);
    if (r != 0) {
      return r;
    }
    vector<pair<int, int>> default_subchunks;
    default_subchunks.push_back(make_pair(0, get_sub_chunk_count()));
    for (auto &&id : minimum_shard_ids) {
      minimum->emplace(id, default_subchunks);
    }
    return 0;
  }

  int ErasureCode::minimum_to_decode(const std::set<int> &want_to_read,
                                  const std::set<int> &available,
                                  std::map<int, std::vector<std::pair<int, int>>> *minimum)
  {
    const shard_id_set _want_to_read(want_to_read);
    const shard_id_set _available(available);
    shard_id_map<vector<pair<int, int>>> _minimum(get_chunk_count());
    int r = minimum_to_decode(_want_to_read, _available, &_minimum);

    for (auto &&it : _minimum) {
      minimum->emplace(std::move(it));
    }
    return r;
  }

  int ErasureCode::minimum_to_decode_with_cost(const shard_id_set &want_to_read,
                                               const shard_id_map<int> &available,
                                               shard_id_set *minimum)
  {
    shard_id_set available_chunks;
    for (shard_id_map<int>::const_iterator i = available.begin();
         i != available.end();
         ++i)
      available_chunks.insert(i->first);
    return _minimum_to_decode(want_to_read, available_chunks, minimum);
  }

  int ErasureCode::encode_prepare(const bufferlist &raw,
                                  shard_id_map<bufferlist> &encoded) const
  {
     unsigned int k = get_data_chunk_count();
     unsigned int m = get_chunk_count() - k;
     unsigned blocksize = get_chunk_size(raw.length());
     unsigned padded_chunks = k - raw.length() / blocksize;
     bufferlist prepared = raw;

    for (unsigned int i = 0; i < k - padded_chunks; i++) {
      bufferlist &chunk = encoded[chunk_index(i)];
      chunk.substr_of(prepared, i * blocksize, blocksize);
      chunk.rebuild_aligned_size_and_memory(blocksize, SIMD_ALIGN);
      ceph_assert(chunk.is_contiguous());
    }
    if (padded_chunks) {
      unsigned remainder = raw.length() - (k - padded_chunks) * blocksize;
      bufferptr buf(buffer::create_aligned(blocksize, SIMD_ALIGN));

      raw.begin((k - padded_chunks) * blocksize).copy(remainder, buf.c_str());
      buf.zero(remainder, blocksize - remainder);
      encoded[chunk_index(k-padded_chunks)].push_back(std::move(buf));

      for (unsigned int i = k - padded_chunks + 1; i < k; i++) {
        bufferptr buf(buffer::create_aligned(blocksize, SIMD_ALIGN));
        buf.zero();
        encoded[chunk_index(i)].push_back(std::move(buf));
      }
    }
    for (unsigned int i = k; i < k + m; i++) {
      bufferlist &chunk = encoded[chunk_index(i)];
      chunk.push_back(buffer::create_aligned(blocksize, SIMD_ALIGN));
    }

    return 0;
  }

  int ErasureCode::encode(const shard_id_set &want_to_encode,
                          const bufferlist &in,
                          shard_id_map<bufferlist> *encoded)
  {
    unsigned int k = get_data_chunk_count();
    unsigned int m = get_chunk_count() - k;

    if (!encoded || !encoded->empty()){
      return -EINVAL;
    }

    int err = encode_prepare(in, *encoded);
    if (err)
      return err;

    shard_id_map<bufferptr> in_shards(get_chunk_count());
    shard_id_map<bufferptr> out_shards(get_chunk_count());

    for (int raw_shard=0; raw_shard < get_chunk_count(); raw_shard++) {
      shard_id_t shard = chunk_index(raw_shard);
      if (!encoded->contains(shard)) continue;

      auto bp = encoded->at(shard).begin().get_current_ptr();
      if (raw_shard < k) in_shards[shard] = bp;
      else out_shards[shard] = bp;
    }

    encode_chunks(in_shards, out_shards);

    for (unsigned int i = 0; i < k + m; i++) {
      if (want_to_encode.count(i) == 0)
        encoded->erase(i);
    }

    return 0;
  }

  int ErasureCode::encode(const std::set<int> &want_to_encode,
             const bufferlist &in,
             std::map<int, bufferlist> *encoded)
  {
    shard_id_set _want_to_encode(want_to_encode);
    shard_id_map<bufferlist> _encoded(get_chunk_count());
    int r = encode(_want_to_encode, in, &_encoded);

    for (auto &&it : _encoded) {
      encoded->emplace(std::move(it));
    }

    return r;
  }

// TODO: write this function! Although every plugin has its own version to override this one
int ErasureCode::encode_chunks(const shard_id_map<bufferptr> &in,
                               shard_id_map<bufferptr> &out)
{
  return 0;
};

int ErasureCode::_decode(const shard_id_set &want_to_read,
			 const shard_id_map<bufferlist> &chunks,
			 shard_id_map<bufferlist> *decoded)
{
  vector<int> have;

  if (!decoded || !decoded->empty()){
    return -EINVAL;
  }
  if (!want_to_read.empty() && chunks.empty()) {
    return -1;
  }

  have.reserve(chunks.size());
  for (shard_id_map<bufferlist>::const_iterator i = chunks.begin();
       i != chunks.end();
       ++i) {
    have.push_back(i->first);
  }
  if (includes(
	have.begin(), have.end(), want_to_read.begin(), want_to_read.end())) {
    for (shard_id_set::const_iterator i = want_to_read.begin();
	 i != want_to_read.end();
	 ++i) {
      (*decoded)[*i] = chunks.find(*i)->second;
    }
    return 0;
  }
  unsigned int k = get_data_chunk_count();
  unsigned int m = get_chunk_count() - k;
  unsigned blocksize = (*chunks.begin()).second.length();
  shard_id_set erasures;
  for (unsigned int i = 0; i < k + m; i++) {
    if (chunks.find(i) == chunks.end()) {
      bufferlist tmp;
      bufferptr ptr(buffer::create_aligned(blocksize, SIMD_ALIGN));
      tmp.push_back(ptr);
      tmp.claim_append((*decoded)[i]);
      (*decoded)[i].swap(tmp);
      erasures.insert(i);
    } else {
      (*decoded)[i] = chunks.find(i)->second;
      (*decoded)[i].rebuild_aligned(SIMD_ALIGN);
    }
  }
  shard_id_map<bufferptr> in(get_chunk_count());
  shard_id_map<bufferptr> out(get_chunk_count());
  for (auto&& [shard, list] : *decoded) {
    auto bp = list.begin().get_current_ptr();
    if (erasures.find(shard) == erasures.end()) in[shard] = bp;
    else out[shard] = bp;
  }
  return decode_chunks(want_to_read, in, out);
}

int ErasureCode::decode(const shard_id_set &want_to_read,
                        const shard_id_map<bufferlist> &chunks,
                        shard_id_map<bufferlist> *decoded, int chunk_size)
{
  return _decode(want_to_read, chunks, decoded);
}

int ErasureCode::decode(const std::set<int> &want_to_read,
                        const std::map<int, bufferlist> &chunks,
                        std::map<int, bufferlist> *decoded, int chunk_size)
{
  shard_id_set _want_to_read(want_to_read);
  shard_id_map<bufferlist> _chunks(get_chunk_count(), chunks);
  shard_id_map<bufferlist> _decoded(get_chunk_count());

  int r = _decode(_want_to_read, _chunks, &_decoded);
  for ( auto &&it : _decoded) {
    decoded->emplace(std::move(it));
  }
  return r;
}

int ErasureCode::parse(const ErasureCodeProfile &profile,
		       ostream *ss)
{
  return to_mapping(profile, ss);
}

const vector<int> &ErasureCode::get_chunk_mapping() const {
  return chunk_mapping;
}

int ErasureCode::to_mapping(const ErasureCodeProfile &profile,
			    ostream *ss)
{
  if (profile.find("mapping") != profile.end()) {
    std::string mapping = profile.find("mapping")->second;
    int position = 0;
    vector<int> coding_chunk_mapping;
    for(std::string::iterator it = mapping.begin(); it != mapping.end(); ++it) {
      if (*it == 'D')
	chunk_mapping.push_back(position);
      else
	coding_chunk_mapping.push_back(position);
      position++;
    }
    chunk_mapping.insert(chunk_mapping.end(),
			 coding_chunk_mapping.begin(),
			 coding_chunk_mapping.end());
  }
  return 0;
}

int ErasureCode::to_int(const std::string &name,
			ErasureCodeProfile &profile,
			int *value,
			const std::string &default_value,
			ostream *ss)
{
  if (profile.find(name) == profile.end() ||
      profile.find(name)->second.size() == 0)
    profile[name] = default_value;
  std::string p = profile.find(name)->second;
  std::string err;
  int r = strict_strtol(p.c_str(), 10, &err);
  if (!err.empty()) {
    *ss << "could not convert " << name << "=" << p
	<< " to int because " << err
	<< ", set to default " << default_value << std::endl;
    *value = strict_strtol(default_value.c_str(), 10, &err);
    return -EINVAL;
  }
  *value = r;
  return 0;
}

int ErasureCode::to_bool(const std::string &name,
			 ErasureCodeProfile &profile,
			 bool *value,
			 const std::string &default_value,
			 ostream *ss)
{
  if (profile.find(name) == profile.end() ||
      profile.find(name)->second.size() == 0)
    profile[name] = default_value;
  const std::string p = profile.find(name)->second;
  *value = (p == "yes") || (p == "true");
  return 0;
}

int ErasureCode::to_string(const std::string &name,
			   ErasureCodeProfile &profile,
			   std::string *value,
			   const std::string &default_value,
			   ostream *ss)
{
  if (profile.find(name) == profile.end() ||
      profile.find(name)->second.size() == 0)
    profile[name] = default_value;
  *value = profile[name];
  return 0;
}

int ErasureCode::decode_concat(const shard_id_set& want_to_read,
			       const shard_id_map<bufferlist> &chunks,
			       bufferlist *decoded)
{
  shard_id_map<bufferlist> decoded_map(get_chunk_count());
  int r = _decode(want_to_read, chunks, &decoded_map);
  if (r == 0) {
    for (unsigned int i = 0; i < get_data_chunk_count(); i++) {
      // XXX: the ErasureCodeInterface allows `decode()` to return
      // *at least* `want_to_read chunks`; that is, they may more.
      // Some implementations are consistently exact but jerasure
      // is quirky: it outputs more only when deailing with degraded.
      // The check below uniforms the behavior.
      if (want_to_read.contains(chunk_index(i)) &&
	  decoded_map.contains(chunk_index(i))) {
        decoded->claim_append(decoded_map[chunk_index(i)]);
      }
    }
  }
  return r;
}

int ErasureCode::decode_concat(const shard_id_set& want_to_read,
                    const std::map<int, bufferlist> &chunks,
                    bufferlist *decoded)
{
  const shard_id_map<bufferlist> _chunks(get_chunk_count(), chunks);
  return decode_concat(want_to_read, _chunks, decoded);
}

int ErasureCode::decode_concat(const shard_id_map<bufferlist> &chunks,
			       bufferlist *decoded)
{
  shard_id_set want_to_read;
  for (unsigned int i = 0; i < get_data_chunk_count(); i++) {
    want_to_read.insert(chunk_index(i));
  }
  return decode_concat(want_to_read, chunks, decoded);
}
}
