#ifndef HIPBATCHPOLICY_H
#define HIPBATCHPOLICY_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace cvhip_batch_policy {

constexpr uint64_t kProbeWorkBudget = 4000000ULL;

// Add independently compiled probe work estimates without wrapping. A wrapped
// total could otherwise produce an accidentally enormous batch allowance.
inline uint64_t saturatingAddWork(uint64_t total, uint64_t add)
{
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return add > maximum - total ? maximum : total + add;
}

// Probe-only filters evaluate one guaranteed climate sample per candidate.
// Keep their per-dispatch work under the existing four-million octave-work
// budget, without inheriting the area-grid path's unrelated 65,536 cap.
inline unsigned probeOnlyBatchLimit(unsigned backendCeiling, uint64_t summedOctaveWork)
{
    if (backendCeiling == 0)
        return 0;

    const uint64_t workPerSeed = summedOctaveWork == 0 ? 1 : summedOctaveWork;
    const uint64_t workLimited = kProbeWorkBudget / workPerSeed;
    const unsigned safeWorkLimit = workLimited == 0 ? 1U :
        static_cast<unsigned>(workLimited > std::numeric_limits<unsigned>::max()
            ? std::numeric_limits<unsigned>::max() : workLimited);
    return backendCeiling < safeWorkLimit ? backendCeiling : safeWorkLimit;
}

// Production-only downward safety policy. Ignore incomplete tail batches and
// invalid timing samples; lower the current ceiling by half only after a full
// dispatch exceeds the latency target. Callers own synchronization and must
// disable this policy during tuning trials.
inline unsigned reducedRuntimeCap(unsigned currentCap, unsigned completedCount,
    double elapsedMs, double thresholdMs = 500.0)
{
    // Ignore already-in-flight batches sized for a previous, larger cap. They
    // can finish after another dispatch lowered the runtime ceiling.
    if (currentCap == 0 || completedCount != currentCap ||
        !std::isfinite(elapsedMs) || elapsedMs <= 0.0 ||
        !std::isfinite(thresholdMs) || thresholdMs <= 0.0 ||
        elapsedMs <= thresholdMs || currentCap <= 1)
        return currentCap;

    const unsigned reduced = currentCap / 2;
    return reduced == 0 ? 1U : reduced;
}

} // namespace cvhip_batch_policy

#endif
