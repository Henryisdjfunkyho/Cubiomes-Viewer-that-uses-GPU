#include "hipsearch.h"
#include "hipreference.h"
#include "hipbatchpolicy.h"
#include "../gpu/adaptive_probe_policy.h"
#include <QCoreApplication>
#include <cmath>
#include <limits>
#include <QDebug>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <cstring>
#include <chrono>
extern "C" double inverf(double);
extern "C" void wilson(double,double,double,double*,double*);

HipSearch::HipSearch()
#ifdef CVHIP_CUDA_ONLY
    : library(QCoreApplication::applicationDirPath()+"/cubiomes_cuda")
#else
    : library(QCoreApplication::applicationDirPath()+"/cubiomes_hip")
#endif
{}

QString HipSearch::deviceIdentity()
{
    const bool wasLoaded = library.isLoaded();
    if (!wasLoaded && !library.load())
        return QString();
    using DeviceIdentity = size_t (*)(char *, size_t);
    auto identity = reinterpret_cast<DeviceIdentity>(library.resolve("cvhip_device_identity"));
    char buffer[512] = {};
    const size_t written = identity ? identity(buffer, sizeof(buffer)) : 0;
    if (!wasLoaded)
        library.unload();
    return written && written <= sizeof(buffer) ? QString::fromLocal8Bit(buffer) : QString();
}
HipSearch::~HipSearch() { shutdown(); }

bool HipBatch::next(unsigned &cursor, unsigned &index, unsigned &state) const {
    if (!encoding) {
        if (cursor >= count) return false;
        index = cursor++; state = 2; return true;
    }
    if (encoding == CVHIP_OFFSETS) {
        if (cursor >= survivors) return false;
        uint32_t value = data[cursor++];
        index = value & 0x7fffffffU; state = value >> 31 ? 2 : 1;
        return true;
    }
    while (cursor < count) {
        unsigned word = cursor / 16, shift = (cursor % 16) * 2;
        if (!data[word]) { cursor = (word + 1) * 16; continue; }
        index = cursor++; state = (data[word] >> shift) & 3U;
        if (state) return true;
    }
    return false;
}

void HipSearch::cancel() {
    {
        // The predicate and notification must use the waiter's mutex. Otherwise
        // a cancellation between predicate-check and sleep can lose its wakeup.
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
        enabled = false;
    }
    queueReady.notify_all();
}

void HipSearch::shutdown() {
    const bool profile=qgetenv("CUBIOMES_HIP_PROFILE")=="1";
    auto begin=std::chrono::steady_clock::now();
    cancel();
    if (coordinator.joinable()) coordinator.join();
    if(profile)std::fprintf(stderr,"HIP profile: coordinator join %.3fs\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count());
    if (context && destroy) destroy(context);
    if(profile)std::fprintf(stderr,"HIP profile: context cleanup %.3fs\n",std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count());
    context = nullptr;
}

void HipSearch::configure(const ConditionTree &tree,int mc,int large,bool useHip,bool full64,bool searchSemantics,bool arbitrarySeeds,unsigned requestedBatchLimit,unsigned requestedInflight,bool useAdaptiveSafety) {
    shutdown();
    configs.clear();
    ids.clear();
    compiledIds.clear();
    deviceBatchLimit = CVHIP_MAX_BATCH;
    backendBatchLimit = CVHIP_MAX_BATCH;
    runtimeBatchLimit.store(CVHIP_MAX_BATCH);
    adaptiveSafety = useAdaptiveSafety;
    maxInflight = requestedInflight ? std::max(1U, std::min(2U, requestedInflight)) : 2;
    if(!useHip||qgetenv("CUBIOMES_HIP")=="0")return;
    std::vector<bool> safeNoise(tree.condvec.size(),true);
    if(searchSemantics) {
        bool hasNoise=false,hasGeneration=false;
        for(const auto &c:tree.condvec)if(c.save&&!(c.meta&Condition::DISABLED)) {
            if(c.type==F_LUA)return; // arbitrary script effects cannot be hoisted
            hasNoise|=c.type==F_CLIMATE_MINMAX||c.type==F_NOISE_SAMPLE;
            hasGeneration|=c.type!=F_CLIMATE_MINMAX&&c.type!=F_NOISE_SAMPLE&&c.type!=F_SLIME&&c.type!=F_SELECT&&c.type!=F_LOGIC_OR&&c.type!=F_SPIRAL;
        }
        // Existing CPU noise caching is history-dependent when a repeated list
        // mixes selected octaves and full generation. Preserve it via fallback.
        if(arbitrarySeeds&&hasNoise&&hasGeneration)return;
        bool generation=false;
        std::vector<bool> visited(tree.condvec.size(),false);
        std::function<void(int)> visit=[&](int id) {
            if(id<0||size_t(id)>=tree.condvec.size()||visited[id])return;
            visited[id]=true;const auto &c=tree.condvec[id];safeNoise[id]=!generation;
            if(c.save&&c.type!=F_CLIMATE_MINMAX&&c.type!=F_NOISE_SAMPLE&&c.type!=F_SLIME&&c.type!=F_SELECT&&c.type!=F_LOGIC_OR&&c.type!=F_SPIRAL)generation=true;
            if(size_t(id)<tree.references.size())for(int child:tree.references[id])visit(child);
        };
        visit(0);
    }
    if(!library.load())return;
    auto available=reinterpret_cast<Available>(library.resolve("cvhip_available"));
    auto version=reinterpret_cast<Version>(library.resolve("cvhip_abi_version"));
    create=reinterpret_cast<Create>(library.resolve("cvhip_create"));
    destroy=reinterpret_cast<Destroy>(library.resolve("cvhip_destroy"));
    submit=reinterpret_cast<Submit>(library.resolve("cvhip_submit"));
    collect=reinterpret_cast<Collect>(library.resolve("cvhip_collect"));
    if(!available||!version||version()!=CVHIP_ABI_VERSION||!create||!destroy||!submit||!collect||!available())return;
    auto resolveOrigin=[&](const Condition &c,hipreference::FixedOrigin &origin,int allowedOr=0) {
        return hipreference::resolveFixedOrigin(tree,c,mc,full64,allowedOr,&origin) &&
            origin.x>=INT_MIN && origin.x<=INT_MAX && origin.z>=INT_MIN && origin.z<=INT_MAX;
    };
    auto fixedOrigin=[&](const Condition &c,int64_t &px,int64_t &pz,int allowedOr=0) {
        hipreference::FixedOrigin origin;
        if(!resolveOrigin(c,origin,allowedOr) || origin.dimension!=DIM_OVERWORLD)return false;
        px=origin.x;pz=origin.z;return true;
    };
    auto remember=[&](int id) {if(std::find(compiledIds.begin(),compiledIds.end(),id)==compiledIds.end())compiledIds.push_back(id);};
    // These values mirror cvhip_extra::PlacementMode without making the Qt
    // executable include the HIP-only header.  Zero remains the legacy
    // regular/large placement path in the ABI.
    constexpr int structureOutpost=2, structureEndCity=3,
        structureFortress=4, structureBastion=5;
    auto sampling=[&](CvHipCondition &cfg,const Condition &c,int64_t cells) {
        if(c.confidence<=0||c.confidence>=1||c.converage<=0||c.converage>1||cells<=0)return false;
        cfg.coverage=c.converage;cfg.zscore=std::sqrt(2.0)*inverf(c.confidence);
        double wn=cfg.zscore*std::sqrt(double(cells)),low,high;
        wilson(wn,c.converage,cfg.zscore,&low,&high);cfg.wilsonWidth=high-low;
        cfg.matchAny=cells<4*wn;
        if(cfg.matchAny&&cells>64)return false;
        return std::isfinite(cfg.zscore)&&std::isfinite(cfg.wilsonWidth);
    };
    std::vector<int> orGroup(tree.condvec.size(),0);
    std::vector<int> orParents;
    for(const auto &parent:tree.condvec) {
        if(parent.type!=F_LOGIC_OR||!parent.save||parent.meta&Condition::DISABLED||
           size_t(parent.save)>=tree.references.size())continue;
        const auto &children=tree.references[parent.save];
        if(children.empty())continue;
        bool valid=true;
        for(int child:children) {
            if(child<0||size_t(child)>=tree.condvec.size()) {valid=false;break;}
            const auto &c=tree.condvec[child];
            if(c.relative!=parent.save||c.save!=child||c.type!=F_CLIMATE_MINMAX||
               c.meta&Condition::DISABLED||c.x1!=c.x2||c.z1!=c.z2||c.rmax>0||
               !(c.minmax&(Condition::E_LOCATE_MIN|Condition::E_LOCATE_MAX))||
               c.para==NP_DEPTH||c.para>=NP_MAX||c.octave>32) {valid=false;break;}
            orGroup[child]=parent.save;
        }
        if(valid)orParents.push_back(parent.save);
        else for(int child:children)if(child>=0&&size_t(child)<orGroup.size())orGroup[child]=0;
    }
    std::vector<int> spiralGroup(tree.condvec.size(),0);
    std::vector<int> spiralParents;
    for(const auto &parent:tree.condvec) {
        if(parent.type!=F_SPIRAL||!parent.save||parent.meta&Condition::DISABLED||
           size_t(parent.save)>=tree.references.size())continue;
        const auto &children=tree.references[parent.save];
        if(children.empty())continue;
        bool valid=true;
        for(int child:children) {
            if(child<0||size_t(child)>=tree.condvec.size()){valid=false;break;}
            const auto &c=tree.condvec[child];
            const bool pointClimate=c.type==F_CLIMATE_MINMAX&&c.x1==c.x2&&c.z1==c.z2&&
               c.rmax<=0&&(c.minmax&(Condition::E_LOCATE_MIN|Condition::E_LOCATE_MAX))&&
               c.para!=NP_DEPTH&&c.para<NP_MAX;
            // F_CLIMATE_NOISE range offsets can only be represented as one
            // spiral point when both their block-coordinate endpoints are
            // identical. Checking a pre-translated quart cell is insufficient
            // when the spiral lattice step or the child offset changes it.
            const bool pointClimateNoise=c.type==F_CLIMATE_NOISE&&c.rmax<=0&&
                c.x1==c.x2&&c.z1==c.z2;
            if(c.relative!=parent.save||c.save!=child||
               (!pointClimate&&!pointClimateNoise)||c.meta&Condition::DISABLED) {valid=false;break;}
            spiralGroup[child]=parent.save;
        }
        if(valid)spiralParents.push_back(parent.save);
        else for(int child:children)if(child>=0&&size_t(child)<spiralGroup.size())spiralGroup[child]=0;
    }
    std::vector<int> dynamicCenterParent(tree.condvec.size(),0);
    std::vector<hipreference::DynamicCenterPointGroup> dynamicCenterGroups;
    for(const auto &parent:tree.condvec) {
        if(!parent.save||parent.meta&Condition::DISABLED)continue;
        hipreference::DynamicCenterPointGroup group;
        if(!hipreference::resolveDynamicCenterPointGroup(tree,parent.save,mc,full64,&group))continue;
        hipreference::FixedOrigin origin;
        int64_t ax1,ax2,az1,az2;
        if(!resolveOrigin(parent,origin)||origin.dimension!=DIM_OVERWORLD||
           !hipreference::addChecked(origin.x,parent.x1,&ax1)||!hipreference::addChecked(origin.x,parent.x2,&ax2)||
           !hipreference::addChecked(origin.z,parent.z1,&az1)||!hipreference::addChecked(origin.z,parent.z2,&az2)||ax2<ax1||az2<az1)continue;
        const int64_t wx=hipreference::floorToQuart(ax2)-hipreference::floorToQuart(ax1)+1;
        const int64_t wz=hipreference::floorToQuart(az2)-hipreference::floorToQuart(az1)+1;
        if(wx<=0||wz<=0||wx>64||wz>64||wx*wz>4096)continue;
        bool childrenFree=true;
        for(int child:group.childNodes) {
            if(child<=0||size_t(child)>=dynamicCenterParent.size()||dynamicCenterParent[child]) {
                childrenFree=false;break;
            }
        }
        if(!childrenFree)continue;
        for(int child:group.childNodes)dynamicCenterParent[child]=parent.save;
        dynamicCenterGroups.push_back(std::move(group));
    }
    std::vector<Condition> candidates;
    for(const auto &source:tree.condvec) {
        if(!source.save||source.meta&Condition::DISABLED)continue;
        if(source.type==F_SPIRAL&&source.save<100&&spiralParents.end()!=std::find(spiralParents.begin(),spiralParents.end(),source.save)) {
            int64_t bx,bz;if(!fixedOrigin(source,bx,bz))continue;
            int step=source.step?source.step:512;if(step<=0)continue;
            int64_t x1,z1,x2,z2,radiusSq=0;
            if(source.rmax>0) {
                int64_t radius=int64_t(source.rmax)-1;
                x1=bx-radius;z1=bz-radius;x2=bx+radius;z2=bz+radius;radiusSq=radius*radius+1;
            } else {x1=bx+source.x1;z1=bz+source.z1;x2=bx+source.x2;z2=bz+source.z2;}
            if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
            auto floorDiv=[](int64_t v,int d){return v/d-(v%d<0);};
            int64_t rx1=floorDiv(x1,step),rz1=floorDiv(z1,step),rx2=floorDiv(x2,step),rz2=floorDiv(z2,step);
            const int64_t rw=rx2-rx1+1,rh=rz2-rz1+1;
            if(rx1<INT_MIN||rx2>INT_MAX||rz1<INT_MIN||rz2>INT_MAX||rx2==INT_MAX||rz2==INT_MAX||
               rw<=0||rh<=0||rw>256||rh>256||rw*rh>256||
               uint64_t(rw)*uint64_t(rh)*tree.references[source.save].size()>1024)continue;
            CvHipCondition cfg{};cfg.reserved=CVHIP_SPIRAL_CLIMATE;cfg.groupId=source.save;cfg.conditionId=source.save;
            cfg.scale=step;cfg.x=int(rx1);cfg.z=int(rz1);cfg.x2=int(rx2);cfg.z2=int(rz2);
            cfg.refx=int(bx);cfg.refz=int(bz);cfg.radiusSq=uint64_t(radiusSq);
            cfg.areaX1=int(x1);cfg.areaZ1=int(z1);cfg.areaX2=int(x2);cfg.areaZ2=int(z2);
            configs.push_back(cfg);continue;
        }
        const int st=g_filterinfo.list[source.type].stype;
        bool regular=false,triangular=false;
        int structureMode=0;
        switch(st) {
        case Desert_Pyramid:case Jungle_Pyramid:case Swamp_Hut:case Igloo:
        case Village:case Ocean_Ruin:case Shipwreck:case Ruined_Portal:
        case Ruined_Portal_N:case Ancient_City:case Trail_Ruins:case Trial_Chambers:
        case Abandoned_Camp:regular=true;break;
        case Monument:case Mansion:regular=true;triangular=true;break;
        case Outpost: regular=true;structureMode=structureOutpost;break;
        case End_City: regular=true;structureMode=structureEndCity;break;
        case Fortress: regular=true;structureMode=structureFortress;break;
        case Bastion: regular=true;structureMode=structureBastion;break;
        default:break;
        }
        // Fortress and bastion placement changed before 1.18; the compact
        // device formulas below are only the modern getFeaturePos path.
        if((st==Fortress||st==Bastion)&&mc<MC_1_18)regular=false;
        if(regular&&source.count>0&&source.rmax<=0) {
            StructureConfig sc{};int64_t px,pz;hipreference::FixedOrigin origin;
            if(!getStructureConfig(st,mc,&sc)||sc.regionSize<=0||sc.chunkRange<=0||
               !resolveOrigin(source,origin))continue;
            const int expectedDimension=g_filterinfo.list[source.type].dim;
            // A direct root condition is evaluated in its own structure
            // dimension by the CPU finder; only transformed references need
            // a resolved dimension check.
            if(source.relative!=0 && !hipreference::dimensionMatches(source,origin.dimension))continue;
            if(source.relative==0 && expectedDimension!=DIM_UNDEF)origin.dimension=expectedDimension;
            px=origin.x;pz=origin.z;
            int64_t x1=px+source.x1,x2=px+source.x2,z1=pz+source.z1,z2=pz+source.z2;
            if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
            auto floorDiv=[](int64_t n,int d){return n/d-(n%d<0);};
            int regionBlocks=sc.regionSize*16;
            if((floorDiv(x2,regionBlocks)-floorDiv(x1,regionBlocks)+1)*
               (floorDiv(z2,regionBlocks)-floorDiv(z1,regionBlocks)+1)>64)continue;
            CvHipCondition cfg{};cfg.reserved=CVHIP_STRUCTURE;cfg.lo=sc.salt;
            cfg.na=sc.regionSize;cfg.nb=sc.chunkRange;cfg.invert=triangular;
            cfg.x=int(x1);cfg.x2=int(x2);cfg.z=int(z1);cfg.z2=int(z2);
            cfg.required=source.count;cfg.skipref=source.skipref;cfg.refx=int(px);cfg.refz=int(pz);
            cfg.conditionId=structureMode;
            configs.push_back(cfg);remember(source.save);continue;
        }
        if(full64&&mc>=MC_1_18&&(source.type==F_BIOME_CENTER||source.type==F_BIOME_CENTER_256)) {
            int64_t px,pz;if(!fixedOrigin(source,px,pz))continue;
            int shift=source.type==F_BIOME_CENTER?2:8;
            int64_t x1,z1,x2,z2;
            if(source.rmax>0) {
                int64_t radius=int64_t(source.rmax)-1;
                x1=px-radius;z1=pz-radius;x2=px+radius;z2=pz+radius;
            } else {x1=px+source.x1;z1=pz+source.z1;x2=px+source.x2;z2=pz+source.z2;}
            if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
            auto floorScale=[&](int64_t v){int64_t d=int64_t(1)<<shift;return v/d-(v%d<0);};
            int64_t bx1=floorScale(x1),bz1=floorScale(z1),bx2=floorScale(x2),bz2=floorScale(z2);
            int64_t w=bx2-bx1+1,h=bz2-bz1+1;
            // Exact center/flood-fill work is bounded to 4096 cells. Larger
            // regions may use only the sparse climate-start rejection pass,
            // and only when the CPU start set is at most 64 cells.
            if(w<=0||h<=0||bx1<INT_MIN||bx2>INT_MAX||bz1<INT_MIN||bz2>INT_MAX||bx2==INT_MAX||bz2==INT_MAX||
               source.biomeId<0||source.biomeId>=192||(source.biomeId>=64&&source.biomeId<128))continue;
            const int *limits=getBiomeParaLimits(mc,source.biomeId);if(!limits)continue;
            CvHipCondition cfg{};cfg.reserved=CVHIP_BIOME_CENTER;cfg.mc=mc;cfg.large=large;
            cfg.dimension=DIM_OVERWORLD;cfg.scale=1<<shift;cfg.x=int(bx1);cfg.z=int(bz1);cfg.x2=int(bx2);cfg.z2=int(bz2);
            cfg.y=source.y>>2;
            cfg.find=uint64_t(source.biomeId);cfg.required=source.count;cfg.tolerance=source.tol;
            cfg.componentSize=source.biomeSize;
            // Keep the original reference position for the locator's
            // skipref comparison.  The GPU path is only allowed to reject
            // conservatively; successful seeds still replay through the CPU
            // locator, which produces the authoritative center positions.
            cfg.refx=int(px);cfg.refz=int(pz);cfg.skipref=source.skipref;
            const auto dynamicGroup=std::find_if(dynamicCenterGroups.begin(),dynamicCenterGroups.end(),[&](const hipreference::DynamicCenterPointGroup &g){return g.parentNode==source.save;});
            const bool includeDynamicChildren=dynamicGroup!=dynamicCenterGroups.end()&&
                configs.size()+1+dynamicGroup->childNodes.size()<=CVHIP_MAX_CONDITIONS;
            if(includeDynamicChildren) {
                cfg.groupId=source.save;cfg.conditionId=source.save;
            }
            int step=source.tol>0?source.tol:1;
            if(source.biomeSize>INT_MAX)continue;
            int minSize=int(source.biomeSize);
            if(minSize<=0)minSize=1;
            if(step==1)step=1+int(std::floor(std::sqrt(double(minSize))*0.5));
            const bool exactCenterWindow=w<=64&&h<=64&&w*h<=4096;
            const uint64_t startX=(uint64_t(w)+uint64_t(step)-1)/uint64_t(step);
            const uint64_t startZ=(uint64_t(h)+uint64_t(step)-1)/uint64_t(step);
            const bool climateOnlyLargeWindow=source.count>0&&startX<=64&&startZ<=64&&startX*startZ<=64;
            if(!exactCenterWindow&&!climateOnlyLargeWindow)continue;
            cfg.matchAny=step;
            const int para[5]={NP_TEMPERATURE,NP_HUMIDITY,NP_EROSION,NP_CONTINENTALNESS,NP_WEIRDNESS};
            for(int k=0;k<5;k++){cfg.limits[2*k]=limits[2*para[k]];cfg.limits[2*k+1]=limits[2*para[k]+1];}
            if(configs.size()<CVHIP_MAX_CONDITIONS)configs.push_back(cfg);
            remember(source.save);continue;
        }
        if(full64&&(source.rmax<=0||source.type==F_BIOME_SAMPLE)&&
           (source.type==F_BIOME||source.type==F_BIOME_SAMPLE||source.type==F_BIOME_NETHER||source.type==F_BIOME_END)&&
           ((source.type!=F_BIOME&&source.type!=F_BIOME_SAMPLE)||mc>MC_1_17)) {
            bool sample=source.type==F_BIOME_SAMPLE;
            int scale=source.step?source.step:g_filterinfo.list[source.type].grid;
            if(sample)scale=4;
            if(scale!=1&&scale!=4&&scale!=16&&scale!=64&&scale!=256)continue;
            // Fine End generation allocates internal height maps. Port its
            // allocator/tile path separately instead of using device malloc.
            if(source.type==F_BIOME_END&&scale<64)continue;
            int shift=0;while((1<<shift)<scale)shift++;
            int64_t px,pz;if(!fixedOrigin(source,px,pz))continue;
            int64_t x1,z1,x2,z2;
            if(sample&&source.rmax>0) {
                int64_t radius=int64_t(source.rmax)-1;
                x1=px-radius;z1=pz-radius;x2=px+radius;z2=pz+radius;
            } else {
                x1=px+source.x1;z1=pz+source.z1;x2=px+source.x2;z2=pz+source.z2;
            }
            if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
            auto floorScale=[&](int64_t v) {int64_t d=int64_t(1)<<shift;return v/d-(v%d<0);};
            int64_t bx1=floorScale(x1),bz1=floorScale(z1),bx2=floorScale(x2),bz2=floorScale(z2);
            int64_t bw=bx2-bx1+1,bh=bz2-bz1+1;
            if(bw<=0||bh<=0||bw>INT_MAX||bh>INT_MAX||
               (!sample&&(bw>64||bh>64||bw*bh>64||bx2==INT_MAX||bz2==INT_MAX)))continue;
            if(!source.bf.biomeToFind&&!source.bf.biomeToFindM&&!source.bf.biomeToExcl&&
               !source.bf.biomeToExclM&&!source.bf.biomeToPick&&!source.bf.biomeToPickM)continue;
            CvHipCondition cfg{};cfg.reserved=sample?CVHIP_BIOME_SAMPLE:CVHIP_BIOME;
            cfg.mc=mc;cfg.large=large;cfg.dimension=g_filterinfo.list[source.type].dim;cfg.scale=scale;
            cfg.x=int(bx1);cfg.z=int(bz1);cfg.y=scale==1?source.y:source.y>>2;
            cfg.x2=int(bx2);cfg.z2=int(bz2);
            cfg.find=source.bf.biomeToFind;cfg.findM=source.bf.biomeToFindM;
            cfg.exclude=source.bf.biomeToExcl;cfg.excludeM=source.bf.biomeToExclM;
            cfg.pick=source.bf.biomeToPick;cfg.pickM=source.bf.biomeToPickM;
            if(sample) {
                cfg.find=source.biomeToFind;cfg.findM=source.biomeToFindM;
                cfg.refx=int(px);cfg.refz=int(pz);
                if(source.rmax>0) {
                    int64_t radius=int64_t(source.rmax)-1;
                    cfg.radiusSq=uint64_t(radius*radius+1);
                }
                if(!sampling(cfg,source,(int64_t(cfg.x2)-cfg.x+1)*(int64_t(cfg.z2)-cfg.z+1)))continue;
            }
            configs.push_back(cfg);remember(source.save);continue;
        }
        if(source.type==F_SLIME&&source.rmax<=0&&source.count>=0) {
            int64_t px,pz;if(!fixedOrigin(source,px,pz))continue;
            int64_t x1=px+source.x1,x2=px+source.x2,z1=pz+source.z1,z2=pz+source.z2;
            if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
            int64_t w=(x2>>4)-(x1>>4)+1,h=(z2>>4)-(z1>>4)+1;
            if(w*h>256)continue;
            CvHipCondition cfg{};cfg.reserved=CVHIP_SLIME;
            cfg.x=int(x1>>4);cfg.x2=int(x2>>4);cfg.z=int(z1>>4);cfg.z2=int(z2>>4);
            cfg.refx=int(px>>4);cfg.refz=int(pz>>4);cfg.skipref=source.skipref;cfg.required=source.count;
            configs.push_back(cfg);remember(source.save);continue;
        }
        if(!full64||mc<=MC_1_17)continue;
        bool safeAfterSingleCenter=false;
        if(!safeNoise[source.save]&&source.type==F_CLIMATE_MINMAX&&source.octave==0&&
           source.relative>0&&size_t(source.relative)<tree.condvec.size()&&
           tree.condvec[source.relative].type==F_BIOME_CENTER&&safeNoise[source.relative]) {
            hipreference::FixedOrigin resolved;
            safeAfterSingleCenter=resolveOrigin(source,resolved);
        }
        if(dynamicCenterParent[source.save])safeAfterSingleCenter=true;
        if(!safeNoise[source.save]&&!safeAfterSingleCenter)continue;
        if(source.type==F_CLIMATE_NOISE&&source.rmax<=0) {
            hipreference::FixedOrigin climateOrigin;
            bool point=false;
            if(spiralGroup[source.save]) {
                // The spiral kernel adds the lattice coordinate to this
                // offset before mapping to a quart cell, so a ranged offset
                // could cross cell boundaries for arbitrary step values.
                point=source.x1==source.x2&&source.z1==source.z2;
            } else if(resolveOrigin(source,climateOrigin)) {
                int64_t absX1,absX2,absZ1,absZ2;
                if(hipreference::addChecked(climateOrigin.x,source.x1,&absX1)&&
                   hipreference::addChecked(climateOrigin.x,source.x2,&absX2)&&
                   hipreference::addChecked(climateOrigin.z,source.z1,&absZ1)&&
                   hipreference::addChecked(climateOrigin.z,source.z2,&absZ2)) {
                    point=hipreference::floorToQuart(absX1)==hipreference::floorToQuart(absX2)&&
                        hipreference::floorToQuart(absZ1)==hipreference::floorToQuart(absZ2);
                }
            }
            for(int para:{NP_TEMPERATURE,NP_HUMIDITY,NP_WEIRDNESS,NP_EROSION,NP_CONTINENTALNESS}) {
                if(point) {
                    if(source.limok[para][0]==INT_MIN&&source.limok[para][1]==INT_MAX&&
                       source.limex[para][0]==INT_MIN&&source.limex[para][1]==INT_MAX)continue;
                    Condition c=source;c.type=F_CLIMATE_MINMAX;c.para=para;c.octave=0;
                    c.x2=c.x1;c.z2=c.z1;c.flags=0;
                    c.minmax=Condition::E_LOCATE_MIN|Condition::E_TEST_LOWER|Condition::E_TEST_UPPER;
                    c.vmin=std::max(source.limok[para][0],source.limex[para][0]);
                    c.vmax=std::min(source.limok[para][1],source.limex[para][1]);
                    candidates.push_back(c);
                } else if(source.limex[para][0]!=INT_MIN||source.limex[para][1]!=INT_MAX) {
                    // For adaptive area scans, one guaranteed CPU sample can
                    // only disprove an exclusion interval. Keep the original
                    // F_CLIMATE_NOISE node for CPU replay on every survivor.
                    Condition c=source;c.type=F_CLIMATE_MINMAX;c.para=para;c.octave=0;
                    c.flags=0;c.minmax=Condition::E_LOCATE_MIN|Condition::E_TEST_LOWER|Condition::E_TEST_UPPER;
                    c.vmin=source.limex[para][0];c.vmax=source.limex[para][1];
                    candidates.push_back(c);
                }
            }
        } else if(source.type==F_NOISE_SAMPLE) {
            Condition c=source;c.type=F_CLIMATE_MINMAX;c.minmax|=Condition::E_LOCATE_MIN;candidates.push_back(c);
        } else candidates.push_back(source);
    }
    // Extract only fixed positional chains, with explicit bounded OR and spiral forms below.
    for(const auto &c:candidates) {
        if(c.save<=0||size_t(c.save)>=dynamicCenterParent.size())continue;
        const int dynamicParent=dynamicCenterParent[c.save];
        bool sample=tree.condvec[c.save].type==F_NOISE_SAMPLE;
        if(c.save==0||c.meta&Condition::DISABLED||c.type!=F_CLIMATE_MINMAX||
           c.para==NP_DEPTH||c.para>=NP_MAX||(c.rmax>0&&!sample)||
           !(c.minmax&(Condition::E_LOCATE_MIN|Condition::E_LOCATE_MAX)))continue;
        bool spiralPoint=spiralGroup[c.save]!=0;
        int64_t px=0,pz=0;if(!spiralPoint&&!dynamicParent&&!fixedOrigin(c,px,pz,orGroup[c.save]))continue;
        int64_t originX=px,originZ=pz,x1,z1,x2,z2;
        if(dynamicParent) {
            x1=x2=c.x1;z1=z2=c.z1;
        } else if(sample&&c.rmax>0) {
            int64_t radius=int64_t(c.rmax)-1;
            x1=px-radius;z1=pz-radius;x2=px+radius;z2=pz+radius;
        } else {x1=px+c.x1;z1=pz+c.z1;x2=px+c.x2;z2=pz+c.z2;}
        if(x1<INT_MIN||x2>INT_MAX||z1<INT_MIN||z2>INT_MAX||x2<x1||z2<z1)continue;
        auto floorQuart=[](int64_t v){return v/4-(v%4<0);};
        int64_t bx1=floorQuart(x1),bz1=floorQuart(z1),bx2=floorQuart(x2),bz2=floorQuart(z2);
        if(bx1<INT_MIN||bx2>INT_MAX||bz1<INT_MIN||bz2>INT_MAX)continue;
        CvHipCondition cfg{};
        static const uint64_t normal[6][2]={
            {0x5c7e6b29735f0d7fULL,0xf7d86f1bbc734988ULL},
            {0x81bb4d22e8dc168eULL,0xf1c8b4bea16303cdULL},
            {0x83886c9d0ae3a662ULL,0xafa638a61b42e8adULL},
            {0xd02491e6058f6fd8ULL,0x4792512c94c17a80ULL},{0,0},
            {0xefc8ef4d36102b34ULL,0x1beeeb324a0f24eaULL}};
        static const uint64_t big[4][2]={
            {0x944b0073edf549dbULL,0x4ff44347e9d22b96ULL},
            {0x71b8ab943dbd5301ULL,0xbb63ddcf39ff7a2bULL},
            {0x9a3f51a113fce8dcULL,0xee2dbd157e5dcdadULL},
            {0x8c984b1f8702a951ULL,0xead7b1f92bae535fULL}};
        static const uint64_t octave[17][2]={
            {0xc613bf766619f992ULL,0x954753f86691b86aULL},{0x7eee475a921c6cf5ULL,0xf2bd39426f8da413ULL},
            {0xfc0027cef9683114ULL,0xb758d3954dcbfdd3ULL},{0xd1fc8a05be565ecaULL,0xdc2a3915cbdda25bULL},
            {0xb198de63a8012672ULL,0x7b84cad43ef7b5a8ULL},{0x0fd787bfbc403ec3ULL,0x74a4a31ca21b48b8ULL},
            {0x36d326eed40efeb2ULL,0x5be9ce18223c636aULL},{0x082fe255f8be6631ULL,0x4e96119e22dedc81ULL},
            {0x0ef68ec68504005eULL,0x48b6bf93a2789640ULL},{0xf11268128982754fULL,0x257a1d670430b0aaULL},
            {0xe51c98ce7d1de664ULL,0x5f9478a733040c45ULL},{0x6d7b49e7e429850aULL,0x2e3063c622a24777ULL},
            {0xbd90d5377ba1b762ULL,0xc07317d419a7548dULL},{0x53d39c6752dac858ULL,0xbcd1c5a80ab65b3eULL},
            {0xb4a24d7a84e7677bULL,0x023ff9668e89b5c4ULL},{0xdffa22b534c5f608ULL,0xb9b67517d3665ca9ULL},
            {0xd50708086cef4d7cULL,0x6e1651ecc7f43309ULL}};
        const auto *salt=large&&c.para<4?big[c.para]:normal[c.para];cfg.lo=salt[0];cfg.hi=salt[1];
        // Use Cubiomes itself to describe the selected octave amplitudes/counts.
        BiomeNoise bn{};setClimateParaSeed(&bn,0,large,c.para,c.octave?c.octave:-1);
        const auto &noise=bn.climate[c.para];cfg.na=noise.octA.octcnt;cfg.nb=noise.octB.octcnt;
        cfg.amplitude=noise.amplitude;cfg.x=int(bx1);cfg.z=int(bz1);
        cfg.x2=int(bx2);cfg.z2=int(bz2);
        int64_t w=int64_t(cfg.x2)-cfg.x+1,h=int64_t(cfg.z2)-cfg.z+1;
        if(spiralPoint){cfg.x=c.x1;cfg.z=c.z1;cfg.reserved=CVHIP_SPIRAL_POINT;cfg.mc=mc;cfg.large=large;}
        if(dynamicParent) {
            // The center device pass adds these raw block offsets to the
            // exact CPU-first center before flooring to the quart grid.
            cfg.x=c.x1;cfg.z=c.z1;cfg.x2=c.x1;cfg.z2=c.z1;
            cfg.reserved=CVHIP_CENTER_POINT;cfg.dimension=DIM_OVERWORLD;
            cfg.groupId=dynamicParent;cfg.conditionId=c.save;
        }
        if(w>INT_MAX||h>INT_MAX)continue;
        // getParaRange is exhaustive (and therefore exactly reproducible by
        // evaluateGrid) below its small-area threshold.  Do not retain the
        // old 16-cell arbitrary cap; larger adaptive ranges remain CPU-only.
        if(w<=0||h<=0)continue;
        if(sample) {
            cfg.reserved=CVHIP_NOISE_SAMPLE;cfg.dimension=DIM_OVERWORLD;
            cfg.refx=int(originX);cfg.refz=int(originZ);
            if(c.rmax>0){int64_t radius=int64_t(c.rmax)-1;cfg.radiusSq=uint64_t(radius*radius+1);}
            if(!sampling(cfg,c,w*h))continue;
        } else if(w*h>1) {
            // The CPU range walker stores the grid area in an int. Do not
            // advertise a probe for rectangles whose product would overflow
            // that same representable domain.
            if(w*h>INT_MAX)continue;
            double lmax=0;for(int j=0;j<noise.octA.octcnt;j++)lmax=std::max(lmax,noise.octA.octaves[j].lacunarity);
            const bool adaptive=double(w*h)>=1000.0*std::sqrt(lmax);
            if(adaptive) {
                const Condition &original=tree.condvec[c.save];
                if(original.type==F_CLIMATE_NOISE) {
                    // A single guaranteed adaptive sample can only prove an
                    // exclusion violation for CLIMATE_NOISE. Do not infer
                    // that the required interval overlaps from one sample.
                    cfg.matchAny=4;
                } else {
                    if(c.flags&Condition::FLG_INVERT)continue;
                    const bool minLower=(c.minmax&Condition::E_LOCATE_MIN)&&
                        (c.minmax&Condition::E_TEST_LOWER);
                    const bool maxUpper=(c.minmax&Condition::E_LOCATE_MAX)&&
                        (c.minmax&Condition::E_TEST_UPPER);
                    cfg.matchAny=(minLower?1:0)|(maxUpper?2:0);
                    if(!cfg.matchAny)continue;
                }
                cfg.reserved=CVHIP_CLIMATE_PROBE;
            } else {
                cfg.reserved=CVHIP_CLIMATE_RANGE;
                cfg.matchAny=(c.minmax&Condition::E_LOCATE_MIN?1:0)|(c.minmax&Condition::E_LOCATE_MAX?2:0);
            }
        }
        cfg.lower=c.minmax&Condition::E_TEST_LOWER?c.vmin:-std::numeric_limits<double>::infinity();
        cfg.upper=c.minmax&Condition::E_TEST_UPPER?c.vmax:std::numeric_limits<double>::infinity();
        cfg.invert=bool(c.flags&Condition::FLG_INVERT);
        if(cfg.na+cfg.nb>32||std::isnan(cfg.lower)||std::isnan(cfg.upper))continue;
        int n=0;bool valid=true;
        for(const auto *side:{&noise.octA,&noise.octB})for(int i=0;i<side->octcnt;i++) {
            const auto &p=side->octaves[i];int k=int(std::lround(std::log2(p.lacunarity)))+16;
            if(k<0||k>16){valid=false;break;}
            cfg.octaves[n++]={octave[k][0],octave[k][1],p.lacunarity,p.amplitude};
        }
        if(valid){
            if(!dynamicParent)cfg.groupId=orGroup[c.save]?orGroup[c.save]:spiralGroup[c.save];
            cfg.conditionId=c.save;
            bool merged=false;
            if(!cfg.groupId&&cfg.reserved==CVHIP_CLIMATE&&!cfg.invert)for(auto &old:configs) {
                if(old.reserved!=CVHIP_CLIMATE||old.groupId||old.invert||old.lo!=cfg.lo||old.hi!=cfg.hi||
                   old.x!=cfg.x||old.z!=cfg.z||old.na!=cfg.na||old.nb!=cfg.nb||old.amplitude!=cfg.amplitude||
                   std::memcmp(old.octaves,cfg.octaves,sizeof(cfg.octaves)))continue;
                old.lower=std::max(old.lower,cfg.lower);old.upper=std::min(old.upper,cfg.upper);
                merged=true;break;
            }
            if(!merged)configs.push_back(cfg);
            remember(c.save);
            // Only actual point MINMAX nodes have the existing CPU skip contract.
            if(!cfg.groupId&&tree.condvec[c.save].type==F_CLIMATE_MINMAX&&cfg.reserved==CVHIP_CLIMATE)ids.push_back(c.save);
            if(configs.size()==CVHIP_MAX_CONDITIONS)break;
        }
    }
    for(const auto &group:dynamicCenterGroups) {
        bool complete=false;
        for(const auto &cfg:configs)if(cfg.reserved==CVHIP_BIOME_CENTER&&
            cfg.conditionId==group.parentNode&&cfg.groupId==group.parentNode){complete=true;break;}
        for(int child:group.childNodes) {
            bool found=false;
            for(const auto &cfg:configs)if(cfg.reserved==CVHIP_CENTER_POINT&&
                cfg.groupId==group.parentNode&&cfg.conditionId==child){found=true;break;}
            if(!found){complete=false;break;}
        }
        if(complete) {
            for(int child:group.childNodes)remember(child);
            continue;
        }
        configs.erase(std::remove_if(configs.begin(),configs.end(),[&](const CvHipCondition &cfg){
            return cfg.reserved==CVHIP_CENTER_POINT&&cfg.groupId==group.parentNode;
        }),configs.end());
        for(auto &cfg:configs)if(cfg.reserved==CVHIP_BIOME_CENTER&&cfg.conditionId==group.parentNode)cfg.groupId=0;
        compiledIds.erase(std::remove_if(compiledIds.begin(),compiledIds.end(),[&](int id){
            return std::find(group.childNodes.begin(),group.childNodes.end(),id)!=group.childNodes.end();
        }),compiledIds.end());
    }
    for(int parent:orParents) {
        const auto &children=tree.references[parent];
        bool complete=configs.size()+1<=CVHIP_MAX_CONDITIONS;
        for(int child:children) {
            bool found=false;
            for(const auto &cfg:configs)if(cfg.groupId==parent&&cfg.conditionId==child){found=true;break;}
            if(!found){complete=false;break;}
        }
        if(complete) {CvHipCondition marker{};marker.reserved=CVHIP_OR_CLIMATE;marker.groupId=parent;configs.push_back(marker);}
        else {
            configs.erase(std::remove_if(configs.begin(),configs.end(),[&](const CvHipCondition &cfg){return cfg.groupId==parent;}),configs.end());
            ids.erase(std::remove_if(ids.begin(),ids.end(),[&](int id){return orGroup[id]==parent;}),ids.end());
            compiledIds.erase(std::remove_if(compiledIds.begin(),compiledIds.end(),[&](int id){return orGroup[id]==parent;}),compiledIds.end());
        }
    }
    for(int parent:spiralParents) {
        const auto &children=tree.references[parent];
        bool hasMarker=false,complete=configs.size()+1<=CVHIP_MAX_CONDITIONS;
        for(const auto &cfg:configs)if(cfg.reserved==CVHIP_SPIRAL_CLIMATE&&cfg.groupId==parent){hasMarker=true;break;}
        complete&=hasMarker;
        for(int child:children) {
            bool found=false;
            for(const auto &cfg:configs)if(cfg.groupId==parent&&cfg.conditionId==child&&cfg.reserved==CVHIP_SPIRAL_POINT){found=true;break;}
            if(!found){complete=false;break;}
        }
        if(complete)continue;
        configs.erase(std::remove_if(configs.begin(),configs.end(),[&](const CvHipCondition &cfg){return cfg.groupId==parent;}),configs.end());
        ids.erase(std::remove_if(ids.begin(),ids.end(),[&](int id){return spiralGroup[id]==parent;}),ids.end());
        compiledIds.erase(std::remove_if(compiledIds.begin(),compiledIds.end(),[&](int id){return spiralGroup[id]==parent;}),compiledIds.end());
    }
    if (configs.empty()) return;
    if(searchSemantics)configs[0].preserveCpuZero=1;
    bool biomes=false,cheap=false;
    for(const auto &cfg:configs) {
        bool advanced=cfg.reserved==CVHIP_BIOME||cfg.reserved==CVHIP_NOISE_SAMPLE||cfg.reserved==CVHIP_BIOME_SAMPLE||cfg.reserved==CVHIP_BIOME_CENTER||cfg.reserved==CVHIP_SPIRAL_CLIMATE||cfg.reserved==CVHIP_SPIRAL_POINT||cfg.reserved==CVHIP_CENTER_POINT;
        biomes|=advanced;cheap|=!advanced;
    }
    // The measured pure-biome fixture is slower on HIP than 16 CPU workers.
    // Use generation only behind an existing cheap filter in real searches.
    if(searchSemantics&&biomes&&!cheap)return;
    uint64_t climateWork=0, probeWork=0;
    bool hasClimateRange=false, hasClimateProbe=false;
    for(const auto &cfg:configs)if(cfg.reserved==CVHIP_CLIMATE_RANGE||cfg.reserved==CVHIP_CLIMATE_PROBE) {
        const uint64_t width=uint64_t(int64_t(cfg.x2)-int64_t(cfg.x)+1);
        const uint64_t height=uint64_t(int64_t(cfg.z2)-int64_t(cfg.z)+1);
        uint64_t cells=width*height;
        if(cfg.reserved==CVHIP_CLIMATE_PROBE) {
            double minLacunarity=INFINITY;
            for(int i=0;i<cfg.na;i++)minLacunarity=std::min(minLacunarity,cfg.octaves[i].frequency);
            int step=0;
            cells=16; // conservative if host geometry cannot mirror device step
            if(cvhip_probe_policy::coarseStep(minLacunarity,&step)) {
                const unsigned prefix=cvhip_probe_policy::coarsePrefixCount(width,height,step);
                if(prefix)cells=prefix;
            }
        }
        const uint64_t work=cells*uint64_t(std::max(1,cfg.na+cfg.nb));
        climateWork=std::min<uint64_t>(UINT_MAX,climateWork+work);
        if(cfg.reserved==CVHIP_CLIMATE_PROBE) {
            hasClimateProbe=true;
            probeWork=std::min<uint64_t>(UINT_MAX,probeWork+work);
        } else hasClimateRange=true;
    }
    if(climateWork) {
        // Keep each dispatch below roughly four million cell/octave samples;
        // this is deliberately conservative for long-lived GUI scans and
        // prevents a 700-cell grid from monopolising the driver.
        // Do not impose an arbitrary 256-seed floor: a session with many
        // wide climate ranges can otherwise exceed the intended dispatch
        // budget by orders of magnitude.  A batch of one is slow, but is the
        // safe upper-bound-preserving fallback for an unusually large range.
        const unsigned safeBatch=hasClimateProbe&&!hasClimateRange
            ? cvhip_batch_policy::probeOnlyBatchLimit(CVHIP_MAX_BATCH,probeWork)
            : std::max(1U,std::min(65536U,unsigned(4000000U/climateWork)));
        deviceBatchLimit=std::min(deviceBatchLimit,safeBatch);
    }
    for(const auto &cfg:configs)if(cfg.reserved==CVHIP_BIOME_CENTER) {
        const uint64_t cells=uint64_t(cfg.x2-cfg.x+1)*uint64_t(cfg.z2-cfg.z+1);
        // Mixed sessions run the cheap filters on the full physical batch and
        // compact expensive center checks into bounded waves in cvhip.hip.
        // Only a pure advanced session needs the small direct-dispatch cap.
        if(cells>64&&!cheap)deviceBatchLimit=std::min(deviceBatchLimit,512U);
    }
    if(biomes&&!cheap)deviceBatchLimit=std::min(deviceBatchLimit,512U);
    backendBatchLimit=deviceBatchLimit;
    if(requestedBatchLimit)deviceBatchLimit=std::min(deviceBatchLimit,requestedBatchLimit);
    runtimeBatchLimit.store(deviceBatchLimit);
    stopping = false;
    started=std::promise<bool>();auto ready=started.get_future();
    try {
        coordinator = std::thread(&HipSearch::dispatch, this);
        if(!ready.get()) {qWarning("GPU BOOST: context allocation failed; using CPU.");shutdown();}
    } catch(const std::exception &e) {
        qWarning("GPU BOOST: startup failed (%s); using CPU.",e.what());
        shutdown();
    }
}

HipBatch HipSearch::filterBatch(uint64_t start, unsigned count, const uint64_t *seeds) {
    HipBatch fallback; fallback.count = count;
    if (!enabled || !count || count > runtimeBatchLimit.load() || (seeds && count > CVHIP_MAX_LIST_BATCH)) return fallback;
    if (!seeds && uint64_t(count - 1) > UINT64_MAX - start) return fallback;
    try {
    auto request = std::make_shared<Request>();
    request->start = start; request->count = count;
    if (seeds) request->seeds.assign(seeds, seeds + count);
    auto answer = request->result.get_future();
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping || !enabled) return fallback;
        requests.push_back(request);
    }
    queueReady.notify_one();
    HipBatch result = answer.get();
    // Device event time excludes host workers waiting in the request queue but
    // includes transfers, GPU kernels, and result collection for this batch.
    double previous = lastDeviceElapsedMs.load();
    while (result.deviceElapsedMilliseconds > previous &&
        !lastDeviceElapsedMs.compare_exchange_weak(previous, result.deviceElapsedMilliseconds))
    {}
    return result;
    } catch(const std::exception &e) {
        qWarning("GPU BOOST: batch allocation/dispatch failed (%s); replaying on CPU.",e.what());
        cancel();
        return fallback;
    }
}

void HipSearch::dispatch() {
    struct Pending { unsigned slot; std::shared_ptr<Request> request; };
    std::deque<Pending> pending;
    std::shared_ptr<Request> activeRequest;
    auto replay = [](const std::shared_ptr<Request> &r) {
        if(!r)return;
        HipBatch b;b.count=r->count;
        try {r->result.set_value(std::move(b));}catch(const std::future_error &){}
    };
    try {
    // All allocations, submissions and cleanup belong to the same HIP thread.
    context=create(configs.data(),unsigned(configs.size()));
    enabled=context!=nullptr;
    started.set_value(context!=nullptr);
    if(!context)return;
    bool busy[2] = {false,false};
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (pending.empty() && requests.empty() && !stopping)
                queueReady.wait(lock,[&]{return stopping || !requests.empty();});
            if (stopping) {
                while (!requests.empty()) { auto r=requests.front(); requests.pop_front(); replay(r); }
            } else {
                for (unsigned slot=0;slot<maxInflight;slot++) if (!busy[slot] && !requests.empty()) {
                    auto r=requests.front(); requests.pop_front();
                    // filterBatch validates the cap before accepting a request.
                    // A queued request was valid when accepted; preserve its
                    // original GPU dispatch even if the runtime ceiling has
                    // since fallen. New requests observe the lower ceiling.
                    activeRequest=r;
                    lock.unlock();
                    int ok=submit(context,slot,r->start,r->seeds.empty()?nullptr:r->seeds.data(),r->count);
                    lock.lock();
                    if (!ok) {
                        qWarning("GPU BOOST: submission failed; unfinished batches will replay on CPU.");
                        enabled=false; stopping=true; replay(r); break;
                    }
                    busy[slot]=true; pending.push_back({slot,r});
                    activeRequest.reset();
                }
            }
        }
        if (pending.empty()) {
            if (stopping) {
                std::lock_guard<std::mutex> lock(queueMutex);
                while (!requests.empty()) {auto r=requests.front();requests.pop_front();replay(r);}
                break;
            }
            continue;
        }
        auto p=pending.front(); pending.pop_front();
        activeRequest=p.request;
        HipBatch b; b.count=p.request->count;
        b.data.resize((b.count+15)/16);
        CvHipBatchInfo info{};
        int ok=collect(context,p.slot,&info,b.data.data());
        busy[p.slot]=false;
        if (ok)
            b.deviceElapsedMilliseconds = double(info.reserved) / 1000.0;
        if(ok)
            latestDeviceElapsedMs.store(b.deviceElapsedMilliseconds);
        if(ok&&info.count==b.count&&info.survivors<=b.count&&adaptiveSafety) {
            const unsigned cap=runtimeBatchLimit.load();
            const unsigned reduced=cvhip_batch_policy::reducedRuntimeCap(
                cap,b.count,b.deviceElapsedMilliseconds);
            if(reduced<cap)runtimeBatchLimit.store(reduced);
        }
        if (ok && !stopping && info.count==b.count && info.survivors<=b.count &&
            (info.encoding==CVHIP_PACKED || (info.encoding==CVHIP_OFFSETS && info.survivors<=b.data.size()))) {
            b.encoding=info.encoding; b.survivors=info.survivors;
            b.data.resize(info.encoding==CVHIP_OFFSETS?info.survivors:(b.count+15)/16);
            if (b.encoding==CVHIP_OFFSETS)
                std::sort(b.data.begin(),b.data.end(),[](uint32_t a,uint32_t c){return (a&0x7fffffffU)<(c&0x7fffffffU);});
        } else {
            if (!ok) qWarning("GPU BOOST: execution/transfer failed; unfinished batches will replay on CPU.");
            enabled=false; stopping=true; b.data.clear();
        }
        p.request->result.set_value(std::move(b));
        activeRequest.reset();
    }
    } catch(const std::exception &e) {
        qWarning("GPU BOOST: coordinator failed (%s); replaying on CPU.",e.what());
        cancel();
        try {started.set_value(false);} catch(const std::future_error &) {}
        std::lock_guard<std::mutex> lock(queueMutex);
        replay(activeRequest);
        for(const auto &p:pending)replay(p.request);
        for(const auto &r:requests)replay(r);
        requests.clear();
    }
    if(context)
        destroy(context);
    context=nullptr;
}

std::vector<unsigned char> HipSearch::filter(uint64_t start,unsigned count) {
    auto batch=filterBatch(start,count);
    std::vector<unsigned char> mask(count,0);
    unsigned cursor=0,index=0,state=0;
    while(batch.next(cursor,index,state))mask[index]=state;
    return mask;
}
