/*
 * Copyright (c) 2020, NVIDIA CORPORATION.
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


#include <unordered_map>
#include <fstream>

namespace HugeCTR {


template <typename KeyType>
class ParameterServerDelegate {
 public:
  using HashTable = std::unordered_map<KeyType, std::pair<size_t, size_t>>; // in case of its replacement

  virtual void load_from_snapshot(std::ofstream& embeding_table,
                                  std::ifstream& snapshot,
                                  const size_t file_size_in_byte,
                                  const size_t embedding_vec_size,
                                  HashTable& hash_table) = 0;

  virtual void store_to_snapshot(std::ofstream& snapshot,
                                 std::ifstream& embedding_table,
                                 const size_t file_size_in_byte,
                                 const size_t embedding_vec_size,
                                 HashTable& hash_table) = 0;
};

}  // namespace HugeCTR