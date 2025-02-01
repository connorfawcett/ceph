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

#pragma once

#include <ostream>
#include <span>

#include "erasure-code/ErasureCodeInterface.h"
#include "include/buffer_fwd.h"
#include "include/ceph_assert.h"
#include "include/encoding.h"
#include "common/interval_map.h"
#include "common/Formatter.h"
#include "osd_types.h"

/// If someone wants these types, but not ExtentCache, move to another file
struct bl_split_merge {
  ceph::buffer::list split(
    uint64_t offset,
    uint64_t length,
    ceph::buffer::list &bl) const {
    ceph::buffer::list out;
    out.substr_of(bl, offset, length);
    return out;
  }
  bool can_merge(const ceph::buffer::list &left, const ceph::buffer::list &right) const {
    return true;
  }
  ceph::buffer::list merge(ceph::buffer::list &&left, ceph::buffer::list &&right) const {
    ceph::buffer::list bl{std::move(left)};
    bl.claim_append(right);
    return bl;
  }
  uint64_t length(const ceph::buffer::list &b) const { return b.length(); }
};

using extent_set = interval_set<uint64_t>;
using extent_map = interval_map<uint64_t, ceph::buffer::list, bl_split_merge>;

// Setting to 1 turns on very large amounts of level 0 debug containing the
// contents of buffers. Even on level 20 this is not really wanted.
#define DEBUG_EC_BUFFERS 1

namespace ECUtil {
  class shard_extent_map_t;

  struct shard_extent_set_t
  {
    // The following boilerplate is just to make this look like a map.
    std::map<int, extent_set> map;
    bool contains(int shard) const { return map.contains(shard); }
    bool empty() const { return map.empty(); }
    void swap(shard_extent_set_t &other) { map.swap(other.map); }
    void clear() { map.clear(); }
    auto erase(int shard) { return map.erase(shard); }
    auto erase(std::map<int, extent_set>::iterator &iter) { return map.erase(iter);}
    void erase_stripe(uint64_t offset, uint64_t length) {
      for (auto it = map.begin(); it != map.end();) {
        it->second.erase(offset, length);
        if (it->second.empty()) it = map.erase(it);
        else ++it;
      }
    }
    auto begin() const { return map.begin(); }
    auto begin() { return map.begin(); }
    auto end() const { return map.end(); }
    void emplace(int shard, extent_set &&set) { map.emplace(shard, std::move(set)); }
    int shard_count() { return map.size(); }
    extent_set &at(int shard) { return map.at(shard); }
    const extent_set &at(int shard) const { return map.at(shard); }
    extent_set &operator[] (int shard) { return map[shard]; }
    bool operator== (shard_extent_set_t const &other) const {
      return map == other.map;
    }

    friend std::ostream& operator<<(std::ostream& lhs, const shard_extent_set_t& rhs) {
      lhs << rhs.map;
      return lhs;
    }
    void get_extent_superset(extent_set &eset) const {
      for (auto &&[_, e]: map) eset.insert(e);
    }
    extent_set get_extent_superset() const {
      extent_set eset;
      get_extent_superset(eset);
      return eset;
    }

    void align(uint64_t a) {
      for (auto &&[_, e]: map) e.align(a);
    }

    void subtract(const shard_extent_set_t &set);
    void insert(const shard_extent_set_t &set);

    /** return the sum of extent_set.size) */
    uint64_t size() const
    {
      uint64_t size = 0;
      for (auto &&[_, e] : map) size += e.size();

      return size;
    }

  };

  class stripe_info_t {
  friend class shard_extent_map_t;

  const uint64_t stripe_width;
  const uint64_t plugin_flags;
  const uint64_t chunk_size;
  const pg_pool_t *pool;
  const unsigned int k; // Can be calculated with a division from above. Better to cache.
  const unsigned int m;
  const std::vector<int> chunk_mapping;
  const std::vector<unsigned int> chunk_mapping_reverse;
private:
  void ro_range_to_shards(
    uint64_t ro_offset,
    uint64_t ro_size,
    ECUtil::shard_extent_set_t *shard_extent_set,
    extent_set *extent_superset,
    buffer::list *bl,
    shard_extent_map_t *shard_extent_map) const;

  static std::vector<int> complete_chunk_mapping(
    std::vector<int> _chunk_mapping, unsigned int n)
  {
    unsigned int size = (int)_chunk_mapping.size();
    std::vector<int> chunk_mapping(n);
    for (unsigned int i = 0; i < n; i++) {
      if (size > i) {
        chunk_mapping.at(i) = _chunk_mapping.at(i);
      } else {
        chunk_mapping.at(i) = static_cast<int>(i);
      }
    }
    return chunk_mapping;
  }
  static std::vector<unsigned int> reverse_chunk_mapping(
    std::vector<int> chunk_mapping)
  {
    unsigned int size = (int)chunk_mapping.size();
    std::vector<unsigned int> reverse(size);
    std::vector<bool> used(size);
    for (unsigned int i = 0; i < size; i++) {
      int shard = chunk_mapping.at(i);
      // Mapping must be a bijection and a permutation
      ceph_assert(!used.at(shard));
      used.at(shard) = true;
      reverse.at(shard) = i;
    }
    return reverse;
  }
public:
  stripe_info_t(ErasureCodeInterfaceRef ec_impl, const pg_pool_t *pool,
		uint64_t stripe_width)
    : stripe_width(stripe_width),
      plugin_flags(ec_impl->get_supported_optimizations()),
      chunk_size(stripe_width / ec_impl->get_data_chunk_count()),
      pool(pool),
      k(ec_impl->get_data_chunk_count()),
      m(ec_impl->get_coding_chunk_count()),
      chunk_mapping(complete_chunk_mapping(ec_impl->get_chunk_mapping(), k + m)),
      chunk_mapping_reverse(reverse_chunk_mapping(chunk_mapping)) {
    ceph_assert(stripe_width % k == 0);
  }
    // Simpler constructors for unit tests
  stripe_info_t(unsigned int k, unsigned int m, uint64_t stripe_width)
    : stripe_width(stripe_width),
      plugin_flags(0xFFFFFFFFFFFFFFFFul), // Everything enabled for test harnesses.
      chunk_size(stripe_width / k),
      pool(nullptr),
      k(k),
      m(m),
      chunk_mapping(complete_chunk_mapping(std::vector<int>(), k + m)),
      chunk_mapping_reverse(reverse_chunk_mapping(chunk_mapping)) {
    ceph_assert(stripe_width % k == 0);
  }
  stripe_info_t(unsigned int k, unsigned int m, uint64_t stripe_width,
		std::vector<int> _chunk_mapping)
    : stripe_width(stripe_width),
      plugin_flags(0xFFFFFFFFFFFFFFFFul), // Everything enabled for test harnesses.
      chunk_size(stripe_width / k),
      pool(nullptr),
      k(k),
      m(m),
      chunk_mapping(complete_chunk_mapping(_chunk_mapping, k + m)),
      chunk_mapping_reverse(reverse_chunk_mapping(chunk_mapping)) {
    ceph_assert(stripe_width % k == 0);
  }
  stripe_info_t(unsigned int k, unsigned int m, uint64_t stripe_width,
                  pg_pool_t *pool, std::vector<int> _chunk_mapping)
      : stripe_width(stripe_width),
        plugin_flags(0xFFFFFFFFFFFFFFFFul), // Everything enabled for test harnesses.
        chunk_size(stripe_width / k),
        pool(pool),
        k(k),
        m(m),
        chunk_mapping(complete_chunk_mapping(_chunk_mapping, k + m)),
        chunk_mapping_reverse(reverse_chunk_mapping(chunk_mapping)) {
    ceph_assert(stripe_width % k == 0);
  }
  stripe_info_t(unsigned int k, unsigned int m, uint64_t stripe_width,
                  pg_pool_t *pool)
      : stripe_width(stripe_width),
        plugin_flags(0xFFFFFFFFFFFFFFFFul), // Everything enabled for test harnesses.
        chunk_size(stripe_width / k),
        pool(pool),
        k(k),
        m(m),
        chunk_mapping(complete_chunk_mapping(std::vector<int>(), k + m)),
        chunk_mapping_reverse(reverse_chunk_mapping(chunk_mapping)) {
    ceph_assert(stripe_width % k == 0);
  }
  uint64_t object_size_to_shard_size(const uint64_t size, int shard) const {
    uint64_t remainder = size % get_stripe_width();
    uint64_t shard_size = (size - remainder) / k;
    unsigned int raw_shard = get_raw_shard(shard);
    if (raw_shard >= get_k()) {
      // coding parity shards have same size as data shard 0
      raw_shard = 0;
    }
    if (remainder > raw_shard * get_chunk_size()) {
      remainder -= raw_shard * get_chunk_size();
      if (remainder > get_chunk_size()) {
	remainder = get_chunk_size();
      }
      shard_size += remainder;
    }
    return shard_size;
  }

  uint64_t ro_offset_to_shard_offset(uint64_t ro_offset, int shard) const
  {
    uint64_t full_stripes = (ro_offset / stripe_width) * chunk_size;
    int offset_shard = (ro_offset / chunk_size) % k;

    if (shard == offset_shard) return full_stripes + ro_offset % chunk_size;
    if (shard < offset_shard) return full_stripes + chunk_size;
    return full_stripes;
  }

  /**
   * Return true if shard does not require metadata updates
   */
  bool is_nonprimary_shard(const shard_id_t shard) const {
    return pool->is_nonprimary_shard(shard);
  }
  bool supports_ec_overwrites() const {
    return pool->allows_ecoverwrites();
  }
  bool require_hinfo() const {
    return !supports_ec_overwrites();
  }
  bool supports_partial_reads() const {
    return (plugin_flags & ErasureCodeInterface::FLAG_EC_PLUGIN_PARTIAL_READ_OPTIMIZATION) != 0;
  }
  bool supports_partial_writes() const {
    return (plugin_flags & ErasureCodeInterface::FLAG_EC_PLUGIN_PARTIAL_WRITE_OPTIMIZATION) != 0;
  }
  bool logical_offset_is_stripe_aligned(uint64_t logical) const {
    return (logical % stripe_width) == 0;
  }
  uint64_t get_stripe_width() const {
    return stripe_width;
  }
  uint64_t get_chunk_size() const {
    return chunk_size;
  }
  unsigned int get_m() const {
    return m;
  }
  unsigned int get_k() const {
    return k;
  }
  unsigned int get_k_plus_m() const {
    return k + m;
  }
  int get_shard(unsigned int raw_shard) const {
    return chunk_mapping[raw_shard];
  }
  unsigned int get_raw_shard(int shard) const
  {
    return chunk_mapping_reverse.at(shard);
  }
  /* Return a "span" - which can be iterated over */
  auto get_parity_shards() const {
    return std::span(chunk_mapping).subspan(k, m);
  }
  // FIXME: get_k() preferred... but changing would create a big change.
  int get_data_chunk_count() const {
    return k;
  }
  uint64_t logical_to_prev_chunk_offset(uint64_t offset) const {
    return (offset / stripe_width) * chunk_size;
  }
  uint64_t logical_to_next_chunk_offset(uint64_t offset) const {
    return ((offset + stripe_width - 1)/ stripe_width) * chunk_size;
  }
  uint64_t logical_to_prev_stripe_offset(uint64_t offset) const {
    return offset - (offset % stripe_width);
  }
  uint64_t logical_to_next_stripe_offset(uint64_t offset) const {
    return ((offset % stripe_width) ?
      (offset - (offset % stripe_width) + stripe_width) :
      offset);
  }
  uint64_t aligned_logical_offset_to_chunk_offset(uint64_t offset) const {
    ceph_assert(offset % stripe_width == 0);
    return (offset / stripe_width) * chunk_size;
  }
  uint64_t chunk_aligned_logical_offset_to_chunk_offset(uint64_t offset) const {
    [[maybe_unused]] const auto residue_in_stripe = offset % stripe_width;
    ceph_assert(residue_in_stripe % chunk_size == 0);
    ceph_assert(stripe_width % chunk_size == 0);
    // this rounds down
    return (offset / stripe_width) * chunk_size;
  }
  uint64_t chunk_aligned_logical_size_to_chunk_size(uint64_t len) const {
    // this rounds up
    return ((len + stripe_width - 1) / stripe_width) * chunk_size;
  }
  uint64_t aligned_chunk_offset_to_logical_offset(uint64_t offset) const {
    ceph_assert(offset % chunk_size == 0);
    return (offset / chunk_size) * stripe_width;
  }
  std::pair<uint64_t, uint64_t> chunk_aligned_offset_len_to_chunk(
    uint64_t off, uint64_t len) const;
  std::pair<uint64_t, uint64_t> offset_len_to_stripe_bounds(
    uint64_t _off, uint64_t _len) const {
    uint64_t off = logical_to_prev_stripe_offset(_off);
    uint64_t len = logical_to_next_stripe_offset(
      (_off - off) + _len);
    return std::make_pair(off, len);
  }
  std::pair<uint64_t, uint64_t> offset_len_to_chunk_bounds(
    std::pair<uint64_t, uint64_t> in) const {
    uint64_t off = in.first - (in.first % chunk_size);
    uint64_t tmp_len = (in.first - off) + in.second;
    uint64_t len = ((tmp_len % chunk_size) ?
      (tmp_len - (tmp_len % chunk_size) + chunk_size) :
      tmp_len);
    return std::make_pair(off, len);
  }
  std::pair<uint64_t, uint64_t> offset_len_to_page_bounds(
  std::pair<uint64_t, uint64_t> in) const {
    uint64_t off = in.first - (in.first % CEPH_PAGE_SIZE);
    uint64_t tmp_len = (in.first - off) + in.second;
    uint64_t len = ((tmp_len % CEPH_PAGE_SIZE) ?
      (tmp_len - (tmp_len % CEPH_PAGE_SIZE) + CEPH_PAGE_SIZE) :
      tmp_len);
    return std::make_pair(off, len);
  }
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> offset_length_to_data_chunk_extents(
    uint64_t off, uint64_t len) const {
    assert(chunk_size > 0);
    const auto first_chunk_idx = (off / chunk_size);
    const auto last_chunk_idx = (chunk_size - 1 + off + len) / chunk_size;
    const auto first_chunk_offset =  first_chunk_idx * chunk_size;
    const auto first_shard_offset = off - first_chunk_offset + first_chunk_offset/stripe_width;
    const auto last_chunk_len = (len == 0) ? 0:off + len - (last_chunk_idx - 1) * chunk_size;
    return {first_chunk_idx, last_chunk_idx, first_shard_offset, last_chunk_len};
  }
  bool offset_length_is_same_stripe(
    uint64_t off, uint64_t len) const {
    if (len == 0) {
      return true;
    }
    assert(chunk_size > 0);
    const auto first_stripe_idx = off / stripe_width;
    const auto last_inc_stripe_idx = (off + len - 1) / stripe_width;
    return first_stripe_idx == last_inc_stripe_idx;
  }

  void ro_range_to_shard_extent_set(
    uint64_t ro_offset,
    uint64_t ro_size,
    ECUtil::shard_extent_set_t &shard_extent_set) const {
    ro_range_to_shards(ro_offset, ro_size, &shard_extent_set, NULL, NULL, NULL);
  }

  void ro_range_to_shard_extent_set(
    uint64_t ro_offset,
    uint64_t ro_size,
    ECUtil::shard_extent_set_t &shard_extent_set,
    extent_set &extent_superset) const {
    ro_range_to_shards(ro_offset, ro_size, &shard_extent_set, &extent_superset, NULL,
                        NULL);
  }

  void ro_range_to_shard_extent_set_with_parity(
    uint64_t ro_offset,
    uint64_t ro_size,
    ECUtil::shard_extent_set_t &shard_extent_set) const {
    extent_set parity;
    ro_range_to_shards(ro_offset, ro_size, &shard_extent_set, &parity, NULL,
                        NULL);

    if (parity.empty()) return;

    for (int shard : get_parity_shards()) {
      shard_extent_set[shard].insert(parity);
    }
  }

  void ro_range_to_shard_extent_map(
    uint64_t ro_offset,
    uint64_t ro_size,
    buffer::list &bl,
    shard_extent_map_t &shard_extent_map) const {

    ro_range_to_shards(ro_offset, ro_size, NULL, NULL, &bl, &shard_extent_map);
  }

  void trim_shard_extent_set_for_ro_offset (uint64_t ro_offset,
    ECUtil::shard_extent_set_t &shard_extent_set) const;

  void ro_size_to_stripe_aligned_read_mask(
  uint64_t ro_size,
  ECUtil::shard_extent_set_t &shard_extent_set) const;

  void ro_size_to_read_mask(
    uint64_t ro_size,
    ECUtil::shard_extent_set_t &shard_extent_set) const;

  void ro_size_to_zero_mask(
    uint64_t ro_size,
    ECUtil::shard_extent_set_t &shard_extent_set) const;
};

inline uint64_t page_mask() {
  static const uint64_t page_mask = ((uint64_t)CEPH_PAGE_SIZE) - 1;
  return page_mask;
}
inline uint64_t align_page_next(uint64_t val) {
  return (val + page_mask()) & ~page_mask();
}
inline uint64_t align_page_prev(uint64_t val) {
  return val & ~page_mask();
}

class HashInfo {
  uint64_t total_chunk_size = 0;
  std::vector<uint32_t> cumulative_shard_hashes;
public:
  HashInfo() {}
  explicit HashInfo(unsigned num_chunks) :
    cumulative_shard_hashes(num_chunks, -1) {}
  void append(uint64_t old_size, std::map<int, bufferptr> &to_append);
  void clear() {
    total_chunk_size = 0;
    cumulative_shard_hashes = std::vector<uint32_t>(
      cumulative_shard_hashes.size(),
      -1);
  }
  void encode(ceph::buffer::list &bl) const;
  void decode(ceph::buffer::list::const_iterator &bl);
  void dump(ceph::Formatter *f) const;
  static void generate_test_instances(std::list<HashInfo*>& o);
  uint32_t get_chunk_hash(int shard) const {
    ceph_assert((unsigned)shard < cumulative_shard_hashes.size());
    return cumulative_shard_hashes[shard];
  }
  uint64_t get_total_chunk_size() const {
    return total_chunk_size;
  }
  void set_total_chunk_size_clear_hash(uint64_t new_chunk_size) {
    cumulative_shard_hashes.clear();
    total_chunk_size = new_chunk_size;
  }
  bool has_chunk_hash() const {
    return !cumulative_shard_hashes.empty();
  }
  void update_to(const HashInfo &rhs) {
    *this = rhs;
  }
  friend std::ostream& operator<<(std::ostream& out, const HashInfo& hi);
};

typedef std::shared_ptr<HashInfo> HashInfoRef;

class shard_extent_map_t
{
  static const uint64_t invalid_offset = std::numeric_limits<uint64_t>::max();

  class slice_iterator
  {
    uint64_t offset = (uint64_t)-1;
    uint64_t length = (uint64_t)-1;
    uint64_t start = (uint64_t)-1;
    uint64_t end = (uint64_t)-1;
    std::map<int, std::pair<extent_map::const_iterator, bufferlist::const_iterator>> iters;
    std::map<int, bufferptr> slice;
    shard_extent_map_t &sem;
    void advance();
  public:
    slice_iterator(shard_extent_map_t &sem);
    std::map<int, bufferptr> &get_bufferptrs() { return slice; }
    uint64_t get_offset() { return offset; }
    uint64_t get_length() { return length; }
    bool is_end() { return slice.empty(); }
    bool is_page_aligned();

    slice_iterator& operator++() {
      advance();
      return *this;
    }
  };

public:
  const stripe_info_t *sinfo;
  // The maximal range of all extents maps within rados object space.
  uint64_t ro_start;
  uint64_t ro_end;
  uint64_t start_offset;
  uint64_t end_offset;
  std::map<int, extent_map> extent_maps;

  slice_iterator begin_slice_iterator();

  /* This caculates the ro offset for an offset into a particular shard */
  uint64_t calc_ro_offset(int raw_shard, int shard_offset) const {
    int stripes = shard_offset / sinfo->chunk_size;
    return stripes * sinfo->stripe_width + raw_shard * sinfo->chunk_size +
      shard_offset % sinfo->chunk_size;
  }

  uint64_t calc_ro_end(int raw_shard, int shard_offset) const {
    return calc_ro_offset(raw_shard, shard_offset - 1) + 1;
  }

  /* This is a relatively expensive operation to update the ro offset/length.
   * Ideally, we should be able to update offset/length incrementally.
   */
  void compute_ro_range()
  {
    uint64_t start = invalid_offset;
    uint64_t end = 0;
    uint64_t o_start = invalid_offset;
    uint64_t o_end = 0;

    for (auto &&[shard, emap] : extent_maps) {
      int raw_shard = sinfo->get_raw_shard(shard);
      uint64_t start_off = emap.get_start_off();
      uint64_t end_off = emap.get_end_off();
      o_start = std::min(o_start, start_off);
      o_end = std::max(o_end, end_off);

      if (raw_shard < sinfo->get_k()) {
        start = std::min(start, calc_ro_offset(raw_shard, start_off));
        end = std::max(end, calc_ro_end(raw_shard, end_off));
      }
    }
    if (end != 0) {
      ro_start = start;
      ro_end = end;
      start_offset = o_start;
      end_offset = o_end;
    } else {
      ro_start = invalid_offset;
      ro_end = invalid_offset;
      start_offset = invalid_offset;
      end_offset = invalid_offset;
    }
  }
public:
  shard_extent_map_t(const stripe_info_t *sinfo) :
    sinfo(sinfo),
    ro_start(invalid_offset),
    ro_end(invalid_offset),
    start_offset(invalid_offset),
    end_offset(invalid_offset)
  {}

  shard_extent_map_t(const stripe_info_t *sinfo, std::map<int, extent_map> &&extent_maps) :
    sinfo(sinfo),
    extent_maps(std::move(extent_maps))
  {
    // Empty shards are not permitted, so clear them out.
    for (auto iter = extent_maps.begin(); iter != extent_maps.end();) {

      if (iter->second.empty())
        iter = extent_maps.erase(iter);
      else
        ++iter;
    }
    compute_ro_range();
  }

  bool empty() const {
    return ro_end == invalid_offset;
  }

  uint64_t get_ro_start()
  {
    return ro_start;
  }

  uint64_t get_ro_end()
  {
    return ro_end;
  }

  /* Return the extent maps.  For reading only, set to const as the returned
   * map should not be modified.
   * We want to avoid:
   *  - Empty extent maps on shards
   *  - getting the offset/length out of sync.
   */
  const std::map<int, extent_map> &get_extent_maps() const {
    return extent_maps;
  }

  /* Return a particlar extent map. This must be const because updating it
   * would cause the shard_extent_map to become inconsistent.
   *
   * * This method will raise an exception if the shard has no extents.
   */
  const extent_map &get_extent_map(int shard) const {
    return extent_maps.at(shard);
  }

  /* Return a particular extent set.
   */
  const extent_set get_extent_set(int shard) const {
    return extent_maps.at(shard).get_interval_set();
  }

  shard_extent_set_t get_shard_extent_set() const {
    shard_extent_set_t ret;
    for (auto &&[shard, emap] : extent_maps) {
      ret.emplace(shard, emap.get_interval_set());
    }
    return ret;
  }

  bool contains_shard(int shard) const {
    return extent_maps.contains(shard);
  }

  void erase_after_ro_offset(uint64_t ro_offset);
  shard_extent_map_t intersect_ro_range(uint64_t ro_offset, uint64_t ro_length) const;
  shard_extent_map_t intersect(std::optional<shard_extent_set_t> const &other) const;
  shard_extent_map_t intersect(shard_extent_set_t const &other) const;
  void insert_in_shard(int shard, uint64_t off, const buffer::list &bl);
  void insert_in_shard(int shard, uint64_t off, const buffer::list &bl, uint64_t new_start, uint64_t new_end);
  void insert_ro_zero_buffer( uint64_t ro_offset, uint64_t ro_length );
  void insert(shard_extent_map_t const &other);
  void append_zeros_to_ro_offset( uint64_t ro_offset );
  void insert_ro_extent_map(const extent_map &host_extent_map);
  extent_set get_extent_superset() const;
  int encode(ErasureCodeInterfaceRef& ec_impl, const HashInfoRef &hinfo, uint64_t before_ro_size);
  int decode(ErasureCodeInterfaceRef& ec_impl, ECUtil::shard_extent_set_t want);
  void get_buffer(int shard, uint64_t offset, uint64_t length, buffer::list &append_to) const;
  void get_shard_first_buffer(int shard, buffer::list &append_to) const;
  uint64_t get_shard_first_offset(int shard) const;
  void zero_pad(int shard, uint64_t offset, uint64_t length);
  void zero_pad(uint64_t offset, uint64_t length);
  bufferlist get_ro_buffer(uint64_t ro_offset, uint64_t ro_length);
  /* Returns a buffer assuming that there is a single contigious buffer
   * represented by the map. */
  bufferlist get_ro_buffer();
  shard_extent_set_t get_extent_set();
  void insert_parity_buffers();
  void erase_shard(int shard);
  std::map<int, bufferlist> slice(int offset, int length) const;
  shard_extent_map_t slice_map(int offset, int length) const;
  std::string debug_string(uint64_t inteval, uint64_t offset) const;
  void erase_stripe(uint64_t offset, uint64_t length);
  bool contains(int shard) const;
  bool contains(int shard, extent_set other_eset) const;
  bool contains(std::optional<shard_extent_set_t> const &other) const;
  bool contains(shard_extent_set_t const &other) const;
  void pad_and_rebuild_to_page_align();
  uint64_t size();
  void clear();
  uint64_t get_start_offset() const { return start_offset; }
  uint64_t get_end_offset() const { return end_offset; }

  void assert_buffer_contents_equal(shard_extent_map_t other) const
  {
    for (auto &&[shard, emap]: extent_maps) {
      for (auto &&i : emap) {
        bufferlist bl = i.get_val();
        bufferlist otherbl;
        other.get_buffer(shard, i.get_off(), i.get_len(), otherbl);
        ceph_assert(bl.contents_equal(otherbl));
      }
    }
  }

  friend std::ostream& operator<<(std::ostream& lhs, const shard_extent_map_t& rhs);
  friend bool operator==(const shard_extent_map_t& lhs, const shard_extent_map_t& rhs)
  {
    return lhs.sinfo == rhs.sinfo
      && lhs.ro_start == rhs.ro_start
      && lhs.ro_end == rhs.ro_end
      && lhs.extent_maps == rhs.extent_maps;
  }
};

typedef enum {
  READ_REQUEST,
  ZERO_REQUEST,
  READ_DONE,
  ZERO_DONE,
  INJECT_EIO,
  CANCELLED,
  ERROR,
  REQUEST_MISSING,
  COMPLETE_ERROR,
  ERROR_CLEAR,
  COMPLETE
} log_event_t;

struct log_entry_t {
  const log_event_t event;
  const pg_shard_t shard;
  const extent_set io;

  log_entry_t(
      const log_event_t event,
      const pg_shard_t &shard,
      const extent_set &io) :
  event(event), shard(shard), io(io) {}
  log_entry_t(
    const log_event_t event,
    const pg_shard_t &shard) :
  event(event), shard(shard) {}
  log_entry_t(
   const log_event_t event,
   const pg_shard_t &shard,
   const shard_extent_map_t &extent_map) :
  event(event), shard(shard),
  io(extent_map.contains(shard.shard.id)?
    extent_map.get_extent_map(shard.shard.id).get_interval_set():
    extent_set()) {}

  friend std::ostream& operator<<(std::ostream& out, const log_entry_t& lhs);
};

bool is_hinfo_key_string(const std::string &key);
const std::string &get_hinfo_key();

WRITE_CLASS_ENCODER(ECUtil::HashInfo)
}
