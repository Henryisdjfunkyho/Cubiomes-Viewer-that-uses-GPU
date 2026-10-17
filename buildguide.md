# Build Cubiomes Viewer that uses GPU

Repository: [Cubiomes Viewer that uses GPU](https://github.com/Henryisdjfunkyho/Cubiomes-Viewer-that-uses-GPU).

## Get the source

Clone with `--recurse-submodules`. Git will download the Viewer and its required Cubiomes library by xpple into the `cubiomes` folder:

```sh
git clone --recurse-submodules https://github.com/Henryisdjfunkyho/Cubiomes-Viewer-that-uses-GPU.git
cd Cubiomes-Viewer-that-uses-GPU
```

You do not need to install Cubiomes separately, even if another copy is already on your computer. The Viewer builds the pinned copy in its `cubiomes` folder as part of the normal build.

If you already cloned the Viewer without the library, run this from the project folder:

```sh
git submodule update --init --recursive
```

The GitHub source ZIP does not include the library files. Use the recursive clone above instead of downloading the source ZIP.

This guide builds the Viewer from source. It does not install a ready-to-run release. Packaged downloads are not available yet.

## Dependencies

Every source build needs:

- Git, to download the Viewer and its Cubiomes submodule.
- [Qt 5.9 or newer](https://www.qt.io/download-qt-installer), with Qt 5 `qmake` for your compiler and system.
- A C/C++ compiler. Use GCC or Clang on Linux. On Windows, use the MinGW compiler that matches your Qt kit.
- [CMake 3.21 or newer](https://cmake.org/download/) and [Ninja](https://github.com/ninja-build/ninja/releases).
- GNU Make on Linux, or `mingw32-make` from the Qt MinGW kit on Windows.

The recursive clone downloads the pinned Cubiomes submodule. The normal build compiles it, so there is no separate Cubiomes installation.

For GPU BOOST, install the toolkit for your GPU:

| GPU | Windows | Linux |
| --- | --- | --- |
| AMD | [AMD HIP SDK](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/) | [ROCm](https://rocm.docs.amd.com/en/latest/install/rocm.html) |
| NVIDIA | [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads), [LLVM Clang with NVPTX support](https://releases.llvm.org/), [C++ build tools](https://visualstudio.microsoft.com/downloads/), and Windows SDK | [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) and [LLVM Clang with NVPTX support](https://releases.llvm.org/) |

Windows NVIDIA builds also need Clang built for the MSVC toolchain.

## GPU support

GPU BOOST checks supported search conditions on the GPU. The CPU still checks candidate seeds and handles conditions the GPU does not support. If GPU setup fails, the Viewer can continue with its CPU search path.

Your existing graphics driver is enough if it supports your GPU and the toolkit. Do not reinstall or update it just for this build.

### AMD GPUs

The HIP SDK and ROCm provide the tools needed to build and run GPU BOOST. Check AMD's [GPU and OS compatibility list](https://rocm.docs.amd.com/en/latest/compatibility/compatibility-matrix.html) to confirm your GPU is supported. Supported models differ between Windows and Linux. Linux AMD GPU runtime support has not yet been tested with this Viewer.

### NVIDIA GPUs

The NVIDIA build uses Clang and the CUDA Toolkit with the GPU code included in this project. Windows and Linux use the same GPU code. You do not need ROCm or AMD's HIP SDK. For details on Clang and CUDA, see the [LLVM CUDA guide](https://llvm.org/docs/CompileCudaWithLLVM.html).

On Windows, Clang must be built for the MSVC toolchain. Install [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/) with the C++ build tools and Windows SDK. The Viewer itself still uses Qt's MinGW kit. Open the **x64 Native Tools Command Prompt for VS 2022**, then add the Qt and matching MinGW `bin` folders to `PATH`. The prompt provides the linker and Windows SDK for Clang; `qmake` and `mingw32-make` come from the Qt kit.

The build uses `nvidia-smi` to find GPU 0 and select its architecture. The CUDA installer usually sets `CUDA_PATH` on Windows. On Linux, CUDA is often installed under `/usr/local/cuda`. If yours is somewhere else, set `CUDA_PATH` or `CUDA_HOME` before building. For example, if CUDA is installed under `/opt/cuda` on Arch Linux, run:

```sh
export CUDA_PATH=/opt/cuda
```

On Windows, `qmake` stops if it detects an NVIDIA GPU but cannot find the required CUDA and Clang tools. Other compiler compatibility errors may appear during the build. The build targets the detected GPU architecture, so a build made for one architecture may not run on a different NVIDIA GPU.

### Other GPUs

If your hardware is not covered above, GPU BOOST may not build or run. The Viewer can still use its CPU processing path.

## Build

On Linux, open a terminal where Qt 5, CMake, Ninja, and your compiler are available on `PATH`. Then run:

```sh
mkdir -p build
cd build
qmake ..
make -j4
```

On Windows with AMD, open the command prompt supplied with your Qt 5 MinGW kit. With NVIDIA, open the x64 Native Tools Command Prompt for VS 2022 and add the Qt and MinGW `bin` folders to `PATH` first. Then run:

```bat
if not exist build mkdir build
cd build
qmake ..
mingw32-make
```

The Linux executable is `build/cubiomes-viewer`. The Windows executable is usually `build/release/cubiomes-viewer.exe`. If `qmake` or make is not found, open the Qt kit's command prompt or add the Qt and matching compiler `bin` folders to `PATH`.

These `qmake` and make commands build the Viewer and any GPU backend detected during setup. The build places the GPU library beside the Viewer executable. Keep the two files together. The program may also need runtime files from the GPU toolkit. Ready-to-run packages are not available yet.

## Troubleshooting

- If the Viewer builds without GPU BOOST, check that the GPU toolkit is installed and that its tools are available in the build terminal. Then build again in a new folder.
- For NVIDIA, check that `nvidia-smi` can find GPU 0 and that Clang supports your GPU. If CUDA is installed outside its usual folder, set `CUDA_PATH` or `CUDA_HOME` before building.
- On Windows with NVIDIA, use the x64 Native Tools Command Prompt for VS 2022. Make sure the Visual Studio C++ tools, Windows SDK, Qt, and MinGW are available there.
- On AMD, set `HIP_PATH` or `ROCM_PATH` if the toolkit is installed outside its usual folder.
- If the program reports that a runtime file is missing, install the matching GPU toolkit. Do not copy graphics driver files from another computer.
