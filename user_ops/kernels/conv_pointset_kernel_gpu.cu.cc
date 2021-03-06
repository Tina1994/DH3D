/* Copyright 2017 ComputerGraphics Tuebingen. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
// Authors: Fabian Groh, Patrick Wieschollek, Hendrik P.A. Lensch
// Modifications copyright (C) 2013 <Technical University of Munich/Juan Du>

#if GOOGLE_CUDA

#define EIGEN_USE_GPU

#include <cub/cub.cuh>

#include "cuda_utils.h"
#include "conv_pointset_op.h"
#include "tensorflow/core/util/cuda_kernel_helper.h"

namespace ConvPointsetCuda {

using CudaLaunchConfig = ::tensorflow::CudaLaunchConfig;

constexpr __host__ __device__ int pmin(int x, int y) { return x <= y ? x : y; }

template <typename Dtype, typename NBtype, int C_N = 256,
          int C_Dout = 32, int C_Din = 64>
struct ForwardKernel;

template <typename Dtype, typename NBtype, int C_N, int C_Dout,
          int C_Din>
__global__ void runForwardKernel(
    const ForwardKernel<Dtype, NBtype, C_N, C_Dout, C_Din> kernel) {
  kernel();
}

template <typename Dtype, typename NBtype, int C_N, int C_Dout,
          int C_Din>
struct ForwardKernel {
  enum {
    PMIN = 3  // only for unrolling
  };

  void launch(int B) {
    dim3 block(C_N);
    dim3 grid((N - 1) / C_N + 1, (Dout - 1) / C_Dout + 1, B);

    size_t shm_size = (C_Din + 1) * C_Dout * sizeof(Dtype);

    runForwardKernel<<<grid, block, shm_size>>>((*this));
  }

  __device__ __forceinline__ void operator()() const {
    Dtype* s_shm = DynamicSharedMemory<Dtype>();

    Dtype* s_theta = (Dtype*)&s_shm[0];
    //Dtype* s_bias = (Dtype*)&s_shm[Dp * C_Din * C_Dout];
    Dtype* s_bias = (Dtype*)&s_shm[ C_Din * C_Dout];

    // glob ids
    int b = blockIdx.z;
    int n = blockIdx.x * C_N + threadIdx.x;

    Dtype result[C_Dout];
    for (int dout = 0; dout < C_Dout; ++dout) {
      result[dout] = 0;
    }

    for (int o_din = 0; o_din < Din; o_din += C_Din) {
      // load shm
      __syncthreads();
      for (int tid = threadIdx.x; tid < C_Din * C_Dout; tid += C_N) {
        int dp = tid / (C_Din * C_Dout);
        int din = (tid % (C_Din * C_Dout)) / C_Dout;
        int dout = tid % C_Dout;

        int g_dout = (dout + blockIdx.y * C_Dout);
        int g_din = o_din + din;

        if (g_dout < Dout && g_din < Din) {
          s_theta[din * C_Dout + dout] =
              d_theta[g_din * Dout + g_dout];
          if (!dp) s_bias[dout] = d_bias[g_dout];
        }
      }
      __syncthreads();

      if (n < N) {
        // Loop over K
        NBtype n_0 = d_neighborhood[b * K * N + n];
        for (int k = 0; k < K && n < N; ++k) {
          NBtype nk = d_neighborhood[b * K * N + k * N + n];

          // Loop over Din
          for (int din = 0; din < C_Din && (o_din + din) < Din; ++din) {
            Dtype fk = d_features[b * Din * N + (o_din + din) * N + nk];
            Dtype f_0 = d_features[b * Din * N + (o_din + din) * N + n_0];

            // Loop over partial Dout
            for (int dout = 0;
                 dout < C_Dout && (dout + blockIdx.y * C_Dout) < Dout; ++dout) {
              Dtype w = s_theta[din * C_Dout + dout];
              result[dout] += w * (fk - f_0); // feature relative distance
            }
          }
        }

        for (int dout = 0; dout < C_Dout && (dout + blockIdx.y * C_Dout) < Dout; ++dout) {
            result[dout] += s_bias[dout]; // feature relative distance
        }
      }
    }

    for (int dout = 0;
         dout < C_Dout && (dout + blockIdx.y * C_Dout) < Dout && n < N;
         ++dout) {
      d_output[b * Dout * N + (dout + blockIdx.y * C_Dout) * N + n] =
          result[dout];
    }
  }

  // features:     incoming features                        [B, Din, N].
  // neighborhood: all K nearest neighbors                	[B, K, N].
  const Dtype* d_features;
  const NBtype* d_neighborhood;

  // theta:		parameters for kernel function             	[Din, Dout].
  // bias:      parameters for kernel function              [Dout].
  const Dtype* d_theta;
  const Dtype* d_bias;

  // output:       each feature description for each point   	[B, Dout, N].
  Dtype* d_output;

  int N;
  int K;
  int Din;
  int Dout;
};

template <typename Dtype>
struct BackwardThetaKernel;

template <typename T>
__global__ void runBackwardKernel(const BackwardThetaKernel<T> kernel) {
  kernel();
}

template <typename Dtype>
struct BackwardThetaKernel {
  enum { C_N = 256, DP_MAX = 3, DEGREE_MAX = 2 };

  void launch() {
    dim3 block(C_N);
    dim3 grid(Dout, Din);

    runBackwardKernel<<<grid, block>>>((*this));
  }

  __device__ __forceinline__ void operator()() const {
    typedef cub::BlockReduce<Dtype, C_N> BlockReduce;
    __shared__ typename BlockReduce::TempStorage temp_storage;

    Dtype theta_diff = 0;

    Dtype bias_diff = 0;

    int dout = blockIdx.x;
    int din = blockIdx.y;

    for (int b = 0; b < B; ++b) {
      for (int n = threadIdx.x; n < N; n += C_N) {
        Dtype topdiff = d_topdiff[b * Dout * N + dout * N + n];

        for (int k = 0; k < K; ++k) {
          int nk0 = d_neigh[b * N * K + 0 * N + n];
          int nk = d_neigh[b * N * K + k * N + n];

          Dtype diff_feature = d_features[b * Din * N + din * N + nk] -
                               d_features[b * Din * N + din * N + nk0];

          theta_diff += diff_feature * topdiff;


        }
        bias_diff +=  topdiff;
      }
    }
    Dtype thread_data = theta_diff;
    Dtype aggregate = BlockReduce(temp_storage).Sum(thread_data, N);
    if (!threadIdx.x) {
        d_theta_out[din * Dout + dout] = aggregate;
      }
    __syncthreads();


    thread_data = bias_diff;

    aggregate = BlockReduce(temp_storage).Sum(thread_data, N);

    if (!threadIdx.x) d_bias_out[dout] = aggregate;
  }

  const Dtype* d_topdiff;
  const Dtype* d_features;
  const int* d_neigh;

  const Dtype* d_theta;
  const Dtype* d_bias;

  Dtype* d_theta_out;
  Dtype* d_bias_out;

  int B;
  int N;
  int K;
  int Ddegree;
  int Dp;
  int Din;
  int Dout;
};

template <typename Dtype>
struct BackwardFeatureKernel;

template <typename T>
__global__ void runBackwardKernel(const BackwardFeatureKernel<T> kernel) {
  kernel();
}

template <typename Dtype>
struct BackwardFeatureKernel {
  enum {
    C_N = 32,
    C_Dout = 32,  // multiple of Warpsize is better

    C_Din = 8  // reduce first
  };

  void launch(int B) {
    dim3 fblock(C_N, C_Din);
    dim3 fgrid((N - 1) / C_N + 1, (Din - 1) / C_Din + 1, B);

    const int theta_size = C_Din * C_Dout;
    const int bias_size = C_Dout;
    const int topdiff_size = C_N * C_Dout;

    const int nk_size = C_N * K;

    int shm =
        (theta_size + bias_size + topdiff_size) * sizeof(Dtype) +
        (nk_size) * sizeof(int);

    runBackwardKernel<<<fgrid, fblock, shm>>>((*this));
  }

  __device__ __forceinline__ void operator()() const {
    // extern __shared__ float s_shm[];
    Dtype* s_shm = DynamicSharedMemory<Dtype>();

    int i_n = threadIdx.x;
    int i_din = threadIdx.y;

    int b = blockIdx.z;
    int n = blockIdx.x * C_N + i_n;
    int din = blockIdx.y * C_Din + i_din;

    Dtype* s_theta = (Dtype*)&s_shm[0];
    Dtype* s_bias = (Dtype*)&s_theta[C_Din * C_Dout];
    Dtype* s_topdiff = (Dtype*)&s_bias[C_Dout];
    int* s_nk = (int*)&s_topdiff[C_N * C_Dout];

    for (int k = threadIdx.y; k < K && n < N; k += blockDim.y) {
      int nk = d_neigh[b * K * N + k * N + n];
      s_nk[k * C_N + i_n] = nk;
    }

    __syncthreads();

    for (int dout_outer = 0; dout_outer < (Dout - 1) / C_Dout + 1; ++dout_outer) {
      __syncthreads();

      // fill s_theta
      int dout = dout_outer * C_Dout + i_n;
      if (din < Din && dout < Dout) {
        s_theta[i_din * C_Dout + i_n] = d_theta[din * Dout + dout];
      }


        s_bias[i_n] = d_bias[dout];


      if (n < N) {
        for (int i_dout = threadIdx.y;
             i_dout < C_Dout && (dout_outer * C_Dout + i_dout) < Dout;
             i_dout += blockDim.y)
          s_topdiff[i_dout * C_N + i_n] =
              d_topdiff[b * Dout * N + (dout_outer * C_Dout + i_dout) * N + n];
      }

      for (int dout_inner = 0;
           dout_inner < C_Dout && (dout_outer * C_Dout + dout_inner) < Dout;
           ++dout_inner) {
        for (int k = 0; k < K; k++) {
          __syncthreads();

          if (n < N && din < Din) {
            Dtype W = s_theta[i_din * C_Dout + dout_inner];
            Dtype value = W * s_topdiff[dout_inner * C_N + i_n];


            // atomicAdd(
            //     &d_features_out[b * Din * N + din * N + s_nk[k * C_N + i_n]],
            //     value);
            tensorflow::CudaAtomicAdd(
                &d_features_out[b * Din * N + din * N + s_nk[k * C_N + i_n]],
                value);
            tensorflow::CudaAtomicAdd(
                &d_features_out[b * Din * N + din * N + s_nk[0 * C_N + i_n]],
                -value);
          }
        }
      }
    }
  }

  const Dtype* d_topdiff;
  const Dtype* d_features;
  const int* d_neigh;
  const Dtype* d_theta;
  const Dtype* d_bias;

  Dtype* d_features_out;

  int N;
  int K;
  int Din;
  int Dout;
};

}

namespace tensorflow {
namespace functor {

template <class Dtype, class NBtype>
struct ForwardKernelType {
  typedef ConvPointsetCuda::ForwardKernel<Dtype, NBtype, 128, 32, 32> type;
};

template <>
struct ForwardKernelType<float, int> {
  typedef ConvPointsetCuda::ForwardKernel<float, int, 128, 32, 64> type;
};

template <typename Dtype>
struct ConvPointsetFunctor<GPUDevice, Dtype> {
  void operator()(::tensorflow::OpKernelContext* ctx, const Tensor& features,
                  const Tensor& theta, const Tensor& bias,
                  const Tensor& neighborhood,
                  Tensor* output) {
    typedef int NBtype;

    const int B = neighborhood.dim_size(0);
    const int K = neighborhood.dim_size(1);
    const int N = neighborhood.dim_size(2);

    const int Din = theta.dim_size(0);
    const int Dout = theta.dim_size(1);

    //    printf("<f> test: %s\n", __PRETTY_FUNCTION__);

    typedef typename ForwardKernelType<Dtype, NBtype>::type FKT;

    FKT fwk;
    fwk.N = N;
    fwk.K = K;
    fwk.Din = Din;
    fwk.Dout = Dout;

    fwk.d_features = features.flat<Dtype>().data();
    fwk.d_neighborhood = neighborhood.flat<NBtype>().data();
    fwk.d_theta = theta.flat<Dtype>().data();
    fwk.d_bias = bias.flat<Dtype>().data();
    fwk.d_output = output->flat<Dtype>().data();

    fwk.launch(B);

    if (!ctx->eigen_gpu_device().ok()) {
      ctx->SetStatus(tensorflow::errors::Internal(
          "ConvPointsetFunctor::forward::ForwardKernel execution failed"));
    }
  }
};

template struct ConvPointsetFunctor<GPUDevice, float>;
template struct ConvPointsetFunctor<GPUDevice, double>;

template <typename Dtype>
struct ConvPointsetGrad<GPUDevice, Dtype> {
  void operator()(::tensorflow::OpKernelContext* ctx, const Tensor& features_,
                  const Tensor& theta_, const Tensor& bias_,
                  const Tensor& neighborhood_,
                  const Tensor& topdiff_, Tensor* grad_features_,
                  Tensor* grad_theta_, Tensor* grad_bias_) {
    const auto features = features_.tensor<Dtype, 3>();
    const auto theta = theta_.tensor<Dtype, 2>();
    const auto bias = bias_.tensor<Dtype, 1>();
    const auto neighborhood = neighborhood_.tensor<int, 3>();
    const auto topdiff = topdiff_.tensor<Dtype, 3>();

    auto grad_features = grad_features_->tensor<Dtype, 3>();
    auto grad_theta = grad_theta_->tensor<Dtype, 2>();
    auto grad_bias = grad_bias_->tensor<Dtype, 1>();

    // get dimensions
    const int B = neighborhood_.dim_size(0);
    const int K = neighborhood_.dim_size(1);
    const int N = neighborhood_.dim_size(2);

    const int Din = theta_.dim_size(0);
    const int Dout = theta_.dim_size(1);

    const int* neighborhood_ptr =
        reinterpret_cast<const int*>(neighborhood.data());

    const Dtype* features_ptr = reinterpret_cast<const Dtype*>(features.data());
    const Dtype* theta_ptr = reinterpret_cast<const Dtype*>(theta.data());
    const Dtype* bias_ptr = reinterpret_cast<const Dtype*>(bias.data());

    const Dtype* topdiff_ptr = reinterpret_cast<const Dtype*>(topdiff.data());

    Dtype* grad_features_ptr = reinterpret_cast<Dtype*>(grad_features.data());
    Dtype* grad_theta_ptr = reinterpret_cast<Dtype*>(grad_theta.data());
    Dtype* grad_bias_ptr = reinterpret_cast<Dtype*>(grad_bias.data());

    cudaMemset(grad_features_ptr, 0, B * Din * N * sizeof(Dtype));

    ::tensorflow::CudaLaunchConfig cfg =
        ::tensorflow::GetCudaLaunchConfig(N, ctx->eigen_device<GPUDevice>());

    typedef ConvPointsetCuda::BackwardFeatureKernel<Dtype> BFK;

    BFK bfk;
    bfk.N = N;
    bfk.K = K;
    bfk.Din = Din;
    bfk.Dout = Dout;


    bfk.d_neigh = neighborhood_ptr;
    bfk.d_features = features_ptr;
    bfk.d_theta = theta_ptr;
    bfk.d_bias = bias_ptr;

    bfk.d_topdiff = topdiff_ptr;

    bfk.d_features_out = grad_features_ptr;

    bfk.launch(B);

    if (!ctx->eigen_gpu_device().ok()) {
      ctx->SetStatus(
          tensorflow::errors::Internal("CUDA: BackwardFeatureKernel Error!\n"));
    }

    typedef ConvPointsetCuda::BackwardThetaKernel<Dtype> BTK;

    BTK btk;
    btk.B = B;
    btk.N = N;
    btk.K = K;
    btk.Din = Din;
    btk.Dout = Dout;

    btk.d_neigh = neighborhood_ptr;
    btk.d_features = features_ptr;
    btk.d_theta = theta_ptr;
    btk.d_bias = bias_ptr;

    btk.d_topdiff = topdiff_ptr;

    btk.d_theta_out = grad_theta_ptr;
    btk.d_bias_out = grad_bias_ptr;

    btk.launch();

    if (!ctx->eigen_gpu_device().ok()) {
      ctx->SetStatus(
          tensorflow::errors::Internal("CUDA: BackwardThetaKernel Error!\n"));
    }
  }
};

template struct ConvPointsetGrad<GPUDevice, float>;
template struct ConvPointsetGrad<GPUDevice, double>;

}  // namespace functor
}  // namespace tensorflow

#endif  // GOOGLE_CUDA
