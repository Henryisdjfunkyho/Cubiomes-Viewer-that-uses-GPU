#ifndef SEARCHTHREAD_H
#define SEARCHTHREAD_H

#include "search.h"
#include "config.h"
#include "hipsearch.h"

#include <QThread>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include <QTimer>
#include <QMessageBox>

#include <deque>

struct Session
{
    void writeHeader(QTextStream& stream);
    bool save(QWidget *widget, QTextStream& stream);
    bool load(QWidget *widget, QTextStream& stream, bool quiet);

    WorldInfo wi;
    SearchConfig sc;
    Gen48Config gen48;
    std::vector<Condition> cv;
    std::vector<uint64_t> slist;
};

struct SearchWorker;

// A position in the search scheduler's native ordering. Keeping the list index
// as well as the seed preserves repeated entries and family-search positions.
struct SearchCursor
{
    uint64_t seed = 0;
    uint64_t idx = 0;
    uint64_t prog = 0;
    bool done = false;
};

struct SearchMaster : QObject
{
    Q_OBJECT
public:
    SearchMaster(QWidget *parent);
    virtual ~SearchMaster();

    bool set(QWidget *widget, const Session& s);

    void preSearch();

    void startSearch();
    void stopSearch();

    // Get search progress:
    //  status  : progress status summary
    //  prog    : scheduled progress in search space
    //  end     : size of search space
    //  seed    : current seed to be processed
    // Get the search speed (provided it is called at regular intervals):
    //  min,max : lower and upper search speed quartiles
    //  avg     : search speed average
    bool getProgress(QString *status, uint64_t *prog, uint64_t *end, uint64_t *seed, qreal *min, qreal *avg, qreal *max);

    bool requestItem(SearchWorker *item);
    uint64_t completedSeeds();
    bool captureSafeCursor(SearchCursor *cursor);
    void setResumeCursor(const SearchCursor &cursor);

public slots:
    void onWorkerResult(uint64_t seed);
    void onWorkerFinished();

signals:
    void searchResult(uint64_t seed);
    void searchFinish(bool done);

public:
    struct TProg { uint64_t ns, prog; };

public:
    std::vector<SearchWorker*>  workers;

    QMutex                      mutex;
    std::atomic_bool            stop;

    std::deque<TProg>           proghist;
    QElapsedTimer               progtimer;
    QElapsedTimer               itemtimer;
    uint64_t                    count;
    uint64_t                    completedTotal = 0;

    SearchThreadEnv             env;

    int                         searchtype;
    int                         mc;
    int                         large;
    ConditionTree               condtree;
    int                         itemsize;   // number of seeds per search item
    int                         threadcnt;  // numbr of worker threads
    Gen48Config                 gen48;      // 48-bit generator settings
    std::vector<uint64_t>       slist;      // candidate list
    uint64_t                    idx;        // index within candidate list
    uint64_t                    scnt;       // search space size
    uint64_t                    prog;       // search space progress tracker
    uint64_t                    seed;       // current seed (next to be processed)
    uint64_t                    smin;
    uint64_t                    smax;
    bool                        isdone;
    bool                        stoppingSearch = false;
    HipSearch                   hip;
    unsigned                    hipBatchLimitOverride = 0;
    unsigned                    hipInflightOverride = 0;
    bool                        hipAdaptiveSafety = false;
    bool                        trackCompletedSeeds = false;
    SearchCursor                resumeCursor;
    bool                        hasResumeCursor = false;
    bool                        useHip = qgetenv("CUBIOMES_HIP") == "1";
};


struct SearchWorker : QThread
{
    Q_OBJECT
public:
    SearchWorker(SearchMaster *master);
    ~SearchWorker();

    bool getNextItem();
    virtual void run() override;

signals:
    void result(uint64_t seed);

public:
    SearchMaster      * master;

    const uint64_t    * slist;      // candidate list
    uint64_t            len;        // number of candidates

    /// current work item
    uint64_t            prog;       // search space progress
    uint64_t            idx;        // current index in candidate buffer
    uint64_t            sstart;     // starting seed
    int                 scnt;       // number of seeds to process in this item
    uint64_t            seed;       // (out) current seed while processing
    std::atomic<unsigned> completedInItem{0};
    // the end seed is the highest unsigned seed value in the search space
    // (or the last entry in the seed list)

private:
    void runCpuLegacy();
    SearchThreadEnv     env;
};





#endif // SEARCHTHREAD_H
