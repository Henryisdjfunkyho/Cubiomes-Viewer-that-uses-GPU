#ifndef FORMSEARCHCONTROL_H
#define FORMSEARCHCONTROL_H

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMessageBox>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QWidget>

#include "config.h"
#include "searchthread.h"

#include <deque>

namespace Ui {
class FormSearchControl;
}

class MainWindow;

class SeedTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit SeedTableModel(QObject *parent = nullptr) :
        QAbstractTableModel(parent) {}

    enum { COL_SEED, COL_TOP16, COL_HEX48, COL_MAX };

    virtual int rowCount(const QModelIndex&) const override { return seeds.size(); }
    virtual int columnCount(const QModelIndex&) const override { return COL_MAX; }

    virtual QVariant data(const QModelIndex& index, int role) const override;
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    int insertSeeds(QVector<uint64_t> seeds);
    void removeRow(int row);
    void reset();

    struct Seed
    {
        uint64_t seed;
        QVariant varSeed, varHex48, varTop16;
        QVariant txtSeed, txtHex48, txtTop16;
    };
    QList<Seed> seeds;
};

class SeedSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    SeedSortProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent),column(),order(Qt::DescendingOrder) {}

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Vertical && role == Qt::DisplayRole)
            return QVariant::fromValue(section + 1);
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

    virtual bool lessThan(const QModelIndex& a, const QModelIndex& b) const override
    {
        uint64_t av = sourceModel()->data(a, Qt::UserRole).toULongLong();
        uint64_t bv = sourceModel()->data(b, Qt::UserRole).toULongLong();
        if (a.column() == SeedTableModel::COL_SEED)
            return (int64_t) bv < (int64_t) av;
        else
            return bv < av;
    }

    virtual void sort(int column, Qt::SortOrder order) override
    {
        if (column >= columnCount())
            return;
        if (this->column == -1)
            QSortFilterProxyModel::sort(-1, order);
        else
            QSortFilterProxyModel::sort(column, order);
        this->column = column;
        this->order = order;
    }

    int column;
    Qt::SortOrder order;
};

class FormSearchControl : public QWidget
{
    Q_OBJECT
    friend class FormSearchControlTestAccess;

public:
    explicit FormSearchControl(MainWindow *parent);
    ~FormSearchControl();

    bool event(QEvent *) Q_DECL_OVERRIDE;

    std::vector<uint64_t> getResults();
    SearchConfig getSearchConfig();
    bool setSearchConfig(SearchConfig s, bool quiet);

    void stopSearch();
    bool setList64(QString path, bool quiet);
    bool setList64(QTextStream& stream);

    void setResultsPath(QString path);

    void searchLockUi(bool lock);

    void setSearchMode(int mode);

    bool getSeed(int row, uint64_t *seed);
    void sessionLoaded();

signals:
    void selectedSeedChanged(uint64_t seed);
    void searchStatusChanged(bool running);
    void resultsAdded(int cnt);

public slots:
    void setSearchRange(uint64_t smin, uint64_t smax);

    void on_buttonClear_clicked();
    void on_buttonStart_clicked();
    void on_buttonMore_clicked();
    void on_buttonTune_clicked();

    void onSort(int column, Qt::SortOrder);
    void onSeedSelectionChanged();
    void on_results_clicked(const QModelIndex &index);
    void on_results_customContextMenuRequested(const QPoint& pos);

    void on_buttonSearchHelp_clicked();

    void on_comboSearchType_currentIndexChanged(int index);

    void pasteResults();
    int pasteList(bool dummy);
    void onBufferTimeout();
    void searchResult(uint64_t seed);
    int searchResultsAdd(std::vector<uint64_t> seeds, bool countonly);
    void searchProgressReset();
    void updateSearchProgress(uint64_t last, uint64_t end, int64_t seed);
    void searchFinish(bool done);
    void progressTimeout();
    void tuneTimeout();
    void removeCurrent();
    void copySeed();
    void copyResults();

protected:
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    enum TunePhase { TUNE_WARMUP, TUNE_BASELINE, TUNE_EXPLORE, TUNE_CONFIRM_BASELINE, TUNE_CONFIRM_BEST, TUNE_CLEANUP };
    struct TuneCandidate
    {
        TuneCandidate(int threadCount = 1, bool gpu = false, unsigned batch = 0, unsigned inflightCount = 2)
            : threads(threadCount), useHip(gpu), batchLimit(batch), inflight(inflightCount) {}
        int threads;
        bool useHip;
        unsigned batchLimit;
        unsigned inflight;
    };

    void startTuneCandidate();
    void finishTuneCandidate();
    void finishSpeedTune(bool cancelled);
    std::vector<int> tuneThreadCounts() const;
    bool sameTuneCandidate(const TuneCandidate &a, const TuneCandidate &b) const;
    static double acceptedTuneRate(double measuredRate, bool gpuRequested, bool gpuActive, double deviceElapsedMs);
    static bool canGrowTuneBatch(double deviceElapsedMs, unsigned actualBatch, unsigned backendCap);
    static unsigned smallerTuneBatch(unsigned actualBatch);
    QString currentTuneProfileKey();
    void saveTuneProfile(const TuneCandidate &candidate);
    bool restoreTuneProfile();

    MainWindow *parent;
    Ui::FormSearchControl *ui;
    SeedTableModel *model;
    SeedSortProxy *proxy;
    SearchMaster sthread;
    SearchMaster tuningThread;
    QElapsedTimer elapsed;
    QTimer stimer;
    QTimer tuneTimer;
    QElapsedTimer tuneClock;
    QElapsedTimer tuneTrialClock;
    QElapsedTimer tuneMeasureClock;
    QFile resultfile;

    Session tuneSession;
    SearchConfig tuneOriginalConfig;
    std::vector<TuneCandidate> tuneCandidates;
    size_t tuneCandidateIndex = 0;
    TuneCandidate tuneBest;
    TuneCandidate tuneOriginalCandidate;
    TuneCandidate tuneBaselineCandidate;
    double tuneBestRate = 0;
    double tuneBaselineRate = 0;
    double tuneConfirmedRate = 0;
    uint64_t tuneMeasureStart = 0;
    bool tuningActive = false;
    bool tuneResumeSearch = false;
    bool tuneHasSafeCursor = false;
    bool tuneConfirming = false;
    bool tuneGpuAvailable = false;
    bool tuneGpuChecked = false;
    bool tuneMeasureStarted = false;
    bool tuneBaselineMeasured = false;
    bool tuneConfirmMeasured = false;
    bool applyingTuneSettings = false;
    bool manualTuneOverride = false;
    bool tunerReplayActive = false;
    SearchCursor tuneResumeCursor;
    SearchCursor tuneTrialCursor;
    QSet<uint64_t> tuneExistingResults;
    int tuneDurationMinutes = 2;
    int tunePhase = 0;
    qint64 tuneWarmupEndMs = 0;
    qint64 tuneBaselineEndMs = 0;
    qint64 tuneExploreEndMs = 0;
    qint64 tuneConfirmEndMs = 0;
    qint64 tuneConfirmBaselineEndMs = 0;
    qint64 tuneTrialStartMs = 0;
    qint64 tuneTrialWarmEndMs = 0;
    qint64 tuneDurationMs = 120000;
    uint64_t tuneTrialStartCount = 0;
    bool tuneTrialMeasured = false;
    double tuneConfirmedBaselineRate = 0;
    unsigned tuneBestEffectiveBatch = 0;
    unsigned tuneGpuMaxAllowedBatch = 0;
    QString tuneProfileKey;
    QString activeTuneProfileKey;

    // the seed list option is not stored in a widget but is loaded with the "..." button
    QString slist64path;
    QString slist64fnam; // file name without directory
    std::vector<uint64_t> slist64;

    // min and max seeds values
    uint64_t smin, smax;

    // found seeds that are waiting to be added to results
    std::vector<uint64_t> qbuf;
    quint64 nextupdate;
    quint64 updt;
};

#endif // FORMSEARCHCONTROL_H
