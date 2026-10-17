#ifndef CVHIP_ADAPTIVE_PROBE_POLICY_H
#define CVHIP_ADAPTIVE_PROBE_POLICY_H

#include <stdint.h>

#if defined(__HIPCC__) || defined(__CUDACC__)
#define CVHIP_PROBE_HD __host__ __device__
#else
#define CVHIP_PROBE_HD
#endif

namespace cvhip_probe_policy {

/* Match Cubiomes' getParaRange() initial adaptive-lattice step exactly:
 * (int)(0.5 / lmin - FLT_EPSILON) + 1. */
CVHIP_PROBE_HD inline bool coarseStep(double minLacunarity, int *step) {
    if (!step || !(minLacunarity > 0.0)) return false;
    const double raw = 0.5 / minLacunarity - 1.1920928955078125e-7;
    if (!(raw >= 0.0) || raw >= 2147483647.0) return false;
    const int candidate = static_cast<int>(raw) + 1;
    if (candidate <= 0) return false;
    *step = candidate;
    return true;
}

/* Number of initial lattice points visited, capped to the first 16. Width and
 * height are inclusive rectangle dimensions, not coordinate endpoints. */
CVHIP_PROBE_HD inline unsigned coarsePrefixCount(
    int64_t width, int64_t height, int step) {
    if (width <= 0 || height <= 0 || step <= 0) return 0;
    const int64_t cols = (width - 1) / step + 1;
    const int64_t rows = (height - 1) / step + 1;
    if (cols >= 16 || rows >= 16) return 16;
    const int64_t product = cols * rows; // both operands are at most 15
    return static_cast<unsigned>(product < 16 ? product : 16);
}

/* Return the row-major initial-lattice offset for a prefix index. The CPU's
 * traversal has i as the inner loop and j as the outer loop. */
CVHIP_PROBE_HD inline bool coarsePrefixOffset(
    unsigned sampleIndex, int64_t width, int64_t height, int step,
    int64_t *dx, int64_t *dz) {
    if (!dx || !dz || sampleIndex >= coarsePrefixCount(width, height, step))
        return false;
    const int64_t cols = (width - 1) / step + 1;
    const int64_t x = static_cast<int64_t>(sampleIndex % cols) * step;
    const int64_t z = static_cast<int64_t>(sampleIndex / cols) * step;
    if (x >= width || z >= height) return false;
    *dx = x;
    *dz = z;
    return true;
}

/* Necessary-failure directions for the adaptive area predicates. Callers
 * must first reject nonfinite or numerically ambiguous samples. */
CVHIP_PROBE_HD inline bool necessaryFailure(
    unsigned matchAny, double value, double lower, double upper) {
    if (matchAny & 4u) return value < lower || value > upper;
    return ((matchAny & 1u) && value < lower) ||
           ((matchAny & 2u) && value > upper);
}

} // namespace cvhip_probe_policy

#undef CVHIP_PROBE_HD
#endif
