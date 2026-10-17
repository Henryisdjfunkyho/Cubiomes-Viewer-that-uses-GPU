#include "formsearchcontrol.h"
#include "ui_formsearchcontrol.h"

#include "mainwindow.h"
#include "message.h"
#include "rangedialog.h"
#include "search.h"
#include "util.h"

#include <QAction>
#include <QClipboard>
#include <QFileDialog>
#include <QFontMetrics>
#include <QMenu>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QSysInfo>
#include <QDebug>
#include <algorithm>
#include <cmath>


QVariant SeedTableModel::data(const QModelIndex& index, int role) const
{
    if (role == Qt::DisplayRole)
    {
        if (index.column() == COL_SEED)
            return seeds[index.row()].txtSeed;
        if (index.column() == COL_HEX48)
            return seeds[index.row()].txtHex48;
        if (index.column() == COL_TOP16)
            return seeds[index.row()].txtTop16;
    }
    else if (role == Qt::UserRole)
    {
        if (index.column() == COL_HEX48)
            return seeds[index.row()].varHex48;
        if (index.column() == COL_TOP16)
            return seeds[index.row()].varTop16;
        return seeds[index.row()].varSeed;
    }
    else if (role == Qt::TextAlignmentRole)
    {
        static QVariant align = QVariant::fromValue((int)Qt::AlignRight | Qt::AlignVCenter);
        if (index.column() != COL_HEX48)
            return align;
    }
    return QVariant();
}

QVariant SeedTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0)
        return QVariant::Invalid;
    if (role == Qt::InitialSortOrderRole)
        return QVariant::fromValue(Qt::DescendingOrder);
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        if (section == COL_SEED)
            return QVariant::fromValue(tr("seed"));
        if (section == COL_TOP16)
            return QVariant::fromValue(tr("top 16"));
        if (section == COL_HEX48)
            return QVariant::fromValue(tr("lower 48 bit"));
    }
    if (role == Qt::DisplayRole && orientation == Qt::Vertical)
        return QVariant::fromValue(section + 1);
    return QVariant();
}

int SeedTableModel::insertSeeds(QVector<uint64_t> newseeds)
{
    int row = seeds.size();
    beginInsertRows(QModelIndex(), row, row + newseeds.size()-1);
    for (uint64_t seed : qAsConst(newseeds))
    {
        Seed s;
        s.seed = seed;
        s.varSeed = QVariant::fromValue(seed);
        s.varTop16 = QVariant::fromValue((quint64)(seed>>48) & 0xFFFF);
        s.varHex48 = QVariant::fromValue((quint64)(seed & MASK48));
        s.txtSeed = QVariant::fromValue(QString::asprintf("%" PRId64, seed));
        s.txtTop16 = QVariant::fromValue(QString::asprintf("%04llx", (quint64)(seed>>48) & 0xFFFF));
        s.txtHex48 = QVariant::fromValue(QString::asprintf("%012llx", (quint64)(seed & MASK48)));
        seeds.append(s);
    }
    endInsertRows();
    return row;
}

void SeedTableModel::removeRow(int row)
{
    beginRemoveRows(QModelIndex(), row, row);
    seeds.removeAt(row);
    endRemoveRows();
}

void SeedTableModel::reset()
{
    if (seeds.empty())
        return;
    beginRemoveRows(QModelIndex(), 0, seeds.size());
    seeds.clear();
    endRemoveRows();
}

FormSearchControl::FormSearchControl(MainWindow *parent)
    : QWidget(parent)
    , parent(parent)
    , ui(new Ui::FormSearchControl)
    , model()
    , proxy()
    , sthread(this)
    , tuningThread(this)
    , elapsed()
    , stimer()
    , tuneTimer()
    , resultfile()
    , slist64path()
    , slist64fnam()
    , slist64()
    , smin(0)
    , smax(~(uint64_t)0)
    , qbuf()
    , nextupdate()
    , updt(20)
{
    ui->setupUi(this);

    ui->comboSearchType->addItem(tr("incremental"), SEARCH_INC);
    ui->comboSearchType->addItem(tr("48-bit only"), SEARCH_48ONLY);
    ui->comboSearchType->addItem(tr("48-bit family blocks"), SEARCH_BLOCKS);
    ui->comboSearchType->addItem(tr("seed list from file..."), SEARCH_LIST);

    model = new SeedTableModel(ui->results);
    proxy = new SeedSortProxy(ui->results);

    proxy->setSourceModel(model);
    ui->results->setModel(proxy);

    ui->results->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    connect(ui->results->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, &FormSearchControl::onSort);
    ui->results->sortByColumn(-1, Qt::AscendingOrder);

    connect(&sthread, &SearchMaster::searchResult, this, &FormSearchControl::searchResult, Qt::QueuedConnection);
    connect(&sthread, &SearchMaster::searchFinish, this, &FormSearchControl::searchFinish, Qt::QueuedConnection);

    connect(&stimer, &QTimer::timeout, this, QOverload<>::of(&FormSearchControl::progressTimeout));
    connect(&tuneTimer, &QTimer::timeout, this, &FormSearchControl::tuneTimeout);
    tuneTimer.setInterval(100);

    connect(
        ui->results->selectionModel(),
        SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
        SLOT(onSeedSelectionChanged()));

    searchProgressReset();
    ui->spinThreads->setMaximum(QThread::idealThreadCount());
    ui->spinThreads->setValue(QThread::idealThreadCount());
    ui->checkHip->setChecked(sthread.useHip);
    QSettings tunerSettings(APP_STRING, APP_STRING);
    tuneDurationMinutes = std::max(1, std::min(30, tunerSettings.value("speedTuner/durationMinutes", 2).toInt()));
    ui->spinTuneMinutes->setValue(tuneDurationMinutes);
    connect(ui->spinTuneMinutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        tuneDurationMinutes = std::max(1, std::min(30, value));
        QSettings settings(APP_STRING, APP_STRING);
        settings.setValue("speedTuner/durationMinutes", tuneDurationMinutes);
    });
    connect(ui->spinThreads, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        if (!applyingTuneSettings)
        {
            manualTuneOverride = true;
            activeTuneProfileKey.clear();
            sthread.hipBatchLimitOverride = 0;
            sthread.hipInflightOverride = 0;
        }
    });
    connect(ui->checkHip, &QCheckBox::toggled, this, [this](bool) {
        if (!applyingTuneSettings)
        {
            manualTuneOverride = true;
            activeTuneProfileKey.clear();
            sthread.hipBatchLimitOverride = 0;
            sthread.hipInflightOverride = 0;
        }
    });

    searchLockUi(false);
}

FormSearchControl::~FormSearchControl()
{
    stimer.stop();
    tuneTimer.stop();
    tuningThread.stopSearch();
    sthread.stopSearch();
    delete ui;
}

bool FormSearchControl::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest)
    {
        QFontMetrics fm = QFontMetrics(ui->results->font());
        ui->results->setColumnWidth(SeedTableModel::COL_SEED, txtWidth(fm, QString(24, '#')));
        ui->results->setColumnWidth(SeedTableModel::COL_TOP16, txtWidth(fm, QString(12, '#')));
        ui->results->verticalHeader()->setDefaultSectionSize(fm.height());
    }
    return QWidget::event(e);
}

std::vector<uint64_t> FormSearchControl::getResults()
{
    int n = proxy->rowCount();
    std::vector<uint64_t> results (n);
    for (int i = 0; i < n; i++)
    {
        results[i] = proxy->data(proxy->index(i, SeedTableModel::COL_SEED), Qt::UserRole).toULongLong();
    }
    return results;
}

SearchConfig FormSearchControl::getSearchConfig()
{
    SearchConfig s;
    s.searchtype = ui->comboSearchType->currentData().toInt();
    s.threads = ui->spinThreads->value();
    s.slist64path = slist64path;
    s.startseed = ui->lineStart->text().toLongLong();
    s.stoponres = ui->checkStop->isChecked();
    s.smin = smin;
    s.smax = smax;
    return s;
}

bool FormSearchControl::setSearchConfig(SearchConfig s, bool quiet)
{
    bool ok = true;
    if (s.searchtype >= SEARCH_INC && s.searchtype <= SEARCH_48ONLY)
    {
        ui->comboSearchType->setCurrentIndex(ui->comboSearchType->findData(s.searchtype));
        on_comboSearchType_currentIndexChanged(s.searchtype);
    }
    else
    {
        ok = false;
    }

    ui->spinThreads->setValue(s.threads);
    ui->checkStop->setChecked(s.stoponres);
    smin = s.smin;
    smax = s.smax;

#if WASM
    (void) quiet;
#else
    if (ok)
        ok &= setList64(s.slist64path, quiet);
#endif

    ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)s.startseed));

    return ok;
}

void FormSearchControl::stopSearch()
{
    sthread.stopSearch();
    onBufferTimeout();
}

bool FormSearchControl::setList64(QString path, bool quiet)
{
    if (!path.isEmpty())
    {
        QFileInfo finfo(path);
        parent->prevdir = finfo.absolutePath();
        slist64fnam = finfo.fileName();
        slist64path = path;
        uint64_t *l = NULL;
        uint64_t len;
        QByteArray ba = path.toLocal8Bit();
        l = loadSavedSeeds(ba.data(), &len);
        if (l != NULL)
        {
            slist64.assign(l, l+len);
            updateSearchProgress(0, len, l[0]);
            free(l);
            return true;
        }
        else if (!quiet)
        {
            int button = warn(this, tr("Warning"),
                tr("Failed to load 64-bit seed list from file:\n\"%1\"").arg(path),
                tr("Reset list path?"), QMessageBox::Reset | QMessageBox::Ignore);
            if (button != QMessageBox::Ignore)
            {
                slist64fnam.clear();
                slist64path.clear();
                slist64.clear();
            }
        }
    }
    return false;
}

bool FormSearchControl::setList64(QTextStream& stream)
{
    slist64fnam.clear();
    slist64path.clear();
    slist64.clear();

    while (!stream.atEnd())
    {
        QByteArray line = stream.readLine().toLocal8Bit();
        uint64_t s = 0;
        if (sscanf(line.data(), "%" PRId64, (int64_t*)&s) == 1)
            slist64.push_back(s);
    }

    if (!slist64.empty())
        updateSearchProgress(0, slist64.size(), slist64[0]);

    return true;
}

void FormSearchControl::setResultsPath(QString path)
{
    resultfile.close();
    resultfile.setFileName(path);
}

void FormSearchControl::searchLockUi(bool lock)
{
    if (lock)
    {
        ui->comboSearchType->setEnabled(false);
        ui->spinThreads->setEnabled(false);
        ui->checkHip->setEnabled(false);
        ui->buttonMore->setEnabled(false);
    }
    else
    {
        ui->buttonStart->setText(tr("Start search"));
        ui->buttonStart->setIcon(QIcon(":/icons/search.png"));
        ui->buttonStart->setChecked(false);
        ui->buttonStart->setEnabled(true);
        ui->comboSearchType->setEnabled(true);
        ui->spinThreads->setEnabled(true);
        ui->checkHip->setEnabled(true);
        int type = ui->comboSearchType->currentData().toInt();
        ui->buttonMore->setEnabled(type == SEARCH_INC || type == SEARCH_LIST);
    }
    emit searchStatusChanged(lock);
}

void FormSearchControl::setSearchMode(int mode)
{
    ui->comboSearchType->setCurrentIndex(ui->comboSearchType->findData(mode));
    if (mode == SEARCH_LIST)
    {
        on_buttonMore_clicked();
    }
    else
    {
        slist64.clear();
        slist64path.clear();
        slist64fnam.clear();
    }
}

void FormSearchControl::setSearchRange(uint64_t smin, uint64_t smax)
{
    this->smin = smin;
    this->smax = smax;
    searchProgressReset();
}

bool FormSearchControl::getSeed(int row, uint64_t *seed)
{
    QAbstractItemModel *model = ui->results->model();
    if (row < 0 || row >= model->rowCount())
        return false;
    QModelIndex idx = model->index(row, SeedTableModel::COL_SEED);
    *seed = model->data(idx, Qt::UserRole).toULongLong();
    return true;
}

QString FormSearchControl::currentTuneProfileKey()
{
    Session session;
    parent->getSeed(&session.wi);
    session.cv = parent->formCond->getConditions();
    session.sc = getSearchConfig();
    session.gen48 = parent->formGen48->getConfig(true);
    if (session.sc.searchtype == SEARCH_LIST)
        session.slist = slist64;
    else if (session.gen48.mode == GEN48_LIST)
        session.slist = parent->formGen48->getList48();

    QString header;
    QTextStream stream(&header);
    session.writeHeader(stream);
    QByteArray keyData;
    for (const QString &line : header.split('\n'))
    {
        // These are runtime controls or progress, not workload identity. In
        // particular #Progress moves after each run and must not invalidate a
        // profile that was just saved at the end of the tuning run.
        if (line.startsWith("#Time:") || line.startsWith("#Progress:") ||
            line.startsWith("#Threads:") || line.startsWith("#ResStop:") ||
            line.startsWith("#List64:") || line.startsWith("#List48:"))
            continue;
        keyData.append(line.toUtf8()).append('\n');
    }
    for (uint64_t seed : session.slist)
        keyData.append(QByteArray::number(static_cast<qulonglong>(seed))).append('\n');
    keyData.append(QSysInfo::currentCpuArchitecture().toUtf8()).append('|');
    keyData.append(qgetenv("PROCESSOR_IDENTIFIER")).append('|');
    keyData.append(QSysInfo::productType().toUtf8()).append('|');
    keyData.append(QByteArray::number(QSysInfo::productVersion().toDouble(), 'f', 1)).append('|');
    keyData.append(QByteArray::number(QThread::idealThreadCount())).append('|');
    keyData.append(QByteArray::number(g_extgen.experimentalVers)).append('|');
    keyData.append(QByteArray::number(g_extgen.estimateTerrain)).append('|');
    keyData.append(QByteArray::number(g_extgen.saltOverride)).append('|');
    for (uint64_t salt : g_extgen.salts)
        keyData.append(QByteArray::number(static_cast<qulonglong>(salt))).append('|');
    keyData.append(sthread.hip.deviceIdentity().toUtf8()).append('|');
    keyData.append(QByteArray::number(CVHIP_ABI_VERSION));
    keyData.append("|tuner-v3");
    return QString::fromLatin1(QCryptographicHash::hash(keyData, QCryptographicHash::Sha256).toHex());
}

void FormSearchControl::saveTuneProfile(const TuneCandidate &candidate)
{
    QSettings settings(APP_STRING, APP_STRING);
    const QString path = "speedTuner/profiles/" + tuneProfileKey;
    settings.setValue(path + "/threads", candidate.threads);
    settings.setValue(path + "/gpu", candidate.useHip);
    settings.setValue(path + "/batch", candidate.batchLimit);
    settings.setValue(path + "/inflight", candidate.inflight);
    settings.setValue(path + "/rate", tuneConfirmedRate);
    activeTuneProfileKey = tuneProfileKey;
}

bool FormSearchControl::restoreTuneProfile()
{
    if (manualTuneOverride)
        return false;
    const QString key = currentTuneProfileKey();
    QSettings settings(APP_STRING, APP_STRING);
    const QString path = "speedTuner/profiles/" + key;
    const auto discardAppliedProfile = [this, &key]() {
        if (activeTuneProfileKey == key)
        {
            sthread.hipBatchLimitOverride = 0;
            sthread.hipInflightOverride = 0;
            activeTuneProfileKey.clear();
        }
    };
    const QStringList fields = {"threads", "gpu", "batch", "inflight", "rate"};
    for (const QString &field : fields)
        if (!settings.contains(path + "/" + field))
        {
            discardAppliedProfile();
            return false;
        }

    bool threadsOk = false, batchOk = false, inflightOk = false, rateOk = false;
    const int threads = settings.value(path + "/threads").toInt(&threadsOk);
    const uint batch = settings.value(path + "/batch").toUInt(&batchOk);
    const uint inflight = settings.value(path + "/inflight").toUInt(&inflightOk);
    const double rate = settings.value(path + "/rate").toDouble(&rateOk);
    const QVariant gpuValue = settings.value(path + "/gpu");
    const bool gpu = gpuValue.toBool();
    const bool gpuValueValid = gpuValue.type() == QVariant::Bool ||
        gpuValue.toString() == QLatin1String("true") || gpuValue.toString() == QLatin1String("false") ||
        gpuValue.toString() == QLatin1String("1") || gpuValue.toString() == QLatin1String("0");
    const int maxThreads = std::max(1, QThread::idealThreadCount());
    const bool validGpuSettings = gpu ? batch > 0 && batch <= CVHIP_MAX_BATCH && inflight >= 1 && inflight <= 2
                                      : batch == 0 && inflight >= 1 && inflight <= 2;
    if (!threadsOk || threads < 1 || threads > maxThreads || !gpuValueValid ||
        !batchOk || !inflightOk || !rateOk || !std::isfinite(rate) || rate <= 0 || !validGpuSettings)
    {
        discardAppliedProfile();
        return false;
    }

    TuneCandidate candidate(threads, gpu, batch, inflight);
    applyingTuneSettings = true;
    ui->spinThreads->setValue(candidate.threads);
    ui->checkHip->setChecked(candidate.useHip);
    sthread.useHip = candidate.useHip;
    sthread.hipBatchLimitOverride = candidate.useHip ? candidate.batchLimit : 0;
    sthread.hipInflightOverride = candidate.useHip ? candidate.inflight : 0;
    activeTuneProfileKey = key;
    applyingTuneSettings = false;
    ui->labelTuneStatus->clear();
    return true;
}

void FormSearchControl::sessionLoaded()
{
    manualTuneOverride = false;
    tuneExistingResults.clear();
    for (const SeedTableModel::Seed &row : model->seeds)
        tuneExistingResults.insert(row.seed);
    const QString key = currentTuneProfileKey();
    if (!activeTuneProfileKey.isEmpty() && activeTuneProfileKey != key)
    {
        sthread.hipBatchLimitOverride = 0;
        sthread.hipInflightOverride = 0;
        activeTuneProfileKey.clear();
    }
    restoreTuneProfile();
}


void FormSearchControl::on_buttonClear_clicked()
{
    tunerReplayActive = false;
    tuneExistingResults.clear();
    model->reset();
    searchProgressReset();
    ui->lineStart->setText("0");
}

void FormSearchControl::on_buttonStart_clicked()
{
    if (ui->buttonStart->isChecked())
    {
        if (!activeTuneProfileKey.isEmpty() && currentTuneProfileKey() != activeTuneProfileKey)
        {
            // Batch/in-flight overrides are profile-specific runtime choices;
            // a condition/version/generator edit invalidates only these and
            // leaves the user's chosen thread count and GPU toggle untouched.
            sthread.hipBatchLimitOverride = 0;
            sthread.hipInflightOverride = 0;
            activeTuneProfileKey.clear();
        }
        Session session;
        parent->getSeed(&session.wi);
        session.cv = parent->formCond->getConditions();
        session.sc = getSearchConfig();
        int ok = true;

        if (session.cv.empty())
        {
            warn(this, tr("Please define some constraints using the \"Add\" button."));
            ok = false;
        }
        if (session.sc.searchtype == SEARCH_LIST && slist64.empty())
        {
            warn(this, tr("No seed list file selected."));
            ok = false;
        }
        if (ok)
        {
            session.gen48 = parent->formGen48->getConfig(true);
            // the search can either use a full list or a 48-bit list
            if (session.sc.searchtype == SEARCH_LIST)
                session.slist = slist64;
            else if (session.gen48.mode == GEN48_LIST)
                session.slist = parent->formGen48->getList48();
            else
                session.slist.clear();

            ok = sthread.set(parent, session);
        }

        if (ok)
        {
            tunerReplayActive = false;
            tuneExistingResults.clear();
            for (const SeedTableModel::Seed &row : model->seeds)
                tuneExistingResults.insert(row.seed);
            if (!resultfile.fileName().isEmpty())
            {
                resultfile.close();
                resultfile.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Text);
                QTextStream stream(&resultfile);
                session.writeHeader(stream);
            }
            ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)session.sc.startseed));
            ui->buttonStart->setText(tr("Abort search"));
            ui->buttonStart->setIcon(QIcon(":/icons/cancel.png"));
            searchLockUi(true);
            nextupdate = 0;
            updt = 20;
            sthread.useHip = ui->checkHip->isChecked();
            sthread.startSearch();
            elapsed.start();
            stimer.start(250);
        }
        else
        {
            ui->buttonStart->setChecked(false);
        }
    }
    else
    {
        ui->buttonStart->setText(tr("Start search"));
        ui->buttonStart->setIcon(QIcon(":/icons/search.png"));
        ui->buttonStart->setChecked(false);

        // disable until finish
        ui->buttonStart->setEnabled(false);
        stopSearch();
    }

    update();
}

void FormSearchControl::on_buttonMore_clicked()
{
    int type = ui->comboSearchType->currentData().toInt();
    if (type == SEARCH_LIST)
    {
        QString filter = tr("Text files (*.txt);;Any files (*)");
#if WASM
        auto fileOpenCompleted = [=](const QString &fnam, const QByteArray &content) {
            if (!fnam.isEmpty()) {
                QTextStream stream(content);
                setList64(stream);
            }
        };
        QFileDialog::getOpenFileContent(filter, fileOpenCompleted);
#else
        QString fnam = QFileDialog::getOpenFileName(this, tr("Load seed list"), parent->prevdir, filter);
        setList64(fnam, false);
#endif
    }
    else if (type == SEARCH_INC)
    {
        RangeDialog *dialog = new RangeDialog(this, smin, smax);
        connect(dialog, &RangeDialog::applyBounds, this, &FormSearchControl::setSearchRange);
        dialog->show();
    }
}

std::vector<int> FormSearchControl::tuneThreadCounts() const
{
    const int maximum = std::max(1, QThread::idealThreadCount());
    const int current = std::max(1, std::min(maximum, ui->spinThreads->value()));
    std::vector<int> counts = {std::max(1, maximum / 4), std::max(1, maximum / 2),
        std::max(1, (maximum * 3) / 4), current, maximum};
    std::sort(counts.begin(), counts.end());
    counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
    return counts;
}

bool FormSearchControl::sameTuneCandidate(const TuneCandidate &a, const TuneCandidate &b) const
{
    return a.threads == b.threads && a.useHip == b.useHip &&
        a.batchLimit == b.batchLimit && a.inflight == b.inflight;
}

double FormSearchControl::acceptedTuneRate(double measuredRate, bool gpuRequested,
    bool gpuActive, double deviceElapsedMs)
{
    // CPU-side queue wait is deliberately not part of this safety decision.
    (void)deviceElapsedMs; // Stream event time only controls batch growth.
    return gpuRequested && !gpuActive ? 0.0 : measuredRate;
}

bool FormSearchControl::canGrowTuneBatch(double deviceElapsedMs, unsigned actualBatch, unsigned backendCap)
{
    return deviceElapsedMs > 0.0 && deviceElapsedMs * 2.0 < 250.0 && actualBatch < backendCap;
}

unsigned FormSearchControl::smallerTuneBatch(unsigned actualBatch)
{
    return actualBatch > 1 ? actualBatch / 2 : 0;
}

static void showTuneCountdown(QLabel *label, qint64 remainingMs)
{
    remainingMs = std::max<qint64>(0, remainingMs);
    label->setText(QObject::tr("· %1:%2 left")
        .arg(remainingMs / 60000).arg((remainingMs / 1000) % 60, 2, 10, QChar('0')));
}

void FormSearchControl::on_buttonTune_clicked()
{
    if (tuningActive)
    {
        finishSpeedTune(true);
        return;
    }

    tuneSession = Session();
    parent->getSeed(&tuneSession.wi);
    tuneSession.cv = parent->formCond->getConditions();
    tuneSession.sc = getSearchConfig();
    if (tuneSession.cv.empty())
    {
        warn(this, tr("Please define some constraints using the \"Add\" button."));
        return;
    }
    for (const Condition &condition : tuneSession.cv)
    {
        if (condition.type == F_LUA && !(condition.meta & Condition::DISABLED))
        {
            ui->labelTuneStatus->setText(tr("Tuning is unavailable for sessions with enabled Lua conditions."));
            return;
        }
    }
    tuneSession.gen48 = parent->formGen48->getConfig(true);
    if (tuneSession.sc.searchtype == SEARCH_LIST)
    {
        if (slist64.empty())
        {
            warn(this, tr("No seed list file selected."));
            return;
        }
        tuneSession.slist = slist64;
    }
    else if (tuneSession.gen48.mode == GEN48_LIST)
    {
        tuneSession.slist = parent->formGen48->getList48();
    }

    tuneOriginalConfig = tuneSession.sc;
    tuneResumeSearch = ui->buttonStart->isChecked() && !sthread.workers.empty() && !sthread.stop.load();
    tuneHasSafeCursor = false;
    if (tuneResumeSearch)
    {
        if (!sthread.captureSafeCursor(&tuneResumeCursor))
        {
            ui->labelTuneStatus->setText(tr("Could not capture a safe search position; tuning was not started."));
            tuneResumeSearch = false;
            return;
        }
        tuneResumeSearch = !tuneResumeCursor.done;
        tuneHasSafeCursor = tuneResumeSearch;
        ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)tuneResumeCursor.seed));
    }

    tuneOriginalCandidate = TuneCandidate(tuneOriginalConfig.threads, ui->checkHip->isChecked(),
        sthread.hipBatchLimitOverride, sthread.hipInflightOverride ? sthread.hipInflightOverride : 2);
    tuneBaselineCandidate = tuneOriginalCandidate;
    tuneBest = tuneOriginalCandidate;
    tuneBestRate = 0;
    tuneBaselineRate = 0;
    tuneConfirmedRate = 0;
    tuneBaselineMeasured = false;
    tuneConfirmMeasured = false;
    tuneConfirmedBaselineRate = 0;
    tuneBestEffectiveBatch = 0;
    tuneCandidateIndex = 0;
    tuneCandidates.clear();
    auto addCandidate = [&](const TuneCandidate &candidate) {
        if (std::none_of(tuneCandidates.begin(), tuneCandidates.end(),
            [&](const TuneCandidate &existing) { return sameTuneCandidate(existing, candidate); }))
            tuneCandidates.push_back(candidate);
    };
    const std::vector<int> threadCounts = tuneThreadCounts();
    const int maxThreads = *std::max_element(threadCounts.begin(), threadCounts.end());
    addCandidate(TuneCandidate{maxThreads, false, 0, 2});
    unsigned batchCap = CVHIP_MAX_BATCH;
    if (tuneSession.sc.searchtype == SEARCH_LIST || !tuneSession.slist.empty())
        batchCap = CVHIP_MAX_LIST_BATCH;
    else if (tuneSession.sc.searchtype == SEARCH_BLOCKS)
        batchCap = 4096;
    else if (tuneSession.sc.searchtype == SEARCH_48ONLY)
        batchCap = 65536;
    const unsigned firstGpuBatch = std::max(1U, batchCap / 4);
    addCandidate(TuneCandidate{maxThreads, true, firstGpuBatch, 2});
    for (auto it = threadCounts.rbegin(); it != threadCounts.rend(); ++it)
    {
        addCandidate(TuneCandidate{*it, false, 0, 2});
    }
    for (unsigned batch : {std::max(1U, batchCap / 2), batchCap})
        addCandidate(TuneCandidate{maxThreads, true, batch, 2});
    for (int threads : threadCounts)
        addCandidate(TuneCandidate{threads, true, firstGpuBatch, 2});
    tuneGpuMaxAllowedBatch = firstGpuBatch;
    tuneGpuAvailable = false;
    tuneGpuChecked = false;
    tuneConfirming = false;
    tuneMeasureStarted = false;
    tuneTrialMeasured = false;
    tuningActive = true;
    tuneClock.start();
    tuneDurationMs = qint64(tuneDurationMinutes) * 60000;
    tuneWarmupEndMs = qint64(std::llround(tuneDurationMs * 0.125));
    tuneBaselineEndMs = qint64(std::llround(tuneDurationMs * 0.2083333333));
    tuneExploreEndMs = qint64(std::llround(tuneDurationMs * 0.6666666667));
    tuneConfirmBaselineEndMs = qint64(std::llround(tuneDurationMs * 0.7916666667));
    tuneConfirmEndMs = qint64(std::llround(tuneDurationMs * 0.9166666667));
    tunePhase = TUNE_WARMUP;
    tuneProfileKey = currentTuneProfileKey();
    tuneExistingResults.clear();
    for (const SeedTableModel::Seed &row : model->seeds)
        tuneExistingResults.insert(row.seed);

    searchLockUi(true);
    ui->buttonStart->setEnabled(false);
    ui->spinTuneMinutes->setEnabled(false);
    ui->buttonTune->setText(tr("Cancel tuning"));
    if (tuneResumeSearch)
    {
        const int originalResultCount = model->seeds.size();
        stimer.stop();
        sthread.stopSearch();
        onBufferTimeout();
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
        tuneExistingResults.clear();
        for (const SeedTableModel::Seed &row : model->seeds)
            tuneExistingResults.insert(row.seed);
        const bool resultStoppedDuringDrain = model->seeds.size() > originalResultCount &&
            (ui->checkStop->isChecked() || tuneOriginalConfig.stoponres ||
                model->seeds.size() >= parent->config.maxMatching);
        if (resultStoppedDuringDrain)
        {
            tuneResumeSearch = false;
            ui->labelTuneStatus->setText(tr("A result/limit stopped the original scan during pause; it will not be resumed."));
        }
    }
    showTuneCountdown(ui->labelTuneStatus, tuneDurationMs - tuneClock.elapsed());
    tuneTimer.start();
    startTuneCandidate();
}

void FormSearchControl::startTuneCandidate()
{
    if (!tuningActive)
        return;
    const qint64 now = tuneClock.elapsed();
    if (now >= tuneDurationMs)
    {
        finishSpeedTune(false);
        return;
    }
    TuneCandidate candidate = tuneOriginalCandidate;
    if (tunePhase == TUNE_WARMUP)
        candidate = tuneOriginalCandidate;
    else if (tunePhase == TUNE_BASELINE || tunePhase == TUNE_CONFIRM_BASELINE)
        candidate = tuneBaselineCandidate;
    else if (tunePhase == TUNE_CONFIRM_BEST)
        candidate = tuneBest;
    else if (tunePhase == TUNE_EXPLORE && tuneCandidateIndex < tuneCandidates.size())
        candidate = tuneCandidates[tuneCandidateIndex];
    else if (tunePhase == TUNE_CLEANUP)
    {
        showTuneCountdown(ui->labelTuneStatus, tuneDurationMs - now);
        return;
    }
    else
    {
        tunePhase = TUNE_CONFIRM_BASELINE;
        startTuneCandidate();
        return;
    }

    if (tunePhase == TUNE_EXPLORE && candidate.useHip && candidate.batchLimit > tuneGpuMaxAllowedBatch)
    {
        ++tuneCandidateIndex;
        startTuneCandidate();
        return;
    }

    if (candidate.useHip && tuneGpuChecked && !tuneGpuAvailable)
    {
        if (tunePhase == TUNE_WARMUP)
            return; // Keep the scheduled warm-up interval; baseline will skip GPU.
        if (tunePhase == TUNE_BASELINE)
        {
            tuneBaselineMeasured = false;
            tuneBaselineCandidate = TuneCandidate(tuneOriginalCandidate.threads, false, 0, 2);
            startTuneCandidate();
            return;
        }
        if (tunePhase == TUNE_CONFIRM_BASELINE || tunePhase == TUNE_CONFIRM_BEST)
        {
            tuneConfirmedRate = 0;
            tuneConfirmMeasured = false;
            tunePhase = TUNE_CLEANUP;
            return;
        }
        else
        {
            ++tuneCandidateIndex;
            startTuneCandidate();
            return;
        }
    }
    if (tunePhase == TUNE_CLEANUP)
        return;

    Session trial = tuneSession;
    trial.sc.threads = candidate.threads;
    trial.sc.stoponres = false;
    if (!tuningThread.set(this, trial))
    {
        if (tunePhase == TUNE_WARMUP)
            return;
        if (tunePhase == TUNE_BASELINE)
        {
            if (candidate.useHip)
            {
                tuneBaselineCandidate = TuneCandidate(tuneOriginalCandidate.threads, false, 0, 2);
                startTuneCandidate();
            }
            else
                tunePhase = TUNE_CLEANUP;
            return;
        }
        else if (tunePhase == TUNE_CONFIRM_BASELINE || tunePhase == TUNE_CONFIRM_BEST)
            tunePhase = TUNE_CLEANUP;
        else
            ++tuneCandidateIndex;
        startTuneCandidate();
        return;
    }
    tuningThread.useHip = candidate.useHip;
    tuningThread.trackCompletedSeeds = true;
    // The tuner compares fixed candidates; runtime auto-clipping would turn
    // one candidate into a changing batch policy and distort its measurements.
    tuningThread.hipAdaptiveSafety = false;
    tuningThread.hipBatchLimitOverride = candidate.batchLimit;
    tuningThread.hipInflightOverride = candidate.inflight;
    tuningThread.hip.resetDeviceElapsedMilliseconds();
    if (tuneHasSafeCursor)
        tuningThread.setResumeCursor(tuneResumeCursor);
    tuningThread.startSearch();

    if (candidate.useHip && !tuningThread.hip.active())
    {
        tuneGpuChecked = true;
        tuneGpuAvailable = false;
        tuningThread.stopSearch();
        if (tunePhase == TUNE_WARMUP)
            return; // The scheduled warm-up interval still elapses normally.
        if (tunePhase == TUNE_BASELINE)
        {
            tuneBaselineMeasured = false;
            tuneBaselineCandidate = TuneCandidate(tuneOriginalCandidate.threads, false, 0, 2);
            startTuneCandidate();
            return;
        }
        else if (tunePhase == TUNE_CONFIRM_BASELINE || tunePhase == TUNE_CONFIRM_BEST)
        {
            tuneConfirmedRate = 0;
            tuneConfirmMeasured = false;
            tunePhase = TUNE_CLEANUP;
        }
        else
        {
            ++tuneCandidateIndex;
            ui->labelTuneStatus->setText(tr("GPU BOOST unavailable; continuing with CPU settings."));
            startTuneCandidate();
        }
        return;
    }
    if (candidate.useHip)
    {
        tuneGpuChecked = true;
        tuneGpuAvailable = true;
        const unsigned backendCap = tuningThread.hip.backendBatchCap();
        if (backendCap > 0)
            tuneGpuMaxAllowedBatch = std::min(tuneGpuMaxAllowedBatch, backendCap);
        const unsigned actualBatch = tuningThread.hip.batchLimit();
        if (actualBatch > 0)
            candidate.batchLimit = actualBatch;
        candidate.inflight = tuningThread.hip.inflightLimit();
        if (tunePhase == TUNE_WARMUP)
            tuneBaselineCandidate = candidate;
        else if (tunePhase == TUNE_BASELINE)
            tuneBaselineCandidate = candidate;
        else if (tunePhase == TUNE_CONFIRM_BASELINE)
            tuneBaselineCandidate = candidate;
        else if (tunePhase == TUNE_EXPLORE && tuneCandidateIndex < tuneCandidates.size())
            tuneCandidates[tuneCandidateIndex] = candidate;
        else if (tunePhase == TUNE_CONFIRM_BEST)
            tuneBest = candidate;
    }

    tuneTrialStartMs = now;
    tuneTrialStartCount = tuningThread.completedSeeds();
    tuneMeasureStarted = false;
    tuneTrialMeasured = false;
    tuneTrialWarmEndMs = now + 2000;
    tuneTrialClock.start();
    showTuneCountdown(ui->labelTuneStatus, tuneDurationMs - tuneClock.elapsed());
}

void FormSearchControl::tuneTimeout()
{
    if (!tuningActive)
        return;
    const qint64 now = tuneClock.elapsed();
    if (now >= tuneDurationMs)
    {
        finishSpeedTune(false);
        return;
    }
    showTuneCountdown(ui->labelTuneStatus, tuneDurationMs - now);
    if (tunePhase == TUNE_WARMUP && now >= tuneWarmupEndMs)
    {
        const bool originalIsBaseline = tuneOriginalCandidate.threads == tuneBaselineCandidate.threads &&
            tuneOriginalCandidate.useHip == tuneBaselineCandidate.useHip &&
            tuneOriginalCandidate.inflight == tuneBaselineCandidate.inflight &&
            (!tuneOriginalCandidate.batchLimit || !tuneBaselineCandidate.batchLimit ||
                tuneOriginalCandidate.batchLimit == tuneBaselineCandidate.batchLimit) &&
            !tuningThread.workers.empty() && !tuningThread.stop.load() &&
            (!tuneOriginalCandidate.useHip || tuningThread.hip.active());
        tunePhase = TUNE_BASELINE;
        if (originalIsBaseline)
        {
            // Keep the warmed-up workers and GPU queue intact when warm-up
            // already uses the baseline settings. This avoids draining a
            // large HIP batch only to immediately relaunch the same batch.
            tuneMeasureStart = tuningThread.completedSeeds();
            tuneMeasureClock.start();
            tuneMeasureStarted = true;
            showTuneCountdown(ui->labelTuneStatus, tuneDurationMs - tuneClock.elapsed());
        }
        else
        {
            tuningThread.stopSearch();
            startTuneCandidate();
        }
        return;
    }
    if (tunePhase == TUNE_BASELINE && now >= tuneBaselineEndMs)
    {
        finishTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_EXPLORE && now >= tuneExploreEndMs)
    {
        finishTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_CONFIRM_BASELINE && now >= tuneConfirmBaselineEndMs)
    {
        finishTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_CONFIRM_BEST && now >= tuneConfirmEndMs)
    {
        finishTuneCandidate();
        return;
    }

    const qint64 elapsedMs = tuneTrialClock.elapsed();
    if (!tuneMeasureStarted && elapsedMs >= 2000)
    {
        tuneMeasureStart = tuningThread.completedSeeds();
        tuneMeasureClock.start();
        tuneMeasureStarted = true;
        return;
    }
    if (tunePhase == TUNE_EXPLORE && tuneMeasureStarted && tuneMeasureClock.elapsed() >= 5000)
        finishTuneCandidate();

}

void FormSearchControl::finishTuneCandidate()
{
    if (!tuningActive)
        return;

    TuneCandidate candidate = tuneOriginalCandidate;
    if (tunePhase == TUNE_EXPLORE && tuneCandidateIndex < tuneCandidates.size())
        candidate = tuneCandidates[tuneCandidateIndex];
    else if (tunePhase == TUNE_BASELINE || tunePhase == TUNE_CONFIRM_BASELINE)
        candidate = tuneBaselineCandidate;
    else if (tunePhase == TUNE_CONFIRM_BEST)
        candidate = tuneBest;
    const unsigned effectiveBatch = tuningThread.hip.batchLimit();
    const unsigned backendCap = tuningThread.hip.backendBatchCap();
    const bool gpuActive = tuningThread.hip.active();
    tuningThread.stopSearch(); // Drain CPU verifiers and pending GPU requests before counting.
    const double deviceElapsedMs = tuningThread.hip.lastDeviceElapsedMilliseconds();
    const uint64_t completed = tuningThread.completedSeeds();
    const qint64 sampleMs = std::max<qint64>(1, tuneMeasureClock.elapsed());
    double rate = tuneMeasureStarted && completed >= tuneMeasureStart
        ? double(completed - tuneMeasureStart) * 1000.0 / sampleMs : 0.0;
    rate = acceptedTuneRate(rate, candidate.useHip, gpuActive, deviceElapsedMs);
    qInfo("Speed tuner trial: phase=%d candidate=%llu/%llu mode=%s threads=%d requested-batch=%u effective-batch=%u gpu-active=%d device-stream=%.3fms verified-rate=%.0f/s",
        int(tunePhase), qulonglong(tuneCandidateIndex + 1), qulonglong(tuneCandidates.size()),
        candidate.useHip ? "GPU" : "CPU", candidate.threads, candidate.batchLimit,
        effectiveBatch, int(gpuActive), deviceElapsedMs, rate);

    if (tunePhase == TUNE_WARMUP)
    {
        tunePhase = TUNE_BASELINE;
        startTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_BASELINE)
    {
        tuneBaselineRate = rate;
        tuneBaselineMeasured = rate > 0;
        if (rate > tuneBestRate)
        {
            tuneBestRate = rate;
            tuneBest = tuneBaselineCandidate;
        }
        tunePhase = TUNE_EXPLORE;
        tuneCandidateIndex = 0;
        startTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_CONFIRM_BASELINE)
    {
        tuneConfirmedBaselineRate = rate;
        tunePhase = TUNE_CONFIRM_BEST;
        startTuneCandidate();
        return;
    }
    if (tunePhase == TUNE_CONFIRM_BEST)
    {
        tuneConfirmedRate = rate;
        tuneConfirmMeasured = rate > 0;
        tunePhase = TUNE_CLEANUP;
        return;
    }

    if (tunePhase != TUNE_EXPLORE)
        return;
    if (candidate.useHip && !gpuActive)
        tuneGpuAvailable = false;
    if (rate > tuneBestRate)
    {
        tuneBestRate = rate;
        tuneBest = candidate;
        tuneBestEffectiveBatch = effectiveBatch;
    }

    if (candidate.useHip && gpuActive)
    {
        // Candidate sizes are preloaded as a bounded quarter/half/cap ladder.
        // Increase the allowed ceiling only when the worst completed request
        // predicts <250 ms; >500 ms excludes this size and all larger sizes.
        const unsigned actual = effectiveBatch ? effectiveBatch : candidate.batchLimit;
        if (deviceElapsedMs > 500.0)
        {
            tuneGpuMaxAllowedBatch = std::min(tuneGpuMaxAllowedBatch,
                std::max(1U, smallerTuneBatch(actual)));
            const unsigned smaller = smallerTuneBatch(actual);
            TuneCandidate option{candidate.threads, true, smaller, candidate.inflight};
            if (smaller > 0 && std::none_of(tuneCandidates.begin(), tuneCandidates.end(),
                [&](const TuneCandidate &existing) { return sameTuneCandidate(existing, option); }))
                tuneCandidates.insert(tuneCandidates.begin() + std::min(tuneCandidateIndex + 1, tuneCandidates.size()), option);
        }
        else if (canGrowTuneBatch(deviceElapsedMs, actual, backendCap))
        {
            const unsigned larger = std::min(backendCap, actual * 2);
            tuneGpuMaxAllowedBatch = std::max(tuneGpuMaxAllowedBatch, larger);
            TuneCandidate option{candidate.threads, true, larger, candidate.inflight};
            if (larger > actual && std::none_of(tuneCandidates.begin(), tuneCandidates.end(),
                [&](const TuneCandidate &existing) { return sameTuneCandidate(existing, option); }))
                tuneCandidates.insert(tuneCandidates.begin() + std::min(tuneCandidateIndex + 1, tuneCandidates.size()), option);
        }
        TuneCandidate oneInflight{candidate.threads, true, candidate.batchLimit, candidate.inflight == 1 ? 2U : 1U};
        if (std::none_of(tuneCandidates.begin(), tuneCandidates.end(),
            [&](const TuneCandidate &existing) { return sameTuneCandidate(existing, oneInflight); }))
            tuneCandidates.push_back(oneInflight);
        for (int threads : tuneThreadCounts())
        {
            TuneCandidate option{threads, true, candidate.batchLimit, candidate.inflight};
            if (std::none_of(tuneCandidates.begin(), tuneCandidates.end(),
                [&](const TuneCandidate &existing) { return sameTuneCandidate(existing, option); }))
                tuneCandidates.push_back(option);
        }
    }

    ++tuneCandidateIndex;
    if (tuneCandidateIndex >= tuneCandidates.size() || tuneClock.elapsed() >= tuneExploreEndMs)
    {
        tunePhase = TUNE_CONFIRM_BASELINE;
        startTuneCandidate();
        return;
    }
    startTuneCandidate();
}

void FormSearchControl::finishSpeedTune(bool cancelled)
{
    if (!tuningActive)
        return;

    tuneTimer.stop();
    tuningThread.stopSearch();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);

    TuneCandidate selected = tuneBest;
    const bool confirmedGain = tuneBaselineMeasured && tuneConfirmMeasured && tuneConfirmedBaselineRate > 0 &&
        tuneConfirmedRate >= std::max(tuneBaselineRate, tuneConfirmedBaselineRate) * 1.05;
    if (cancelled || !confirmedGain)
    {
        selected = tuneOriginalCandidate;
    }

    applyingTuneSettings = true;
    ui->spinThreads->setValue(selected.threads);
    ui->checkHip->setChecked(selected.useHip);
    applyingTuneSettings = false;
    sthread.hipBatchLimitOverride = selected.useHip ? selected.batchLimit : 0;
    sthread.hipInflightOverride = selected.useHip ? selected.inflight : 0;
    sthread.useHip = selected.useHip;
    ui->buttonTune->setText(tr("Tune speed"));
    ui->spinTuneMinutes->setEnabled(true);

    // Only persist a profile when its selected settings beat both baseline
    // measurements by the acceptance margin. Otherwise the selected settings
    // are the original ones, while tuneConfirmedRate still describes the
    // rejected best candidate and must not be stored beside them.
    if (!cancelled && confirmedGain)
        saveTuneProfile(selected);

    const bool resume = tuneResumeSearch;
    tuneSession.sc.threads = selected.threads;
    tuneSession.sc.stoponres = tuneOriginalConfig.stoponres;
    tuningActive = false;
    if (resume)
    {
        if (sthread.set(parent, tuneSession))
        {
            sthread.useHip = selected.useHip;
            sthread.hipBatchLimitOverride = selected.useHip ? selected.batchLimit : 0;
            sthread.hipInflightOverride = selected.useHip ? selected.inflight : 0;
            sthread.setResumeCursor(tuneResumeCursor);
            tunerReplayActive = true;
            ui->lineStart->setText(QString::asprintf("%" PRId64, (int64_t)tuneResumeCursor.seed));
            ui->buttonStart->setEnabled(true);
            ui->buttonStart->setChecked(true);
            ui->buttonStart->setText(tr("Abort search"));
            ui->buttonStart->setIcon(QIcon(":/icons/cancel.png"));
            searchLockUi(true);
            sthread.startSearch();
            elapsed.start();
            stimer.start(250);
            ui->labelTuneStatus->clear();
        }
        else
        {
            searchLockUi(false);
            ui->buttonStart->setEnabled(true);
            ui->labelTuneStatus->setText(tr("Tuning ended, but the saved search could not be resumed."));
        }
    }
    else
    {
        searchLockUi(false);
        ui->buttonStart->setEnabled(true);
        ui->labelTuneStatus->clear();
    }
}

void FormSearchControl::onSeedSelectionChanged()
{
    uint64_t s;
    if (getSeed(ui->results->currentIndex().row(), &s))
        emit selectedSeedChanged(s);
}

void FormSearchControl::on_results_clicked(const QModelIndex &)
{
    onSeedSelectionChanged();
}

void FormSearchControl::on_results_customContextMenuRequested(const QPoint &pos)
{
    QMenu *menu = new QMenu(this);

    // this is a contextual temporary menu so shortcuts are only indicated here,
    // but will not function - see keyReleaseEvent() for shortcut implementation

    QAction *actremove = menu->addAction(QIcon::fromTheme("list-remove"),
        tr("Remove selected seed"), this,
        &FormSearchControl::removeCurrent, QKeySequence::Delete);
    actremove->setEnabled(ui->results->selectionModel()->hasSelection());

    QAction *actcopyseed = menu->addAction(QIcon::fromTheme("edit-copy"),
        tr("Copy selected seed"), this,
        &FormSearchControl::copySeed, QKeySequence::Copy);
    actcopyseed->setEnabled(ui->results->selectionModel()->hasSelection());

    QAction *actcopylist = menu->addAction(QIcon::fromTheme("edit-copy"),
        tr("Copy seed list"), this,
        &FormSearchControl::copyResults, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    actcopylist->setEnabled(ui->results->model()->rowCount() > 0);

    int n = pasteList(true);
    QAction *actpaste = menu->addAction(QIcon::fromTheme("edit-paste"),
        tr("Paste %n seed(s) from clipboard", "", n), this,
        &FormSearchControl::pasteResults, QKeySequence::Paste);
    actpaste->setEnabled(n > 0);
    menu->popup(ui->results->mapToGlobal(pos));
}

void FormSearchControl::on_buttonSearchHelp_clicked()
{
    QMessageBox *mb = new QMessageBox(this);
    mb->setIcon(QMessageBox::Information);
    mb->setWindowTitle(tr("Help: search types"));
    mb->setText(tr(
        "<html><head/><body><p>"
        "The <b>incremental</b> search checks seeds in numerical order, "
        "except for grouping seeds into work items for parallelization. "
        "This is the recommended option for general searches. You can "
        "restrict this type of search to a value range using the "
        "&quot;...&quot; button."
        "</p><p>"
        "When using <b>48-bit only</b>, the search checks partial seeds and "
        "will not test the full conditions. Instead it yields seed bases "
        "that may satify the conditions without knowing the upper 16-bit of "
        "the seed. A session file saved from this search is suitable to be used "
        "later with the 48-bit generator to look for matching seeds."
        "</p><p>"
        "With <b>48-bit family blocks</b> the search looks for suitable "
        "48-bit seeds first and parallelizes the search through the upper "
        "16-bits. This search type can be a better match for exhaustive searches "
        "and those with very restrictive structure requirements."
        "</p><p>"
        "Load a <b>seed list from a file</b> to search through an "
        "existing set of seeds. The seeds should be in decimal ASCII text, "
        "separated by newline characters. You can browse for a file using "
        "the &quot;...&quot; button. (The seed generator is ignored with "
        "this option.)"
        "</p></body></html>"
        ));
    mb->show();
}

void FormSearchControl::on_comboSearchType_currentIndexChanged(int)
{
    int type = ui->comboSearchType->currentData().toInt();
    ui->buttonMore->setEnabled(type == SEARCH_INC || type == SEARCH_LIST);
    searchProgressReset();
}

void FormSearchControl::pasteResults()
{
    pasteList(false);
}

int FormSearchControl::pasteList(bool dummy)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QStringList slist = clipboard->text().split('\n');
    std::vector<uint64_t> seeds;

    for (QString s : qAsConst(slist))
    {
        s = s.trimmed();
        if (s.isEmpty())
            continue;
        bool ok = true;
        uint64_t seed = (uint64_t) s.toLongLong(&ok);
        if (!ok)
            return 0;
        seeds.push_back(seed);
    }

    if (!seeds.empty())
    {
        return searchResultsAdd(seeds, dummy);
    }
    return 0;
}

void FormSearchControl::onSort(int, Qt::SortOrder)
{
    // We want to achieve: none -> descending -> ascending -> none
    // The headerview flips the indicator with the logic:
    //  if (same_section)
    //      new_order = old_order == descending ? ascending : descending
    //  else
    //      new_order = InitialSortOrder (else ascending)
    QHeaderView *header = ui->results->horizontalHeader();

    if (proxy->order == Qt::AscendingOrder && proxy->column != -1)
    {
        header->setSortIndicatorShown(false);
        header->setSortIndicator(-1, Qt::DescendingOrder);
        proxy->column = -1;
    }
    else
    {
        header->setSortIndicatorShown(true);
    }
}

void FormSearchControl::searchResult(uint64_t seed)
{
    if (tunerReplayActive && tuneExistingResults.contains(seed))
        return;
    if (tunerReplayActive)
        tuneExistingResults.insert(seed);

    if (resultfile.isOpen())
    {
        char s[32];
        snprintf(s, sizeof(s), "%" PRId64 "\n", seed);
        resultfile.write(s);
        resultfile.flush();
    }

    qbuf.push_back(seed);
    if (ui->checkStop->isChecked())
    {
        searchResultsAdd(qbuf, false);
        qbuf.clear();
        return;
    }

    quint64 ns = elapsed.nsecsElapsed();
    if (ns > nextupdate)
    {
        nextupdate = ns + updt * 1e6;
        QTimer::singleShot(updt, this, &FormSearchControl::onBufferTimeout);
    }
}

void FormSearchControl::onBufferTimeout()
{
    uint64_t t = -elapsed.elapsed();

    searchResultsAdd(qbuf, false);
    qbuf.clear();

    QApplication::processEvents(); // force processing of events so we can time correctly

    t += elapsed.elapsed();
    if (8*t > updt)
        updt = 4*t;
    nextupdate = elapsed.nsecsElapsed() + 1e6 * updt;
}

int FormSearchControl::searchResultsAdd(std::vector<uint64_t> seeds, bool countonly)
{
    if (seeds.empty())
        return 0;
    const Config& config = parent->config;
    int n = model->seeds.size();
    int nold = n;
    bool discarded = false;

    if (n >= config.maxMatching)
    {
        sthread.stopSearch();
        discarded = true;
    }
    if (n + (ssize_t)seeds.size() > config.maxMatching)
    {
        sthread.stopSearch();
        discarded = true;
        seeds.resize(config.maxMatching - n);
    }

    QSet<uint64_t> current;
    current.reserve(n + seeds.size());
    for (int i = 0; i < n; i++)
        current.insert(model->seeds[i].seed);

    QVector<uint64_t> newseeds;
    for (uint64_t s : seeds)
    {
        if (current.contains(s))
            continue;
        if (!countonly)
        {
            current.insert(s);
            newseeds.append(s);
        }
        n++;
    }
    if (!newseeds.empty())
    {
        ui->results->setSortingEnabled(false);
        model->insertSeeds(newseeds);
        ui->results->setSortingEnabled(true);
    }

    int addcnt = n - nold;
    if (ui->checkStop->isChecked() && addcnt)
        sthread.stopSearch();

    if (!countonly && discarded)
    {
        // guard against recursive calls, which can occur when the
        // eventloop is continued in the warning message dialog
        thread_local bool warn_active = false;
        if (!warn_active)
        {
            warn_active = true;
            warn(this, tr("Maximum number of results reached (%1).").arg(config.maxMatching));
            warn_active = false;
        }
    }

    if (addcnt)
        emit resultsAdded(addcnt);

    return addcnt;
}

void FormSearchControl::searchProgressReset()
{
    uint64_t cnt = parent->formGen48->estimateSeedCnt();
    if (cnt > MASK48)
        cnt = ~(uint64_t)0;
    else
        cnt <<= 16;

    QString fmt;
    int searchtype = ui->comboSearchType->currentData().toInt();
    if (searchtype == SEARCH_LIST)
    {
        if (!slist64fnam.isEmpty())
        {
            fmt = slist64fnam + ": ";
            cnt = slist64.size();
        }
    }
    if (searchtype == SEARCH_INC)
    {
        if (smin != 0 || smax != ~(uint64_t)0)
        {
            fmt = QString::asprintf(" [%" PRIu64 " - %" PRIu64 "]: ", smin, smax);
            cnt = 0;
        }
    }

    if (cnt)
        fmt += QString::asprintf("0 / %" PRIu64 " (0.00%%)", cnt);
    else
        fmt += "0 / ? (0.00%%)";

    ui->progressBar->setValue(0);
    ui->progressBar->setFormat(fmt);

    if (parent)
        parent->setProgressIndication();
}

void FormSearchControl::updateSearchProgress(uint64_t prog, uint64_t end, int64_t seed)
{
    ui->lineStart->setText(QString::asprintf("%" PRId64, seed));

    if (!end)
        return;

    double value = (double)prog / end;
    int v = (int) floor(10000 * value);
    ui->progressBar->setValue(v);
    QString fmt = QString::asprintf(
                "%" PRIu64 " / %" PRIu64 " (%d.%02d%%)",
                prog, end, v / 100, v % 100
                );
    int searchtype = ui->comboSearchType->currentData().toInt();
    if (searchtype == SEARCH_LIST)
    {
        if (!slist64fnam.isEmpty())
        {
            fmt = slist64fnam + ": " + fmt;
        }
    }
    if (searchtype == SEARCH_INC)
    {
        if (smin != 0 || smax != ~(uint64_t)0)
        {
            fmt = QString::asprintf("[%" PRIu64 " - %" PRIu64 "]: ", smin, smax) + fmt;
        }
    }
    ui->progressBar->setFormat(fmt);

    if (parent)
        parent->setProgressIndication(value);
}

void FormSearchControl::searchFinish(bool done)
{
    if (tuningActive)
        return;
    stimer.stop();
    onBufferTimeout();
    progressTimeout();
    if (done)
    {
        ui->lineStart->setText(QString::asprintf("%" PRId64, sthread.smax));
        ui->progressBar->setValue(10000);
        ui->progressBar->setFormat(tr("Done", "Progressbar"));
    }
    ui->labelStatus->setText(tr("Idle", "Progressbar"));
    searchLockUi(false);

    if (parent)
        parent->setProgressIndication();
}

void FormSearchControl::progressTimeout()
{
    uint64_t prog, end, seed;
    qreal min, avg, max;
    QString status = tr("Running...", "Progressbar");
    if (!sthread.getProgress(&status, &prog, &end, &seed, &min, &avg, &max))
        return;

    updateSearchProgress(prog, end, seed);

    ui->labelStatus->setText(status);

    update();
}

void FormSearchControl::removeCurrent()
{
    QModelIndex index = ui->results->currentIndex();
    int row = proxy->mapToSource(index).row();
    if (row >= 0)
    {
        model->removeRow(row);
    }
}

void FormSearchControl::copySeed()
{
    uint64_t seed;
    if (getSeed(ui->results->currentIndex().row(), &seed))
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::asprintf("%" PRId64, seed));
    }
}

void FormSearchControl::copyResults()
{
    QString text;
    int n = ui->results->model()->rowCount();
    for (int i = 0; i < n; i++)
    {
        uint64_t seed;
        if (getSeed(i, &seed))
            text += QString::asprintf("%" PRId64 "\n", seed);
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
}

void FormSearchControl::keyReleaseEvent(QKeyEvent *event)
{
    if (ui->results->hasFocus())
    {
        if (event->matches(QKeySequence::Delete))
            removeCurrent();
        else if (event->matches(QKeySequence::Copy))
            copySeed();
        else if ((event->modifiers() & Qt::CTRL) && (event->modifiers() & Qt::SHIFT) && event->key() == Qt::Key_C)
            copyResults();
        else if (event->matches(QKeySequence::Paste))
            pasteResults();
    }
    QWidget::keyReleaseEvent(event);
}
