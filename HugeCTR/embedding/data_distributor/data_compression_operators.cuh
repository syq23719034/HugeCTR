/*
 * Copyright (c) 2023, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <nccl.h>

#include <core/core.hpp>
#include <embedding/common.hpp>
#include <embedding/data_distributor/data_compression_operators.hpp>
#include <gpu_cache/include/hash_functions.cuh>
#include <memory>
#include <utils.cuh>
#include <vector>

namespace HugeCTR {

template <typename KeyType>
struct KeyPair {
  KeyType key;
  int feature_id;
};

struct Hash {
  HOST_DEVICE_INLINE size_t operator()(const KeyPair<uint32_t> &key_pair) {
    using hash_func = MurmurHash3_32<uint32_t>;
    uint32_t key_hash = hash_func::hash(key_pair.key);
    uint32_t feature_id_hash = hash_func::hash(key_pair.feature_id);
    return hash_func ::hash_combine(key_hash, feature_id_hash);
  }
  HOST_DEVICE_INLINE size_t operator()(const KeyPair<int32_t> &key_pair) {
    using hash_func = MurmurHash3_32<int32_t>;
    uint32_t key_hash = hash_func::hash(key_pair.key);
    uint32_t feature_id_hash = hash_func::hash(key_pair.feature_id);
    return hash_func ::hash_combine(key_hash, feature_id_hash);
  }
  HOST_DEVICE_INLINE size_t operator()(const KeyPair<uint64_t> &key_pair) {
    using hash_func = MurmurHash3_32<uint64_t>;
    uint32_t key_hash = hash_func::hash(key_pair.key);
    uint32_t feature_id_hash = hash_func::hash(key_pair.feature_id);
    return hash_func ::hash_combine(key_hash, feature_id_hash);
  }
  HOST_DEVICE_INLINE size_t operator()(const KeyPair<int64_t> &key_pair) {
    using hash_func = MurmurHash3_32<int64_t>;
    uint32_t key_hash = hash_func::hash(key_pair.key);
    uint32_t feature_id_hash = hash_func::hash(key_pair.feature_id);
    return hash_func ::hash_combine(key_hash, feature_id_hash);
  }
};

struct ShardPartitionerView {
  int *gpu_ids;
  int *num_shard_range;

  template <typename KeyType>
  DEVICE_INLINE int operator()(const KeyPair<KeyType> &key_pair) const noexcept {
    const auto &key = key_pair.key;
    const int &feature_id = key_pair.feature_id;
    int num_shard = num_shard_range[feature_id + 1] - num_shard_range[feature_id];
    uint64_t shard_id = (uint64_t)key % (uint64_t)num_shard;
    return gpu_ids[num_shard_range[feature_id] + shard_id];
  }
};

struct ShardPartitioner {
  core23::Tensor gpu_ids;
  core23::Tensor num_shard_range;

  ShardPartitioner() = default;

  ShardPartitioner(std::shared_ptr<core::CoreResourceManager> core,
                   const std::vector<embedding::LookupParam> &lookup_params,
                   const std::vector<std::vector<int>> &shard_matrix,
                   const std::vector<int> &lookup_ids);

  using view_type = ShardPartitionerView;

  view_type view() const noexcept {
    return view_type{gpu_ids.data<int>(), num_shard_range.data<int>()};
  }
};

struct TablePartitionerView {
  int *lookup_id_to_local_table_id;

  template <typename KeyType>
  DEVICE_INLINE int operator()(const KeyPair<KeyType> &key_pair) const noexcept {
    return lookup_id_to_local_table_id[key_pair.feature_id];
  }
};

struct TablePartitioner {
  core23::Tensor lookup_id_to_local_table_id;

  TablePartitioner() = default;

  TablePartitioner(std::shared_ptr<core::CoreResourceManager> core, int num_lookup,
                   const std::vector<int> &local_lookup_id_to_global_lookup_ids,
                   const embedding::WgradAttr &wgrad_attr);
  using view_type = TablePartitionerView;

  view_type view() const noexcept { return view_type{lookup_id_to_local_table_id.data<int>()}; }
};

union TableValue {
  uint64_t value;
  struct Detail {
    uint32_t r_idx_plus_one;
    uint32_t feature_id_and_key_lo;
  } detail;
};

template <typename KeyType>
struct TableEntry {
  KeyType key;
  TableValue value;
};

template <typename KeyType, typename BucketRangeType, typename Partitioner>
struct UniqueTableView {
  TableEntry<KeyType> *table;
  size_t capacity;

  Partitioner partitioner;

  using ResultType = PartitionedDataView<KeyType, BucketRangeType>;

  DEVICE_INLINE uint32_t find(const KeyPair<KeyType> &key_pair, ResultType &result) noexcept {
    int partition_id = partitioner(key_pair);

    KeyType *current_partitioned_keys =
        result.partitioned_keys + partition_id * result.max_num_key_per_partition;
    int *current_feature_ids = result.feature_ids + partition_id * result.max_num_key_per_partition;
    BucketRangeType *current_d_num_key = result.d_num_key_per_partition + partition_id;

    const KeyType &key = key_pair.key;
    const KeyType key_hi = (key | 0x1);
    const uint32_t key_lo = static_cast<uint32_t>(key & 0x1);
    const int &feature_id = key_pair.feature_id;
    size_t pos = Hash()(key_pair) % capacity;

    uint32_t r_idx_plus_one = 0;
    while (r_idx_plus_one == 0) {
      bool prob_next = false;

      KeyType *key_ptr = &table[pos].key;
      volatile uint64_t *table_value_ptr = &table[pos].value.value;

      const KeyType old_key = atomicCAS(key_ptr, 0, key_hi);
      if (old_key == 0) {
        BucketRangeType insert_pos = atomic_add(current_d_num_key, 1);
        r_idx_plus_one = static_cast<uint32_t>(insert_pos) + 1;
        TableValue insert_value;
        insert_value.detail.r_idx_plus_one = r_idx_plus_one;
        insert_value.detail.feature_id_and_key_lo = (feature_id << 1U | key_lo);
        *table_value_ptr = insert_value.value;
        current_partitioned_keys[r_idx_plus_one - 1] = key;

        assert(r_idx_plus_one <= result.max_num_key_per_partition);
        current_feature_ids[r_idx_plus_one - 1] = feature_id;
      } else if (old_key == key_hi) {
        TableValue insert_value;
        insert_value.value = *table_value_ptr;
        uint32_t table_r_idx_plus_one = insert_value.detail.r_idx_plus_one;
        uint32_t table_feature_id_and_key_lo = insert_value.detail.feature_id_and_key_lo;

        if (table_r_idx_plus_one == 0 && table_feature_id_and_key_lo == 0) {
          // do nothing
        } else if ((table_feature_id_and_key_lo & 0x1) == key_lo &&
                   table_feature_id_and_key_lo >> 1U == feature_id) {
          r_idx_plus_one = table_r_idx_plus_one;
        } else {
          prob_next = true;
        }
      } else {
        prob_next = true;
      }

      if (prob_next) {
        pos += 1;
        if (pos >= capacity) {
          pos -= capacity;
        }
      }
    }
    return partition_id * result.max_num_key_per_partition + r_idx_plus_one;
  }
};

static constexpr uint32_t kInvalidReverseIdx = std::numeric_limits<uint32_t>::max();

template <typename KeyType, typename BucketRangeType, typename Partitioner>
struct FrequentTableView {
  TableEntry<KeyType> *table;
  size_t capacity;

  Partitioner partitioner;

  using ResultType = PartitionedDataView<KeyType, BucketRangeType>;

  DEVICE_INLINE uint32_t find(const KeyPair<KeyType> &key_pair, ResultType &result) noexcept {
    int partition_id = partitioner(key_pair);

    KeyType *current_partitioned_keys =
        result.partitioned_keys + partition_id * result.max_num_key_per_partition;
    int *current_feature_ids = result.feature_ids + partition_id * result.max_num_key_per_partition;
    BucketRangeType *current_d_num_key = result.d_num_key_per_partition + partition_id;

    const KeyType &key = key_pair.key;
    const KeyType key_hi = (key | 0x1);
    const uint32_t key_lo = static_cast<uint32_t>(key & 0x1);
    const int &feature_id = key_pair.feature_id;
    size_t pos = Hash()(key_pair) % capacity;

    while (true) {
      const KeyType old_key = table[pos].key;

      // not a frequent key
      if (old_key == 0) break;

      if (old_key == key_hi) {
        TableValue insert_value;
        insert_value.value = table[pos].value.value;
        uint32_t table_feature_id_and_key_lo = insert_value.detail.feature_id_and_key_lo;

        // is a frequent key
        if ((table_feature_id_and_key_lo & 0x1) == key_lo &&
            (table_feature_id_and_key_lo | 0x1) >> 1U == feature_id) {
          BucketRangeType insert_pos = atomic_add(current_d_num_key, 1);
          uint32_t r_idx_plus_one = static_cast<uint32_t>(insert_pos) + 1;
          current_partitioned_keys[r_idx_plus_one - 1] = key;
          current_feature_ids[r_idx_plus_one - 1] = feature_id;
          return partition_id * result.max_num_key_per_partition + r_idx_plus_one;
        }
      }
      pos += 1;
      if (pos >= capacity) {
        pos -= capacity;
      }
    }
    return kInvalidReverseIdx;
  }
};

template <typename KeyType, typename BucketRangeType, typename Partitioner>
struct InfrequentTableView {
  TableEntry<KeyType> *table;
  size_t capacity;

  Partitioner partitioner;

  using ResultType = PartitionedDataView<KeyType, BucketRangeType>;

  DEVICE_INLINE uint32_t find(const KeyPair<KeyType> &key_pair, ResultType &result) noexcept {
    int partition_id = partitioner(key_pair);

    KeyType *current_partitioned_keys =
        result.partitioned_keys + partition_id * result.max_num_key_per_partition;
    int *current_feature_ids = result.feature_ids + partition_id * result.max_num_key_per_partition;
    BucketRangeType *current_d_num_key = result.d_num_key_per_partition + partition_id;

    const KeyType &key = key_pair.key;
    const KeyType key_hi = (key | 0x1);
    const uint32_t key_lo = static_cast<uint32_t>(key & 0x1);
    const int &feature_id = key_pair.feature_id;
    size_t pos = Hash()(key_pair) % capacity;

    while (true) {
      const KeyType old_key = table[pos].key;

      // is a frequent key
      if (old_key == 0) {
        TableValue insert_value;
        insert_value.value = table[pos].value.value;
        uint32_t table_feature_id_and_key_lo = insert_value.detail.feature_id_and_key_lo;
        if ((table_feature_id_and_key_lo & 0x1) == key_lo &&
            (table_feature_id_and_key_lo | 0x1) >> 1U == feature_id)
          break;
      }

      // is a infrequent key
      if (old_key == 0) {
        BucketRangeType insert_pos = atomic_add(current_d_num_key, 1);
        uint32_t r_idx_plus_one = static_cast<uint32_t>(insert_pos) + 1;
        current_partitioned_keys[r_idx_plus_one - 1] = key;
        current_feature_ids[r_idx_plus_one - 1] = feature_id;
        return partition_id * result.max_num_key_per_partition + r_idx_plus_one;
      }
      pos += 1;
      if (pos >= capacity) {
        pos -= capacity;
      }
    }
    return kInvalidReverseIdx;
  }
};

}  // namespace HugeCTR