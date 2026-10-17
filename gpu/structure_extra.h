#ifndef CVHIP_STRUCTURE_EXTRA_H
#define CVHIP_STRUCTURE_EXTRA_H

#include <stdint.h>

/*
 * Device-safe structure placement primitives which are deliberately kept out
 * of the ABI until their call sites have been reviewed.  The formulas mirror
 * cubiomes/finders.h and finders.c for the current (1.18+) branches only.
 * Biome, terrain, variant and structure-piece checks remain CPU work.
 */
#if defined(__HIPCC__) || defined(__CUDACC__)
#define CVHIP_EXTRA_HD __host__ __device__
#else
#define CVHIP_EXTRA_HD
#endif

namespace cvhip_extra {

struct StructureConfig {
    int32_t salt;
    int32_t regionSize;
    int32_t chunkRange;
};

enum PlacementMode {
    FEATURE = 0,
    LARGE = 1,
    OUTPOST = 2,
    END_CITY = 3,
    FORTRESS_1_18 = 4,
    BASTION_1_18 = 5,
};

struct Placement {
    int32_t x;
    int32_t z;
    uint8_t valid;
};

static const uint64_t JAVA_MULTIPLIER = 0x5deece66dULL;
static const uint64_t JAVA_MASK = (1ULL << 48) - 1;
static const uint64_t JAVA_ADDEND = 0xbULL;

CVHIP_EXTRA_HD inline uint64_t lcg(uint64_t state)
{
    return (state * JAVA_MULTIPLIER + JAVA_ADDEND) & JAVA_MASK;
}

CVHIP_EXTRA_HD inline uint32_t next31(uint64_t &state)
{
    state = lcg(state);
    return uint32_t(state >> 17);
}

CVHIP_EXTRA_HD inline uint32_t next32(uint64_t &state)
{
    state = lcg(state);
    return uint32_t(state >> 16);
}

CVHIP_EXTRA_HD inline int32_t next32Signed(uint64_t &state)
{
    state = lcg(state);
    return int32_t(uint32_t(state >> 16));
}

CVHIP_EXTRA_HD inline uint64_t nextLong(uint64_t &state)
{
    /* cubiomes' next() returns int.  Its signed low half is deliberately
     * preserved here: Java's nextLong implementation adds that int to the
     * high half, so a negative low half subtracts 2^32 rather than being
     * zero-extended. */
    const uint64_t hi = uint64_t(uint32_t(next32Signed(state))) << 32;
    const int64_t lo = next32Signed(state);
    return hi + uint64_t(lo);
}

/* Exact Java Random.nextInt(bound), including rejection for non-powers of 2. */
CVHIP_EXTRA_HD inline int nextInt(uint64_t &state, int bound)
{
    if (bound <= 0)
        return 0;
    const uint32_t n = uint32_t(bound);
    if ((n & (n - 1U)) == 0)
        return int((uint64_t(n) * next31(state)) >> 31);
    uint32_t bits;
    uint32_t value;
    do {
        bits = next31(state);
        value = bits % n;
    } while (int32_t(bits - value + (n - 1U)) < 0);
    return int(value);
}

/* getLargeStructureChunkInRegion() intentionally uses raw modulo in cubiomes
 * (the upstream helper still has a power-of-two TODO), so keep it distinct
 * from Java Random.nextInt() above. */
CVHIP_EXTRA_HD inline int largeModulo(uint64_t &state, int bound)
{
    return bound > 0 ? int(next31(state) % uint32_t(bound)) : 0;
}

/* getFeatureChunkInRegion() uses a raw modulo for non-power-of-two ranges.
 * This is intentionally not Java Random.nextInt(): cubiomes mirrors the
 * vanilla candidate calculation here, and the rejection step would consume a
 * different number of RNG values for some seeds. */
CVHIP_EXTRA_HD inline int featureOffset(uint64_t &state, int bound)
{
    if (bound <= 0)
        return 0;
    const uint32_t n = uint32_t(bound);
    const uint32_t bits = next31(state);
    if ((n & (n - 1U)) != 0)
        return int(bits % n);
    return int((uint64_t(n) * bits) >> 31);
}

CVHIP_EXTRA_HD inline int32_t wrap32(uint64_t value)
{
    return int32_t(uint32_t(value));
}

CVHIP_EXTRA_HD inline int32_t floorDiv16(int32_t value)
{
    const int64_t v = value;
    return int32_t(v >= 0 ? v / 16 : -(((-v) + 15) / 16));
}

CVHIP_EXTRA_HD inline int32_t featureCoord(uint64_t region, int32_t regionSize,
    int offset)
{
    return wrap32((uint64_t(region) * uint64_t(uint32_t(regionSize)) +
        uint64_t(uint32_t(offset))) << 4);
}

CVHIP_EXTRA_HD inline void featureChunk(uint64_t seed,
    const StructureConfig &config, int32_t regionX, int32_t regionZ,
    int32_t &chunkX, int32_t &chunkZ)
{
    uint64_t state = seed + uint64_t(regionX) * 341873128712ULL +
        uint64_t(regionZ) * 132897987541ULL + uint64_t(int64_t(config.salt));
    state = (state ^ JAVA_MULTIPLIER) & JAVA_MASK;
    chunkX = featureOffset(state, config.chunkRange);
    chunkZ = featureOffset(state, config.chunkRange);
}

CVHIP_EXTRA_HD inline Placement feature(uint64_t seed,
    const StructureConfig &config, int32_t regionX, int32_t regionZ,
    bool large)
{
    Placement out{0, 0, 0};
    if (config.regionSize <= 0 || config.chunkRange <= 0)
        return out;

    uint64_t state = seed + uint64_t(regionX) * 341873128712ULL +
        uint64_t(regionZ) * 132897987541ULL + uint64_t(int64_t(config.salt));
    state = (state ^ JAVA_MULTIPLIER) & JAVA_MASK;
    int32_t x = large ? largeModulo(state, config.chunkRange) :
        featureOffset(state, config.chunkRange);
    if (large)
        x += largeModulo(state, config.chunkRange);
    int32_t z = large ? largeModulo(state, config.chunkRange) :
        featureOffset(state, config.chunkRange);
    if (large)
        z += largeModulo(state, config.chunkRange);
    if (large) {
        x >>= 1;
        z >>= 1;
    }
    out.x = featureCoord(uint64_t(regionX), config.regionSize, x);
    out.z = featureCoord(uint64_t(regionZ), config.regionSize, z);
    out.valid = 1;
    return out;
}

CVHIP_EXTRA_HD inline bool outpostAttempt(uint64_t seed, int32_t blockX,
    int32_t blockZ)
{
    /* setAttemptSeed receives chunk coordinates, then shifts each once more
     * to the 16x16-chunk attempt grid.  Keep the signed-to-uint64 conversion
     * from finders.c; negative values are sign-extended before the shift. */
    const int32_t chunkX = floorDiv16(blockX);
    const int32_t chunkZ = floorDiv16(blockZ);
    const int32_t attemptX = floorDiv16(chunkX);
    const int32_t attemptZ = floorDiv16(chunkZ);
    uint64_t state = seed ^ uint64_t(attemptX) ^
        (uint64_t(attemptZ) << 4);
    state = (state ^ JAVA_MULTIPLIER) & JAVA_MASK;
    (void)next31(state); // setAttemptSeed() discards next(seed, 31)
    return nextInt(state, 5) == 0;
}

CVHIP_EXTRA_HD inline bool bastionAttempt(uint64_t seed, int32_t blockX,
    int32_t blockZ)
{
    uint64_t state = seed;
    state = (state ^ JAVA_MULTIPLIER) & JAVA_MASK;
    const uint64_t a = nextLong(state);
    const uint64_t b = nextLong(state);
    state = a * uint64_t(floorDiv16(blockX)) ^
        b * uint64_t(floorDiv16(blockZ)) ^ seed;
    state = (state ^ JAVA_MULTIPLIER) & JAVA_MASK;
    return nextInt(state, 5) >= 2;
}

CVHIP_EXTRA_HD inline Placement placement(uint64_t seed, int mode,
    const StructureConfig &config, int32_t regionX, int32_t regionZ)
{
    Placement out = feature(seed, config, regionX, regionZ,
        mode == LARGE || mode == END_CITY);
    if (!out.valid)
        return out;
    if (mode == OUTPOST)
        out.valid = outpostAttempt(seed, out.x, out.z) ? 1 : 0;
    else if (mode == END_CITY) {
        const int64_t x = out.x;
        const int64_t z = out.z;
        out.valid = x * x + z * z >= 1008LL * 1008LL ? 1 : 0;
    } else if (mode == BASTION_1_18)
        out.valid = bastionAttempt(seed, out.x, out.z) ? 1 : 0;
    /* FORTRESS_1_18 and ordinary FEATURE/LARGE have no placement-only gate. */
    return out;
}

} // namespace cvhip_extra

#undef CVHIP_EXTRA_HD

#endif
