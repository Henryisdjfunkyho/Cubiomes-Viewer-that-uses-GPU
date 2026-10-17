#ifndef CV_CUDA_COMPAT_H
#define CV_CUDA_COMPAT_H

#include <cuda_runtime.h>
#include <cstddef>

using hipError_t = cudaError_t;
using hipStream_t = cudaStream_t;
using hipEvent_t = cudaEvent_t;

constexpr hipError_t hipSuccess = cudaSuccess;
constexpr hipError_t hipErrorInvalidValue = cudaErrorInvalidValue;
constexpr unsigned hipStreamNonBlocking = cudaStreamNonBlocking;
constexpr unsigned hipEventDisableTiming = cudaEventDisableTiming;
constexpr cudaMemcpyKind hipMemcpyHostToDevice = cudaMemcpyHostToDevice;
constexpr cudaMemcpyKind hipMemcpyDeviceToHost = cudaMemcpyDeviceToHost;

template<class T>
inline hipError_t cvhipCudaMalloc(T **ptr, size_t size) {
    return cudaMalloc(reinterpret_cast<void **>(ptr), size);
}

template<class T>
inline hipError_t cvhipCudaHostMalloc(T **ptr, size_t size) {
    return cudaHostAlloc(reinterpret_cast<void **>(ptr), size, cudaHostAllocPortable);
}

#define hipGetDeviceCount cudaGetDeviceCount
#define hipGetErrorString cudaGetErrorString
#define hipSetDevice cudaSetDevice
#define hipMalloc cvhipCudaMalloc
#define hipFree cudaFree
#define hipMemcpy cudaMemcpy
#define hipMemcpyAsync cudaMemcpyAsync
#define hipMemsetAsync cudaMemsetAsync
#define hipHostMalloc cvhipCudaHostMalloc
#define hipHostFree cudaFreeHost
#define hipStreamCreateWithFlags cudaStreamCreateWithFlags
#define hipStreamSynchronize cudaStreamSynchronize
#define hipStreamDestroy cudaStreamDestroy
#define hipEventCreate cudaEventCreate
#define hipEventCreateWithFlags cudaEventCreateWithFlags
#define hipEventDestroy cudaEventDestroy
#define hipEventRecord cudaEventRecord
#define hipEventSynchronize cudaEventSynchronize
#define hipEventElapsedTime cudaEventElapsedTime
#define hipGetLastError cudaGetLastError

// This source has the same host/device conditional in both GPU builds.
#if defined(__CUDA_ARCH__) && !defined(__HIP_DEVICE_COMPILE__)
#define __HIP_DEVICE_COMPILE__ 1
#endif

#endif
