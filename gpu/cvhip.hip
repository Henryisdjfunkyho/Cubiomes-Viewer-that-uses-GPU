#ifdef CVHIP_CUDA
#include "cuda/cuda_compat.h"
#else
#include <hip/hip_runtime.h>
#endif
#include "cvhip.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include <new>
#include <cstdio>
#include <cstring>
#include "noise_device.h"
#include "structure_extra.h"

#if defined(_WIN32) && !defined(__HIP_DEVICE_COMPILE__)
#define CVHIP_EXPORT __declspec(dllexport)
#else
#define CVHIP_EXPORT
#endif

void *cvhip_create_biomes(const CvHipCondition*,unsigned);
void cvhip_destroy_biomes(void*);
hipError_t cvhip_launch_biome_filter(void*,hipStream_t,uint64_t,unsigned,const uint64_t*,const CvHipCondition*,unsigned,unsigned char*,const uint32_t*,unsigned char*,size_t,unsigned);
unsigned cvhip_biome_center_cells(void*);
size_t cvhip_biome_center_stride(void*);

namespace {
#ifdef CVHIP_CUDA
#define CVHIP_LAUNCH(kernel, grid, block, shared, stream, ...) \
    kernel<<<grid, block, shared, stream>>>(__VA_ARGS__)
#define CVHIP_LAUNCH_TEMPLATE(kernel, grid, block, shared, stream, ...) \
    kernel<<<grid, block, shared, stream>>>(__VA_ARGS__)
#else
#define CVHIP_LAUNCH(kernel, grid, block, shared, stream, ...) \
    hipLaunchKernelGGL(kernel, grid, block, shared, stream, __VA_ARGS__)
#define CVHIP_LAUNCH_TEMPLATE(kernel, grid, block, shared, stream, ...) \
    hipLaunchKernelGGL((kernel), grid, block, shared, stream, __VA_ARGS__)
#endif
__device__ uint64_t rot(uint64_t a,int n) { return (a<<n)|(a>>(64-n)); }
__device__ uint64_t next(uint64_t &a,uint64_t &b) {
    uint64_t r=rot(a+b,17)+a, v=b^a;
    a=rot(a,49)^v^(v<<21); b=rot(v,28); return r;
}
__device__ void seedRng(uint64_t seed,uint64_t &a,uint64_t &b) {
    a=seed^0x6a09e667f3bcc909ULL; b=a+0x9e3779b97f4a7c15ULL;
    a=(a^(a>>30))*0xbf58476d1ce4e5b9ULL; b=(b^(b>>30))*0xbf58476d1ce4e5b9ULL;
    a=(a^(a>>27))*0x94d049bb133111ebULL; b=(b^(b>>27))*0x94d049bb133111ebULL;
    a^=a>>31; b^=b>>31;
}
__device__ double uniform(uint64_t &a,uint64_t &b) { return (next(a,b)>>11)*0x1.0p-53; }
__device__ unsigned bounded(uint64_t &a,uint64_t &b,unsigned bound) {
    uint64_t product=uint64_t(uint32_t(next(a,b)))*bound;
    uint32_t low=uint32_t(product);
    if(low<bound) { unsigned threshold=(-bound)%bound; while(low<threshold) {
        product=uint64_t(uint32_t(next(a,b)))*bound; low=uint32_t(product);
    }}
    return unsigned(product>>32);
}
__device__ double fade(double v) {return v*v*v*(v*(v*6-15)+10);}
__device__ double mix(double t,double a,double b) {return a+t*(b-a);}
__device__ double grad(unsigned i,double x,double y,double z) {
    switch(i&15) {
    case 0:return x+y;case 1:return -x+y;case 2:return x-y;case 3:return -x-y;
    case 4:return x+z;case 5:return -x+z;case 6:return x-z;case 7:return -x-z;
    case 8:return y+z;case 9:return -y+z;case 10:return y-z;case 11:return -y-z;
    case 12:return x+y;case 13:return -y+z;case 14:return -x+y;default:return -y-z;
    }
}
// Java Random and slime arithmetic mirror cubiomes/finders.h and rng.h.
// Explicit unsigned products preserve Java's signed 32-bit wraparound.
__device__ int javaNextInt(uint64_t &seed,int bound) {
    if((bound&(bound-1))==0) {
        seed=(seed*0x5deece66dULL+11)&((1ULL<<48)-1);
        return int((uint64_t(bound)*(seed>>17))>>31);
    }
    uint32_t bits,value;
    do {
        seed=(seed*0x5deece66dULL+11)&((1ULL<<48)-1);
        bits=unsigned(seed>>17);value=bits%unsigned(bound);
    } while(int32_t(bits-value+unsigned(bound-1))<0);
    return int(value);
}
__device__ bool slime(uint64_t seed,int x,int z) {
    uint32_t cx=uint32_t(x),cz=uint32_t(z);
    uint64_t rnd=seed;
    rnd+=uint64_t(int64_t(int32_t(cx*0x5ac0dbU)));
    rnd+=uint64_t(int64_t(int32_t(cx*cx*0x4c1906U)));
    rnd+=uint64_t(int64_t(int32_t(cz*0x5f24fU)));
    rnd+=uint64_t(int64_t(int32_t(cz*cz)))*0x4307a7ULL;
    rnd=((rnd^0x3ad8025fULL)^0x5deece66dULL)&((1ULL<<48)-1);
    return javaNextInt(rnd,10)==0;
}
__device__ bool slimeCondition(uint64_t seed,const CvHipCondition &cfg) {
    int matches=0;
    for(int z=cfg.z;z<=cfg.z2;z++)for(int x=cfg.x;x<=cfg.x2;x++) {
        if(cfg.skipref&&x==cfg.refx&&z==cfg.refz)continue;
        if(!slime(seed,x,z))continue;
        if(cfg.required==0)return false;
        if(++matches>=cfg.required)return true;
    }
    return cfg.required==0;
}
__device__ int floorDivide(int value,int divisor) {
    int q=value/divisor;return q-(value%divisor<0);
}
// Placement only. Biome, terrain, variant and result-location checks stay on CPU.
// Match the pinned Cubiomes inline helpers, including their modulo shortcut.
__device__ bool structureCondition(uint64_t seed,const CvHipCondition &c) {
    int regionBlocks=c.na*16,matches=0;
    for(int rz=floorDivide(c.z,regionBlocks);rz<=floorDivide(c.z2,regionBlocks);rz++)
    for(int rx=floorDivide(c.x,regionBlocks);rx<=floorDivide(c.x2,regionBlocks);rx++) {
        if(c.conditionId==cvhip_extra::FORTRESS_1_18 || c.conditionId==cvhip_extra::BASTION_1_18 ||
           c.conditionId==cvhip_extra::OUTPOST || c.conditionId==cvhip_extra::END_CITY) {
            cvhip_extra::StructureConfig extra{int32_t(c.lo),c.na,c.nb};
            const cvhip_extra::Placement p=cvhip_extra::placement(seed,c.conditionId,extra,rx,rz);
            if(!p.valid || p.x<c.x || p.x>c.x2 || p.z<c.z || p.z>c.z2)continue;
            if(c.skipref&&p.x==c.refx&&p.z==c.refz)continue;
            if(++matches>=c.required)return true;
            continue;
        }
        uint64_t s=(seed+uint64_t(rx)*341873128712ULL+uint64_t(rz)*132897987541ULL+c.lo)^0x5deece66dULL;
        int offsets[2];
        for(int axis=0;axis<2;axis++) {
            s=(s*0x5deece66dULL+11)&((1ULL<<48)-1);
            unsigned bits=unsigned(s>>17);
            if(c.invert) {
                int first=bits%unsigned(c.nb);
                s=(s*0x5deece66dULL+11)&((1ULL<<48)-1);
                offsets[axis]=(first+int((s>>17)%unsigned(c.nb)))>>1;
            } else offsets[axis]=(c.nb&(c.nb-1))?bits%unsigned(c.nb):int((uint64_t(c.nb)*bits)>>31);
        }
        int x=int((uint64_t(rx)*unsigned(c.na)+unsigned(offsets[0]))<<4);
        int z=int((uint64_t(rz)*unsigned(c.na)+unsigned(offsets[1]))<<4);
        if(x<c.x||x>c.x2||z<c.z||z>c.z2)continue;
        if(c.skipref&&x==c.refx&&z==c.refz)continue;
        if(++matches>=c.required)return true;
    }
    return false;
}
__device__ double perlin(uint64_t a,uint64_t b,double x,double z,unsigned char *d) {
    double ox=uniform(a,b)*256, oy=uniform(a,b)*256, oz=uniform(a,b)*256;
    // Interleave permutations by lane in LDS. A private 257-byte array spills
    // to device scratch and makes the shuffle much more expensive.
    for(unsigned i=0;i<256;i++)d[i*64]=i;
    for(unsigned i=0;i<256;i++){unsigned j=i+bounded(a,b,256-i);unsigned char v=d[i*64];d[i*64]=d[j*64];d[j*64]=v;}d[256*64]=d[0];
    x+=ox;z+=oz;double ix=floor(x), iz=floor(z), iy=floor(oy);
    x-=ix;z-=iz;double y=oy-iy, tx=fade(x),ty=fade(y),tz=fade(z);
    unsigned hx=unsigned(int(ix))&255,hz=unsigned(int(iz))&255,hy=unsigned(int(iy))&255;
    unsigned ax=(d[hx*64]+hy)&255,bx=(d[(hx+1)*64]+hy)&255;
    unsigned aa=d[ax*64],ab=d[(ax+1)*64],ba=d[bx*64],bb=d[(bx+1)*64];
    double a0=grad(d[((aa+hz)&255)*64],x,y,z),a1=grad(d[((ba+hz)&255)*64],x-1,y,z);
    double b0=grad(d[((ab+hz)&255)*64],x,y-1,z),b1=grad(d[((bb+hz)&255)*64],x-1,y-1,z);
    double c0=grad(d[((aa+hz+1)&255)*64],x,y,z-1),c1=grad(d[((ba+hz+1)&255)*64],x-1,y,z-1);
    double d0=grad(d[((ab+hz+1)&255)*64],x,y-1,z-1),d1=grad(d[((bb+hz+1)&255)*64],x-1,y-1,z-1);
    return mix(tz,mix(ty,mix(tx,a0,a1),mix(tx,b0,b1)),mix(ty,mix(tx,c0,c1),mix(tx,d0,d1)));
}
__device__ bool climatePointPass(uint64_t lo,uint64_t hi,const CvHipCondition &cfg,unsigned char *d) {
    uint64_t a=lo^cfg.lo,b=hi^cfg.hi;double sums[2]={0,0};unsigned index=0;
    for(int side=0;side<2;side++) {
        uint64_t sl=next(a,b),sh=next(a,b);int n=side?cfg.nb:cfg.na;
        for(int j=0;j<n;j++,index++) {
            const auto &o=cfg.octaves[index];double f=side?337.0/331.0:1.0;
            sums[side]+=o.amplitude*perlin(sl^o.lo,sh^o.hi,(cfg.x*f)*o.frequency,(cfg.z*f)*o.frequency,d);
        }
    }
    double value=(sums[0]+sums[1])*cfg.amplitude*10000.0;
    if(!isfinite(value)||fabs(value-cfg.lower)<1e-6||fabs(value-cfg.upper)<1e-6)return true;
    return cfg.invert?(value<=cfg.lower||value>=cfg.upper):(value>=cfg.lower&&value<=cfg.upper);
}
__global__ void filter(uint64_t start,unsigned count,const CvHipCondition *program,unsigned ncfg,unsigned char *mask,const uint64_t *seeds) {
    __shared__ unsigned char permutations[257*64];
    unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;
    if(program[0].preserveCpuZero&&(seeds?seeds[i]:start+i)==0){mask[i]=2;return;}
    bool uncertain=false;
    uint64_t rootA,rootB;seedRng(seeds?seeds[i]:start+i,rootA,rootB);
    uint64_t lo=next(rootA,rootB),hi=next(rootA,rootB);
    for(unsigned condition=0;condition<ncfg;condition++) {
    const CvHipCondition &cfg=program[condition];
    if(cfg.reserved==CVHIP_BIOME||cfg.reserved==CVHIP_NOISE_SAMPLE||cfg.reserved==CVHIP_BIOME_SAMPLE||cfg.reserved==CVHIP_BIOME_CENTER||cfg.reserved==CVHIP_SPIRAL_CLIMATE||cfg.reserved==CVHIP_SPIRAL_POINT||cfg.reserved==CVHIP_CENTER_POINT) {
#ifdef CVHIP_CUDA
        // CUDA currently evaluates the cheap necessary conditions only. Never
        // mark a seed exact when its Cubiomes biome condition ran on the CPU.
        uncertain=true;
#endif
        continue;
    }
    if(cfg.reserved==CVHIP_OR_CLIMATE) {
        bool any=false;
        for(unsigned k=0;k<ncfg;k++)if(program[k].groupId==cfg.groupId&&program[k].reserved==CVHIP_CLIMATE) {
            if(climatePointPass(lo,hi,program[k],permutations+threadIdx.x)){any=true;break;}
        }
        if(!any){mask[i]=0;return;}
        uncertain=true;continue;
    }
    if(cfg.groupId)continue;
    if(cfg.reserved==CVHIP_SLIME) {
        if(!slimeCondition(seeds?seeds[i]:start+i,cfg)){mask[i]=0;return;}
        continue;
    }
    if(cfg.reserved==CVHIP_STRUCTURE) {
        if(!structureCondition(seeds?seeds[i]:start+i,cfg)){mask[i]=0;return;}
        continue;
    }
    if(cfg.reserved==CVHIP_CLIMATE_RANGE) {
        double low,high;cvhip_noise::evaluateGrid(lo,hi,cfg,permutations+threadIdx.x,low,high);
        double evalmin=cfg.matchAny&1?low:high,evalmax=cfg.matchAny&2?high:low;
        bool ambiguous=!isfinite(evalmin)||!isfinite(evalmax)||fabs(evalmin-cfg.lower)<1e-6||fabs(evalmax-cfg.upper)<1e-6;
        uncertain|=ambiguous;
        bool keep=cfg.invert?!(evalmin>cfg.lower&&evalmax<cfg.upper):evalmin>=cfg.lower&&evalmax<=cfg.upper;
        if(!ambiguous&&!keep){mask[i]=0;return;}
        continue;
    }
    if(cfg.reserved==CVHIP_CLIMATE_PROBE) {
#ifdef CVHIP_CUDA
        // CUDA has no adaptive-range probe implementation yet. Keep the
        // original CPU predicate authoritative instead of guessing.
        uncertain=true;
#else
        if(cvhip_noise::rejectAdaptiveRangeAtCoarsePrefix(lo,hi,cfg,permutations+threadIdx.x)) {
            mask[i]=0;return;
        }
        uncertain=true;
#endif
        continue;
    }
    uint64_t a=lo^cfg.lo,b=hi^cfg.hi;
    double sums[2]={0,0};unsigned index=0;
    for(int side=0;side<2;side++) {
        uint64_t sl=next(a,b),sh=next(a,b);int n=side?cfg.nb:cfg.na;
        for(int j=0;j<n;j++,index++) {
            const auto &o=cfg.octaves[index];double f=side?337.0/331.0:1.0;
            double x=(cfg.x*f)*o.frequency,z=(cfg.z*f)*o.frequency;
            sums[side]+=o.amplitude*perlin(sl^o.lo,sh^o.hi,x,z,permutations+threadIdx.x);
        }
    }
    double value=(sums[0]+sums[1])*cfg.amplitude*10000.0;
    // CPU checks the exact boundary. Keep any numerical ambiguity.
    bool ambiguous=!isfinite(value)||fabs(value-cfg.lower)<1e-6||fabs(value-cfg.upper)<1e-6;
    uncertain|=ambiguous;
    bool keep=ambiguous;
    keep|=cfg.invert?(value<=cfg.lower||value>=cfg.upper):(value>=cfg.lower&&value<=cfg.upper);
    if(!keep){mask[i]=0;return;}
    }
    mask[i]=uncertain?2:1;
}
}
extern "C" CVHIP_EXPORT int cvhip_available() {
    int n=0;return hipGetDeviceCount(&n)==hipSuccess&&n>0;
}
extern "C" CVHIP_EXPORT size_t cvhip_device_identity(char *buffer,size_t capacity) {
    int device=0,runtime=0,driver=0;
    hipDeviceProp_t properties{};
    if(hipGetDevice(&device)!=hipSuccess||hipGetDeviceProperties(&properties,device)!=hipSuccess)
        return 0;
    if(hipRuntimeGetVersion(&runtime)!=hipSuccess||hipDriverGetVersion(&driver)!=hipSuccess)
        return 0;
#ifdef CVHIP_CUDA
    const char *arch="cuda";
#else
    const char *arch=properties.gcnArchName;
#endif
    const int required=std::snprintf(buffer,capacity,"%s|%s|runtime=%d|driver=%d",
        properties.name,arch,runtime,driver);
    return required<0?0:size_t(required)+1;
}
namespace {
__global__ void packDecisions(const unsigned char *mask,unsigned count,uint32_t *packed,unsigned *total) {
    __shared__ unsigned sums[256];
    unsigned word=blockIdx.x*blockDim.x+threadIdx.x;
    uint32_t value=0;unsigned matches=0;
    for(unsigned j=0;j<16;j++) {
        unsigned i=word*16+j;
        if(i<count) {unsigned state=mask[i];value|=state<<(j*2);matches+=state!=0;}
    }
    if(word<(count+15)/16)packed[word]=value;
    sums[threadIdx.x]=matches;__syncthreads();
    for(unsigned step=128;step;step/=2){if(threadIdx.x<step)sums[threadIdx.x]+=sums[threadIdx.x+step];__syncthreads();}
    if(threadIdx.x==0)atomicAdd(total,sums[0]);
}

__global__ void compactDecisions(const unsigned char *mask,unsigned count,uint32_t *offsets,unsigned *total) {
    __shared__ unsigned bases[2];
    unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
    unsigned state=i<count?mask[i]:0;
    unsigned lane=threadIdx.x%warpSize,warp=threadIdx.x/warpSize;
#if defined(__HIP_PLATFORM_NVIDIA__)
    uint64_t bits=__ballot_sync(0xffffffffU,state!=0);
#else
    uint64_t bits=__ballot(state!=0);
#endif
    if(lane==0)bases[warp]=bits?atomicAdd(total,unsigned(__popcll(bits))):0;
    __syncthreads();
    if(state) {
        unsigned before=unsigned(__popcll(bits&((uint64_t(1)<<lane)-1)));
        offsets[bases[warp]+before]=i|(state==2?0x80000000U:0);
    }
}

struct BatchSlot {
    hipStream_t stream=nullptr;
    hipEvent_t started=nullptr;
    hipEvent_t ready=nullptr;
    unsigned char *mask=nullptr;
    uint32_t *packed=nullptr,*offsets=nullptr,*host=nullptr;
    unsigned *total=nullptr,*hostTotal=nullptr;
    uint64_t *seeds=nullptr,*hostSeeds=nullptr;
    unsigned char *centerScratch=nullptr;
    unsigned capacity=0,seedCapacity=0,centerScratchCapacity=0,count=0;
    bool busy=false;
    bool list=false;
    uint64_t start=0;
};
struct BatchContext {
    CvHipCondition *program=nullptr;
    unsigned ncfg=0;
    bool failed=false;
    bool biomes=false;
    bool cheap=false;
    unsigned advancedWave=128;
    void *biomeContext=nullptr;
    BatchSlot slots[2];
};
bool checked(BatchContext *c,hipError_t result,const char *stage) {
    if(result==hipSuccess)return true;
    c->failed=true;
    std::fprintf(stderr,"GPU BOOST %s: %s (%d). CPU replay required.\n",
#ifdef CVHIP_CUDA
        "CUDA",
#else
        "HIP",
#endif
        hipGetErrorString(result),int(result));
    return false;
}
void freeSlot(BatchSlot &s) {
    if(s.stream)(void)hipStreamSynchronize(s.stream);
    if(s.mask)(void)hipFree(s.mask);if(s.packed)(void)hipFree(s.packed);if(s.offsets)(void)hipFree(s.offsets);
    if(s.host)(void)hipHostFree(s.host);if(s.total)(void)hipFree(s.total);if(s.hostTotal)(void)hipHostFree(s.hostTotal);
    if(s.seeds)(void)hipFree(s.seeds);if(s.hostSeeds)(void)hipHostFree(s.hostSeeds);
    if(s.centerScratch)(void)hipFree(s.centerScratch);
    if(s.started)(void)hipEventDestroy(s.started);if(s.ready)(void)hipEventDestroy(s.ready);if(s.stream)(void)hipStreamDestroy(s.stream);
    s=BatchSlot{};
}
bool reserve(BatchContext *c,BatchSlot &s,unsigned count,bool list) {
    if(count>s.capacity) {
        if(s.mask&&!checked(c,hipFree(s.mask),"release old mask"))return false;
        s.mask=nullptr;
        if(s.packed&&!checked(c,hipFree(s.packed),"release old bitmap"))return false;
        s.packed=nullptr;
        if(s.offsets&&!checked(c,hipFree(s.offsets),"release old offsets"))return false;
        s.offsets=nullptr;
        if(s.host&&!checked(c,hipHostFree(s.host),"release old host output"))return false;
        if(s.centerScratch&&!checked(c,hipFree(s.centerScratch),"release old center scratch"))return false;
        s.mask=nullptr;s.packed=nullptr;s.offsets=nullptr;s.host=nullptr;s.centerScratch=nullptr;s.capacity=0;s.centerScratchCapacity=0;
        size_t bytes=size_t((count+15)/16)*sizeof(uint32_t);
        if(!checked(c,hipMalloc(&s.mask,count),"allocate mask")||
           !checked(c,hipMalloc(&s.packed,bytes),"allocate bitmap")||
           !checked(c,hipMalloc(&s.offsets,bytes),"allocate offsets")||
           !checked(c,hipHostMalloc(&s.host,bytes),"allocate pinned output"))return false;
        if(c->biomeContext&&cvhip_biome_center_stride(c->biomeContext)) {
            // A cheap prefilter can submit millions of seeds, but advanced
            // biome verification is always compacted into <=128-job waves.
            // Never size exact-center scratch from the original batch count.
            const unsigned scratchCount=std::min(count,512U);
            const size_t scratchBytes=size_t(scratchCount)*cvhip_biome_center_stride(c->biomeContext);
            if(!checked(c,hipMalloc(&s.centerScratch,scratchBytes),"allocate center scratch"))return false;
            s.centerScratchCapacity=scratchCount;
        }
        s.capacity=count;
    }
    if(list&&count>s.seedCapacity) {
        if(s.seeds&&!checked(c,hipFree(s.seeds),"release old seeds"))return false;
        s.seeds=nullptr;
        if(s.hostSeeds&&!checked(c,hipHostFree(s.hostSeeds),"release old pinned seeds"))return false;
        s.seeds=nullptr;s.hostSeeds=nullptr;s.seedCapacity=0;
        size_t bytes=size_t(count)*sizeof(uint64_t);
        if(!checked(c,hipMalloc(&s.seeds,bytes),"allocate seed list")||
           !checked(c,hipHostMalloc(&s.hostSeeds,bytes),"allocate pinned seeds"))return false;
        s.seedCapacity=count;
    }
    return true;
}
}

extern "C" CVHIP_EXPORT unsigned cvhip_abi_version() {return CVHIP_ABI_VERSION;}
extern "C" CVHIP_EXPORT void cvhip_destroy(void *handle) {
    auto *c=static_cast<BatchContext*>(handle);if(!c)return;
    (void)hipSetDevice(0);
    for(auto &s:c->slots)freeSlot(s);
    if(c->biomeContext)cvhip_destroy_biomes(c->biomeContext);
    if(c->program)(void)hipFree(c->program);
    delete c;
}
extern "C" CVHIP_EXPORT void *cvhip_create(const CvHipCondition *cfg,unsigned ncfg) {
    if(!cfg||!ncfg||ncfg>CVHIP_MAX_CONDITIONS)return nullptr;
    for(unsigned j=0;j<ncfg;j++) {
        if((cfg[j].reserved==CVHIP_CLIMATE||cfg[j].reserved==CVHIP_CLIMATE_RANGE||cfg[j].reserved==CVHIP_CLIMATE_PROBE||cfg[j].reserved==CVHIP_NOISE_SAMPLE)&&(cfg[j].na<0||cfg[j].nb<0||cfg[j].na+cfg[j].nb>32))return nullptr;
        if(cfg[j].reserved==CVHIP_STRUCTURE&&(cfg[j].na<=0||cfg[j].nb<=0))return nullptr;
    }
    auto *c=new(std::nothrow) BatchContext;if(!c)return nullptr;
    if(!checked(c,hipSetDevice(0),"select device")||
       !checked(c,hipMalloc(&c->program,ncfg*sizeof(CvHipCondition)),"allocate program")||
       !checked(c,hipMemcpy(c->program,cfg,ncfg*sizeof(CvHipCondition),hipMemcpyHostToDevice),"upload program")) {cvhip_destroy(c);return nullptr;}
    c->ncfg=ncfg;
    uint64_t advancedWork=0;
    for(unsigned i=0;i<ncfg;i++) {
        bool advanced=cfg[i].reserved==CVHIP_BIOME||cfg[i].reserved==CVHIP_NOISE_SAMPLE||cfg[i].reserved==CVHIP_BIOME_SAMPLE||cfg[i].reserved==CVHIP_BIOME_CENTER||cfg[i].reserved==CVHIP_SPIRAL_CLIMATE||cfg[i].reserved==CVHIP_SPIRAL_POINT||cfg[i].reserved==CVHIP_CENTER_POINT;
        c->biomes|=advanced;c->cheap|=!advanced;
        if(cfg[i].reserved==CVHIP_SPIRAL_CLIMATE) {
            const int64_t width=int64_t(cfg[i].x2)-cfg[i].x+1;
            const int64_t height=int64_t(cfg[i].z2)-cfg[i].z+1;
            if(width>0&&height>0&&width<=256&&height<=256) {
                uint64_t children=0;
                for(unsigned k=0;k<ncfg;k++)
                    children+=cfg[k].reserved==CVHIP_SPIRAL_POINT&&cfg[k].groupId==cfg[i].groupId;
                const uint64_t work=uint64_t(width)*uint64_t(height)*children;
                advancedWork=std::min<uint64_t>(8192,advancedWork+std::min<uint64_t>(work,8192));
            }
        }
        if(cfg[i].reserved==CVHIP_BIOME_CENTER) {
            const int64_t width=int64_t(cfg[i].x2)-cfg[i].x+1;
            const int64_t height=int64_t(cfg[i].z2)-cfg[i].z+1;
            if(width>0&&height>0) {
                const uint64_t step=unsigned(cfg[i].matchAny>0?cfg[i].matchAny:1);
                const uint64_t starts=((uint64_t(width)+step-1)/step)*((uint64_t(height)+step-1)/step);
                const uint64_t cells=width<=64&&height<=64&&uint64_t(width)*uint64_t(height)<=4096
                    ?uint64_t(width)*uint64_t(height):0;
                // Budget sparse climate checks for broad windows and the
                // bounded exact flood-fill work for windows <=64x64.
                const uint64_t work=std::min<uint64_t>(8192,starts*5+cells);
                advancedWork=std::min<uint64_t>(8192,advancedWork+work);
            }
        }
    }
    if(advancedWork)c->advancedWave=unsigned(std::max<uint64_t>(1,std::min<uint64_t>(128,8192/advancedWork)));
    if(c->biomes){c->biomeContext=cvhip_create_biomes(cfg,ncfg);if(!c->biomeContext){cvhip_destroy(c);return nullptr;}}
    for(auto &s:c->slots) {
        if(!checked(c,hipStreamCreateWithFlags(&s.stream,hipStreamNonBlocking),"create stream")||
           !checked(c,hipEventCreate(&s.started),"create timing start event")||
           !checked(c,hipEventCreate(&s.ready),"create timing completion event")||
           !checked(c,hipMalloc(&s.total,sizeof(unsigned)),"allocate counter")||
           !checked(c,hipHostMalloc(&s.hostTotal,sizeof(unsigned)),"allocate pinned counter")) {cvhip_destroy(c);return nullptr;}
    }
    return c;
}
extern "C" CVHIP_EXPORT int cvhip_submit(void *handle,unsigned slot,uint64_t start,const uint64_t *seeds,unsigned count) {
    auto *c=static_cast<BatchContext*>(handle);
    if(!c||c->failed||slot>=2||!count||count>CVHIP_MAX_BATCH||(c->biomes&&!c->cheap&&count>512)||(seeds&&count>CVHIP_MAX_LIST_BATCH)||
       (!seeds&&uint64_t(count-1)>UINT64_MAX-start))return 0;
    auto &s=c->slots[slot];if(s.busy)return 0;
    if(!checked(c,hipSetDevice(0),"select submission device")||!reserve(c,s,count,seeds!=nullptr))return 0;
    s.busy=true;s.count=count;s.start=start;s.list=seeds!=nullptr;
    if(!checked(c,hipEventRecord(s.started,s.stream),"record dispatch start"))return 0;
    if(seeds) {
        std::memcpy(s.hostSeeds,seeds,size_t(count)*sizeof(uint64_t));
        if(!checked(c,hipMemcpyAsync(s.seeds,s.hostSeeds,size_t(count)*sizeof(uint64_t),hipMemcpyHostToDevice,s.stream),"upload seeds"))return 0;
    }
    CVHIP_LAUNCH(filter,dim3((count+63)/64),dim3(64),0,s.stream,start,count,c->program,c->ncfg,s.mask,seeds?s.seeds:nullptr);
    if(!checked(c,hipGetLastError(),"filter launch"))return 0;
    if(c->biomes&&!c->cheap) {
        const unsigned wave=std::max(1U,c->advancedWave);
        for(unsigned base=0;base<count;base+=wave) {
            const unsigned n=count-base<wave?count-base:wave;
            const uint64_t chunkStart=start+base;
            const uint64_t *chunkSeeds=seeds?s.seeds+base:nullptr;
            if(!checked(c,cvhip_launch_biome_filter(c->biomeContext,s.stream,chunkStart,n,chunkSeeds,c->program,c->ncfg,s.mask+base,nullptr,s.centerScratch,cvhip_biome_center_stride(c->biomeContext),cvhip_biome_center_cells(c->biomeContext)),"biome launch"))return 0;
        }
    }
    if(!checked(c,hipMemsetAsync(s.total,0,sizeof(unsigned),s.stream),"clear counter"))return 0;
    unsigned words=(count+15)/16;
    CVHIP_LAUNCH(packDecisions,dim3((words+255)/256),dim3(256),0,s.stream,s.mask,count,s.packed,s.total);
    return checked(c,hipGetLastError(),"pack launch")&&
        checked(c,hipMemcpyAsync(s.hostTotal,s.total,sizeof(unsigned),hipMemcpyDeviceToHost,s.stream),"copy counter")&&
        checked(c,hipEventRecord(s.ready,s.stream),"record completion");
}
extern "C" CVHIP_EXPORT int cvhip_collect(void *handle,unsigned slot,CvHipBatchInfo *info,uint32_t *output) {
    auto *c=static_cast<BatchContext*>(handle);if(!c||c->failed||slot>=2||!info||!output)return 0;
    auto &s=c->slots[slot];if(!s.busy)return 0;
    if(!checked(c,hipSetDevice(0),"select collection device")||!checked(c,hipEventSynchronize(s.ready),"filter completion"))return 0;
    unsigned matches=*s.hostTotal,words=(s.count+15)/16;
    if(matches>s.count){c->failed=true;return 0;}
    // Preserve the cheap 16M launch. Advanced generation runs only on a small,
    // sparse survivor set. Dense batches retain the original CPU verifier.
    if(c->biomes&&c->cheap&&matches&&matches<=words&&matches<=4096) {
        if(!checked(c,hipMemsetAsync(s.total,0,sizeof(unsigned),s.stream),"clear advanced counter"))return 0;
        CVHIP_LAUNCH(compactDecisions,dim3((s.count+63)/64),dim3(64),0,s.stream,s.mask,s.count,s.offsets,s.total);
        if(!checked(c,hipGetLastError(),"advanced compaction"))return 0;
        const unsigned wave=std::max(1U,c->advancedWave);
        for(unsigned base=0;base<matches;base+=wave) {
            unsigned n=matches-base<wave?matches-base:wave;
            if(!checked(c,cvhip_launch_biome_filter(c->biomeContext,s.stream,s.start,n,s.list?s.seeds:nullptr,c->program,c->ncfg,s.mask,s.offsets+base,s.centerScratch,cvhip_biome_center_stride(c->biomeContext),cvhip_biome_center_cells(c->biomeContext)),"advanced biome launch"))return 0;
        }
        if(!checked(c,hipMemsetAsync(s.total,0,sizeof(unsigned),s.stream),"clear final counter"))return 0;
        CVHIP_LAUNCH(packDecisions,dim3((words+255)/256),dim3(256),0,s.stream,s.mask,s.count,s.packed,s.total);
        if(!checked(c,hipGetLastError(),"final pack")||
           !checked(c,hipMemcpyAsync(s.hostTotal,s.total,sizeof(unsigned),hipMemcpyDeviceToHost,s.stream),"final count")||
           !checked(c,hipStreamSynchronize(s.stream),"advanced completion"))return 0;
        matches=*s.hostTotal;
    }
    unsigned encoding=matches<=words?CVHIP_OFFSETS:CVHIP_PACKED;
    size_t bytes=size_t(encoding==CVHIP_OFFSETS?matches:words)*sizeof(uint32_t);
    if(encoding==CVHIP_OFFSETS&&matches) {
        if(!checked(c,hipMemsetAsync(s.total,0,sizeof(unsigned),s.stream),"clear compact counter"))return 0;
        CVHIP_LAUNCH(compactDecisions,dim3((s.count+63)/64),dim3(64),0,s.stream,s.mask,s.count,s.offsets,s.total);
        if(!checked(c,hipGetLastError(),"compact launch"))return 0;
    }
    if(bytes&&!checked(c,hipMemcpyAsync(s.host,encoding==CVHIP_OFFSETS?s.offsets:s.packed,bytes,hipMemcpyDeviceToHost,s.stream),"copy results"))return 0;
    if(!checked(c,hipEventRecord(s.ready,s.stream),"record final completion")||
       !checked(c,hipEventSynchronize(s.ready),"result completion"))return 0;
    float elapsedMs=0.0f;
    if(!checked(c,hipEventElapsedTime(&elapsedMs,s.started,s.ready),"measure device stream time"))return 0;
    if(bytes)std::memcpy(output,s.host,bytes);
    const double elapsedUs=double(elapsedMs)*1000.0;
    const uint32_t elapsedUs32=elapsedUs>=4294967295.0?UINT32_MAX:uint32_t(elapsedUs+0.5);
    *info={s.count,matches,encoding,elapsedUs32};s.busy=false;return 1;
}
