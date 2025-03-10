#include "search.h"

#include "config.h"
#include "scripts.h"
#include "seedtables.h"
#include "util.h"

#include "cubiomes/finders.h"
#include "cubiomes/quadbase.h"

#include <QApplication>
#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

#define MULTIPLY_CHAR QChar(0xD7)

QString Condition::summary(bool aligntab) const
{
    const FilterInfo& ft = g_filterinfo.list[type];
    QString s;
    if (meta & Condition::DISABLED)
        s = QString("#%1#").arg(save, 2, 10, QChar('0'));
    else
        s = QString("[%1]").arg(save, 2, 10, QChar('0'));

    if (type == 0)
    {
        s += " " + QApplication::translate("Filter", "Conditions");
        return s;
    }

    QString cnts;
    if (ft.branch == FilterInfo::BR_CLUST)
        cnts += MULTIPLY_CHAR + QString::number(count);
    if (skipref)
        cnts += "*";
    QString txts = "";
    if (text[0])
    {
        QByteArray txta = QByteArray(text, sizeof(text));
        txta.resize(qstrlen(txta));
        txts = QString::fromLocal8Bit(txta);
    }
    else
    {
        txts = QApplication::translate("Filter", ft.name);
        if (type == F_LUA)
        {
            QMap<uint64_t, QString> scripts;
            getScripts(scripts);
            if (scripts.contains(hash))
                txts += ": " + QFileInfo(scripts.value(hash)).baseName();
            else
                txts += ": " + QApplication::translate("Filter", "[script missing]");
        }
        else if (step)
        {
            txts += QString(" 1:%1").arg(step);
        }
    }

    if (aligntab)
        s += QString(" %1\t%2").arg(txts).arg(cnts, -4);
    else
        s += QString(" %1%2").arg(txts, -26).arg(cnts, -4);
    if (relative)
        s += QString::asprintf("[%02d]+", relative);
    else
        s += "     ";

    if (rmax > 0)
    {
        s += QString::asprintf("r<%d", rmax-1);
    }
    else
    {
        if (ft.loc & FilterInfo::LOC_1)
            s += QString::asprintf("(%d,%d)", x1, z1);
        if (ft.loc & FilterInfo::LOC_2)
            s += QString::asprintf(",(%d,%d)", x2, z2);
    }
    return s;
}

bool Condition::versionUpgrade()
{
    if (version == VER_LEGACY)
    {
        uint64_t oceanToFind;
        memcpy(&oceanToFind, &biomeId, sizeof(oceanToFind));
        biomeToFind &= ~((1ULL << ocean) | (1ULL << deep_ocean));
        biomeToFind |= oceanToFind;
        skipref = 0;
        memset(pad0, 0, sizeof(pad0));
        memset(text, 0, sizeof(text));
        memset(pad1, 0, sizeof(pad1));
        hash = 0;
        memset(deps, 0, sizeof(deps));
        biomeId = biomeSize = tol = minmax = para = octave = step = 0;
    }
    if (version < VER_2_4_0)
    {
        varflags = varbiome = varstart = 0;
    }
    if (version < VER_3_4_0)
    {
        if (type == F_CLIMATE_MINMAX)
        {
            switch (minmax)
            {
            case 0: // min(<=) -> min <= x
                minmax = E_LOCATE_MIN | E_TEST_UPPER;
                std::swap(vmin, vmax);
                break;
            case 1: // max(>=) -> x <= max
                minmax = E_LOCATE_MAX | E_TEST_LOWER;
                break;
            case 2: // min(>=) -> x <= min
                minmax = E_LOCATE_MIN | E_TEST_LOWER;
                break;
            case 3: // max(<=) -> max <= x
                minmax = E_LOCATE_MAX | E_TEST_UPPER;
                std::swap(vmin, vmax);
                break;
            }
        }
    }
    if (version < VER_4_0_0)
    {
        const FilterInfo& ft = g_filterinfo.list[type];
        switch (type)
        {
        case F_SPIRAL:          step = 1;    break;
        case F_SPIRAL_4:        step = 4;    type = F_SPIRAL; break;
        case F_SPIRAL_16:       step = 16;   type = F_SPIRAL; break;
        case F_SPIRAL_64:       step = 64;   type = F_SPIRAL; break;
        case F_SPIRAL_256:      step = 256;  type = F_SPIRAL; break;
        case F_SPIRAL_512:      step = 512;  type = F_SPIRAL; break;
        case F_SPIRAL_1024:     step = 1024; type = F_SPIRAL; break;
        case F_BIOME:           step = 1;    break;
        case F_BIOME_4:         step = 4;    type = F_BIOME; break;
        case F_BIOME_16:        step = 16;   type = F_BIOME; break;
        case F_BIOME_64:        step = 64;   type = F_BIOME; break;
        case F_BIOME_256:       step = 256;  type = F_BIOME; break;
        case F_BIOME_NETHER:    step = 1;    break;
        case F_BIOME_NETHER_4:  step = 4;    type = F_BIOME_NETHER; break;
        case F_BIOME_NETHER_16: step = 16;   type = F_BIOME_NETHER; break;
        case F_BIOME_NETHER_64: step = 64;   type = F_BIOME_NETHER; break;
        case F_BIOME_NETHER_256:step = 256;  type = F_BIOME_NETHER; break;
        case F_BIOME_END:       step = 1;    break;
        case F_BIOME_END_4:     step = 4;    type = F_BIOME_END; break;
        case F_BIOME_END_16:    step = 16;   type = F_BIOME_END; break;
        case F_BIOME_END_64:    step = 64;   type = F_BIOME_END; break;
        }
        int mult = step ? step : ft.grid;
        if (mult > 1)
        {
            x1 = x1 * mult;
            z1 = z1 * mult;
            x2 = (x2+1) * mult - 1;
            z2 = (z2+1) * mult - 1;
        }
    }

    version = VER_CURRENT;
    return true;
}

QString Condition::apply(int mc)
{
    int in[256] = {}, inlen = 0, ex[256] = {}, exlen = 0;
    for (int i = 0; i < 64; i++)
    {
        if (biomeToFind & (1ULL << i))
            in[inlen++] = i;
        if (biomeToFindM & (1ULL << i))
            in[inlen++] = i + 128;
        if (biomeToExcl & (1ULL << i))
            ex[exlen++] = i;
        if (biomeToExclM & (1ULL << i))
            ex[exlen++] = i + 128;
    }
    uint32_t bfflags = BF_FORCED_OCEAN;
    if (flags & Condition::FLG_APPROX)
        bfflags |= BF_APPROX;
    if (flags & Condition::FLG_MATCH_ANY)
        setupBiomeFilter(&bf, mc, bfflags, 0, 0, ex, exlen, in, inlen);
    else
        setupBiomeFilter(&bf, mc, bfflags, in, inlen, ex, exlen, 0, 0);
    return "";
}

QString Condition::toHex() const
{
    size_t savsize = offsetof(Condition, generated_start);
    return QByteArray((const char*) this, savsize).toHex();
}

bool Condition::readHex(const QString& hex)
{
    if ((size_t)hex.length()/2 < offsetof(Condition, count))
        return false;
    QByteArray ba = QByteArray::fromHex(QByteArray(hex.toLocal8Bit().data()));
    size_t minsize = (size_t)ba.size();
    size_t savsize = offsetof(Condition, generated_start);
    if (savsize < minsize)
        minsize = savsize;
    memset(this, 0, sizeof(Condition));
    memcpy(this, ba.data(), minsize);
    bool ok = (size_t)ba.size() >= minsize &&
            save >= 0 && save < 100 &&
            type >= 0 && type < FILTER_MAX;
    if (ok)
        ok = versionUpgrade();
    return ok;
}

ConditionTree::~ConditionTree()
{
}

QString ConditionTree::set(const std::vector<Condition>& cv, int mc)
{
    int cmax = 0;
    for (const Condition& c : cv)
        if (!(c.meta & Condition::DISABLED) && c.save > cmax)
            cmax = c.save;
    condvec.clear();
    condvec.resize(cmax + 1);
    references.clear();
    references.resize(cmax + 1);
    for (const Condition& c : cv)
    {
        if (c.meta & Condition::DISABLED)
            continue;
        condvec[c.save] = c;
        QString err = condvec[c.save].apply(mc);
        if (!err.isEmpty())
            return err;
        if (c.relative <= cmax)
            references[c.relative].push_back(c.save);
    }
    return "";
}

SearchThreadEnv::SearchThreadEnv()
: condtree()
, mc()
, large()
, seed()
, surfdim(DIM_UNDEF)
, octaves()
, searchpass(PASS_FAST_48)
, stop()
, l_states()
{
    memset(&g, 0, sizeof(g));
    memset(&sn, 0, sizeof(sn));
}

SearchThreadEnv::~SearchThreadEnv()
{
    for (auto& it : l_states)
        lua_close(it.second);
}

QString SearchThreadEnv::init(int mc, bool large, const ConditionTree& condtree)
{
    this->condtree = condtree;
    this->mc = mc;
    this->large = large;
    this->seed = 0;
    this->surfdim = DIM_UNDEF;
    this->octaves = 0;
    uint32_t flags = 0;
    if (large)
        flags |= LARGE_BIOMES;
    setupGenerator(&g, mc, flags);

    QMap<uint64_t, QString> scripts;
    getScripts(scripts);
    for (auto& it : l_states)
        lua_close(it.second);
    l_states.clear();

    for (const Condition& c: condtree.condvec)
    {
        if (c.type != F_LUA)
            continue;
        if (!scripts.contains(c.hash))
            return QApplication::translate("Filter", "missing script for condition %1").arg(c.save);
        QString err;
        lua_State *L = loadScript(scripts.value(c.hash), &err);
        if (!L)
        {
            QString s = QApplication::translate("Filter", "Condition %1:\n").arg(c.save);
            s += err;
            return s;
        }
        l_states[c.hash] = L;
    }
    return "";
}

void SearchThreadEnv::setSeed(uint64_t seed)
{
    this->seed = seed;
    this->octaves = 0;
}

void SearchThreadEnv::init4Dim(int dim)
{
    uint64_t mask = (dim == DIM_OVERWORLD ? ~0ULL : MASK48);
    if (dim != g.dim || (seed & mask) != (g.seed & mask))
    {
        applySeed(&g, dim, seed);
        this->surfdim = DIM_UNDEF;
    }
    else if (g.mc >= MC_1_15 && seed != g.seed)
    {
        g.sha = getVoronoiSHA(seed);
    }
}

void SearchThreadEnv::init4Noise(int nptype, int octaves)
{
    if (octaves <= 0)
        octaves = INT_MAX;
    if (g.bn.nptype == nptype && this->octaves == octaves)
        return; // already initialized for parameter
    if (seed == g.seed && g.bn.nptype == -1)
        return; // fully initialized biome noise
    setClimateParaSeed(&g.bn, seed, large, nptype, octaves);
    this->octaves = octaves;
}

void SearchThreadEnv::prepareSurfaceNoise(int dim)
{
    if (surfdim != dim)
    {
        initSurfaceNoise(&sn, dim, seed);
        surfdim = dim;
    }
}

// The position buffers can exceed the stacksize on some platforms,
// and dynamic heap allocation is too slow. So instead, we assign a
// static memory region on the heap.
static Pos* getPosBuf(uint32_t node)
{
#if DEBUG
    if (node >= 100) return NULL; // fatal
#endif
    thread_local std::vector<Pos> buf(100 * MAX_INSTANCES);
    return &buf[node * MAX_INSTANCES];
}

static
int _testTreeAt(
    Pos                         at,             // relative origin
    SearchThreadEnv           * env,            // thread-local environment
    Pos                       * path,           // output center position(s)
    int                         node
)
{
    const ConditionTree *tree = &env->condtree;
    const Condition& c = tree->condvec[node];
    const std::vector<char>& branches = tree->references[c.save];
    int st, br;
    int rx1, rz1, rx2, rz2;
    Pos pos;
    Pos *inst = getPosBuf(node);

    switch (c.type)
    {
    case F_SPIRAL:

        st = COND_FAILED;
        {   // run a spiral iterator over the rectangle
            int step = c.step ? c.step : 512;
            int rmax, x1, z1, x2, z2;
            if (c.rmax > 0)
            {
                rmax = c.rmax - 1;
                x1 = at.x - rmax;
                z1 = at.z - rmax;
                x2 = at.x + rmax;
                z2 = at.z + rmax;
                rmax = rmax * rmax + 1;
            }
            else
            {
                rmax = 0;
                x1 = c.x1 + at.x;
                z1 = c.z1 + at.z;
                x2 = c.x2 + at.x;
                z2 = c.z2 + at.z;
            }

            rx1 = floordiv(x1, step);
            rz1 = floordiv(z1, step);
            rx2 = floordiv(x2, step);
            rz2 = floordiv(z2, step);

            int rx = (rx1 + rx2) >> 1;
            int rz = (rz1 + rz2) >> 1;
            int i = 0, dl = 1;
            int dx = 1, dz = 0;
            while (true)
            {
                bool inx = (rx >= rx1 && rx <= rx2);
                bool inz = (rz >= rz1 && rz <= rz2);
                if (!inx && !inz)
                    break;
                if (inx && inz)
                {
                    pos.x = rx * step;
                    pos.z = rz * step;

                    bool inr = true;
                    if (rmax)
                    {
                        int dx = pos.x - at.x;
                        int dz = pos.z - at.z;
                        int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                        inr = (rsq < rmax);
                    }
                    else if (pos.x < x1 || pos.x > x2 || pos.z < z1 || pos.z > z2)
                    {
                        inr = false;
                    }

                    if (inr)
                    {
                        // children are combined via AND at the current position
                        int sta = COND_OK;
                        for (int b : branches)
                        {
                            int stb = _testTreeAt(pos, env, path, b);
                            if (*env->stop)
                                return COND_FAILED;
                            if (stb < sta)
                                sta = stb;
                            if (sta == COND_FAILED)
                                break;
                        }
                        if (sta == COND_MAYBE_POS_VALID )
                            sta = COND_MAYBE_POS_INVAL; // position moves => invalidate
                        if (sta > st)
                            st = sta;
                        if (path && st >= COND_MAYBE_POS_VALID)
                            path[c.save] = pos;
                        if (st == COND_OK)
                            return COND_OK;
                    }
                }
                rx += dx;
                rz += dz;
                if (++i == dl)
                {
                    i = 0;
                    int tmp = dx;
                    dx = -dz;
                    dz = tmp;
                    if (dz == 0)
                        dl++;
                }
            }
        }
        return st;


    case F_SCALE_TO_NETHER:
        pos.x = at.x / 8;
        pos.z = at.z / 8;
        goto L_scaled_to_dim;
    case F_SCALE_TO_OVERWORLD:
        pos.x = at.x * 8;
        pos.z = at.z * 8;
        goto L_scaled_to_dim;

    L_scaled_to_dim:
        st = COND_OK;
        for (int b : branches)
        {
            int sta = _testTreeAt(pos, env, path, b);
            if (*env->stop)
                return COND_FAILED;
            if (sta < st)
                st = sta;
            if (st == COND_FAILED)
                break;
        }
        if (path && st >= COND_MAYBE_POS_VALID)
            path[c.save] = pos;
        return st;


    case F_LOGIC_OR:
        if (branches.empty())
        {
            if (path)
                path[c.save].x = path[c.save].z = -1;
            return COND_OK; // empty ORs are ignored
        }
        else
        {
            int b_ok = 0;
            st = COND_FAILED;
            for (int b : branches)
            {
                int sta = _testTreeAt(at, env, path, b);
                if (*env->stop)
                    return COND_FAILED;
                if (sta > st)
                    st = sta;
                if (st >= COND_MAYBE_POS_VALID)
                    b_ok = b;
                if (st == COND_OK)
                    break;
            }
            if (path && st >= COND_MAYBE_POS_VALID)
            {
                path[c.save] = at;
                for (int b : branches)
                {   // invalidate the other branches
                    if (b == b_ok)
                        continue;
                    Pos *p = path + tree->condvec[b].save;
                    p->x = p->z = -1;
                }
            }
        }
        return st;


    case F_LOGIC_NOT: //qDebug() << at.x << at.z;
        if (branches.empty())
            return COND_FAILED;
        st = COND_OK;
        for (int b : branches)
        {
            int sta = _testTreeAt(at, env, path, b);
            if (*env->stop)
                return COND_FAILED;
            if      (sta == COND_OK) { st = COND_FAILED; break; }
            else if (sta == COND_FAILED) { st = COND_OK; break; }
            else if (sta > st) st = sta;
        }
        return st;

    case F_LUA:
        st = COND_OK;
        if (lua_State *L = env->l_states[c.hash])
        {
            Pos *buf = path ? path : &inst[0];
            for (int b : branches)
            {
                int sta = _testTreeAt(at, env, buf, b);
                if (*env->stop)
                    return COND_FAILED;
                if (sta < st) {
                    st = sta;
                    if (st == COND_FAILED)
                        return st;
                }
            }
            if (st <= COND_MAYBE_POS_INVAL)
                return st;
            int sta = runCheckScript(L, at, env, env->searchpass, buf, &c);
            if (*env->stop)
                return COND_FAILED;
            if (sta < st)
                st = sta;
        }
        return st;

    default:

        if (branches.empty())
        {   // this is a leaf node => check only for presence of instances
            int icnt = c.count;
            st = testCondAt(at, env, &inst[0], &icnt, &c);
            if (path && st >= COND_MAYBE_POS_VALID)
            {
                if (icnt == 1)
                    path[c.save] = inst[0];
                else if (icnt > 1 && st == COND_OK)
                    path[c.save] = inst[0];
                else
                    path[c.save].x = path[c.save].z = -1;
            }
            return st;
        }

        br = g_filterinfo.list[c.type].branch;

        if (br == FilterInfo::BR_NONE || (br == FilterInfo::BR_CLUST && c.count != 1))
        {   // this condition cannot branch, position of multiple instances
            // will be averaged to a center point
            if (c.type == 0)
            {   // this is the root condition
                st = COND_OK;
                pos = at;
            }
            else
            {
                st = testCondAt(at, env, &inst[0], NULL, &c);
                if (st == COND_FAILED || st == COND_MAYBE_POS_INVAL)
                    return st;
                pos = inst[0]; // center point of instances
            }
            for (char b : branches)
            {
                if (st == COND_FAILED)
                    break;
                int sta = _testTreeAt(pos, env, path, b);
                if (*env->stop)
                    return COND_FAILED;
                if (sta < st)
                    st = sta;
            }
            if (path && st >= COND_MAYBE_POS_VALID)
                path[c.save] = pos;
            return st;
        }
        else
        {   // check each instance individually, splitting the instances into
            // independent subbranches that are combined via OR
            int icnt = MAX_INSTANCES;
            st = testCondAt(at, env, &inst[0], &icnt, &c);
            if (st == COND_FAILED || st == COND_MAYBE_POS_INVAL)
                return st;
            int sta = COND_FAILED;
            int iok = 0;
            for (int i = 0; i < icnt; i++) // OR instance subbranches
            {
                int stb = COND_OK;
                pos = inst[i];
                for (int b : branches) // AND dependent conditions
                {
                    int stc = _testTreeAt(pos, env, path, b);
                    if (*env->stop)
                        return COND_FAILED;
                    // worst branch dictates status for instance
                    if (stc < stb)
                        stb = stc;
                    if (stb == COND_FAILED)
                        break;
                }
                // best instance dictates status
                if (stb > sta) {
                    sta = stb;
                    if (sta >= COND_MAYBE_POS_VALID)
                        iok = i; // save position with ok path
                }
                if (sta >= st)
                    break; // status is as good as we need
            }
            // status cannot be better than it was for this condition
            if (sta < st)
                st = sta;
            if (path && st >= COND_MAYBE_POS_VALID)
                path[c.save] = inst[iok];
            return st;
        }
    }
}

int testTreeAt(
    Pos                         at,             // relative origin
    SearchThreadEnv           * env,            // thread-local environment
    int                         pass,           // search pass
    Pos                       * path            // ok trigger positions
)
{
    if (pass != PASS_FAST_48)
    {   // do a fast check before continuing with slower checks
        env->searchpass = PASS_FAST_48;
        int st = _testTreeAt(at, env, NULL, 0);
        if (st == COND_FAILED)
            return st;
    }
    env->searchpass = pass;
    return _testTreeAt(at, env, path, 0);
}


static const QuadInfo *getQHInfo(uint64_t cst)
{
    static std::map<uint64_t, QuadInfo> qh_info;
    static QMutex mutex;

    mutex.lock();
    if (qh_info.size() == 0)
    {
        StructureConfig sc;
        getStructureConfig(Swamp_Hut, MC_NEWEST, &sc);
        sc.salt = 0; // ignore version dependent salt offsets

        for (const uint64_t *cst = low20QuadHutBarely; *cst; cst++)
        {
            for (uint64_t s = *cst;; s += 0x100000)
            {
                // find a quad-hut for this constellation
                Pos pc;
                if (scanForQuads(sc, 128, s, low20QuadHutBarely, 20, 0, 0, 0, 1, 1, &pc, 1) < 1)
                    continue;
                qreal rad = isQuadBase(sc, s, 160);
                if (rad == 0)
                    continue;

                QuadInfo *qi = &qh_info[*cst];
                qi->rad = rad;
                qi->c = *cst;
                qi->p[0] = getFeaturePos(sc, s, 0, 0);
                qi->p[1] = getFeaturePos(sc, s, 0, 1);
                qi->p[2] = getFeaturePos(sc, s, 1, 0);
                qi->p[3] = getFeaturePos(sc, s, 1, 1);
                qi->afk = getOptimalAfk(qi->p, 7,7,9, &qi->spcnt);
                qi->typ = Swamp_Hut;

                switch (getQuadHutCst(*cst))
                {
                case CST_IDEAL:   qi->flt = F_QH_IDEAL;   break;
                case CST_CLASSIC: qi->flt = F_QH_CLASSIC; break;
                case CST_NORMAL:  qi->flt = F_QH_NORMAL;  break;
                default:          qi->flt = F_QH_BARELY;
                }
                break;
            }
        }
    }
    mutex.unlock();

    auto it = qh_info.find(cst);
    if (it == qh_info.end())
        return nullptr;
    else
        return &it->second;
}

static const QuadInfo *getQMInfo(uint64_t s48)
{
    static std::map<uint64_t, QuadInfo> qm_info;
    static QMutex mutex;

    mutex.lock();
    if (qm_info.size() == 0)
    {
        StructureConfig sc;
        getStructureConfig(Monument, MC_NEWEST, &sc);
        sc.salt = 0;

        for (const uint64_t *s = g_qm_90; *s; s++)
        {
            QuadInfo *qi = &qm_info[*s];
            qi->rad = isQuadBase(sc, *s, 160);
            qi->c = *s;
            qi->p[0] = getLargeStructurePos(sc, *s, 0, 0);
            qi->p[1] = getLargeStructurePos(sc, *s, 0, 1);
            qi->p[2] = getLargeStructurePos(sc, *s, 1, 0);
            qi->p[3] = getLargeStructurePos(sc, *s, 1, 1);
            qi->afk = getOptimalAfk(qi->p, 58,0/*23*/,58, &qi->spcnt);
            qi->afk.x -= 29;
            qi->afk.z -= 29;
            qi->typ = Monument;
        }
    }
    mutex.unlock();

    auto it = qm_info.find(s48);
    if (it == qm_info.end())
        return nullptr;
    else
        return &it->second;
}


static bool isVariantOk(const Condition *c, SearchThreadEnv *e, int stype, int varbiome, Pos *pos)
{
    StructureVariant sv;

    if (stype == Village)
    {
        if (e->mc < MC_1_10) return true;
        getVariant(&sv, stype, e->mc, e->seed, pos->x, pos->z, varbiome);
        if (c->varflags & Condition::VAR_ABANODONED)
        {
            if ((c->varflags & Condition::VAR_NOT) && sv.abandoned)
                return false;
            if (!(c->varflags & Condition::VAR_NOT) && !sv.abandoned)
                return false;
        }
        if (!(c->varflags & Condition::VAR_WITH_START) || e->mc < MC_1_14) return true;
    }
    else if (stype == Bastion)
    {
        if (e->mc <= MC_1_15) return true;
        getVariant(&sv, stype, e->mc, e->seed, pos->x, pos->z, -1);
        if (!(c->varflags & Condition::VAR_WITH_START)) return true;
    }
    else if (stype == Ruined_Portal || stype == Ruined_Portal_N)
    {
        if (e->mc <= MC_1_15) return true;
        e->init4Dim(stype == Ruined_Portal ? DIM_OVERWORLD : DIM_NETHER);
        varbiome = getBiomeAt(&e->g, 4, (pos->x >> 2) + 2, 0, (pos->z >> 2) + 2);
        getVariant(&sv, stype, e->mc, e->seed, pos->x, pos->z, varbiome);
        if (!(c->varflags & Condition::VAR_WITH_START)) return true;
    }
    else if (stype == Igloo)
    {
        if (!(c->varflags & Condition::VAR_BASEMENT)) return true;
        getVariant(&sv, stype, e->mc, e->seed, pos->x, pos->z, -1);
        return (c->varflags & Condition::VAR_NOT ? !sv.basement : sv.basement);
    }
    else if (stype == End_City)
    {
        if (!(c->varflags & Condition::VAR_ENDSHIP)) return true;
        Piece pieces[END_CITY_PIECES_MAX];
        int i, n = getEndCityPieces(pieces, e->seed, pos->x >> 4, pos->z >> 4);
        bool withship = !(c->varflags & Condition::VAR_NOT);
        for (i = 0; i < n; i++)
            if (pieces[i].type == END_SHIP)
                return withship;
        return !withship;
    }
    else if (stype == Fortress)
    {
        if (!(c->varflags & Condition::VAR_DENSE_BB)) return true;
        enum { FP_MAX = 400 };
        Piece p[FP_MAX];
        int i, n, b;
        n = getFortressPieces(p, FP_MAX, e->mc, e->seed, pos->x >> 4, pos->z >> 4);
        for (b = i = 0; i < n; i++)
            if (p[i].type == FORTRESS_START || p[i].type == BRIDGE_CROSSING)
                p[b++] = p[i];
        if (b < 4) return false;
        for (i = 0; i < b; i++)
        {
            int j, adj = 0;
            for (j = 0; j < b; j++)
            {
                if (p[i].bb0.y != p[j].bb0.y) continue;
                if (p[i].bb1.x != p[j].bb1.x && p[i].bb1.x+1 != p[j].bb0.x) continue;
                if (p[i].bb1.z != p[j].bb1.z && p[i].bb1.z+1 != p[j].bb0.z) continue;
                adj++;
            }
            if (adj >= 4)
            {
                pos->x = p[i].bb1.x;
                pos->z = p[i].bb1.z;
                return true;
            }
        }
        return false;
    }
    else
    {
        return true;
    }

    // check start piece
    uint64_t idxbits = c->varstart;
    while (idxbits)
    {
        int idx = __builtin_ctzll(idxbits);
        idxbits &= idxbits - 1;
        if ((size_t)idx < sizeof(g_start_pieces) / sizeof(g_start_pieces[0]))
        {
            const StartPiece *sp = g_start_pieces + idx;
            if (sp->stype == stype && sp->start == sv.start)
            {
                if (sp->biome != -1 && sp->biome != sv.biome)
                    continue;
                if (sp->giant != -1 && sp->giant != sv.giant)
                    continue;
                return true;
            }
        }
    }
    return false;
}

static bool intersectLineLine(double ax1, double az1, double ax2, double az2, double bx1, double bz1, double bx2, double bz2)
{
    double ax = ax2 - ax1, az = az2 - az1;
    double bx = bx2 - bx1, bz = bz2 - bz1;
    double adotb = ax * bz - az * bx;
    if (adotb == 0)
        return false; // parallel

    double cx = bx1 - ax1, cz = bz1 - az1;
    double t;
    t = (cx * az - cz * ax) / adotb;
    if (t < 0 || t > 1)
        return false;
    t = (cx * bz - cz * bx) / adotb;
    if (t < 0 || t > 1)
        return false;

    return true;
}

// does the line segment l1->l2 intersect the rectangle r1->r2
static bool intersectRectLine(double rx1, double rz1, double rx2, double rz2, double lx1, double lz1, double lx2, double lz2)
{
    if (lx1 >= rx1 && lx1 <= rx2 && lz1 >= rz1 && lz1 <= rz2) return true;
    if (lx2 >= rx1 && lx2 <= rx2 && lz2 >= rz1 && lz2 <= rz2) return true;
    if (intersectLineLine(lx1, lz1, lx2, lz2, rx1, rz1, rx1, rz2)) return true;
    if (intersectLineLine(lx1, lz1, lx2, lz2, rx1, rz2, rx2, rz2)) return true;
    if (intersectLineLine(lx1, lz1, lx2, lz2, rx2, rz2, rx2, rz1)) return true;
    if (intersectLineLine(lx1, lz1, lx2, lz2, rx2, rz1, rx1, rz1)) return true;
    return false;
}

static bool isInnerRingOk(int mc, uint64_t seed, int x1, int z1, int x2, int z2, int r1, int r2)
{
    StrongholdIter sh;
    Pos p = initFirstStronghold(&sh, mc, seed);

    if (p.x >= x1 && p.x <= x2 && p.z >= z1 && p.z <= z2)
        return true;
    // Do a ray cast analysis, checking if any of the generation angles intersect the area.
    double c, s;
    c = cos(sh.angle + M_PI*2/3);
    s = sin(sh.angle + M_PI*2/3);
    if (intersectRectLine(x1, z1, x2, z2, c*r1, s*r1, c*r2, s*r2))
        return true;
    c = cos(sh.angle + M_PI*4/3);
    s = sin(sh.angle + M_PI*4/3);
    if (intersectRectLine(x1, z1, x2, z2, c*r1, s*r1, c*r2, s*r2))
        return true;

    return false;
}

static int f_confine(void *data, int x, int z, double p)
{
    (void) x; (void) z;
    double *lim = (double*) data;
    return p < lim[0] || p > lim[1];
}

struct track_minmax_t
{
    Pos posmin, posmax;
    double vmin, vmax;
};
static int f_track_minmax(void *data, int x, int z, double p)
{
    track_minmax_t *info = (track_minmax_t *) data;
    if (p < info->vmin)
    {
        info->posmin.x = x;
        info->posmin.z = z;
        info->vmin = p;
    }
    if (p > info->vmax)
    {
        info->posmax.x = x;
        info->posmax.z = z;
        info->vmax = p;
    }
    return 0;
}


struct sample_boime_t
{
    const Condition *cond;
    Pos at;
    int rmaxsq;
    int n;
    int64_t xsum;
    int64_t zsum;
    Pos *cent;
    int *imax;
    std::atomic_bool *stop;
};

static int f_biome_sampler(Generator *g, int scale, int x, int y, int z, void *data)
{
    sample_boime_t *info = (sample_boime_t*) data;
    if (info->stop && *info->stop)
        return -2;
    if (info->rmaxsq)
    {
        int dx = (x * scale) - info->at.x;
        int dz = (z * scale) - info->at.z;
        int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
        if (rsq >= info->rmaxsq)
            return -1;
    }

    int id = getBiomeAt(g, scale, x, y, z);
    uint64_t incl = 0, excl = 0;
    if (id < 128) {
        incl = info->cond->biomeToFind & (1ULL << id);
        excl = info->cond->biomeToExcl & (1ULL << id);
    } else {
        incl = info->cond->biomeToFindM & (1ULL << (id-128));
        excl = info->cond->biomeToExclM & (1ULL << (id-128));
    }
    if (incl != 0)
    {
        x *= scale;
        z *= scale;
        if (info->imax && info->n < MAX_INSTANCES)
            info->cent[info->n] = Pos{x, z};
        info->xsum += x;
        info->zsum += z;
        info->n++;
        return 1;
    }
    if (excl != 0)
    {
        return 0;
    }
    return 0;
}

static int f_noise_sampler(Generator *g, int scale, int x, int y, int z, void *data)
{
    (void) y;
    sample_boime_t *info = (sample_boime_t*) data;
    if (info->stop && *info->stop)
        return -2;
    if (info->rmaxsq)
    {
        int dx = (x * scale) - info->at.x;
        int dz = (z * scale) - info->at.z;
        int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
        if (rsq >= info->rmaxsq)
            return -1;
    }

    const Condition *cond = info->cond;
    double v = sampleDoublePerlin(&g->bn.climate[cond->para], x, 0, z);
    double vmin = cond->minmax & Condition::E_TEST_LOWER ? cond->vmin : -INFINITY;
    double vmax = cond->minmax & Condition::E_TEST_UPPER ? cond->vmax : +INFINITY;

    v *= 10000;

    bool ok = (v > vmin && v < vmax);
    if (cond->flags & Condition::FLG_INVERT)
        ok = !ok;

    if (ok)
    {
        x *= scale;
        z *= scale;
        if (info->imax && info->n < MAX_INSTANCES)
            info->cent[info->n] = Pos{x, z};
        info->xsum += x;
        info->zsum += z;
        info->n++;
        return 1;
    }
    return 0;
}



/* Tests if a condition is satisfied with 'at' as origin for a search pass.
 * If sufficiently satisfied (check return value) then:
 * when 'imax' is NULL, the center position is written to 'cent[0]'
 * otherwise a maximum number of '*imax' instance positions are stored in 'cent'
 * and '*imax' is overwritten with the number of found instances.
 * ('*imax' should be at most MAX_INSTANCES)
 */
int
testCondAt(
    Pos                         at,             // relative origin
    SearchThreadEnv           * env,            // thread-local environment
    Pos                       * cent,           // output center position(s)
    int                       * imax,           // max instances (NULL for avg)
    const Condition           * cond            // condition to check
    )
{
    int x1, x2, z1, z2;
    int rx1, rx2, rz1, rz2, rx, rz;
    Pos pc;
    StructureConfig sconf;
    int qual, valid;
    int xt, zt;
    int st;
    int i, n, icnt;
    int64_t s, r, rmin, rmax;
    const uint64_t *seeds;
    Pos *p = getPosBuf(0);

    const FilterInfo& finfo = g_filterinfo.list[cond->type];

    if ((st = finfo.stype) > 0)
    {
        if (!getStructureConfig_override(finfo.stype, env->mc, &sconf))
            return COND_FAILED;
    }
    else memset(&sconf, 0, sizeof(sconf)); // never relevant, but clang-analyzer complains

    if (cond->rmax > 0)
    {
        rmax = cond->rmax - 1;
        x1 = at.x - rmax;
        z1 = at.z - rmax;
        x2 = at.x + rmax;
        z2 = at.z + rmax;
        rmax = rmax * rmax + 1;
    }
    else
    {
        rmax = 0;
        x1 = cond->x1 + at.x;
        z1 = cond->z1 + at.z;
        x2 = cond->x2 + at.x;
        z2 = cond->z2 + at.z;
    }

    switch (cond->type)
    {
    case F_SPIRAL:
    case F_SPIRAL_4:
    case F_SPIRAL_16:
    case F_SPIRAL_64:
    case F_SPIRAL_256:
    case F_SPIRAL_512:
    case F_SPIRAL_1024:
    case F_SCALE_TO_NETHER:
    case F_SCALE_TO_OVERWORLD:
    case F_LOGIC_OR:
    case F_LOGIC_NOT:
    case F_LUA:
        // helper conditions should not reach here
        //exit(1);
        return COND_OK;

    case F_QH_IDEAL:
        seeds = low20QuadIdeal;
        goto L_qh_any;
    case F_QH_CLASSIC:
        seeds = low20QuadClassic;
        goto L_qh_any;
    case F_QH_NORMAL:
        seeds = low20QuadHutNormal;
        goto L_qh_any;
    case F_QH_BARELY:
        seeds = low20QuadHutBarely;

L_qh_any:
        rx1 = x1 >> 9;
        rz1 = z1 >> 9;
        rx2 = x2 >> 9;
        rz2 = z2 >> 9;

        n = scanForQuads(
                sconf, 128, (env->seed) & MASK48, seeds, 20, sconf.salt,
                rx1, rz1, rx2 - rx1 + 1, rz2 - rz1 + 1, &p[0], MAX_INSTANCES);
        if (n < 1)
            return COND_FAILED;
        icnt = 0;
        for (i = 0; i < n; i++)
        {
            pc = p[i];
            s = moveStructure(env->seed, -pc.x, -pc.z);

            // find the constellation info
            const QuadInfo *qi = getQHInfo((s + sconf.salt) & 0xfffff);
            if (!qi || qi->flt > cond->type)
                continue;

            pc.x = (pc.x << 9) + qi->afk.x;
            pc.z = (pc.z << 9) + qi->afk.z;

            if (cond->skipref && pc.x == at.x && pc.z == at.z)
                continue;
            if (rmax)
            {
                int dx = pc.x - at.x;
                int dz = pc.z - at.z;
                int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                if (rsq >= rmax)
                    continue;
            }
            else if (pc.x < x1 || pc.x > x2 || pc.z < z1 || pc.z > z2)
            {
                continue;
            }
            // we don't support finding the center of multiple
            // quad huts, instead we just return the first one
            // (unless we are looking for all instances)
            cent[icnt++] = pc;
            if (imax && icnt >= *imax)
                return COND_OK;
            if (imax == NULL)
                break;
        }
        if (imax)
            *imax = icnt;
        if (icnt > 0)
            return COND_OK;
        return COND_FAILED;

    case F_QM_95:   qual = 58*58*4 * 95 / 100;  goto L_qm_any;
    case F_QM_90:   qual = 58*58*4 * 90 / 100;
L_qm_any:

        rx1 = x1 >> 9;
        rz1 = z1 >> 9;
        rx2 = x2 >> 9;
        rz2 = z2 >> 9;
        // we don't really need to check for more than one instance here
        n = scanForQuads(
                sconf, 160, (env->seed) & MASK48, g_qm_90, 48, sconf.salt,
                rx1, rz1, rx2 - rx1 + 1, rz2 - rz1 + 1, &p[0], 1);
        if (n < 1)
            return COND_FAILED;
        icnt = 0;
        for (i = 0; i < n; i++)
        {
            rx = p[i].x; rz = p[i].z;
            s = moveStructure(env->seed, -rx, -rz);
            if (qmonumentQual(s + sconf.salt) < qual)
                continue;
            const QuadInfo *qi = getQMInfo(s + sconf.salt);
            pc.x = (rx << 9) + qi->afk.x;
            pc.z = (rz << 9) + qi->afk.z;

            if (cond->skipref && pc.x == at.x && pc.z == at.z)
                continue;
            if (rmax)
            {
                int dx = pc.x - at.x;
                int dz = pc.z - at.z;
                int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                if (rsq >= rmax)
                    continue;
            }
            else if (pc.x < x1 || pc.x > x2 || pc.z < z1 || pc.z > z2)
            {
                continue;
            }

            cent[icnt++] = pc;
            if (imax && icnt >= *imax)
                return COND_OK;
            if (imax == NULL)
                break;
        }
        if (imax)
            *imax = icnt;
        if (icnt > 0)
            return COND_OK;
        return COND_FAILED;


    case F_DESERT:
    case F_HUT:
    case F_JUNGLE:
    case F_IGLOO:
    case F_MONUMENT:
    case F_VILLAGE:
    case F_OUTPOST:
    case F_MANSION:
    case F_RUINS:
    case F_SHIPWRECK:
    case F_TREASURE:
    case F_WELL:
    case F_PORTAL:
    case F_PORTALN:
    case F_ANCIENT_CITY:
    case F_TRAILS:

    case F_FORTRESS:
    case F_BASTION:

    case F_ENDCITY:
    case F_GATEWAY:

        if (sconf.regionSize == 32)
        {
            rx1 = x1 >> 9;
            rz1 = z1 >> 9;
            rx2 = x2 >> 9;
            rz2 = z2 >> 9;
        }
        else if (sconf.regionSize == 1)
        {
            rx1 = x1 >> 4;
            rz1 = z1 >> 4;
            rx2 = x2 >> 4;
            rz2 = z2 >> 4;
        }
        else
        {
            rx1 = floordiv(x1, sconf.regionSize << 4);
            rz1 = floordiv(z1, sconf.regionSize << 4);
            rx2 = floordiv(x2, sconf.regionSize << 4);
            rz2 = floordiv(z2, sconf.regionSize << 4);
        }

        cent->x = xt = 0;
        cent->z = zt = 0;
        icnt = 0;

        // Note "<="
        for (rz = rz1; rz <= rz2 && !*env->stop; rz++)
        {
            for (rx = rx1; rx <= rx2; rx++)
            {
                if (!getStructurePos(st, env->mc, env->seed, rx+0, rz+0, &pc))
                    continue;
                if (cond->skipref && pc.x == at.x && pc.z == at.z)
                    continue;
                if (rmax)
                {
                    int dx = pc.x - at.x;
                    int dz = pc.z - at.z;
                    int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                    if (rsq >= rmax)
                        continue;
                }
                else if (pc.x < x1 || pc.x > x2 || pc.z < z1 || pc.z > z2)
                {
                    continue;
                }
                if ((env->searchpass == PASS_FULL_64) ||
                    (env->searchpass == PASS_FULL_48 && !finfo.dep64))
                {
                    if (*env->stop) return COND_FAILED;

                    if (st == Village && cond->varflags)
                    {   // we can test for abandoned villages before the
                        // biome checks by trying each suitable biome
                        int vv[] = {
                            plains, desert, savanna, taiga, snowy_tundra,
                            // plains village variant covers meadows
                        };
                        int vn = env->mc <= MC_1_13 ? 1 : sizeof(vv) / sizeof(int);
                        int i;
                        for (i = 0; i < vn; i++)
                            if (isVariantOk(cond, env, st, vv[i], &pc))
                                break;
                        if (i >= vn) // no suitable village variants here
                            continue;
                    }

                    env->init4Dim(finfo.dim);
                    int id = isViableStructurePos(st, &env->g, pc.x, pc.z, 0);
                    if (!id)
                        continue;
                    if (st == End_City)
                    {
                        env->prepareSurfaceNoise(DIM_END);
                        if (!isViableEndCityTerrain(&env->g, &env->sn, pc.x, pc.z))
                            continue;
                    }
                    if (cond->varflags)
                    {
                        if (!isVariantOk(cond, env, st, id, &pc))
                            continue;
                    }
                    if (env->mc >= MC_1_18)
                    {
                        if (g_extgen.estimateTerrain &&
                            !isViableStructureTerrain(st, &env->g, pc.x, pc.z))
                        {
                            continue;
                        }
                    }
                }

                icnt++;
                if (imax == NULL)
                {
                    xt += pc.x;
                    zt += pc.z;
                }
                else if (*imax)
                {
                    cent[icnt-1] = pc;
                    if (icnt >= *imax)
                        goto L_struct_decide;
                }
                else
                {
                    goto L_struct_decide;
                }
            }
        }
    L_struct_decide:
        if (cond->count <= 0)
        {   // structure exclusion filter
            cent->x = (x1 + x2) >> 1;
            cent->z = (z1 + z2) >> 1;
            if (imax) *imax = 1;
            if (icnt == 0)
                return COND_OK;
            else
            {
                if (env->searchpass == PASS_FULL_64)
                    return COND_FAILED;
                if (env->searchpass == PASS_FULL_48 && !finfo.dep64)
                    return COND_FAILED;
                return COND_MAYBE_POS_VALID;
            }
        }
        else if (icnt >= cond->count)
        {
            if (imax)
            {
                *imax = icnt;
            }
            else
            {
                cent->x = xt / icnt;
                cent->z = zt / icnt;
            }

            if (env->searchpass == PASS_FULL_64)
                return COND_OK;
            if (env->searchpass == PASS_FULL_48 && !finfo.dep64)
                return COND_OK;
            // some non-exhaustive structure clusters do not
            // have known center positions with 48-bit seeds
            if (cond->count != (1+rx2-rx1) * (1+rz2-rz1))
                return COND_MAYBE_POS_INVAL;
            return COND_MAYBE_POS_VALID;
        }
        return COND_FAILED;


    case F_MINESHAFT:

        rx1 = x1 >> 4;
        rz1 = z1 >> 4;
        rx2 = x2 >> 4;
        rz2 = z2 >> 4;

        if (imax && cond->count > 0)
        {   // just check there are at least *inst (== cond->count) instances
            *imax = icnt =
                getMineshafts(env->mc, env->seed, rx1, rz1, rx2, rz2, cent, *imax);
            if (rmax)
            {   // filter out the instances that are outside the radius
                int j = 0;
                for (int i = 0; i < icnt; i++)
                {
                    int dx = cent[i].x - at.x;
                    int dz = cent[i].z - at.z;
                    int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                    if (rsq < rmax)
                        cent[j++] = cent[i];
                }
                *imax = icnt = j;
            }
            if (cond->skipref && icnt > 0)
            {   // remove origin instance
                for (int i = 0; i < icnt; i++)
                {
                    if (cent[i].x == at.x && cent[i].z == at.z)
                    {
                        cent[i] = cent[icnt-1];
                        *imax = --icnt;
                        break;
                    }
                }
            }
            if (icnt >= cond->count)
                return COND_OK;
        }
        else
        {   // we need the average position of all instances
            icnt = getMineshafts(env->mc, env->seed, rx1, rz1, rx2, rz2, &p[0], MAX_INSTANCES);
            if (icnt < cond->count)
                return COND_FAILED;
            xt = zt = 0;
            int j = 0;
            for (int i = 0; i < icnt; i++)
            {
                if (rmax)
                {   // skip instances outside the radius
                    int dx = cent[i].x - at.x;
                    int dz = cent[i].z - at.z;
                    int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                    if (rsq >= rmax)
                        continue;
                }
                if (cond->skipref && p[i].x == at.x && p[i].z == at.z)
                    continue;
                xt += p[i].x;
                zt += p[i].z;
                j++;
            }
            if (cond->count <= 0)
            {
                cent->x = (x1 + x2) >> 1;
                cent->z = (z1 + z2) >> 1;
                if (imax) *imax = 1;
                if (j == 0)
                    return COND_OK;
            }
            else if (j >= cond->count)
            {
                cent->x = xt / j;
                cent->z = zt / j;
                if (imax) *imax = 1;
                return COND_OK;
            }
        }
        return COND_FAILED;


    case F_SPAWN:

        cent->x = cent->z = 0;
        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_INVAL;

        if (*env->stop) return COND_FAILED;
        env->init4Dim(DIM_OVERWORLD);
        pc = getSpawn(&env->g);
        if (rmax)
        {
            int dx = pc.x - at.x;
            int dz = pc.z - at.z;
            int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
            if (rsq >= rmax)
                return COND_FAILED;
        }
        else if (pc.x < x1 || pc.x > x2 || pc.z < z1 || pc.z > z2)
        {
            return COND_FAILED;
        }

        if (cond->skipref && pc.x == at.x && pc.z == at.z)
            return COND_FAILED;
        *cent = pc;
        if (imax) *imax = 1;
        return COND_OK;


    case F_FIRST_STRONGHOLD:
        {
            StrongholdIter sh;
            *cent = pc = initFirstStronghold(&sh, env->mc, env->seed);
            if (imax) *imax = 1;
        }
        if (cond->rmax > 0)
        {
            int dx = pc.x - at.x;
            int dz = pc.z - at.z;
            int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
            if (rsq > rmax)
                return COND_FAILED;
        }
        else
        {
            if (pc.x < x1 || pc.x > x2 || pc.z < z1 || pc.z > z2)
                return COND_FAILED;
        }
        if (cond->skipref && pc.x == at.x && pc.z == at.z)
            return COND_FAILED;
        return COND_OK;


    case F_STRONGHOLD:

        // the position is rounded to the nearest chunk and then centered on (8,8)
        // for the pre-selection we will subtract this offset
        if (cond->rmax > 0)
        {
            x1 = at.x - cond->rmax - 8;
            z1 = at.z - cond->rmax - 8;
            x2 = at.x + cond->rmax - 8;
            z2 = at.z + cond->rmax - 8;
        }
        else
        {
            x1 = cond->x1 + at.x - 8;
            z1 = cond->z1 + at.z - 8;
            x2 = cond->x2 + at.x - 8;
            z2 = cond->z2 + at.z - 8;
        }
        rx1 = abs(x1); rx2 = abs(x2);
        rz1 = abs(z1); rz2 = abs(z2);
        // lets treat the final (+/-112) blocks as (+/-120) to account for chunk rounding
        if (x1 <= 112+8 && x2 >= -112-8 && z1 <= 112+8 && z2 >= -112-8)
        {
            rmin = 0;
        }
        else
        {
            xt = (rx1 < rx2 ? rx1 : rx2) - 112-8;
            zt = (rz1 < rz2 ? rz1 : rz2) - 112-8;
            rmin = xt*xt + zt*zt;
        }
        xt = (rx1 > rx2 ? rx1 : rx2) + 112+8;
        zt = (rz1 > rz2 ? rz1 : rz2) + 112+8;
        rmax = xt*xt + zt*zt;

        // undo (8,8) offset
        x1 += 8; z1 += 8;
        x2 += 8; z2 += 8;
        cent->x = (x1 + x2) >> 1;
        cent->z = (z1 + z2) >> 1;

        // -MC_1_8 formula:
        // r = 640 + [0,1]*512 (+/-112)
        // MC_1_9+ formula:
        // r = 1408 + 3072*n + 1280*[0,1] (+/-112)

        if (env->mc < MC_1_9)
        {
            if (rmax < 640*640 || rmin > 1152*1152)
                return cond->count == 0 ? COND_OK : COND_FAILED;
            r = 0;
            rmin = 640;
            rmax = 1152;
        }
        else
        {   // check if the area is entirely outside the radii ranges in which strongholds can generate
            if (rmax < 1408*1408)
                return cond->count == 0 ? COND_OK : COND_FAILED;
            rmin = sqrt(rmin);
            rmax = sqrt(rmax);
            r = (rmax - 1408) / 3072;       // maximum relevant ring number
            if (rmax - rmin < 3072-1280)    // area does not span more than one ring
            {
                if (rmin > 1408+1280+3072*r)// area is between rings
                    return cond->count == 0 ? COND_OK : COND_FAILED;
            }
            rmin = 1408;
            rmax = 1408+1280;
        }
        // if we are only looking at the inner ring, we can check if the generation angles are suitable
        if (r == 0 && !isInnerRingOk(env->mc, env->seed, x1-112-8, z1-112-8, x2+112+8, z2+112+8, rmin, rmax))
            return cond->count == 0 ? COND_OK : COND_FAILED;

        // pre-biome-checks complete, the area appears to line up with possible generation positions
        if (env->searchpass != PASS_FULL_64)
        {
            return COND_MAYBE_POS_INVAL;
        }
        else
        {
            if (cond->rmax > 0)
                rmax = (cond->rmax-1) * (cond->rmax-1) + 1;
            else
                rmax = 0;

            StrongholdIter sh;
            initFirstStronghold(&sh, env->mc, env->seed);
            icnt = 0;
            xt = zt = 0;
            env->init4Dim(DIM_OVERWORLD);
            while (nextStronghold(&sh, &env->g) > 0)
            {
                if (*env->stop)
                    break;
                bool inside;
                if (rmax)
                {
                    int dx = sh.pos.x - at.x;
                    int dz = sh.pos.z - at.z;
                    int64_t rsq = dx*(int64_t)dx + dz*(int64_t)dz;
                    inside = (rsq < rmax);
                }
                else
                {
                    inside = (sh.pos.x >= x1 && sh.pos.x <= x2 &&
                              sh.pos.z >= z1 && sh.pos.z <= z2);
                }
                if (cond->skipref && sh.pos.x == at.x && sh.pos.z == at.z)
                    inside = false;
                if (inside)
                {
                    if (cond->count == 0)
                    {   // exclude
                        return COND_FAILED;
                    }
                    else if (imax)
                    {
                        cent[icnt] = sh.pos;
                        icnt++;
                        if (icnt >= *imax)
                            return COND_OK;
                    }
                    else
                    {
                        xt += sh.pos.x;
                        zt += sh.pos.z;
                        icnt++;
                    }
                }
                if (sh.ringnum > r)
                    break;
            }
            if (cond->count == 0)
            {   // exclusion
                if (imax) *imax = 1;
                return COND_OK;
            }
            else if (imax)
            {
                *imax = icnt;
            }
            else if (icnt)
            {
                cent->x = xt / icnt;
                cent->z = zt / icnt;
            }
            if (icnt >= cond->count)
                return COND_OK;
        }
        return COND_FAILED;


    case F_SLIME:

        rx1 = x1 >> 4;
        rz1 = z1 >> 4;
        rx2 = x2 >> 4;
        rz2 = z2 >> 4;

        icnt = 0;
        xt = zt = 0;
        for (int rz = rz1; rz <= rz2; rz++)
        {
            for (int rx = rx1; rx <= rx2; rx++)
            {
                if (cond->skipref && rx == at.x >> 4 && rz == at.z >> 4)
                    continue;
                if (isSlimeChunk(env->seed, rx, rz))
                {
                    if (cond->count == 0)
                    {
                        return COND_FAILED;
                    }
                    else if (imax)
                    {
                        cent[icnt].x = rx << 4;
                        cent[icnt].z = rz << 4;
                        icnt++;
                        if (icnt >= *imax)
                            return COND_OK;
                    }
                    else
                    {
                        xt += rx;
                        zt += rz;
                        icnt++;
                    }
                }
            }
        }
        if (cond->count == 0)
        {   // exclusion filter
            cent->x = (x1 + x2) >> 1;
            cent->z = (z1 + z2) >> 1;
            if (imax) *imax = 1;
            return COND_OK;
        }
        else if (imax)
        {
            *imax = icnt;
        }
        else if (icnt)
        {
            cent->x = (xt << 4) / icnt + 8;
            cent->z = (zt << 4) / icnt + 8;
        }
        if (icnt >= cond->count)
            return COND_OK;
        return COND_FAILED;


    case F_BIOME_SAMPLE:
    case F_NOISE_SAMPLE:

        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_INVAL;
        if (cond->confidence <= 0 || cond->confidence >= 1)
            return COND_FAILED;
        if (cond->converage <= 0 || cond->converage > 1)
            return COND_FAILED;
        if (cond->type == F_NOISE_SAMPLE && env->mc <= MC_1_17)
            return COND_FAILED;

        s = 2;
        rx1 = x1 >> s;
        rz1 = z1 >> s;
        rx2 = x2 >> s;
        rz2 = z2 >> s;
        {
            int w = rx2 - rx1 + 1;
            int h = rz2 - rz1 + 1;
            Range r = {1<<s, rx1, rz1, w, h, s == 0 ? cond->y : cond->y >> 2, 1};
            sample_boime_t sample;
            sample.cond = cond;
            sample.at = at;
            sample.rmaxsq = rmax;
            sample.n = 0;
            sample.xsum = 0;
            sample.zsum = 0;
            sample.imax = imax;
            sample.cent = cent;
            sample.stop = env->stop;

            uint64_t rng;
            setSeed(&rng, env->seed);

            int (*f)(Generator *, int, int, int, int, void *);

            if (cond->type == F_NOISE_SAMPLE)
            {
                env->init4Noise(cond->para, cond->octave);
                f = f_noise_sampler;
            }
            else
            {
                env->init4Dim(DIM_OVERWORLD);
                f = f_biome_sampler;
            }
            int ok = monteCarloBiomes(&env->g, r, &rng, cond->converage, cond->confidence, f, &sample);
            if (imax && cond->count == 1)
            {
                *imax = sample.n;
            }
            else if (sample.n)
            {
                cent->x = sample.xsum / sample.n + 2;
                cent->z = sample.zsum / sample.n + 2;
                if (imax) *imax = 1;
            }
            else
            {
                *cent = at;
                if (imax) *imax = 1;
            }
            return ok == 1 ? COND_OK : COND_FAILED;
        }
        return COND_FAILED;


    case F_BIOME_4_RIVER:
    case F_BIOME_256_OTEMP:

        if (env->mc > MC_1_17)
            return COND_FAILED;

        s = cond->type == F_BIOME_4_RIVER ? 2 : 8;
        rx1 = x1 >> s;
        rz1 = z1 >> s;
        rx2 = x2 >> s;
        rz2 = z2 >> s;
        cent->x = (x1 + x2) >> 1;
        cent->z = (z1 + z2) >> 1;
        if (imax) *imax = 1;
        if (env->searchpass == PASS_FAST_48)
            return COND_MAYBE_POS_VALID;
        if (env->searchpass == PASS_FULL_48)
        {
            if (env->mc < MC_1_13 || cond->type != F_BIOME_256_OTEMP)
                return COND_MAYBE_POS_VALID;
        }
        valid = COND_FAILED;
        if (rx2 >= rx1 || rz2 >= rz1 || !*env->stop)
        {
            int w = rx2-rx1+1;
            int h = rz2-rz1+1;
            //env->init4Dim(0); // seed gets applied by checkForBiomesAtLayer
            Layer *entry;
            if (cond->type == F_BIOME_4_RIVER)
                entry = &env->g.ls.layers[L_RIVER_4];
            else
                entry = &env->g.ls.layers[L_OCEAN_TEMP_256];
            if (checkForBiomesAtLayer(&env->g.ls, entry,
                NULL, env->seed, rx1, rz1, w, h, &cond->bf) > 0)
            {
                valid = COND_OK;
            }
        }
        return valid;


    case F_TEMPS:
        if (env->mc > MC_1_17)
            return COND_FAILED;
        rx1 = x1 >> 10;
        rz1 = z1 >> 10;
        rx2 = x2 >> 10;
        rz2 = z2 >> 10;
        cent->x = (x1 + x2) >> 1;
        cent->z = (z1 + z2) >> 1;
        if (imax) *imax = 1;
        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_VALID;
        env->init4Dim(DIM_OVERWORLD);
        if (checkForTemps(&env->g.ls, env->seed, rx1, rz1, rx2-rx1+1, rz2-rz1+1, cond->temps))
            return COND_OK;
        return COND_FAILED;


    case F_BIOME:
    case F_BIOME_NETHER:
    case F_BIOME_END:

        switch (cond->step)
        {
        case 1:   s = 0; break;
        case 4:   s = 2; break;
        case 16:  s = 4; break;
        case 64:  s = 6; break;
        case 256: s = 8; break;
        default: return COND_FAILED;
        }

        rx1 = x1 >> s;
        rz1 = z1 >> s;
        rx2 = x2 >> s;
        rz2 = z2 >> s;
        cent->x = (x1 + x2) >> 1;
        cent->z = (z1 + z2) >> 1;
        if (imax) *imax = 1;
        if (env->searchpass == PASS_FAST_48)
            return COND_MAYBE_POS_VALID;
        // the Nether and End require only the 48-bit seed
        // (except voronoi uses the full 64-bits)
        if (env->searchpass != PASS_FULL_64 && (finfo.dep64 || s == 0))
            return COND_MAYBE_POS_VALID;
        else
        {
            int w = rx2 - rx1 + 1;
            int h = rz2 - rz1 + 1;
            int y = (s == 0 ? cond->y : cond->y >> 2);
            Range r = {1<<s, rx1, rz1, w, h, y, 1};
            valid = checkForBiomes(&env->g, NULL, r, finfo.dim, env->seed,
                &cond->bf, (volatile char*)env->stop) > 0;
        }
        return valid ? COND_OK : COND_FAILED;


    case F_BIOME_CENTER:
    case F_BIOME_CENTER_256:
        if (env->searchpass == PASS_FULL_64)
        {
            s = cond->type == F_BIOME_CENTER ? 2 : 8;
            rx1 = x1 >> s;
            rz1 = z1 >> s;
            rx2 = x2 >> s;
            rz2 = z2 >> s;
            int w = rx2 - rx1 + 1;
            int h = rz2 - rz1 + 1;
            Range r = {1<<s, rx1, rz1, w, h, cond->y >> 2, 1};
            env->init4Dim(DIM_OVERWORLD);

            if (cond->count <= 0)
            {   // exclusion
                icnt = getBiomeCenters(
                    cent, NULL, 1, &env->g, r, cond->biomeId, cond->biomeSize, cond->tol,
                    (volatile char*)env->stop
                );
                if (icnt == 0)
                {
                    cent->x = (x1 + x2) >> 1;
                    cent->z = (z1 + z2) >> 1;
                    if (imax) *imax = 1;
                    return COND_OK;
                }
            }
            else if (imax)
            {   // just check there are at least *inst (== cond->count) instances
                *imax = icnt = getBiomeCenters(
                    cent, NULL, cond->count, &env->g, r, cond->biomeId, cond->biomeSize, cond->tol,
                    (volatile char*)env->stop
                );
                if (cond->skipref && icnt > 0)
                {   // remove origin instance
                    for (int i = 0; i < icnt; i++)
                    {
                        if (cent[i].x == at.x && cent[i].z == at.z)
                        {
                            cent[i] = cent[icnt-1];
                            *imax = --icnt;
                            break;
                        }
                    }
                }
                if (icnt >= cond->count)
                    return COND_OK;
            }
            else
            {   // we need the average position of all instances
                icnt = getBiomeCenters(
                    &p[0], NULL, MAX_INSTANCES, &env->g, r, cond->biomeId, cond->biomeSize, cond->tol,
                    (volatile char*)env->stop
                );
                xt = zt = 0;
                int j = 0;
                for (int i = 0; i < icnt; i++)
                {
                    if (cond->skipref && p[i].x == at.x && p[i].z == at.z)
                        continue;
                    xt += p[i].x;
                    zt += p[i].z;
                    j++;
                }
                if (j >= cond->count)
                {
                    cent->x = xt / j + (1 << s) / 2;
                    cent->z = zt / j + (1 << s) / 2;
                    return COND_OK;
                }
            }
            return COND_FAILED;
        }
        return COND_MAYBE_POS_INVAL;

    case F_CLIMATE_MINMAX:
        if (env->mc <= MC_1_17 || cond->para >= NP_MAX)
            return COND_FAILED;
        rx1 = x1 >> 2;
        rz1 = z1 >> 2;
        rx2 = x2 >> 2;
        rz2 = z2 >> 2;
        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_INVAL;
        {
            track_minmax_t info = {at, at, +INFINITY, -INFINITY};
            int w = rx2 - rx1 + 1;
            int h = rz2 - rz1 + 1;
            env->init4Noise(cond->para, cond->octave);
            double para[2] = {+INFINITY, -INFINITY};
            double *p_min = (cond->minmax & Condition::E_LOCATE_MIN) ? para+0 : nullptr;
            double *p_max = (cond->minmax & Condition::E_LOCATE_MAX) ? para+1 : nullptr;
            getParaRange(&env->g.bn.climate[cond->para], p_min, p_max,
                    rx1, rz1, w, h, &info, f_track_minmax);
            double vmin = cond->minmax & Condition::E_TEST_LOWER ? cond->vmin : -INFINITY;
            double vmax = cond->minmax & Condition::E_TEST_UPPER ? cond->vmax : +INFINITY;
            double evalmin = p_min ? info.vmin : info.vmax;
            double evalmax = p_max ? info.vmax : info.vmin;
            if (cond->flags & Condition::FLG_INVERT)
            {
                if (evalmin > vmin && evalmax < vmax)
                    return COND_FAILED;
            }
            else
            {
                if (evalmin < vmin || evalmax > vmax)
                    return COND_FAILED;
            }
            *cent = at;
            if (imax) *imax = 1;
            if (cond->minmax & Condition::E_LOCATE_MIN)
            {
                cent->x = info.posmin.x << 2;
                cent->z = info.posmin.z << 2;
            }
            else if (cond->minmax & Condition::E_LOCATE_MAX)
            {
                cent->x = info.posmax.x << 2;
                cent->z = info.posmax.z << 2;
            }
            return COND_OK;
        }

    case F_CLIMATE_NOISE:
        if (env->mc <= MC_1_17)
            return COND_FAILED;
        rx1 = x1 >> 2;
        rz1 = z1 >> 2;
        rx2 = x2 >> 2;
        rz2 = z2 >> 2;
        cent->x = (x1 + x2) >> 1;
        cent->z = (z1 + z2) >> 1;
        if (imax) *imax = 1;
        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_VALID;
        else
        {
            int w = rx2 - rx1 + 1;
            int h = rz2 - rz1 + 1;
            //int y = cond->y >> 2;
            env->init4Dim(DIM_OVERWORLD);
            int order[] = { // use this order for performance
                NP_TEMPERATURE,
                NP_HUMIDITY,
                NP_WEIRDNESS,
                NP_EROSION,
                NP_CONTINENTALNESS,
            };
            valid = 1;
            for (uint j = 0; j < sizeof(order)/sizeof(int); j++)
            {
                int i = order[j];
                if (cond->limok[i][0] == INT_MIN && cond->limok[i][1] == INT_MAX &&
                    cond->limex[i][0] == INT_MIN && cond->limex[i][1] == INT_MAX)
                {
                    continue;
                }
                double pmin, pmax;
                double bounds[] = {
                    (double)cond->limex[i][0],
                    (double)cond->limex[i][1],
                };
                int err = getParaRange(&env->g.bn.climate[i], &pmin, &pmax, rx1, rz1, w, h, (void*)bounds, f_confine);
                if (err)
                {
                    valid = 0;
                    break;
                }
                int lmin, lmax;
                lmin = cond->limok[i][0];
                lmax = cond->limok[i][1];
                if (pmin > lmax || pmax < lmin)
                {   // outside required limits
                    valid = 0;
                    break;
                }
                lmin = cond->limex[i][0];
                lmax = cond->limex[i][1];
                if (pmin < lmin || pmax > lmax)
                {   // ouside exclusion limits
                    valid = 0;
                    break;
                }
            }
        }
        return valid ? COND_OK : COND_FAILED;


    case F_HEIGHT:
        rx1 = x1 >> 2;
        rz1 = z1 >> 2;
        cent->x = x1;
        cent->z = z1;
        if (imax) *imax = 1;
        if (env->searchpass != PASS_FULL_64)
            return COND_MAYBE_POS_VALID;
        env->init4Dim(DIM_OVERWORLD);
        env->prepareSurfaceNoise(DIM_OVERWORLD);
        {
            int ymin = cond->limok[NP_DEPTH][0];
            int ymax = cond->limok[NP_DEPTH][1];
            float y;
            mapApproxHeight(&y, nullptr, &env->g, &env->sn, rx1, rz1, 1, 1);
            if (cond->flags & Condition::FLG_IN_RANGE)
                valid = y >= ymin && y <= ymax;
            else
                valid = y <= ymin || y >= ymax;
        }
        return valid ? COND_OK : COND_FAILED;

    default:
        break;
    }

    return COND_MAYBE_POS_INVAL;
}


void findQuadStructs(int styp, Generator *g, QVector<QuadInfo> *out)
{
    StructureConfig sconf;
    if (!getStructureConfig_override(styp, g->mc, &sconf))
        return;

    int qmax = 1000;
    Pos *qlist = new Pos[qmax];
    int r = 3e7 / 512;
    int qcnt;

    if (styp == Swamp_Hut)
    {
        qcnt = scanForQuads(
            sconf, 128, g->seed & MASK48,
            low20QuadHutBarely, 20, sconf.salt,
            -r, -r, 2*r, 2*r, qlist, qmax
        );

        for (int i = 0; i < qcnt; i++)
        {
            Pos qr = qlist[i];
            Pos qs[4];
            getStructurePos(styp, g->mc, g->seed, qr.x+0, qr.z+0, qs+0);
            getStructurePos(styp, g->mc, g->seed, qr.x+0, qr.z+1, qs+1);
            getStructurePos(styp, g->mc, g->seed, qr.x+1, qr.z+0, qs+2);
            getStructurePos(styp, g->mc, g->seed, qr.x+1, qr.z+1, qs+3);
            if (isViableStructurePos(styp, g, qs[0].x, qs[0].z, 0) &&
                isViableStructurePos(styp, g, qs[1].x, qs[1].z, 0) &&
                isViableStructurePos(styp, g, qs[2].x, qs[2].z, 0) &&
                isViableStructurePos(styp, g, qs[3].x, qs[3].z, 0))
            {
                QuadInfo qinfo;
                for (int j = 0; j < 4; j++)
                    qinfo.p[j] = qs[j];
                qinfo.c = (g->seed + sconf.salt) & 0xfffff;
                qinfo.flt = 0; // TODO
                qinfo.typ = styp;
                qinfo.afk = getOptimalAfk(qs, 7,7,9, &qinfo.spcnt);
                qinfo.rad = isQuadBase(sconf, moveStructure(g->seed,-qr.x,-qr.z), 160);
                out->push_back(qinfo);
            }
        }
    }
    else if (styp == Monument)
    {
        qcnt = scanForQuads(
            sconf, 160, g->seed & MASK48,
            g_qm_90, 48, sconf.salt,
            -r, -r, 2*r, 2*r, qlist, qmax
        );

        for (int i = 0; i < qcnt; i++)
        {
            Pos qr = qlist[i];
            Pos qs[4];
            getStructurePos(styp, g->mc, g->seed, qr.x+0, qr.z+0, qs+0);
            getStructurePos(styp, g->mc, g->seed, qr.x+0, qr.z+1, qs+1);
            getStructurePos(styp, g->mc, g->seed, qr.x+1, qr.z+0, qs+2);
            getStructurePos(styp, g->mc, g->seed, qr.x+1, qr.z+1, qs+3);
            if (isViableStructurePos(styp, g, qs[0].x, qs[0].z, 0) &&
                isViableStructurePos(styp, g, qs[1].x, qs[1].z, 0) &&
                isViableStructurePos(styp, g, qs[2].x, qs[2].z, 0) &&
                isViableStructurePos(styp, g, qs[3].x, qs[3].z, 0))
            {
                QuadInfo qinfo;
                for (int j = 0; j < 4; j++)
                    qinfo.p[j] = qs[j];
                qinfo.c = (g->seed + sconf.salt) & MASK48;
                qinfo.flt = 0; // TODO
                qinfo.typ = styp;
                qinfo.afk = getOptimalAfk(qs, 58,0/*23*/,58, &qinfo.spcnt);
                qinfo.afk.x -= 29;
                qinfo.afk.z -= 29;
                qinfo.rad = isQuadBase(sconf, moveStructure(g->seed,-qr.x,-qr.z), 160);
                out->push_back(qinfo);
            }
        }
    }

    delete[] qlist;
}







