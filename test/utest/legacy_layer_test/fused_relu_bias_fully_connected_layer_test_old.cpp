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

#include <cublas_v2.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <layers/fused_relu_bias_fully_connected_layer.hpp>
#include <utest/test_utils.hpp>
#include <vector>

using namespace HugeCTR;

static void cpu_mm(__half *c, const __half *a, bool transpose_a, const __half *b, bool transpose_b,
                   int m, int k, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      float sum = 0.0f;
      for (int kk = 0; kk < k; ++kk) {
        int ai = transpose_a ? kk * m + i : i * k + kk;
        int bi = transpose_b ? j * k + kk : kk * n + j;
        sum += __half2float(a[ai]) * __half2float(b[bi]);
      }
      c[i * n + j] = sum;
    }
  }
}

static void cpu_add_bias_and_re(__half *top, __half *middle, const __half *bias, int m, int n) {
  for (int i = 0; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      __half t = top[i * n + j] + bias[j];
      middle[i * n + j] = t;
      top[i * n + j] = __half2float(t) < 0 ? __float2half(0.0f) : t;
    }
  }
}

static void cpu_reverse_add_bias_and_re(__half *bias_grad, __half *top, const __half *bprop_out,
                                        int m, int n) {
  for (int i = 0; i < m; ++i)
    for (int j = 0; j < n; ++j) {
      if (__half2float(top[i * n + j]) < 0) {
        top[i * n + j] = 0.0f;
      } else {
        top[i * n + j] = bprop_out[i * n + j];
      }
    }

  for (int i = 0; i < n; ++i) {
    float sum = 0.0f;
    for (int j = 0; j < m; ++j) sum += __half2float(top[j * n + i]);
    bias_grad[i] = sum;
  }
}

static float compare_array(const __half *arr1, const __half *arr2, size_t n, float threshold) {
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    if (fabs(__half2float(arr1[i] - arr2[i])) > threshold) {
      m++;
    }
  }
  return 1.0f * m / n;
}

static void fully_connected_layer_test(size_t m, size_t n, size_t k) {
  HCTR_LOG(INFO, WORLD, "Testing m=%zu, n=%zu, k=%zu\n", m, n, k);

  test::GaussianDataSimulator simulator(0.0f, 1.0f);

  std::shared_ptr<GeneralBuffer2<CudaAllocator>> blobs_buff =
      GeneralBuffer2<CudaAllocator>::create();
  std::shared_ptr<BufferBlock2<float>> master_weights_buff = blobs_buff->create_block<float>();
  std::shared_ptr<BufferBlock2<__half>> weights_buff = blobs_buff->create_block<__half>();
  std::shared_ptr<BufferBlock2<__half>> weights_grad_buff = blobs_buff->create_block<__half>();

  Tensor2<__half> train_in_tensor, mask_in_tensor, dRelu_in_tensor, db_in_tensor;
  blobs_buff->reserve({m, k}, &train_in_tensor);
  blobs_buff->reserve({m, k}, &mask_in_tensor);
  Tensor2<__half> train_out_tensor, mask_out_tensor, dRelu_out_tensor, db_out_tensor;
  blobs_buff->reserve({m, n}, &train_out_tensor);
  blobs_buff->reserve({m, n}, &mask_out_tensor);
  blobs_buff->reserve({m, n}, &dRelu_out_tensor);

  FusedReluBiasFullyConnectedLayer fully_connected_layer(
      master_weights_buff, weights_buff, weights_grad_buff, blobs_buff, train_in_tensor,
      mask_in_tensor, dRelu_in_tensor, db_in_tensor, train_out_tensor, mask_out_tensor,
      dRelu_out_tensor, db_out_tensor, test::get_default_gpu(), FcPosition_t::Isolated,
      Activation_t::Relu, false, std::vector<Initializer_t>(), false, true);

  // Initialize tensors to 0 and choose cublas algorithms
  blobs_buff->allocate();
  fully_connected_layer.initialize();
  // fully_connected_layer.search_algorithm();
  // Reset tensors to 0 to ensure all the data are the same as original utest(clear the side effect
  // of optimize)

  Tensor2<__half> weights = weights_buff->as_tensor();
  Tensor2<__half> weights_grad = weights_grad_buff->as_tensor();
  HCTR_LIB_THROW(cudaMemset(weights.get_ptr(), 0, weights.get_size_in_bytes()));
  HCTR_LIB_THROW(cudaMemset(weights_grad.get_ptr(), 0, weights_grad.get_size_in_bytes()));
  // TODO: result check
  __half *d_kernel = weights.get_ptr();
  __half *d_bias = weights.get_ptr() + k * n;
  __half *d_kernel_grad = weights_grad.get_ptr();
  __half *d_bias_grad = weights_grad.get_ptr() + k * n;
  __half *d_bottom = train_in_tensor.get_ptr();
  __half *d_bprop_in = mask_in_tensor.get_ptr();
  __half *d_top = train_out_tensor.get_ptr();
  __half *d_mask_out = mask_out_tensor.get_ptr();

  std::unique_ptr<__half[]> h_kernel(new __half[k * n]);
  std::unique_ptr<__half[]> h_kernel_grad(new __half[k * n]);
  std::unique_ptr<__half[]> h_bias_grad(new __half[n]);
  std::unique_ptr<__half[]> h_bottom(new __half[m * k]);
  std::unique_ptr<__half[]> h_bprop_in(new __half[m * k]);
  std::unique_ptr<__half[]> h_middle(new __half[m * n]);
  std::unique_ptr<__half[]> h_top(new __half[m * n]);
  std::unique_ptr<__half[]> h_bprop_out(new __half[m * n]);
  std::unique_ptr<__half[]> h_bias(new __half[n]);

  std::unique_ptr<__half[]> d2h_top(new __half[m * n]);
  std::unique_ptr<__half[]> d2h_bprop_in(new __half[m * k]);
  std::unique_ptr<__half[]> d2h_bottom(new __half[m * k]);
  std::unique_ptr<__half[]> d2h_kernel_grad(new __half[k * n]);
  std::unique_ptr<__half[]> d2h_bias_grad(new __half[n]);

  simulator.fill(h_bottom.get(), m * k);
  simulator.fill(h_kernel.get(), k * n);
  simulator.fill(h_bias.get(), n);

  HCTR_LIB_THROW(
      cudaMemcpy(d_kernel, h_kernel.get(), sizeof(__half) * k * n, cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(cudaMemcpy(d_bias, h_bias.get(), sizeof(__half) * n, cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(
      cudaMemcpy(d_bottom, h_bottom.get(), sizeof(__half) * m * k, cudaMemcpyHostToDevice));

  // cpu fprop
  cpu_mm(h_top.get(), h_bottom.get(), false, h_kernel.get(), false, m, k, n);
  cpu_add_bias_and_re(h_top.get(), h_middle.get(), h_bias.get(), m, n);

  // gpu fprop
  HCTR_LIB_THROW(cudaDeviceSynchronize());
  fully_connected_layer.fprop(true);
  HCTR_LIB_THROW(cudaDeviceSynchronize());

  HCTR_LIB_THROW(cudaMemcpy(d2h_top.get(), d_top, sizeof(__half) * m * n, cudaMemcpyDeviceToHost));

  // check result
  ASSERT_LT(compare_array(h_top.get(), d2h_top.get(), m * n, 1e-3), 0.15f)
      << "fprop cross_check result fail" << std::endl;

  simulator.fill(h_top.get(), m * n);
  simulator.fill(h_bprop_out.get(), m * n);

  HCTR_LIB_THROW(
      cudaMemcpy(d_top, h_bprop_out.get(), sizeof(__half) * m * n, cudaMemcpyHostToDevice));
  HCTR_LIB_THROW(
      cudaMemcpy(d_mask_out, h_top.get(), sizeof(__half) * m * n, cudaMemcpyHostToDevice));

  // cpu bprop
  cpu_reverse_add_bias_and_re(h_bias_grad.get(), h_top.get(), h_bprop_out.get(), m, n);

  cpu_mm(h_kernel_grad.get(), h_bottom.get(), true, h_top.get(), false, k, m, n);
  cpu_mm(h_bprop_in.get(), h_top.get(), false, h_kernel.get(), true, m, n, k);

  // gpu bprop
  HCTR_LIB_THROW(cudaDeviceSynchronize());
  fully_connected_layer.bprop();
  HCTR_LIB_THROW(cudaDeviceSynchronize());

  HCTR_LIB_THROW(
      cudaMemcpy(d2h_bprop_in.get(), d_bprop_in, sizeof(__half) * m * k, cudaMemcpyDeviceToHost));
  HCTR_LIB_THROW(cudaMemcpy(d2h_kernel_grad.get(), d_kernel_grad, sizeof(__half) * k * n,
                            cudaMemcpyDeviceToHost));
  HCTR_LIB_THROW(
      cudaMemcpy(d2h_bias_grad.get(), d_bias_grad, sizeof(__half) * n, cudaMemcpyDeviceToHost));

  // check result
  ASSERT_LT(compare_array(h_bprop_in.get(), d2h_bprop_in.get(), m * k, 1e-1), 0.05f)
      << " bprop cross_check input_grad fail" << std::endl;
  ASSERT_LT(compare_array(h_kernel_grad.get(), d2h_kernel_grad.get(), k * n, 1e-1), 0.05f)
      << " bprop cross_check weight_grad fail" << std::endl;
  ASSERT_LT(compare_array(h_bias_grad.get(), d2h_bias_grad.get(), n, 1e-1), 0.05f)
      << " bprop cross_check bias_grad fail" << std::endl;
}

TEST(fused_relu_bias_fully_connected_layer_old, fp16_32x64x32) {
  fully_connected_layer_test(32, 128, 32);
}
TEST(fused_relu_bias_fully_connected_layer_old, fp16_2048x512x16) {
  fully_connected_layer_test(2048, 512, 16);
}
TEST(fused_relu_bias_fully_connected_layer_old, fp16_2048x1024x480) {
  fully_connected_layer_test(2048, 1024, 480);
}
TEST(fused_relu_bias_fully_connected_layer_old, fp16_2048x512x1024) {
  fully_connected_layer_test(2048, 512, 1024);
}
TEST(fused_relu_bias_fully_connected_layer_old, fp16_2048x1024x1024) {
  fully_connected_layer_test(2048, 1024, 1024);
}
