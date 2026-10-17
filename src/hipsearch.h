#ifndef HIPSEARCH_H
#define HIPSEARCH_H
#include "search.h"
#include "../gpu/cvhip.h"
#include <QLibrary>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct HipBatch {
    unsigned count = 0;
    unsigned encoding = 0; // zero means replay every seed on CPU
    unsigned survivors = 0;
    // GPU stream time from the device start event through final result transfer.
    double deviceElapsedMilliseconds = 0.0;
    std::vector<uint32_t> data;
    // Original seed-index order; state 2 needs CPU recomputation.
    bool next(unsigned &cursor, unsigned &index, unsigned &state) const;
};

class HipSearch {
public:
    HipSearch();
    ~HipSearch();
    void configure(const ConditionTree &tree, int mc, int large, bool useHip, bool full64 = true,
        bool searchSemantics = false, bool arbitrarySeeds = false,
        unsigned requestedBatchLimit = 0, unsigned requestedInflight = 0,
        bool adaptiveSafety = false);
    void cancel();
    bool active() const { return enabled.load(); }
    size_t conditionCount() const { return compiledIds.size(); }
    size_t operationCount() const { return configs.size(); }
    size_t operationTypeCount(int type) const {
        size_t count = 0;
        for (const auto &cfg : configs) count += cfg.reserved == type;
        return count;
    }
    uint64_t operationOctaveWork(int type) const {
        uint64_t work = 0;
        for (const auto &cfg : configs) if (cfg.reserved == type)
            work += uint64_t(std::max(1, cfg.na + cfg.nb));
        return work;
    }
    unsigned batchLimit() const { return runtimeBatchLimit.load(); }
    unsigned backendBatchCap() const { return backendBatchLimit; }
    unsigned inflightLimit() const { return maxInflight; }
    double lastDeviceElapsedMilliseconds() const { return lastDeviceElapsedMs.load(); }
    double latestDeviceElapsedMilliseconds() const { return latestDeviceElapsedMs.load(); }
    void resetDeviceElapsedMilliseconds() { lastDeviceElapsedMs.store(0.0); latestDeviceElapsedMs.store(0.0); }
    QString deviceIdentity();
    const std::vector<int> &conditionIds() const { return ids; }
    HipBatch filterBatch(uint64_t start, unsigned count, const uint64_t *seeds = nullptr);
    std::vector<unsigned char> filter(uint64_t start, unsigned count);
private:
    struct Request {
        uint64_t start;
        unsigned count;
        std::vector<uint64_t> seeds;
        std::promise<HipBatch> result;
    };
    void dispatch();
    void shutdown();
    QLibrary library;
    using Available = int (*)();
    using Version = unsigned (*)();
    using Create = void *(*)(const CvHipCondition *, unsigned);
    using Destroy = void (*)(void *);
    using Submit = int (*)(void *, unsigned, uint64_t, const uint64_t *, unsigned);
    using Collect = int (*)(void *, unsigned, CvHipBatchInfo *, uint32_t *);
    Destroy destroy = nullptr;
    Submit submit = nullptr;
    Collect collect = nullptr;
    Create create = nullptr;
    void *context = nullptr;
    std::promise<bool> started;
    std::thread coordinator;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<std::shared_ptr<Request>> requests;
    std::vector<CvHipCondition> configs;
    std::vector<int> ids;
    std::vector<int> compiledIds;
    unsigned deviceBatchLimit = CVHIP_MAX_BATCH;
    unsigned backendBatchLimit = CVHIP_MAX_BATCH;
    std::atomic<unsigned> runtimeBatchLimit{CVHIP_MAX_BATCH};
    bool adaptiveSafety = false;
    unsigned maxInflight = 2;
    std::atomic_bool enabled{false};
    std::atomic_bool stopping{true};
    std::atomic<double> lastDeviceElapsedMs{0.0};
    std::atomic<double> latestDeviceElapsedMs{0.0};
};
#endif
