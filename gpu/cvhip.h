#ifndef CVHIP_H
#define CVHIP_H
#include <stdint.h>
#include <stddef.h>
// Plain C ABI: the Qt/MinGW executable and ROCm DLL use different C++ runtimes.
struct CvHipOctave { uint64_t lo, hi; double frequency, amplitude; };
struct CvHipCondition {
    uint64_t lo, hi;
    int32_t x, z, na, nb;
    double amplitude, lower, upper;
    int32_t invert, reserved;
    CvHipOctave octaves[32];
    // reserved selects the operation; zero retains point-climate semantics.
    int32_t x2, z2, required, refx, refz, skipref;
    int32_t mc, large, dimension, scale, y, matchAny;
    uint64_t find, findM, exclude, excludeM, pick, pickM;
    int32_t preserveCpuZero;
    uint64_t radiusSq;
    int32_t groupId;
    int32_t conditionId;
    int32_t tolerance;
    uint64_t componentSize;
    int32_t limits[10];
    int32_t areaX1, areaZ1, areaX2, areaZ2;
    double coverage, zscore, wilsonWidth;
};
enum CvHipOperation { CVHIP_CLIMATE = 0, CVHIP_SLIME = 1, CVHIP_BIOME = 2, CVHIP_STRUCTURE = 3, CVHIP_CLIMATE_RANGE = 4, CVHIP_NOISE_SAMPLE = 5, CVHIP_BIOME_SAMPLE = 6, CVHIP_OR_CLIMATE = 7, CVHIP_BIOME_CENTER = 8, CVHIP_SPIRAL_CLIMATE = 9, CVHIP_SPIRAL_POINT = 10, CVHIP_CLIMATE_PROBE = 11, CVHIP_CENTER_POINT = 12 };

// Versioned ABI. No STL objects or allocations cross the DLL boundary.
#define CVHIP_ABI_VERSION 9
#define CVHIP_MAX_CONDITIONS 600
#define CVHIP_MAX_BATCH 16777216U
#define CVHIP_MAX_LIST_BATCH 262144U
enum CvHipEncoding { CVHIP_OFFSETS = 1, CVHIP_PACKED = 2 };
struct CvHipBatchInfo { uint32_t count, survivors, encoding, reserved; };
// Optional ABI-6 metadata export. Callers may fall back when older DLLs omit it.
// Returns the required byte count, including the terminator.
#ifdef __cplusplus
extern "C" {
#endif
size_t cvhip_device_identity(char *buffer, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
