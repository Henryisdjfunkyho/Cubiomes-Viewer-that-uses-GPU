#include "searchthread.h"

#include "aboutdialog.h"
#include "formsearchcontrol.h"
#include "message.h"
#include "seedtables.h"
#include "searchrateformat.h"

#include "cubiomes/quadbase.h"
#include "cubiomes/util.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QDirIterator>
#include <QVector>


void Session::writeHeader(QTextStream& stream)
{
    stream << "#Version:  " << VERS_MAJOR << "." << VERS_MINOR << "." << VERS_PATCH << "\n";
    stream << "#Time:     " << QDateTime::currentDateTime().toString() << "\n";
    // MC version of the session should take priority over the one in the settings
    wi.write(stream);

    sc.write(stream);
    gen48.write(stream);

    for (Condition &c : cv)
        stream << "#Cond: " << c.toHex() << "\n";
    stream.flush();
}

bool Session::save(QWidget *widget, QTextStream& stream)
{
    (void) widget;
    writeHeader(stream);
    for (uint64_t s : slist)
        stream << QString::asprintf("%" PRId64 "\n", (int64_t)s);
    stream.flush();
    return true;
}

bool Session::load(QWidget *widget, QTextStream& stream, bool quiet)
{
    int major = 0, minor = 0, patch = 0;
    QString line;
    line = stream.readLine();
    int lno = 1;

    if (sscanf(line.toLocal8Bit().data(), "#Version: %d.%d.%d", &major, &minor, &patch) != 3)
    {
        if (quiet)
            return false;
        int button = warn(widget, QApplication::tr("Warning"),
            QApplication::tr("File does not look like a session file.\nProgress may be incomplete or broken."),
            QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
        if (button != QMessageBox::Yes)
            return false;
    }
    else if (cmpVers(major, minor, patch) > 0)
    {
        if (quiet)
            return false;
        int button = warn(widget, QApplication::tr("Warning"),
            QApplication::tr("Session file was created with a newer version.\nProgress may be incomplete or broken."),
            QApplication::tr("Continue loading progress anyway?"), QMessageBox::Abort | QMessageBox::Yes);
        if (button != QMessageBox::Yes)
            return false;
    }

    while (stream.status() == QTextStream::Ok && !stream.atEnd())
    {
        lno++;
        line = stream.readLine();

        if (line.isEmpty()) continue;
        if (line.startsWith("#Time:")) continue;
        if (line.startsWith("#Title:")) continue;
        if (line.startsWith("#Desc:")) continue;
        if (sc.read(line)) continue;
        if (gen48.read(line)) continue;
        if (wi.read(line)) continue;

        if (line.startsWith("#Cond:"))
        {   // Conditions
            Condition c;
            if (c.readHex(line.mid(6).trimmed()))
            {
                cv.push_back(c);
            }
            else
            {
                if (quiet)
                    return false;
                int button = warn(widget, QApplication::tr("Warning"),
                    QApplication::tr("Condition [%1] at line %2 is not supported.").arg(c.save).arg(lno),
                    QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
                if (button != QMessageBox::Yes)
                    return false;
            }
        }
        else
        {   // Seeds
            QByteArray ba = line.toLocal8Bit();
            const char *p = ba.data();
            uint64_t s;
            if (sscanf(p, "%" PRId64, (int64_t*)&s) == 1)
            {
                slist.push_back(s);
            }
            else
            {
                if (quiet)
                    return false;
                int button = warn(widget, QApplication::tr("Warning"),
                    QApplication::tr("Failed to parse line %1 of file:\n%2").arg(lno).arg(line),
                    QApplication::tr("Continue anyway?"), QMessageBox::Abort | QMessageBox::Yes);
                if (button != QMessageBox::Yes)
                    return false;
            }
        }
    }
    return true;
}


SearchMaster::SearchMaster(QWidget *parent)
    : QObject(parent)
    , mutex()
    , stop()
    , proghist()
    , progtimer()
    , itemtimer()
    , count()
    , env()
    , searchtype()
    , mc()
    , large()
    , condtree()
    , itemsize()
    , threadcnt()
    , gen48()
    , slist()
    , idx()
    , scnt()
    , prog()
    , seed()
    , smin()
    , smax()
    , isdone()
{
    env.stop = &stop;
}

SearchMaster::~SearchMaster()
{
    stopSearch();
}

bool SearchMaster::set(QWidget *widget, const Session& s)
{
    char refbuf[100] = {};
    char disabled[100] = {};

    for (const Condition& c : s.cv)
        if (c.meta & Condition::DISABLED)
            disabled[c.save] = 1;

    for (const Condition& c : s.cv)
    {
        char cid[8];
        snprintf(cid, sizeof(cid), "[%02d]", c.save);
        if (c.save < 1 || c.save > 99)
        {
            warn(widget, tr("Condition with invalid ID %1.").arg(cid));
            return false;
        }
        if (c.type < 0 || c.type >= FILTER_MAX)
        {
            warn(widget, tr("Encountered invalid filter type %1 in condition ID %2.").arg(c.type).arg(cid));
            return false;
        }
        if (disabled[c.save])
            continue;

        const FilterInfo& finfo = g_filterinfo.list[c.type];

        if (c.relative && (refbuf[c.relative] == 0 || disabled[c.relative]))
        {
            warn(widget, tr("Condition with ID %1 has a broken reference position:\n"
                            "condition missing or out of order.").arg(cid));
            return false;
        }
        if (++refbuf[c.save] > 1)
        {
            warn(widget, tr("More than one condition with ID %1.").arg(cid));
            return false;
        }
        if (s.wi.mc < finfo.mcmin)
        {
            const char *mcs = mc2str(finfo.mcmin);
            warn(widget, tr("Condition %1 requires a minimum Minecraft version of %2.").arg(cid, mcs));
            return false;
        }
        if (s.wi.mc > finfo.mcmax)
        {
            const char *mcs = mc2str(finfo.mcmax);
            warn(widget, tr("Condition %1 not available for Minecraft versions above %2.").arg(cid, mcs));
            return false;
        }
        if (c.type == F_BIOME ||
            c.type == F_BIOME_4_RIVER ||
            c.type == F_BIOME_256_OTEMP ||
            c.type == F_BIOME_SAMPLE
        ){
            uint64_t b = c.biomeToFind;
            uint64_t m = c.biomeToFindM;
            if ((c.biomeToExcl & b) || (c.biomeToExclM & m))
            {
                warn(widget, tr("Biome condition with ID %1 has contradicting flags for include and exclude.").arg(cid));
                return false;
            }
            if ((b | m | c.biomeToExcl | c.biomeToExclM) == 0)
            {
                warn(widget, tr("Biome condition with ID %1 specifies no biomes.").arg(cid));
                return false;
            }

            int layerId = 0;
            int scale = c.step;
            if (c.type == F_BIOME_4_RIVER)
            {
                layerId = L_RIVER_4;
                scale = 4;
            }
            else if (c.type == F_BIOME_256_OTEMP)
            {
                layerId = L_OCEAN_TEMP_256;
                scale = 256;
            }
            if (layerId == 0 && s.wi.mc <= MC_1_17)
            {
                Generator tmp;
                setupGenerator(&tmp, s.wi.mc, 0);
                const Layer *l = getLayerForScale(&tmp, scale ? scale : 4);
                if (l)
                    layerId = l - tmp.ls.layers;
            }
            uint64_t ab, am;
            uint32_t flags = 0;
            getAvailableBiomes(&ab, &am, layerId, s.wi.mc, flags);
            b ^= (ab & b);
            m ^= (am & m);
            if (b || m)
            {
                int cnt = __builtin_popcountll(b) + __builtin_popcountll(m);
                QString msg = tr("Biome condition with ID %1 includes %n biome(s) "
                                 "that do not generate in MC %2.", "", cnt);
                warn(widget, msg.arg(cid, mc2str(s.wi.mc)));
                return false;
            }
        }
        if (c.type == F_TEMPS)
        {
            int w = (c.x2 >> 10) - (c.x1 >> 10) + 1;
            int h = (c.z2 >> 10) - (c.z1 >> 10) + 1;
            if (c.count > w * h)
            {
                QString msg = tr("Temperature category condition with ID %1 has too "
                                 "many restrictions (%2) for the area (%3 x %4 @ scale 1:1024).");
                warn(widget, msg.arg(cid).arg(c.count).arg(w).arg(h));
                return false;
            }
        }
        if (finfo.cat == CAT_STRUCT)
        {
            if (c.count >= 128)
            {
                warn(widget, tr("Structure condition %1 checks for too many instances (>= 128).").arg(cid));
                return false;
            }
        }
        if (c.skipref && c.rmax == 0 && c.x1 == 0 && c.x2 == 0 && c.z1 == 0 && c.z2 == 0)
        {
            warn(widget, tr("Condition %1 ignores its only location of size 1.").arg(cid));
            return false;
        }
    }

    QString err = condtree.set(s.cv, s.wi.mc);
    if (err.isEmpty())
    {
        err = env.init(s.wi.mc, s.wi.large, condtree);
    }
    if (!err.isEmpty())
    {
        warn(widget, tr("Failed to setup search environment:\n%1").arg(err));
        return false;
    }

    this->searchtype = s.sc.searchtype;
    this->mc = s.wi.mc;
    this->large = s.wi.large;
    this->itemsize = 1;
    this->threadcnt = s.sc.threads;
    this->slist = s.slist;
    this->gen48 = s.gen48;
    this->idx = 0;
    this->scnt = ~(uint64_t)0;
    this->prog = 0;
    this->seed = s.sc.startseed;
    this->smin = s.sc.smin;
    this->smax = s.sc.smax;
    this->isdone = false;
    this->stop = false;
    return true;
}

static void genQHBases(int qual, uint64_t salt, std::vector<uint64_t>& list48)
{
    int cst_type = 0;
    switch (qual)
    {
    case IDEAL_SALTED:
    case IDEAL:   cst_type = CST_IDEAL; break;
    case CLASSIC: cst_type = CST_CLASSIC; break;
    case NORMAL:  cst_type = CST_NORMAL; break;
    case BARELY:  cst_type = CST_BARELY; break;
    }

    QDirIterator it(":/qh");
    while (it.hasNext())
    {
        QString fnam = it.next();
        printf("> %s\n", fnam.toLocal8Bit().data());
        QFile file(fnam);
        uint64_t low = it.fileInfo().baseName().toUInt(nullptr, 16);
        uint64_t mid = 0;
        if (getQuadHutCst(low) > cst_type)
            continue;

        file.open(QIODevice::ReadOnly);
        QTextStream stream(&file);

        while (!stream.atEnd())
        {
            QString line = stream.readLine();
            uint64_t diff = line.toULongLong(nullptr, 16);
            if (diff == 0)
                break;
            mid += diff;
            uint64_t s48 = (mid << 20) + low;
            list48.push_back(s48 - salt);
        }
    }
}

static bool applyTranspose(std::vector<uint64_t>& slist,
        const Gen48Config& gen48, uint64_t bufmax)
{
    std::vector<uint64_t> list48;

    int x = gen48.x1;
    int z = gen48.z1;
    int w = gen48.x2 - x + 1;
    int h = gen48.z2 - z + 1;

    // does the set of candidates for this condition fit in memory?
    if ((uint64_t)slist.size() * sizeof(int64_t) * w*h >= bufmax)
    {
        slist.clear();
        return false;
    }

    try {
        list48.resize(slist.size() * w*h);
    } catch (...) {
        slist.clear();
        return false;
    }

    uint64_t *p = list48.data();
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            for (uint64_t b : slist)
                *p++ = moveStructure(b, x+i, z+j);

    std::sort(list48.begin(), list48.end());
    auto last = std::unique(list48.begin(), list48.end());
    list48.erase(last, list48.end());
    slist.swap(list48);
    return !slist.empty();
}

void SearchMaster::preSearch()
{
    uint64_t sstart = seed;

    if (gen48.mode == GEN48_AUTO)
    {   // resolve automatic mode
        for (const Condition& c : condtree.condvec)
        {
            if (c.type >= F_QH_IDEAL && c.type <= F_QH_BARELY)
            {
                gen48.mode = GEN48_QH;
                break;
            }
            else if (c.type >= F_QM_95 && c.type <= F_QM_90)
            {
                gen48.mode = GEN48_QM;
                break;
            }
        }
    }

    if (searchtype != SEARCH_LIST)
    {
        if (gen48.mode == GEN48_QH)
        {
            uint64_t salt = 0;
            if (gen48.qual == IDEAL_SALTED)
                salt = gen48.salt;
            else
            {
                StructureConfig sconf;
                getStructureConfig(Swamp_Hut, mc, &sconf);
                salt = sconf.salt;
            }
            slist.clear();
            genQHBases(gen48.qual, salt, slist);
        }
        else if (gen48.mode == GEN48_QM)
        {
            StructureConfig sconf;
            getStructureConfig(Monument, mc, &sconf);
            slist.clear();
            for (const uint64_t *s = g_qm_90; *s; s++)
                if (qmonumentQual(*s) >= gen48.qmarea)
                    slist.push_back((*s - sconf.salt) & MASK48);
        }
        else if (gen48.mode == GEN48_LIST)
        {
            if (gen48.listsalt)
            {
                for (uint64_t& rs : slist)
                    rs += gen48.listsalt;
            }
        }

        if (!slist.empty())
            applyTranspose(slist, gen48, PRECOMPUTE48_BUFSIZ);
    }

    if (searchtype == SEARCH_LIST)
    {
        if (!slist.empty())
        {   // 64-bit seed list
            scnt = slist.size();
            for (idx = 0; idx < scnt; idx++)
                if (slist[idx] == sstart)
                    break;
            if (idx == scnt)
                idx = 0;
            seed = slist[idx];
            smax = slist.back();
            prog = idx;
        }
        else
        {   // slist should not be empty for a meaningful list search
            scnt = smax = ~(uint64_t)0;
            prog = seed = sstart;
            idx = 0;
        }
    }

    if (searchtype == SEARCH_48ONLY)
    {
        if (!slist.empty())
        {   // 48-bit seed list
            scnt = slist.size();
            for (idx = 0; idx < scnt; idx++)
                if (slist[idx] == sstart)
                    break;
            if (idx == scnt)
                idx = 0;
            seed = slist[idx];
            smax = slist.back();
            prog = idx;
        }
        else
        {
            prog = seed = sstart;
            smax = MASK48;
            scnt = MASK48 + 1;
            if (seed > smax)
                isdone = true;
        }
    }

    if (searchtype == SEARCH_INC)
    {   // smin & smax are given by user
        if (!slist.empty())
        {   // incremental search with a 48-bit list (incl. quad-searches)
            seed = sstart;
            if (seed < smin)
                seed = smin;
            scnt = 0x10000 * slist.size();
            uint64_t high = (seed >> 48) & 0xffff;
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (seed & MASK48))
                    break;
            if (idx == slist.size())
            {
                if (high++ >= (smax >> 48))
                    isdone = true;
                idx = 0;
            }
            seed = (high << 48) | slist[idx];
            // trim the search space to the range [smin, smax]
            uint64_t idxmin, idxmax;
            for (idxmin = 0; idxmin < slist.size(); idxmin++)
                if (slist[idxmin] >= (smin & MASK48))
                    break;
            for (idxmax = 0; idxmax < slist.size(); idxmax++)
                if (slist[idxmax] > (smax & MASK48))
                    break;
            high = (high - (smin >> 48)) & 0xffff;
            prog = high * slist.size() + idx - idxmin;
            high = ((smax >> 48) - (smin >> 48)) & 0xffff;
            scnt = high * slist.size() + idxmax - idxmin;
            if(!scnt || prog>=scnt)isdone=true;
        }
        else
        {   // simple incremental search
            seed = sstart;
            if (seed < smin)
                seed = smin;
            prog = seed - smin;
            scnt = smax - smin;
            if(scnt != UINT64_MAX)scnt++; // inclusive finite range
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            scnt = 0x10000 * slist.size();
            for (idx = 0; idx < slist.size(); idx++)
                if (slist[idx] >= (sstart & MASK48))
                    break;
            if (idx == slist.size())
                isdone = true;
            else
            {
                seed = (sstart & ~MASK48) | slist[idx];
                prog = 0x10000 * idx + (seed >> 48);
            }
            smax = slist.back() | (0xffffULL << 48);
        }
        else
        {
            scnt = smax = ~(uint64_t)0;
            seed = sstart;
            prog = (seed << 16) | (seed >> 48);
        }
    }
}

void SearchMaster::startSearch()
{
    stopSearch();
    stop = false;
    preSearch();

    if (hasResumeCursor)
    {
        seed = resumeCursor.seed;
        idx = resumeCursor.idx;
        prog = resumeCursor.prog;
        isdone = resumeCursor.done;
        hasResumeCursor = false;
    }
    completedTotal = prog;

    if (isdone)
    {
        emit searchFinish(true);
        return;
    }

    if (stop)
    {
        emit searchFinish(false);
        return;
    }

    const unsigned searchBatchCeiling = searchtype == SEARCH_BLOCKS ? 4096U :
        (searchtype == SEARCH_48ONLY ? 65536U :
        (searchtype == SEARCH_LIST || !slist.empty() ? CVHIP_MAX_LIST_BATCH : CVHIP_MAX_BATCH));
    const unsigned requestedHipBatch = hipBatchLimitOverride
        ? std::min(hipBatchLimitOverride, searchBatchCeiling) : searchBatchCeiling;
    hip.configure(condtree, mc, large, useHip && (searchtype != SEARCH_LIST || !slist.empty()),
        searchtype != SEARCH_48ONLY, true, searchtype == SEARCH_LIST || !slist.empty(),
        requestedHipBatch, hipInflightOverride, hipAdaptiveSafety);
    if (hip.active()) itemsize = searchtype == SEARCH_BLOCKS ? 4096 :
        (searchtype == SEARCH_LIST || !slist.empty() ? CVHIP_MAX_LIST_BATCH : CVHIP_MAX_BATCH);
    if(hip.active()&&searchtype==SEARCH_48ONLY)itemsize=65536;
    if(hip.active())itemsize=std::min(itemsize,int(hip.batchLimit()));

    for (int i = 0; i < threadcnt; i++)
    {
        SearchWorker *worker = new SearchWorker(this);
        QObject::connect(
            worker, &SearchWorker::result,
            this, &SearchMaster::onWorkerResult,
            Qt::BlockingQueuedConnection);
        QObject::connect(
            worker, &SearchWorker::finished,
            this, &SearchMaster::onWorkerFinished,
            Qt::QueuedConnection);

        workers.push_back(worker);
    }

    QMutexLocker locker(&mutex);

    proghist.clear();
    progtimer.start();
    itemtimer.start();
    count = 0;

    for (SearchWorker *worker : workers)
    {
        worker->start();
    }
}

void SearchMaster::stopSearch()
{
    if(stoppingSearch){stop=true;hip.cancel();return;}
    stop = true;
    hip.cancel();
    if (workers.empty())
        return;
    stoppingSearch = true;

    // Workers must finish before a new condition tree/program is installed.
    // Process queued results so BlockingQueuedConnection cannot deadlock stop.
    for (;;)
    {
        bool busy = false;
        for (SearchWorker *worker : workers)
            busy |= worker->isRunning();
        if (!busy)
            break;
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        QThread::msleep(10);
    }

    // Preserve the actually-completed count before releasing worker records.
    // This lets the tuner include final drained work without counting an
    // entire scheduled item merely because its worker was stopped.
    uint64_t completed = prog;
    for (SearchWorker *worker : workers)
    {
        const unsigned done = std::min(worker->completedInItem.load(),
            unsigned(std::max(worker->scnt, 0)));
        const unsigned pending = unsigned(std::max(worker->scnt, 0)) - done;
        completed = completed >= pending ? completed - pending : 0;
    }
    completedTotal = completed;

    for (SearchWorker *worker : workers)
    {
        worker->disconnect(this);
        if (worker->isRunning())
            connect(worker, &SearchWorker::finished, worker, &QObject::deleteLater);
        else
            delete worker;
    }
    workers.clear();
    stoppingSearch = false;

    emit searchFinish(false);
}


static QString getAbbrNum(double x)
{
    return SearchRateFormat::format(x);
}

bool SearchMaster::getProgress(QString *status, uint64_t *prog, uint64_t *end, uint64_t *seed, qreal *min, qreal *avg, qreal *max)
{
    if (!mutex.tryLock(10))
    {
        if (searchtype == SEARCH_BLOCKS && slist.empty())
        {   // a block search with no list looks for candidates in the search
            // master and can therefore make progress outside of workers
            *prog = this->prog;
            *end  = this->scnt;
            *seed = this->seed;
            return true;
        }
        return false;
    }
    *prog = this->prog;
    *end  = this->scnt;
    *seed = this->seed;
    *min = *avg = *max = nan("");

    bool valid = false;
    uint64_t completed = this->prog;
    for (SearchWorker *worker: workers)
    {
        unsigned done = std::min(worker->completedInItem.load(), unsigned(std::max(worker->scnt,0)));
        unsigned pending = unsigned(std::max(worker->scnt,0))-done;
        completed = completed >= pending ? completed-pending : 0;
        uint64_t safe = worker->prog + done;
        if (pending && safe < *prog)
        {
            *prog = safe;
            if(searchtype==SEARCH_LIST || (searchtype==SEARCH_48ONLY&&!slist.empty())) *seed=slist[worker->idx+done];
            else if(searchtype==SEARCH_BLOCKS) *seed=(((worker->sstart>>48)+done)<<48)|(worker->sstart&MASK48);
            else if(!slist.empty()) {
                uint64_t position=worker->idx+done;
                *seed=(((worker->sstart>>48)+position/slist.size())<<48)|slist[position%slist.size()];
            } else *seed=worker->sstart+done;
        }
        valid = true;
    }

    if (isdone && workers.empty())
    {
        *prog = this->scnt;
    }

    mutex.unlock();

    // track the progress over a few seconds so we can estimate the search speed
    enum { SAMPLE_SEC = 20 };
    if (valid)
    {
        TProg tp = { (uint64_t) progtimer.nsecsElapsed(), completed };
        proghist.push_front(tp);
        while (proghist.size() > 1 && proghist.back().ns < tp.ns - SAMPLE_SEC*1e9)
            proghist.pop_back();
    }

    if (proghist.size() > 1 && proghist.front().ns > proghist.back().ns)
    {
        std::vector<qreal> samples;
        samples.reserve(proghist.size());
        auto it_prev = proghist.begin();
        auto it = it_prev;
        while (++it != proghist.end())
        {
            qreal dp = it_prev->prog - it->prog;
            qreal dt = 1e-9 * (it_prev->ns - it->ns);
            if (dt > 0)
                samples.push_back(dp / dt);
            it_prev = it;
        }
        std::sort(samples.begin(), samples.end());
        qreal speedtot = 0;
        qreal weightot = 1e-6;
        int n = (int) samples.size();
        int r = (int) (n / SAMPLE_SEC);
        int has_zeros = 0;
        for (int i = -r; i <= r; i++)
        {
            int j = n/2 + i;
            if (j < 0 || j >= n)
                continue;
            has_zeros += samples[j] == 0;
            speedtot += samples[j];
            weightot += 1.0;
        }
        *min = samples[n*1/4]; // lower quartile
        *avg = speedtot / weightot; // median
        *max = samples[n*3/4]; // upper quartile
        if (*avg && has_zeros)
        {   // probably a slow sampling regime, use whole range for estimate
            speedtot = 0;
            for (qreal s : samples)
                speedtot += s;
            *avg = speedtot / n;
        }
    }

    qreal remain = ((qreal)*end - *prog) / (*avg + 1e-6);
    QString eta;
    if (remain >= 3600*24*1000)
        eta = "years";
    else if (remain > 0)
    {
        int s = (int) remain;
        if (s > 86400)
            eta = QString("%1d:%2").arg(s / 86400).arg((s % 86400) / 3600, 2, 10, QLatin1Char('0'));
        else if (s > 3600)
            eta = QString("%1h:%2").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
        else
            eta = QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    }
    *status = QString("seeds/sec: %1 min: %2 max: %3 isize: %4 eta: %5")
        .arg(getAbbrNum(*avg), -8)
        .arg(getAbbrNum(*min), -8)
        .arg(getAbbrNum(*max), -8)
        .arg(itemsize, -3)
        .arg(eta);
#ifdef CVHIP_CUDA_ONLY
    *status += hip.active() ? QString(" prefilter: CUDA GPU, %1 conditions (biome checks on CPU)").arg(hip.conditionCount()) :
        (useHip ? " prefilter: CPU (GPU BOOST fallback)" : " prefilter: CPU");
#else
    *status += hip.active() ? QString(" prefilter: GPU BOOST, %1 conditions").arg(hip.conditionCount()) :
        (useHip ? " prefilter: CPU (GPU BOOST fallback)" : " prefilter: CPU");
#endif

    return valid;
}

bool SearchMaster::requestItem(SearchWorker *item)
{
    if (isdone)
        return false;

    // QMutexLocker locker(&mutex);

    // check if we should adjust the item size
    uint64_t nsec = itemtimer.nsecsElapsed();
    count++;
    if (nsec > 0.1e9 && !hip.active())
    {
        if (count < 1e2 && itemsize > 1)
            itemsize /= 2;
        if (count > 1e3 && itemsize < 0x10000)
            itemsize *= 2;
        itemtimer.start();
        count = 0;
    }

    item->prog      = prog;
    item->idx       = idx;
    item->sstart    = seed;
    item->scnt      = itemsize;
    item->seed      = seed;
    item->completedInItem = 0;

    if (searchtype == SEARCH_INC && slist.empty() && smax >= seed &&
        smax - seed < uint64_t(itemsize - 1))
        item->scnt = int(smax - seed + 1);

    if (searchtype == SEARCH_BLOCKS)
        item->scnt = std::min(item->scnt, int(0x10000 - (seed >> 48)));
    if (searchtype == SEARCH_48ONLY && slist.empty())
        item->scnt = int(std::min<uint64_t>(item->scnt, MASK48 - seed + 1));
    if (searchtype == SEARCH_LIST || (searchtype == SEARCH_48ONLY && !slist.empty()))
        item->scnt = int(std::min<uint64_t>(item->scnt, scnt - idx));
    if(searchtype == SEARCH_INC && !slist.empty()) {
        uint64_t remaining=(0x10000-(seed>>48))*slist.size()-idx;
        remaining=std::min(remaining,scnt-prog);
        item->scnt=int(std::min<uint64_t>(item->scnt,remaining));
    }
    prog += item->scnt;

    if (searchtype == SEARCH_LIST)
    {
        if (idx + itemsize > scnt)
            item->scnt = scnt - idx;
        idx += item->scnt;
        if (idx >= scnt)
            isdone = true;
    }

    if (searchtype == SEARCH_48ONLY)
    {
        if (!slist.empty())
        {
            if (idx + itemsize > scnt)
                item->scnt = scnt - idx;
            idx += item->scnt;
            if (idx >= scnt)
                isdone = true;
        }
        else
        {
            seed += item->scnt;
            if (seed > MASK48)
                isdone = true;
        }
    }

    if (searchtype == SEARCH_INC)
    {
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            idx += item->scnt;
            high += idx / slist.size();
            idx %= slist.size();
            seed = (high << 48) | slist[idx];
            if (high > (smax >> 48))
                isdone = true;
        }
        else
        {
            // seed += itemsize; with overflow detection
            uint64_t s = seed + item->scnt;
            if (s < seed)
                isdone = true; // overflow
            seed = s;
        }
        if (seed > smax)
            isdone = true;
    }

    if (searchtype == SEARCH_BLOCKS)
    {
        if (!slist.empty())
        {
            uint64_t high = (seed >> 48) & 0xffff;
            high += item->scnt;
            if (high >= 0x10000)
            {
                high = 0;
                idx++;
            }
            if (idx >= slist.size())
                isdone = true;
            else
                seed = (high << 48) | slist[idx];
        }
        else
        {
            Pos origin = {0,0};
            uint64_t high = (seed >> 48) & 0xffff;
            uint64_t low = seed & MASK48;
            high += item->scnt;
            if (high >= 0x10000)
            {
                high = 0;
                low++;

                for (; low <= MASK48 && !stop; low++)
                {
                    env.setSeed(low);
                    if (testTreeAt(origin, &env, PASS_FAST_48, nullptr)
                        != COND_FAILED)
                    {
                        break;
                    }
                    // update progress for skipped block
                    seed = low;
                    prog += 0x10000;
                }
                if (low > MASK48)
                    isdone = true;
            }
            seed = (high << 48) | low;
        }
    }

    return true;
}

uint64_t SearchMaster::completedSeeds()
{
    QMutexLocker locker(&mutex);
    if (workers.empty())
        return completedTotal;
    uint64_t completed = prog;
    for (SearchWorker *worker : workers)
    {
        unsigned done = std::min(worker->completedInItem.load(), unsigned(std::max(worker->scnt, 0)));
        unsigned pending = unsigned(std::max(worker->scnt, 0)) - done;
        completed = completed >= pending ? completed - pending : 0;
    }
    return completed;
}

void SearchMaster::setResumeCursor(const SearchCursor &cursor)
{
    resumeCursor = cursor;
    hasResumeCursor = true;
}

bool SearchMaster::captureSafeCursor(SearchCursor *cursor)
{
    if (!cursor)
        return false;

    QMutexLocker locker(&mutex);
    if (workers.empty())
        return false;

    bool found = false;
    SearchCursor earliest;
    uint64_t earliestProg = UINT64_MAX;
    auto consider = [&](SearchCursor candidate, uint64_t order) {
        if (!found || order < earliestProg)
        {
            earliest = candidate;
            earliestProg = order;
            found = true;
        }
    };

    for (SearchWorker *worker : workers)
    {
        const uint64_t done = std::min<uint64_t>(worker->completedInItem.load(),
            uint64_t(std::max(worker->scnt, 0)));
        SearchCursor candidate;
        candidate.prog = worker->prog + done;
        if (candidate.prog < worker->prog)
            candidate.prog = UINT64_MAX;
        candidate.idx = worker->idx;
        const uint64_t order = candidate.prog;
        bool valid = true;

        if (searchtype == SEARCH_LIST ||
            (searchtype == SEARCH_48ONLY && !slist.empty()))
        {
            candidate.idx += done;
            if (candidate.idx >= slist.size())
                valid = false;
            else
                candidate.seed = slist[candidate.idx];
        }
        else if (searchtype == SEARCH_INC && !slist.empty())
        {
            candidate.idx += done;
            uint64_t high = (worker->sstart >> 48) & 0xffff;
            high += candidate.idx / slist.size();
            candidate.idx %= slist.size();
            if (high > 0xffff)
                valid = false;
            else
            {
                candidate.seed = (high << 48) | slist[candidate.idx];
                if (candidate.seed > smax)
                    valid = false;
            }
        }
        else if (searchtype == SEARCH_BLOCKS && !slist.empty())
        {
            uint64_t high = (worker->sstart >> 48) + done;
            if (high >= 0x10000)
            {
                high = 0;
                ++candidate.idx;
            }
            if (candidate.idx >= slist.size())
                valid = false;
            else
                candidate.seed = (high << 48) | slist[candidate.idx];
        }
        else if (searchtype == SEARCH_BLOCKS)
        {
            uint64_t high = (worker->sstart >> 48) + done;
            uint64_t low = worker->sstart & MASK48;
            if (high >= 0x10000)
            {
                high = 0;
                ++low;
            }
            if (low > MASK48)
                valid = false;
            else
                candidate.seed = (high << 48) | low;
        }
        else
        {
            if (done > UINT64_MAX - worker->sstart)
                valid = false;
            else
                candidate.seed = worker->sstart + done;
            if (searchtype == SEARCH_48ONLY && slist.empty() && candidate.seed > MASK48)
                valid = false;
            if (searchtype == SEARCH_INC && slist.empty() && candidate.seed > smax)
                valid = false;
        }

        if (valid)
            consider(candidate, order);
    }

    if (found)
    {
        *cursor = earliest;
        return true;
    }

    // Every assigned item has completed. Resume from the scheduler's next
    // position, which may be at the end of the finite search.
    cursor->seed = seed;
    cursor->idx = idx;
    cursor->prog = prog;
    cursor->done = isdone;
    return true;
}

void SearchMaster::onWorkerResult(uint64_t seed)
{
    emit searchResult(seed);
}

void SearchMaster::onWorkerFinished()
{
    QMutexLocker locker(&mutex);
    if (workers.empty() || stoppingSearch)
        return;
    for (SearchWorker *worker : workers)
        if (!worker->isFinished())
            return;
    uint64_t completed = prog;
    for (SearchWorker *worker : workers)
    {
        const unsigned done = std::min(worker->completedInItem.load(),
            unsigned(std::max(worker->scnt, 0)));
        const unsigned pending = unsigned(std::max(worker->scnt, 0)) - done;
        completed = completed >= pending ? completed - pending : 0;
    }
    completedTotal = completed;
    for (SearchWorker *worker: workers)
        delete worker;
    workers.clear();
    emit searchFinish(isdone && !stop);
}


SearchWorker::SearchWorker(SearchMaster *master)
    : QThread(nullptr)
    , master(master)
{
    this->slist         = master->slist.empty() ? NULL : master->slist.data();
    this->len           = master->slist.size();

    this->prog          = master->prog;
    this->idx           = master->idx;
    this->sstart        = master->seed;
    this->scnt          = 0;
    this->seed          = master->seed;

    this->env.stop      = &master->stop;
}

SearchWorker::~SearchWorker()
{
}

bool SearchWorker::getNextItem()
{
    QMutexLocker locker(&master->mutex);
    completedInItem = unsigned(std::max(scnt, 0));
    return master->requestItem(this);
}

void SearchWorker::run()
{
    if (!master->hip.active()) { runCpuLegacy(); return; }
    Pos origin = {0,0};
    env.init(master->mc, master->large, master->condtree);
    env.hipConditions = master->hip.conditionIds();
    while (!*env.stop && getNextItem()) {
        uint64_t high=sstart>>48,low=sstart&MASK48;
        env.hipVerifiedSeed=false;
        if(master->searchtype==SEARCH_BLOCKS) {
            if(slist) {if(idx>=len){completedInItem=scnt;continue;}low=slist[idx];}
            env.setSeed(low);
            if(testTreeAt(origin,&env,PASS_FULL_48,nullptr)==COND_FAILED) {completedInItem=scnt;continue;}
        }
        auto seedAt=[&](unsigned offset)->uint64_t {
            if(master->searchtype==SEARCH_LIST)return slist[idx+offset];
            if(master->searchtype==SEARCH_48ONLY)return slist?slist[idx+offset]:sstart+offset;
            if(master->searchtype==SEARCH_BLOCKS)return ((high+offset)<<48)|low;
            if(slist) {uint64_t position=idx+offset;return ((high+position/len)<<48)|slist[position%len];}
            return sstart+offset;
        };
        const bool contiguous=master->searchtype==SEARCH_INC&&!slist;
        for(unsigned base=0;base<unsigned(scnt)&&!*env.stop;) {
            // Runtime GPU safety may lower the maximum while this worker is
            // processing its current scheduler item. Read it at each dispatch.
            const unsigned limit=master->hip.active()?std::min(master->hip.batchLimit(),contiguous?CVHIP_MAX_BATCH:CVHIP_MAX_LIST_BATCH):65536;
            unsigned count=std::min(limit,unsigned(scnt)-base);
            std::vector<uint64_t> seeds;
            if(!contiguous) {seeds.resize(count);for(unsigned i=0;i<count;i++)seeds[i]=seedAt(base+i);}
            auto batch=master->hip.filterBatch(seedAt(base),count,seeds.empty()?nullptr:seeds.data());
            unsigned cursor=0,index=0,state=0;
            while(!*env.stop&&batch.next(cursor,index,state)) {
                // Rejected gaps are complete, but the current survivor is not.
                completedInItem=base+index;
                seed=seedAt(base+index);
                env.hipVerifiedSeed=master->searchtype!=SEARCH_48ONLY&&batch.encoding&&state==1;
                env.setSeed(seed);
                int pass=master->searchtype==SEARCH_48ONLY?PASS_FULL_48:PASS_FULL_64;
                int status=testTreeAt(origin,&env,pass,nullptr);
                if((pass==PASS_FULL_64?status==COND_OK:status!=COND_FAILED)&&!*env.stop)emit result(seed);
                completedInItem=base+index+1;
            }
            if(*env.stop)break;
            base+=count;completedInItem=base;
        }
    }
}

void SearchWorker::runCpuLegacy()
{
    Pos origin = {0,0};
    const bool trackProgress = master->trackCompletedSeeds;
    env.init(master->mc, master->large, master->condtree);
    env.hipConditions = master->hip.conditionIds();

    switch (master->searchtype)
    {
    case SEARCH_LIST:
        while (!*env.stop && getNextItem())
        {   // seed = slist[..]
            uint64_t ie = idx+scnt < len ? idx+scnt : len;
            for (uint64_t i = idx; i < ie; i++)
            {
                if (trackProgress && ((i - idx) & 4095) == 0)
                    completedInItem = unsigned(i - idx);
                seed = slist[i];
                env.setSeed(seed);
                if (testTreeAt(origin, &env, PASS_FULL_64, nullptr) == COND_OK)
                {
                    if (!*env.stop)
                        emit result(seed);
                }
            }
            //if (ie == len) // done
            //   break;
        }
        break;

    case SEARCH_48ONLY:
        while (!*env.stop && getNextItem())
        {
            if (slist)
            {
                uint64_t ie = idx+scnt < len ? idx+scnt : len;
                for (uint64_t i = idx; i < ie; i++)
                {
                    if (trackProgress && ((i - idx) & 4095) == 0)
                        completedInItem = unsigned(i - idx);
                    seed = slist[i];
                    env.setSeed(seed);
                    if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) != COND_FAILED)
                    {
                        if (!*env.stop)
                            emit result(seed);
                    }
                }
            }
            else
            {
                seed = sstart;
                for (int i = 0; i < scnt; i++)
                {
                    if (trackProgress && (i & 4095) == 0)
                        completedInItem = unsigned(i);
                    env.setSeed(seed);
                    if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) != COND_FAILED)
                    {
                        if (!*env.stop)
                            emit result(seed);
                    }

                    if (seed >= MASK48)
                    {   // done
                        break;
                    }
                    seed++;
                }
            }
        }
        break;

    case SEARCH_INC:
        while (!*env.stop && getNextItem())
        {
            if (slist)
            {   // seed = (high << 48) | slist[..]
                uint64_t high = (sstart >> 48) & 0xffff;
                uint64_t lowidx = idx;

                for (int i = 0; i < scnt; i++)
                {
                    if (trackProgress && (i & 4095) == 0)
                        completedInItem = unsigned(i);
                    seed = (high << 48) | slist[lowidx];

                    env.setSeed(seed);
                    if (testTreeAt(origin, &env, PASS_FULL_64, nullptr) == COND_OK)
                    {
                        if (!*env.stop)
                            emit result(seed);
                    }

                    if (++lowidx >= len)
                    {
                        lowidx = 0;
                        if (++high >= 0x10000)
                        {   // done
                            break;
                        }
                    }
                }
            }
            else
            {   // seed++
                seed = sstart;
                const auto mask = master->hip.active()
                    ? master->hip.filter(sstart, scnt) : std::vector<unsigned char>();
                for (int i = 0; i < scnt; i++)
                {
                    if (trackProgress && (i & 4095) == 0)
                        completedInItem = unsigned(i);
                    if (*env.stop) break;
                    if (!mask.empty() && !mask[i]) {
                        if (seed == ~(uint64_t)0) break;
                        seed++;
                        continue;
                    }
                    env.hipVerifiedSeed = !mask.empty() && mask[i] == 1 && master->hip.active();
                    env.setSeed(seed);
                    if (testTreeAt(origin, &env, PASS_FULL_64, nullptr) == COND_OK)
                    {
                        if (!*env.stop)
                            emit result(seed);
                    }

                    if (seed == ~(uint64_t)0)
                    {   // done
                        break;
                    }
                    seed++;
                }
            }
        }
        break;

    case SEARCH_BLOCKS:
        while (!*env.stop && getNextItem())
        {   // seed = ([..] << 48) | low
            if (slist && idx >= len)
            {
                continue;
            }

            uint64_t high = (sstart >> 48) & 0xffff;
            uint64_t low;
            if (slist)
                low = slist[idx];
            else
                low = sstart & MASK48;

            env.setSeed(low);
            if (testTreeAt(origin, &env, PASS_FULL_48, nullptr) == COND_FAILED)
            {
                continue;
            }

            for (int i = 0; i < scnt; i++)
            {
                if (trackProgress && (i & 4095) == 0)
                    completedInItem = unsigned(i);
                seed = (high << 48) | low;

                env.setSeed(seed);
                if (testTreeAt(origin, &env, PASS_FULL_64, nullptr) == COND_OK)
                {
                    if (!*env.stop)
                        emit result(seed);
                }

                if (++high >= 0x10000)
                    break; // done
            }
        }
        break;
    }
}



