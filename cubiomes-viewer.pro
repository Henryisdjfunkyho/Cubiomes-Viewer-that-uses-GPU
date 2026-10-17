#-------------------------------------------------
#
# Project created by QtCreator 2020-07-11T11:37:33
#
#-------------------------------------------------

QT += core widgets

# uncomment to override the profile compiler
#QMAKE_CC = clang
#QMAKE_CXX = clang++

CHARSET                 = -finput-charset=UTF-8 -fexec-charset=UTF-8
QMAKE_CFLAGS            = $$CHARSET -fwrapv -DSTRUCT_CONFIG_OVERRIDE=1
QMAKE_CXXFLAGS          = $$QMAKE_CFLAGS
QMAKE_CXXFLAGS_RELEASE  *= -O3 -g3

greaterThan(QT_MAJOR_VERSION, 5) {
    QMAKE_CXXFLAGS += -std=gnu++17
    DEFINES += QT_DISABLE_DEPRECATED_UP_TO=0x050F00
} else {
    QMAKE_CXXFLAGS += -std=gnu++11
    equals(QMAKE_CXX, g++) {
        QMAKE_CXXFLAGS += -Wno-deprecated-copy
    }
}

QMAKE_APPLE_DEVICE_ARCHS = x86_64 arm64

win32: {
    CONFIG += static_gnu
    # thank you nullprogram for dealing with the Windows UTF-16 nonsense
    LIBWINSANE          = $$PWD/src/libwinsane
    libwinsane.target   = libwinsane
    libwinsane.output   = $$LIBWINSANE/libwinsane.o
    libwinsane.commands = $(MAKE) -C $$LIBWINSANE -f $$LIBWINSANE/Makefile
    QMAKE_EXTRA_TARGETS += libwinsane
    PRE_TARGETDEPS      += libwinsane
    LIBS                += $$LIBWINSANE/libwinsane.o
} else {
    DEFINES += "LUA_USE_POSIX=1"
}

wasm: {
    DEFINES += "WASM=1"
    #QT_WASM_SOURCE_MAP=1
    QT_WASM_INITIAL_MEMORY = 256MB
    QT_WASM_PTHREAD_POOL_SIZE = 32
    CONFIG(debug, debug|release): {
        #QMAKE_CFLAGS += -O3 -gsource-map
    }
}
#CONFIG += sanitizer
#CONFIG += sanitize_undefined
#CONFIG += sanitize_thread

static_gnu: {
    LIBS += -static -static-libgcc -static-libstdc++
}

CONFIG(debug, debug|release): {
    CUTARGET = debug
} else {
    CUTARGET = release
}

# compile cubiomes
CUPATH              = $$PWD/cubiomes
QMAKE_PRE_LINK += cmake -S $$CUPATH -B lib_build -G Ninja -DCMAKE_C_COMPILER=$$QMAKE_CC -DCMAKE_CXX_COMPILER=$$QMAKE_CXX -DCMAKE_BUILD_TYPE=Release && cmake --build lib_build --target cubiomes_static
CVHIP_OUTPUT = $$OUT_PWD
win32: CVHIP_OUTPUT = $$OUT_PWD/$$CUTARGET
!isEmpty(DESTDIR): CVHIP_OUTPUT = $$absolute_path($$DESTDIR, $$OUT_PWD)
!contains(DEFINES, CVHIP_CUDA_ONLY) {
HIP_ROOT = $$(HIP_PATH)
isEmpty(HIP_ROOT): HIP_ROOT = $$(ROCM_PATH)
HIP_ROOT = $$replace(HIP_ROOT, \\\\, /)
HIP_ROOT = $$clean_path($$HIP_ROOT)
!exists($$HIP_ROOT/include/hip/hip_runtime.h) {
    HIP_ROOT =
    HIP_ROOTS = /opt/rocm
    win32: HIP_ROOTS += $$files("C:/Program Files/AMD/ROCm/*")
    for(root, HIP_ROOTS) {
        exists($$root/include/hip/hip_runtime.h): HIP_ROOT = $$root
    }
}
HIP_AMD_COMPILER = $$HIP_ROOT/bin/clang++
win32: HIP_AMD_COMPILER = $${HIP_AMD_COMPILER}.exe
!exists($$HIP_AMD_COMPILER) {
    HIP_AMD_COMPILER = $$HIP_ROOT/llvm/bin/clang++
    win32: HIP_AMD_COMPILER = $${HIP_AMD_COMPILER}.exe
}
HIP_BACKEND =
NVIDIA_CUDA_ROOT = $$(CUDA_PATH)
isEmpty(NVIDIA_CUDA_ROOT): NVIDIA_CUDA_ROOT = $$(CUDA_HOME)
isEmpty(NVIDIA_CUDA_ROOT): unix: NVIDIA_CUDA_ROOT = /usr/local/cuda
NVIDIA_CUDA_ROOT = $$replace(NVIDIA_CUDA_ROOT, \\, /)
NVIDIA_CUDA_ROOT = $$clean_path($$NVIDIA_CUDA_ROOT)
NVIDIA_CLANG =
NVIDIA_SMI =
win32: {
    # The Viewer may use MinGW, but the separate CUDA DLL must use one
    # Windows ABI for both of CMake's compiler checks. Prefer the standard
    # LLVM install, then accept another Clang only if it has NVPTX and targets
    # the MSVC ABI. AMD HIP SDK's Clang often lacks the NVPTX backend.
    NVIDIA_CLANG_CANDIDATES =
    exists("C:/Program Files/LLVM/bin/clang++.exe"): NVIDIA_CLANG_CANDIDATES += "C:/Program Files/LLVM/bin/clang++.exe"
    NVIDIA_CLANG_CANDIDATES += $$system(where.exe clang++.exe 2>NUL, lines)
    for(NVIDIA_CANDIDATE, NVIDIA_CLANG_CANDIDATES) {
        isEmpty(NVIDIA_CLANG) {
            NVIDIA_CANDIDATE = $$system_path($$NVIDIA_CANDIDATE)
            NVIDIA_CANDIDATE_TARGETS = $$system($$system_quote($$NVIDIA_CANDIDATE) --print-targets 2>NUL, lines)
            NVIDIA_CANDIDATE_VERSION = $$system($$system_quote($$NVIDIA_CANDIDATE) --version 2>NUL, lines)
            contains(NVIDIA_CANDIDATE_TARGETS, .*nvptx.*):contains(NVIDIA_CANDIDATE_VERSION, .*windows-msvc.*): NVIDIA_CLANG = $$NVIDIA_CANDIDATE
        }
    }

    # NVIDIA's Windows driver installs nvidia-smi here, but it is not always
    # added to PATH. Prefer that copy, then use a PATH installation.
    exists("C:/Program Files/NVIDIA Corporation/NVSMI/nvidia-smi.exe"): NVIDIA_SMI = "C:/Program Files/NVIDIA Corporation/NVSMI/nvidia-smi.exe"
    isEmpty(NVIDIA_SMI) {
        NVIDIA_SMI_PATHS = $$system(where.exe nvidia-smi.exe 2>NUL, lines)
        NVIDIA_SMI = $$first(NVIDIA_SMI_PATHS)
    }
}
unix: {
    NVIDIA_CLANG = $$system(command -v clang++ 2>/dev/null)
    NVIDIA_CLANG = $$first(NVIDIA_CLANG)
    NVIDIA_SMI = $$system(command -v nvidia-smi 2>/dev/null)
    NVIDIA_SMI = $$first(NVIDIA_SMI)
}
NVIDIA_GPU =
!isEmpty(NVIDIA_SMI) {
    win32 {
        # CSV output without noheader is split into qmake's output lines.
        # The first line is the column name, the second is GPU 0's value.
        NVIDIA_GPU_CMD = $$system_quote($$system_path($$NVIDIA_SMI)) --id=0 --query-gpu=name --format=csv 2>NUL
        NVIDIA_GPU_ROWS = $$system($$NVIDIA_GPU_CMD, lines)
        NVIDIA_GPU = $$member(NVIDIA_GPU_ROWS, 1)
    } else:unix {
        # Avoid commas in $$system() arguments: qmake treats them as separators.
        # Query GPU 0 because the runtime currently selects device 0 as well.
        NVIDIA_GPU_CMD = $$system_quote($$NVIDIA_SMI) --id=0 --query-gpu=name --format=csv 2>/dev/null | sed -n '2p'
        NVIDIA_GPU = $$system($$NVIDIA_GPU_CMD)
        NVIDIA_GPU = $$first(NVIDIA_GPU)
    }
}
NVIDIA_TOOLCHAIN =
exists($$NVIDIA_CUDA_ROOT/include/cuda.h):exists($$NVIDIA_CLANG): NVIDIA_TOOLCHAIN = yes
win32:!isEmpty(NVIDIA_GPU):isEmpty(NVIDIA_TOOLCHAIN) {
    error("NVIDIA GPU '$$NVIDIA_GPU' was found, but the Windows NVIDIA build tools are missing. Install the CUDA Toolkit and LLVM Clang with NVPTX support and an MSVC Windows target, then set CUDA_PATH if needed. The build will not silently select another backend or CPU-only on this configuration.")
}

# Prefer NVIDIA when its device and Clang/CUDA toolchain are present, even if
# an AMD toolkit is also installed. Without an NVIDIA device, retain the AMD
# path when available; CUDA-only cross builds still select the NVIDIA backend.
!isEmpty(NVIDIA_GPU):!isEmpty(NVIDIA_TOOLCHAIN): HIP_BACKEND = nvidia
isEmpty(HIP_BACKEND):exists($$HIP_ROOT/include/hip/hip_runtime.h):exists($$HIP_AMD_COMPILER): HIP_BACKEND = amd
isEmpty(HIP_BACKEND):!isEmpty(NVIDIA_TOOLCHAIN): HIP_BACKEND = nvidia

!isEmpty(HIP_BACKEND) {
    equals(HIP_BACKEND, amd) {
        HIP_COMPILER = $$HIP_AMD_COMPILER
        GPU_CXX_COMPILER = $$HIP_COMPILER
        HIP_BUILD = $$OUT_PWD/hip_backend
    } else {
        HIP_COMPILER = $$NVIDIA_CLANG
        GPU_CXX_COMPILER = $$QMAKE_CXX
        # The GPU library is a separate DLL with a plain C ABI. Build it with
        # the same MSVC-target Clang driver as CUDA, not Qt's MinGW compiler.
        win32: GPU_CXX_COMPILER = $$NVIDIA_CLANG
        HIP_BUILD = $$OUT_PWD/hip_backend_nvidia
    }
    HIP_LIBRARY = $$HIP_BUILD/libcubiomes_hip.so
    win32: HIP_LIBRARY = $$HIP_BUILD/cubiomes_hip.dll
    HIP_BACKEND_OPTIONS =
    equals(HIP_BACKEND, amd) {
        HIP_ARCH_TOOL = $$HIP_ROOT/bin/amdgpu-arch
        win32: HIP_ARCH_TOOL = $${HIP_ARCH_TOOL}.exe
        HIP_ARCH_OPTION =
        exists($$HIP_ARCH_TOOL) {
            HIP_ARCH = $$system($$system_quote($$system_path($$HIP_ARCH_TOOL)))
            !isEmpty(HIP_ARCH): HIP_ARCH_OPTION = -DCMAKE_HIP_ARCHITECTURES=$$first(HIP_ARCH)
        }
        HIP_BACKEND_OPTIONS = $$HIP_ARCH_OPTION
        HIP_COMPILER_OPTION = -DCMAKE_HIP_COMPILER=$$system_quote($$system_path($$HIP_COMPILER))
    } else {
        HIP_BACKEND_OPTIONS = -DCVHIP_NVIDIA_CLANG_CUDA=ON \
            -DCMAKE_CUDA_COMPILER=$$system_quote($$system_path($$NVIDIA_CLANG)) \
            -DCUDAToolkit_ROOT=$$system_quote($$system_path($$NVIDIA_CUDA_ROOT))
        !isEmpty(NVIDIA_SMI) {
            win32 {
                NVIDIA_CC_CMD = $$system_quote($$system_path($$NVIDIA_SMI)) --id=0 --query-gpu=compute_cap --format=csv 2>NUL
                NVIDIA_CC_ROWS = $$system($$NVIDIA_CC_CMD, lines)
                NVIDIA_CC = $$member(NVIDIA_CC_ROWS, 1)
            } else:unix {
                NVIDIA_CC_CMD = $$system_quote($$NVIDIA_SMI) --id=0 --query-gpu=compute_cap --format=csv 2>/dev/null | sed -n '2p' | tr -d '[:space:]'
                NVIDIA_CC = $$system($$NVIDIA_CC_CMD)
                NVIDIA_CC = $$first(NVIDIA_CC)
            }
            NVIDIA_CC_MAJOR = $$section(NVIDIA_CC, ., 0, 0)
            NVIDIA_CC_MINOR = $$section(NVIDIA_CC, ., 1, 1)
            contains(NVIDIA_CC_MAJOR, ^[0-9]+$):contains(NVIDIA_CC_MINOR, ^[0-9]+$) {
                NVIDIA_CC = $$NVIDIA_CC_MAJOR$$NVIDIA_CC_MINOR
                HIP_BACKEND_OPTIONS += -DCMAKE_CUDA_ARCHITECTURES=$$NVIDIA_CC
            } else:!isEmpty(NVIDIA_GPU) {
                error("NVIDIA GPU '$$NVIDIA_GPU' is visible, but its compute capability could not be parsed from nvidia-smi. Check the driver and nvidia-smi output.")
            }
        }
        isEmpty(NVIDIA_GPU):isEmpty(NVIDIA_CC) {
            # A host without an NVIDIA GPU can still cross-build the backend.
            # Use a conservative virtual baseline; real NVIDIA hosts above use
            # the device's exact compute capability automatically.
            HIP_BACKEND_OPTIONS += -DCMAKE_CUDA_ARCHITECTURES=75
        }
    }
    QMAKE_PRE_LINK += $$escape_expand(\n\t) cmake -S $$system_quote($$PWD/gpu) -B $$system_quote($$HIP_BUILD) -G Ninja -DCMAKE_CXX_COMPILER=$$system_quote($$GPU_CXX_COMPILER) $$HIP_COMPILER_OPTION $$HIP_BACKEND_OPTIONS -DCMAKE_BUILD_TYPE=Release
    QMAKE_PRE_LINK += $$escape_expand(\n\t) cmake --build $$system_quote($$HIP_BUILD)
    QMAKE_PRE_LINK += $$escape_expand(\n\t) cmake -E copy_if_different $$system_quote($$HIP_LIBRARY) $$system_quote($$CVHIP_OUTPUT)
    # Kernel-only edits must also rebuild the GPU library on incremental builds.
    PRE_TARGETDEPS += $$PWD/gpu/cvhip.h $$PWD/gpu/cvhip.hip $$PWD/gpu/cubiomes_device.hip \
        $$PWD/gpu/noise_device.h $$PWD/gpu/adaptive_probe_policy.h $$PWD/gpu/structure_extra.h $$PWD/gpu/CMakeLists.txt
    PRE_TARGETDEPS += $$PWD/cubiomes/xrms.h \
        $$PWD/cubiomes/tables/btree18.h $$PWD/cubiomes/tables/btree19.h \
        $$PWD/cubiomes/tables/btree192.h $$PWD/cubiomes/tables/btree20.h \
        $$PWD/cubiomes/tables/btree21wd.h $$PWD/cubiomes/tables/btree215.h \
        $$PWD/cubiomes/tables/btree262.h $$PWD/cubiomes/tables/btree263.h
    equals(HIP_BACKEND, nvidia) {
        PRE_TARGETDEPS += $$files($$PWD/gpu/vendor/hip-nvidia/include/hip/*) \
            $$files($$PWD/gpu/vendor/hip-nvidia/include/hip/nvidia_detail/*) \
            $$PWD/gpu/vendor/hip-nvidia/include/hip/amd_detail/amd_hip_runtime_pt_api.h
    }
}
}
# QMAKE_PRE_LINK      += $(MAKE) -C $$CUPATH -f $$CUPATH/makefile CC=\"$$QMAKE_CC\" CFLAGS=\"$(CFLAGS) $$QMAKE_CFLAGS\" $$CUTARGET
QMAKE_CLEAN         += $$CUPATH/*.o $$CUPATH/build/build/libcubiomes.a
LIBS                += -L$$OUT_PWD/lib_build -lcubiomes_static -lm

LUAPATH = $$PWD/lua/src

TARGET = cubiomes-viewer
SOURCES += src/hipsearch.cpp
HEADERS += src/hipsearch.h src/hipreference.h src/hipbatchpolicy.h src/searchrateformat.h gpu/cvhip.h gpu/structure_extra.h gpu/noise_device.h gpu/adaptive_probe_policy.h
DISTFILES += gpu/cvhip.hip gpu/cubiomes_device.hip gpu/CMakeLists.txt gpu/prepare_cubiomes_overlay.cmake \
    $$files($$PWD/gpu/vendor/hip-nvidia/include/hip/*) \
    $$files($$PWD/gpu/vendor/hip-nvidia/include/hip/nvidia_detail/*) \
    $$PWD/gpu/vendor/hip-nvidia/include/hip/amd_detail/amd_hip_runtime_pt_api.h \
    gpu/vendor/hip-nvidia/LICENSE-hip.txt gpu/vendor/hip-nvidia/LICENSE-hipother.txt \
    gpu/vendor/hip-nvidia/LICENSE-clr.txt gpu/vendor/hip-nvidia/README.md

SOURCES += \
        $$LUAPATH/lapi.c \
        $$LUAPATH/lauxlib.c \
        $$LUAPATH/lbaselib.c \
        $$LUAPATH/lcode.c \
        $$LUAPATH/lcorolib.c \
        $$LUAPATH/lctype.c \
        $$LUAPATH/ldblib.c \
        $$LUAPATH/ldebug.c \
        $$LUAPATH/ldo.c \
        $$LUAPATH/ldump.c \
        $$LUAPATH/lfunc.c \
        $$LUAPATH/lgc.c \
        $$LUAPATH/linit.c \
        $$LUAPATH/liolib.c \
        $$LUAPATH/llex.c \
        $$LUAPATH/lmathlib.c \
        $$LUAPATH/lmem.c \
        $$LUAPATH/loadlib.c \
        $$LUAPATH/lobject.c \
        $$LUAPATH/lopcodes.c \
        $$LUAPATH/loslib.c \
        $$LUAPATH/lparser.c \
        $$LUAPATH/lstate.c \
        $$LUAPATH/lstring.c \
        $$LUAPATH/lstrlib.c \
        $$LUAPATH/ltable.c \
        $$LUAPATH/ltablib.c \
        $$LUAPATH/ltm.c \
        $$LUAPATH/lundump.c \
        $$LUAPATH/lutf8lib.c \
        $$LUAPATH/lvm.c \
        $$LUAPATH/lzio.c \
        src/aboutdialog.cpp \
        src/biomecolordialog.cpp \
        src/conditiondialog.cpp \
        src/config.cpp \
        src/configdialog.cpp \
        src/extgendialog.cpp \
        src/exportdialog.cpp \
        src/formconditions.cpp \
        src/formgen48.cpp \
        src/formsearchcontrol.cpp \
        src/gotodialog.cpp \
        src/headless.cpp \
        src/maptoolsdialog.cpp \
        src/message.cpp \
        src/presetdialog.cpp \
        src/layerdialog.cpp \
        src/mapview.cpp \
        src/rangedialog.cpp \
        src/scripts.cpp \
        src/search.cpp \
        src/searchthread.cpp \
        src/tabbiomes.cpp \
        src/tablocations.cpp \
        src/tabstructures.cpp \
        src/mainwindow.cpp \
        src/main.cpp \
        src/util.cpp \
        src/widgets.cpp \
        src/world.cpp

HEADERS += \
        $$CUPATH/finders.h \
        $$CUPATH/generator.h \
        $$CUPATH/layers.h \
        $$CUPATH/biomes.h \
        $$CUPATH/quadbase.h \
        $$CUPATH/util.h \
        $$LUAPATH/lapi.h \
        $$LUAPATH/lauxlib.h \
        $$LUAPATH/lcode.h \
        $$LUAPATH/lctype.h \
        $$LUAPATH/ldebug.h \
        $$LUAPATH/ldo.h \
        $$LUAPATH/lfunc.h \
        $$LUAPATH/lgc.h \
        $$LUAPATH/ljumptab.h \
        $$LUAPATH/llex.h \
        $$LUAPATH/llimits.h \
        $$LUAPATH/lmem.h \
        $$LUAPATH/lobject.h \
        $$LUAPATH/lopcodes.h \
        $$LUAPATH/lopnames.h \
        $$LUAPATH/lparser.h \
        $$LUAPATH/lprefix.h \
        $$LUAPATH/lstate.h \
        $$LUAPATH/lstring.h \
        $$LUAPATH/ltable.h \
        $$LUAPATH/ltm.h \
        $$LUAPATH/lua.h \
        $$LUAPATH/lua.hpp \
        $$LUAPATH/luaconf.h \
        $$LUAPATH/lualib.h \
        $$LUAPATH/lundump.h \
        $$LUAPATH/lvm.h \
        $$LUAPATH/lzio.h \
        src/aboutdialog.h \
        src/biomecolordialog.h \
        src/conditiondialog.h \
        src/config.h \
        src/configdialog.h \
        src/extgendialog.h \
        src/exportdialog.h \
        src/formconditions.h \
        src/formgen48.h \
        src/formsearchcontrol.h \
        src/gotodialog.h \
        src/headless.h \
        src/maptoolsdialog.h \
        src/message.h \
        src/presetdialog.h \
        src/layerdialog.h \
        src/mapview.h \
        src/qzipwriter.h \
        src/rangedialog.h \
        src/scripts.h \
        src/search.h \
        src/searchthread.h \
        src/seedtables.h \
        src/tabbiomes.h \
        src/tablocations.h \
        src/tabstructures.h \
        src/mainwindow.h \
        src/util.h \
        src/widgets.h \
        src/world.h

FORMS += \
        src/aboutdialog.ui \
        src/biomecolordialog.ui \
        src/conditiondialog.ui \
        src/configdialog.ui \
        src/extgendialog.ui \
        src/exportdialog.ui \
        src/formconditions.ui \
        src/formgen48.ui \
        src/formsearchcontrol.ui \
        src/gotodialog.ui \
        src/maptoolsdialog.ui \
        src/presetdialog.ui \
        src/layerdialog.ui \
        src/mainwindow.ui \
        src/rangedialog.ui \
        src/tabbiomes.ui \
        src/tablocations.ui \
        src/tabstructures.ui

RESOURCES += \
        rc/icons.qrc \
        rc/style.qrc \
        rc/examples.qrc \
        rc/lang.qrc \
        rc/qh.qrc

# ----- translations (pluralization) -----

TRANSLATIONS += \
        rc/lang/en_US.ts \
        rc/lang/de_DE.ts \
        rc/lang/zh_CN.ts


# enable network features with: qmake CONFIG+=with_network
with_network: {
    QT += network
    DEFINES += "WITH_UPDATER=1"
    SOURCES += src/updater.cpp
    HEADERS += src/updater.h
}

# enable dbus features with: qmake CONFIG+=with_dbus
with_dbus: {
    QT += dbus
    DEFINES += "WITH_DBUS=1"
}
