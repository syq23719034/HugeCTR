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

#include <gtest/gtest.h>

#include <layers/masked_softmax_layer.hpp>
#include <layers/sequence_mask_layer.hpp>
#include <utest/test_utils.hpp>
#include <vector>

using namespace HugeCTR;
using namespace std;

namespace {

const float eps = 1e-4;

template <typename T>
void f2i_input(T* input, size_t in_size, size_t max_sequence_len) {
  for (size_t i = 0; i < in_size; i++) {
    input[i] = abs(floor(input[i] * max_sequence_len));
  }
}

template <typename T>
void max_per_line_cpu(T* bottom, const T* mask, int batch_size, int head_num, int seq_len_from,
                      int seq_len_to, float scalar, T* workspace) {
  float local_max = -1e20f;
  for (int i = 0; i < batch_size * head_num * seq_len_from; i++) {
    local_max = -1e20f;
    int input_offset = i * seq_len_to;
    int mask_offset = (i / (head_num * seq_len_from)) * seq_len_from * seq_len_to +
                      (i % seq_len_from) * seq_len_to;
    for (int j = 0; j < seq_len_to; j++) {
      float in_val = static_cast<float>(bottom[input_offset + j]);
      float mask_val = (float)mask[mask_offset + j];
      mask_val = (1.0f - mask_val) * 10000.0f;
      bottom[input_offset + j] = in_val * scalar - (float)mask_val;
      local_max = max(local_max, bottom[input_offset + j]);
    }
    workspace[i] = static_cast<T>(local_max);
  }
}

template <typename T>
void sum_ex_cpu(T* top, int embedding_vector_size, int dim0, T* workspace) {
  // sum(e^xi) i = [0, embedding_vector_size -1];
  for (int i = 0; i < dim0; i++) {
    workspace[i] = 0;
    int offset = i * embedding_vector_size;
    for (int j = 0; j < embedding_vector_size; j++) {
      workspace[i] += top[offset + j];
    }
  }
}

template <typename T>
void ex_cpu(T* top, const T* bottom, const T* workspace, int dim0, int embedding_vector_size) {
  // e^xi
  for (int i = 0; i < dim0; i++) {
    int offset = i * embedding_vector_size;
    for (int j = 0; j < embedding_vector_size; j++) {
      top[offset + j] = expf(bottom[offset + j] - workspace[i]);
    }
  }
}

template <typename T>
void sum_grad_softmax(const T* d_top, const T* softmax_out, int embedding_vector_size, int dim0,
                      T* workspace) {
  for (int i = 0; i < dim0; i++) {
    float grad_sum = 0.0;
    int offset = i * embedding_vector_size;
    for (int j = 0; j < embedding_vector_size; j++) {
      grad_sum += (float)(d_top[offset + j] * softmax_out[offset + j]);
    }
    workspace[i] = static_cast<T>(grad_sum);
  }
}

template <typename T>
void masked_softmax_fprop_cpu(T* top, T* bottom, const T* mask, int batch_size, int head_num,
                              int seq_len_from, int seq_len_to, float scalar) {
  int dim0 = batch_size * head_num * seq_len_from;
  T* workspace = new T[batch_size * head_num * seq_len_from];

  // max per line
  max_per_line_cpu(bottom, mask, batch_size, head_num, seq_len_from, seq_len_to, scalar, workspace);

  // e^xi
  ex_cpu(top, bottom, workspace, dim0, seq_len_to);
  // sum(e^xi) i = [0, embedding_vector_size -1];
  sum_ex_cpu(top, seq_len_to, dim0, workspace);

  // softmax : e^xi / sum(e^xi); i = [0, len - 1];
  for (int i = 0; i < dim0; i++) {
    for (int j = 0; j < seq_len_to; j++) {
      int index = i * seq_len_to + j;
      top[index] = top[index] / workspace[i];
    }
  }
  delete[] workspace;
}

template <typename T>
void masked_softmax_bprop_cpu(T* d_bottom, const T* d_top, const T* softmax_out, int dim0,
                              int embedding_vector_size, float scalar) {
  T* workspace = new T[dim0];

  sum_grad_softmax(d_top, softmax_out, embedding_vector_size, dim0, workspace);
  for (int i = 0; i < dim0; i++) {
    for (int j = 0; j < embedding_vector_size; j++) {
      int index = i * embedding_vector_size + j;
      d_bottom[index] = softmax_out[index] * (d_top[index] - workspace[i]);
      d_bottom[index] = d_bottom[index] * scalar;
    }
  }
  delete[] workspace;
}

template <typename T>
void masked_softmax_test(size_t batch_size, size_t head_num, size_t seq_len_from, size_t seq_len_to,
                         float scalar) {
  std::shared_ptr<GeneralBuffer2<CudaAllocator>> buf = GeneralBuffer2<CudaAllocator>::create();
  std::vector<size_t> dims_output = {batch_size, head_num, seq_len_from, seq_len_to};
  std::vector<size_t> dims_input = {batch_size, head_num, seq_len_from, seq_len_to};
  std::vector<size_t> dims_mask = {batch_size, 1, seq_len_from, seq_len_to};
  std::vector<size_t> dims_input_len = {batch_size};

  Tensors2<T> seq_mask_in_tensors;
  Tensor2<T> input_from_len_tensor;
  buf->reserve(dims_input_len, &input_from_len_tensor);
  seq_mask_in_tensors.push_back(input_from_len_tensor);
  Tensor2<T> input_to_len_tensor;
  buf->reserve(dims_input_len, &input_to_len_tensor);
  seq_mask_in_tensors.push_back(input_to_len_tensor);

  Tensors2<T> bottom_tensors;
  Tensor2<T> mask_tensor;
  Tensor2<T> input_tensor;

  buf->reserve(dims_input, &input_tensor);
  bottom_tensors.push_back(input_tensor);
  buf->reserve(dims_mask, &mask_tensor);
  bottom_tensors.push_back(mask_tensor);

  Tensor2<T> top_tensor;
  buf->reserve(dims_output, &top_tensor);

  MaskedSoftmaxLayer<T> masked_softmax_layer(bottom_tensors, top_tensor, scalar, buf,
                                             test::get_default_gpu());
  SequenceMaskLayer<T> sequence_mask_layer(seq_mask_in_tensors, mask_tensor, seq_len_from,
                                           seq_len_to, buf, test::get_default_gpu());
  buf->allocate();

  const size_t tensor_size = batch_size * head_num * seq_len_from * seq_len_to;

  std::unique_ptr<T[]> h_in_from_len(new T[batch_size]);
  std::unique_ptr<T[]> h_in_to_len(new T[batch_size]);
  std::unique_ptr<T[]> h_mask(new T[batch_size * seq_len_from * seq_len_to]);
  std::unique_ptr<T[]> h_bottom(new T[tensor_size]);
  std::unique_ptr<T[]> h_top(new T[tensor_size]);
  std::unique_ptr<T[]> h_softmax_out(new T[tensor_size]);
  std::unique_ptr<T[]> d2h_top(new T[tensor_size]);
  std::unique_ptr<T[]> h_bottom_grad(new T[tensor_size]);
  std::unique_ptr<T[]> d2h_bottom_grad(new T[tensor_size]);

  test::GaussianDataSimulator simulator(0.0f, 1.0f);
  simulator.fill(h_in_from_len.get(), batch_size);
  f2i_input(h_in_from_len.get(), batch_size, seq_len_from);
  simulator.fill(h_in_to_len.get(), batch_size);
  f2i_input(h_in_to_len.get(), batch_size, seq_len_from);

  simulator.fill(h_bottom.get(), tensor_size);

  // generate sequence mask
  HCTR_LIB_THROW(cudaMemcpy(input_from_len_tensor.get_ptr(), h_in_from_len.get(),
                            batch_size * sizeof(T), cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaMemcpy(input_to_len_tensor.get_ptr(), h_in_to_len.get(),
                            batch_size * sizeof(T), cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaMemcpy(input_tensor.get_ptr(), h_bottom.get(), tensor_size * sizeof(T),
                            cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaDeviceSynchronize());
  sequence_mask_layer.fprop(true);
  HCTR_LIB_THROW(cudaMemcpy(h_mask.get(), mask_tensor.get_ptr(),
                            batch_size * seq_len_from * seq_len_to * sizeof(T),
                            cudaMemcpyDeviceToHost));
  HCTR_LIB_THROW(cudaDeviceSynchronize());

  masked_softmax_layer.fprop(true);

  HCTR_LIB_THROW(cudaMemcpy(d2h_top.get(), top_tensor.get_ptr(), tensor_size * sizeof(T),
                            cudaMemcpyDeviceToHost));

  masked_softmax_fprop_cpu<T>(h_top.get(), h_bottom.get(), h_mask.get(), batch_size, head_num,
                              seq_len_from, seq_len_to, scalar);

  ASSERT_TRUE(test::compare_array_approx<T>(d2h_top.get(), h_top.get(), tensor_size, eps));

  // bprop
  simulator.fill(h_top.get(), tensor_size);
  masked_softmax_fprop_cpu<T>(h_softmax_out.get(), h_bottom.get(), h_mask.get(), batch_size,
                              head_num, seq_len_from, seq_len_to, scalar);

  HCTR_LIB_THROW(cudaMemcpy(top_tensor.get_ptr(), h_top.get(), tensor_size * sizeof(T),
                            cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaMemcpy(masked_softmax_layer.get_softmax_tensor().get_ptr(),
                            h_softmax_out.get(), tensor_size * sizeof(T), cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaDeviceSynchronize());
  masked_softmax_layer.bprop();
  HCTR_LIB_THROW(cudaDeviceSynchronize());
  HCTR_LIB_THROW(cudaMemcpy(d2h_bottom_grad.get(), input_tensor.get_ptr(), tensor_size * sizeof(T),
                            cudaMemcpyDeviceToHost));
  masked_softmax_bprop_cpu<T>(h_bottom_grad.get(), h_top.get(), h_softmax_out.get(),
                              batch_size * head_num * seq_len_from, seq_len_to, scalar);

  ASSERT_TRUE(
      test::compare_array_approx<T>(d2h_bottom_grad.get(), h_bottom_grad.get(), tensor_size, eps));
}
}  // namespace

TEST(masked_softmax_layer, fp32_16x2x16x32) { masked_softmax_test<float>(16, 2, 16, 32, 0.25); }
TEST(masked_softmax_layer, fp32_512x4x50x128) {
  masked_softmax_test<float>(512, 4, 50, 128, 0.884);
}
TEST(masked_softmax_layer, fp32_256x4x8x8) { masked_softmax_test<float>(256, 4, 8, 8, 0.353); }
