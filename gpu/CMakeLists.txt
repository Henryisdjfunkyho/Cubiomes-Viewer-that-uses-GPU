cmake_minimum_required(VERSION 3.21)

# CMake added its native NVCC-backed HIP platform in 3.28. Keep the existing
# 3.21 requirement for AMD ROCm, which is the currently validated path.
if(CMAKE_HIP_PLATFORM STREQUAL "nvidia" AND CMAKE_VERSION VERSION_LESS "3.28")
    message(FATAL_ERROR "HIP on NVIDIA requires CMake 3.28 or newer (found ${CMAKE_VERSION}).")
endif()

if(CVHIP_NVIDIA_CLANG_CUDA)
    # The upstream HIP compatibility layer supports Clang's CUDA frontend.
    # NVCC cannot compile the imported Cubiomes functions because the source
    # relies on Clang's force_cuda_host_device pragma.
    project(cubiomes_hip LANGUAGES CXX CUDA)
    set_source_files_properties(cvhip.hip cubiomes_device.hip PROPERTIES LANGUAGE CUDA)
    set(CMAKE_CUDA_STANDARD 17)
else()
    project(cubiomes_hip LANGUAGES CXX HIP)
    set(CMAKE_HIP_STANDARD 17)
endif()

set(CUBIOMES_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../cubiomes")
set(CUBIOMES_DEVICE_OVERLAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/cubiomes_device_overlay")
include("${CMAKE_CURRENT_LIST_DIR}/prepare_cubiomes_overlay.cmake")

add_library(cubiomes_hip SHARED cvhip.hip cubiomes_device.hip)
set_target_properties(cubiomes_hip PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cubiomes_hip BEFORE PRIVATE "${CUBIOMES_DEVICE_OVERLAY_DIR}")

if(CVHIP_NVIDIA_CLANG_CUDA)
    target_include_directories(cubiomes_hip PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/hip-nvidia/include")
    target_compile_definitions(cubiomes_hip PRIVATE __HIP_PLATFORM_NVIDIA__=1)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(cubiomes_hip PRIVATE CUDA::cudart)
    # Device functions and kernels stay within their own translation units;
    # only host API functions cross between cvhip.hip and cubiomes_device.hip.
    # Avoid relocatable device code so CMake does not invoke the incompatible
    # CUDA fatbinary -im rule observed with newer CUDA toolkits.
    set_target_properties(cubiomes_hip PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF
        CUDA_RESOLVE_DEVICE_SYMBOLS OFF)
    target_compile_options(cubiomes_hip PRIVATE
        "$<$<COMPILE_LANGUAGE:CUDA>:-O3>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-ffp-contract=off>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-fno-fast-math>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-fno-strict-aliasing>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-fwrapv>")
else()
    target_compile_options(cubiomes_hip PRIVATE
        -O3 -ffp-contract=off -fno-fast-math -fno-strict-aliasing -fwrapv
        -Wno-parentheses)
endif()
