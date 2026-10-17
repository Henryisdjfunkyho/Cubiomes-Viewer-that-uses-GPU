# Bundled HIP headers for the NVIDIA backend

This directory contains the HIP public headers and NVIDIA overlay used to compile the project's HIP sources with Clang's CUDA frontend. The headers map HIP APIs to CUDA APIs; the CUDA Toolkit and NVIDIA driver remain external requirements. These headers are build support, not a bundled CUDA SDK or runtime.

The headers are pinned to matching upstream ROCm 7.2.4 source revisions:

- `ROCm/hip`, commit `bc9af25177f96c0fea93198b89cf4c3cf08f3ea3` (`include/hip/`).
- `ROCm/hipother`, commit `e56867449451fa4e12d068fedf502423a8d29191` (`hipnv/include/hip/nvidia_detail/`).
- `ROCm/clr`, commit `fe5035afc8713dfc6adedd3c00c4306c93a160f8` (the upstream AMD API header included by the common HIP API header).

The vendored set contains 43 headers (850,729 bytes): 30 common HIP headers (568,652 bytes), 12 NVIDIA overlay headers (271,718 bytes), and one small upstream AMD API header (10,359 bytes) required by the common header include graph. `hip_version.h` is generated for this pinned 7.2.4 header set with PCH disabled. The upstream license texts are retained as `LICENSE-hip.txt`, `LICENSE-hipother.txt`, and `LICENSE-clr.txt`; individual headers also retain their notices.

The NVIDIA build was validated with Clang 18.1.3, CUDA Toolkit 12.9.86, CMake 3.28.3, and GCC 13. On a host with an NVIDIA device, the build uses `nvidia-smi` to select that device's compute capability. Cross-building without a device uses the configured `sm_75` fallback. See the root README for validation scope and limitations.
