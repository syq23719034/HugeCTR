/*
 * Copyright (c) 2019, NVIDIA CORPORATION.
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

#include "HugeCTR/include/regularizer.hpp"

#include "HugeCTR/include/common.hpp"
#include "HugeCTR/include/general_buffer.hpp"
#include "HugeCTR/include/tensor.hpp"
#include "HugeCTR/include/utils.hpp"

#include <memory>

namespace HugeCTR {

/**
 * @brief L2Regularizer
 */
class L2Regularizer : public Regularizer {
 public:
 /*
  * Constructor of L2Regularizer
  * @param weight_buff GeneralBuffer containing all the layers' weights
  * @param wgrad_buff GeneralBuffer containing all the layers' wgrads
  * @param batch_size Network batch size
  * @param device_id Device to be used
  */
  L2Regularizer(const std::shared_ptr<GeneralBuffer<float>>& weight_buff,
                const std::shared_ptr<GeneralBuffer<float>>& wgrad_buff,
                const int batch_size,
                const float lambda,
                cublasHandle_t cublas_handle,
                const int device_id);

 /*
  * Destructor of L2Regularizer
  */
  ~L2Regularizer() override {}

 private:
  void do_compute_rterm(const float* weight, float* h_rterm,
                        int num_elements,
                        cudaStream_t stream) override;
  void do_initialize_wgrad(const float* weight, float* wgrad,
                         int num_elements,
                         cudaStream_t stream) override;

  const float lambda_;
  cublasHandle_t cublas_handle_;
};

}  // namespace HugeCTR
