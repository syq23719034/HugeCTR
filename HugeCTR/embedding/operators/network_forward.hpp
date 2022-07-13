/*
 * Copyright (c) 2022, NVIDIA CORPORATION.
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

#include "HugeCTR/core/buffer.hpp"
#include "HugeCTR/core/registry.hpp"
#include "HugeCTR/embedding/common.hpp"

namespace embedding {
using core::CoreResourceManager;
using core::DataType;
using core::Device;
using core::Shape;
using core::Tensor;
using core::TensorList;

class NetworkForward {
  std::shared_ptr<CoreResourceManager> core_;
  int num_gpus_;

 public:
  NetworkForward() = default;

  NetworkForward(std::shared_ptr<CoreResourceManager> core, int num_gpus)
      : core_(core), num_gpus_(num_gpus) {}

  void compute(const Tensor& bucket_range, const Tensor& d_combiner_list,
               const TensorList& network_comm_buffer, const Tensor& gpu_idx_offset,
               const TensorList& global_ev_offset, const Tensor& network_idx,
               const Tensor& network_offset, const Tensor& network_dst, Tensor& output_buffer,
               const Tensor& d_ev_size_offset, int batch_size);
};

}  // namespace embedding