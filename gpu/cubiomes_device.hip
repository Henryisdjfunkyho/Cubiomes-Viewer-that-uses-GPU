// Compile the pinned upstream generator itself for HIP. Do not substitute
// biome tables, noise parameters, spline evaluation, or generation algorithms.
// Upstream source and its license remain in the vendored cubiomes directory.
#include <hip/hip_runtime.h>
#include "cvhip.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>

// CUDA's device math API does not provide the C nan(const char *) overload
// used by upstream Cubiomes. Only the CUDA device pass needs the equivalent
// quiet-NaN macro; leave the host and AMD implementations untouched.
#if defined(__HIP_PLATFORM_NVIDIA__) && defined(__CUDA_ARCH__)
#define nan(tag) NAN
#endif

namespace {
struct DeviceAllocation {
    void *p;
    template<class T> __host__ __device__ operator T*() const {return static_cast<T*>(p);}
};
__host__ __device__ DeviceAllocation deviceMalloc(size_t n) {return {malloc(n)};}
__host__ __device__ void *deviceCalloc(size_t n,size_t size) {
    if(size&&n>SIZE_MAX/size)return nullptr;
    void *p=malloc(n*size);if(p)memset(p,0,n*size);return p;
}
__host__ __device__ void deviceExit(int code) {
#ifdef __HIP_DEVICE_COMPILE__
    __builtin_trap();
#else
    std::exit(code);
#endif
}
__host__ __device__ void *deviceMemmove(void *dst,const void *src,size_t n) {
    auto *d=static_cast<unsigned char*>(dst);auto *s=static_cast<const unsigned char*>(src);
    if(uintptr_t(d)<uintptr_t(s))for(size_t i=0;i<n;i++)d[i]=s[i];
    else for(size_t i=n;i;i--)d[i-1]=s[i-1];return dst;
}
template<class... T> __host__ __device__ int devicePrintf(const char *format,T... args) {
#ifdef __HIP_DEVICE_COMPILE__
    // Invalid upstream inputs still trap via deviceExit. Device printf would
    // require a host-RPC service even for its never-taken diagnostic branches.
    return 0;
#else
    if constexpr(sizeof...(args)==0)return std::printf("%s",format);
    else return std::printf(format,args...);
#endif
}
}
#define malloc deviceMalloc
#define calloc deviceCalloc
#define exit deviceExit
#define memmove deviceMemmove
#define printf devicePrintf
#pragma clang force_cuda_host_device begin
#include <cubiomes/noise.c>
#include <cubiomes/biomenoise.c>
#include <cubiomes/layers.c>
#include <cubiomes/generator.c>
#include <cubiomes/biomes.c>
#pragma clang force_cuda_host_device end
#if defined(__HIP_PLATFORM_NVIDIA__) && defined(__CUDA_ARCH__)
#undef nan
#endif
#undef malloc
#undef calloc
#undef exit
#undef memmove
#undef printf
#include "noise_device.h"

namespace {
struct FlatSpline {int len,typ;float loc[12],der[12],value;int child[12];};
struct BiomeProgram { CvHipCondition climate[NP_MAX]; FlatSpline splines[193];int root;BiomeTree tree; };
struct HostBiomeProgram {BiomeProgram *device=nullptr;void *steps=nullptr,*param=nullptr,*nodes=nullptr;unsigned dimensions=0,centerCells=0;size_t centerStride=0;};
__device__ float flatSpline(const BiomeProgram *p,int index,const float *vals) {
    struct Frame {int index,stage,i;float f,k,n;};Frame stack[8]{};
    int depth=0;stack[0].index=index;float result=NAN;
    while(depth>=0) {
        auto &f=stack[depth];if(f.index<0||f.index>=193)return NAN;
        const auto &s=p->splines[f.index];
        if(f.stage==0) {
            if(s.len==1){result=s.value;depth--;continue;}
            if(s.len<=0||s.len>=12||s.typ<0||s.typ>=4||depth==7)return NAN;
            f.f=vals[s.typ];f.i=0;while(f.i<s.len&&s.loc[f.i]<f.f)f.i++;
            if(f.i==0||f.i==s.len){if(f.i)f.i--;f.stage=1;stack[++depth]={s.child[f.i],0,0,0,0,0};}
            else{f.k=(f.f-s.loc[f.i-1])/(s.loc[f.i]-s.loc[f.i-1]);f.stage=2;stack[++depth]={s.child[f.i-1],0,0,0,0,0};}
        } else if(f.stage==1){result=result+s.der[f.i]*(f.f-s.loc[f.i]);depth--;}
        else if(f.stage==2){f.n=result;f.stage=3;stack[++depth]={s.child[f.i],0,0,0,0,0};}
        else {
            float n=f.n,o=result,k=f.k,g=s.loc[f.i-1],h=s.loc[f.i];
            float a=s.der[f.i-1]*(h-g)-(o-n),b=-s.der[f.i]*(h-g)+(o-n);
            result=(n+k*(o-n))+k*(1.0F-k)*(a+k*(b-a));depth--;
        }
    }
    return result;
}
__device__ int flatBiomeTree(const uint64_t *np,const BiomeTree &tree,bool coarse) {
    struct Frame {int idx,depth,leaf,inner,i;unsigned step;uint64_t ds,innerDistance;bool waiting;};
    Frame stack[8]{};int depth=0,result=0;
    stack[0]={0,0,0,0,0,0,coarse?get_np_dist(np,&tree,0):UINT64_MAX,0,false};
    while(depth>=0) {
        auto &f=stack[depth];
        if(!f.step&&!f.waiting&&f.i==0) {
            if(tree.steps[f.depth]==0){result=f.idx;depth--;continue;}
            do{f.step=tree.steps[f.depth++];}while(f.idx+f.step>=tree.len);
            f.inner=tree.nodes[f.idx]>>48;
        }
        if(f.waiting) {
            uint64_t distance=f.inner==result?f.innerDistance:get_np_dist(np,&tree,result);
            if(distance<f.ds){f.ds=distance;f.leaf=result;}f.waiting=false;f.i++;f.inner+=f.step;
        }
        if(f.i>=int(tree.order)||f.inner>=tree.len){result=f.leaf;depth--;continue;}
        f.innerDistance=get_np_dist(np,&tree,f.inner);
        if(f.innerDistance<f.ds) {
            if(depth==7)return -1;
            f.waiting=true;Frame child{f.inner,f.depth,f.leaf,0,0,0,f.ds,0,false};stack[++depth]=child;
        }else{f.i++;f.inner+=f.step;}
    }
    return result;
}

__device__ int modernBiome(const CvHipCondition &c,uint64_t seed,const BiomeProgram *p,unsigned char *permutation) {
    int x=c.x,y=c.y,z=c.z;
    if(c.scale==1)voronoiAccess3D(getVoronoiSHA(seed),x,y,z,&x,&y,&z);
    else if(c.scale>4){int scale=c.scale/4;x=x*scale+scale/2;z=z*scale+scale/2;}
    uint64_t a,b;cvhip_noise::seed(seed,a,b);uint64_t lo=cvhip_noise::next(a,b),hi=cvhip_noise::next(a,b);
    double px=x,pz=z,v,unused;
    if(c.scale<=4) {
        double sx,sz;cvhip_noise::evaluate(lo,hi,p->climate[NP_SHIFT],x,0,z,z,x,0,true,permutation,sx,sz);
        px+=sx*4;pz+=sz*4;
    }
    cvhip_noise::evaluate(lo,hi,p->climate[NP_CONTINENTALNESS],px,0,pz,0,0,0,false,permutation,v,unused);float cont=v;
    cvhip_noise::evaluate(lo,hi,p->climate[NP_EROSION],px,0,pz,0,0,0,false,permutation,v,unused);float erosion=v;
    cvhip_noise::evaluate(lo,hi,p->climate[NP_WEIRDNESS],px,0,pz,0,0,0,false,permutation,v,unused);float weird=v;
    float vals[]={cont,erosion,peaksAndValleys(weird),weird};
    double off=flatSpline(p,p->root,vals)+0.015F;
    if(!isfinite(off))return none;
    float depth=1.0-(y*4)/128.0-83.0/160.0+off;
    cvhip_noise::evaluate(lo,hi,p->climate[NP_TEMPERATURE],px,0,pz,0,0,0,false,permutation,v,unused);float temp=v;
    cvhip_noise::evaluate(lo,hi,p->climate[NP_HUMIDITY],px,0,pz,0,0,0,false,permutation,v,unused);float humid=v;
    float scaled[]={10000.0F*temp,10000.0F*humid,10000.0F*cont,10000.0F*erosion,10000.0F*depth,10000.0F*weird};
    int64_t np[6];
    for(int i=0;i<6;i++) {
        // Keep float-to-integer boundary cases for the original CPU evaluator.
        if(!isfinite(scaled[i])||fabs(double(scaled[i])-round(double(scaled[i])))<0.001)return none;
        np[i]=int64_t(scaled[i]);
    }
    const auto *parameters=reinterpret_cast<const uint64_t*>(np);
    int leaf=flatBiomeTree(parameters,p->tree,c.scale>4);if(leaf<0)return none;
    return (p->tree.nodes[leaf]>>48)&255;
}
// Same coarse branch as genEndScaled. Fine End branches allocate height maps
// and would introduce device host-RPC services even when never executed.
__device__ int coarseEndBiome(const EndNoise *noise,const CvHipCondition &c) {
    if(c.mc<=MC_1_8)return the_end;
    float scale=c.scale/8.0;
    int64_t x=int64_t(c.x*scale),z=int64_t(c.z*scale);
    uint64_t distance=x*x+z*z;
    if(distance<=16384L)return the_end;
    if(c.mc>MC_1_13&&int(distance)<0)return end_barrens;
    float h=getEndHeightNoise(noise,x,z,4);
    return h>40?end_highlands:h>=0?end_midlands:h>=-20?end_barrens:small_end_islands;
}
// A center must begin at one of Cubiomes' climate-qualified sample cells.
// This rejects only when every candidate cell is outside the exact limits;
// surviving seeds still run getBiomeCenters and its flood fill on the CPU.
__device__ int centerMayContain(const CvHipCondition &c,uint64_t seed,const BiomeProgram *p,unsigned char *d) {
    uint64_t a,b;cvhip_noise::seed(seed,a,b);uint64_t lo=cvhip_noise::next(a,b),hi=cvhip_noise::next(a,b);
    const int para[5]={NP_TEMPERATURE,NP_HUMIDITY,NP_EROSION,NP_CONTINENTALNESS,NP_WEIRDNESS};
    int step=c.matchAny>0?c.matchAny:1;
    const int64_t width=int64_t(c.x2)-int64_t(c.x)+1;
    const int64_t height=int64_t(c.z2)-int64_t(c.z)+1;
    if(width<=0||height<=0)return 2;
    const int64_t nx=(width+step-1)/step, nz=(height+step-1)/step;
    const bool largeWindow=width>64||height>64||width*height>4096;
    // The large-window route is only a climate necessary-condition test.
    // Bound it by the number of sparse starts used by getBiomeCenters; if a
    // malformed or unexpectedly broad config exceeds this, leave it to CPU.
    if(largeWindow&&(nx>64||nz>64||nx*nz>64))return 2;
    for(int64_t z=c.z;z<=c.z2;z+=step)for(int64_t x=c.x;x<=c.x2;x+=step) {
        bool candidate=true,ambiguous=false;
        double px=double(x)*c.scale/4.0,pz=double(z)*c.scale/4.0;
        for(int k=0;k<5;k++) {
            int loLimit=c.limits[k*2],hiLimit=c.limits[k*2+1];
            if(loLimit==INT_MIN&&hiLimit==INT_MAX)continue;
            double value,unused;cvhip_noise::evaluate(lo,hi,p->climate[para[k]],px,0,pz,0,0,0,false,d,value,unused);
            double scaled=value*10000.0;
            if(!isfinite(scaled)||fabs(scaled-nearbyint(scaled))<0.001){ambiguous=true;break;}
            if(scaled<double(loLimit)-2.0||scaled>double(hiLimit)+2.0){candidate=false;break;}
            if(scaled<double(loLimit)+2.0||scaled>double(hiLimit)-2.0)ambiguous=true;
        }
        if(candidate&&ambiguous)return 2;
        if(candidate)return 1;
    }
    return 0;
}

// Mirror Cubiomes' bounded getBiomeCenters path closely enough to make a
// conservative GPU rejection.  This is intentionally not a replacement for
// the CPU locator: the CPU performs the authoritative center calculation and
// dependent-reference propagation for every surviving seed.
//
// Cubiomes first marks climate-disqualified sparse start cells, then performs
// a DFS-like flood fill (N,S,W,E pushes, LIFO pop) over the whole bounded
// range.  It marks cells on pop, so duplicate queue entries and the exact
// traversal order matter when tol > 1.  Keep that behavior here and return
// 2 whenever the fixed local queue or a numerical boundary makes the result
// unsafe to use as a rejection.
__device__ int centerExactResult(const CvHipCondition &c,uint64_t seed,
    const BiomeProgram *p,unsigned char *d,unsigned char *scratch,unsigned scratchCells,
    int *firstCenterX,int *firstCenterZ) {
    if(firstCenterX)*firstCenterX=0;
    if(firstCenterZ)*firstCenterZ=0;
    const int64_t w64=int64_t(c.x2)-int64_t(c.x)+1;
    const int64_t h64=int64_t(c.z2)-int64_t(c.z)+1;
    if(w64<=0||h64<=0||w64>64||h64>64||w64*h64>4096)return 2;
    const int w=int(w64),h=int(h64);
    const int tol=c.tolerance>0?c.tolerance:1;
    const int minSize=c.componentSize>0&&c.componentSize<=uint64_t(INT_MAX)?int(c.componentSize):1;
    const int step=c.matchAny>0?c.matchAny:1;
    const int cells=w*h;
    if(cells>64&&(!scratch||scratchCells<unsigned(cells)))return 2;
    int localIds[64];unsigned char localClass[64];
    int localQi[257],localQj[257],localQd[257];
    int *ids=localIds;unsigned char *classes=localClass;
    int *qi=localQi,*qj=localQj,*qd=localQd;int queueCap=257;
    if(scratch&&scratchCells>=unsigned(cells)) {
        // One bounded candidate owns a global scratch slice.  The map and
        // queue are shared-memory-sized logically, but live in device global
        // memory so a 64x64 window never spills every lane's stack.
        const unsigned requestedQcap=4U*scratchCells+1U;
        const unsigned qcap=requestedQcap<16385U?requestedQcap:16385U;
        ids=reinterpret_cast<int*>(scratch);
        const unsigned classBytes=(scratchCells+3U)&~3U;
        classes=scratch+scratchCells*sizeof(int);
        qi=reinterpret_cast<int*>(classes+classBytes);
        qj=qi+qcap;qd=qj+qcap;queueCap=int(qcap);
    }
    for(int k=0;k<cells;k++){ids[k]=-1;classes[k]=255;}

    uint64_t a,b;cvhip_noise::seed(seed,a,b);
    uint64_t lo=cvhip_noise::next(a,b),hi=cvhip_noise::next(a,b);
    const int para[5]={NP_TEMPERATURE,NP_HUMIDITY,NP_EROSION,NP_CONTINENTALNESS,NP_WEIRDNESS};
    // Keep the same climate gate used by getBiomeCenters.  Values near a
    // limit are left to the CPU rather than being turned into a GPU reject.
    for(int j=0;j<h;j+=step)for(int i=0;i<w;i+=step) {
        bool uncertain=false;
        const double px=double(c.x+i)*c.scale/4.0;
        const double pz=double(c.z+j)*c.scale/4.0;
        for(int k=0;k<5;k++) {
            const int low=c.limits[k*2],high=c.limits[k*2+1];
            if(low==INT_MIN&&high==INT_MAX)continue;
            double value,unused;
            cvhip_noise::evaluate(lo,hi,p->climate[para[k]],px,0,pz,0,0,0,false,d,value,unused);
            const double scaled=value*10000.0;
            // CPU truncates 10000*noise to int.  A two-unit guard covers
            // device/host floating-point differences without losing a
            // possible candidate at an inclusive boundary.
            if(!isfinite(scaled))return 2;
            if(scaled<double(low)-2.0||scaled>double(high)+2.0) {
                ids[j*w+i]=-2;uncertain=false;break;
            }
            if(scaled<double(low)+2.0||scaled>double(high)-2.0||
               fabs(scaled-nearbyint(scaled))<0.001)uncertain=true;
        }
        if(uncertain)return 2;
    }

    const int wanted=c.required>0?c.required:1;
    int found=0;bool skippedCenter=false;
    for(int j=0;j<h;j+=step)for(int i=0;i<w;i+=step) {
        if(ids[j*w+i]!=-1)continue;
        int qn=1;qi[0]=i;qj[0]=j;qd[0]=0;
        int area=0;int64_t sumx=0,sumz=0;
        while(qn>0) {
            --qn;int x=qi[qn],z=qj[qn],gap=qd[qn];
            const int key=z*w+x;
            if(ids[key]==INT_MAX)continue;
            ids[key]=INT_MAX;
            CvHipCondition point=c;point.x=c.x+x;point.z=c.z+z;
            int biome;
            if(classes[key]!=255)biome=classes[key]?c.find:0;
            else {
                biome=modernBiome(point,seed,p,d);
                if(biome<0)return 2;
                classes[key]=biome==c.find?1:0;
            }
            if(biome<0)return 2;
            if(biome==c.find) {area++;sumx+=x;sumz+=z;gap=0;}
            else if(++gap>=tol)continue;
            // Preserve floodFillGen's N,S,W,E push order and LIFO pop.
            const int pxs[4]={x,x,x-1,x+1};
            const int pzs[4]={z-1,z+1,z,z};
            for(int side=0;side<4;side++) {
                const int xx=pxs[side],zz=pzs[side];
                if(xx<0||xx>=w||zz<0||zz>=h||ids[zz*w+xx]==INT_MAX)continue;
                if(qn>=queueCap)return 2;
                qi[qn]=xx;qj[qn]=zz;qd[qn]=gap;++qn;
            }
        }
        if(area>=minSize) {
            ++found;
            const double centerX=(double(c.x)+double(sumx)/double(area)+0.5)*c.scale;
            const double centerZ=(double(c.z)+double(sumz)/double(area)+0.5)*c.scale;
            if(c.groupId) {
                const double fracX=centerX-floor(centerX),fracZ=centerZ-floor(centerZ);
                if(fabs(fracX-0.5)<1e-7||fabs(fracZ-0.5)<1e-7)return 2;
            }
            if(c.required<=0)return 1;
            if(c.skipref) {
                if(int(round(centerX))==c.refx&&int(round(centerZ))==c.refz)
                    skippedCenter=true;
            }
            // CPU collects at most `count` centers and then removes the
            // origin instance for skipref.  Reproduce that decision for the
            // rejection-only result; successful seeds still go to CPU.
            if(found>=wanted) {
                if(c.skipref&&skippedCenter)return found-1>=wanted?1:0;
                if(firstCenterX)*firstCenterX=int(round(centerX));
                if(firstCenterZ)*firstCenterZ=int(round(centerZ));
                return 1;
            }
        }
    }
    return 0;
}
__device__ bool centerWindowHasExactDeviceSupport(const CvHipCondition &c) {
    const int64_t w=int64_t(c.x2)-int64_t(c.x)+1;
    const int64_t h=int64_t(c.z2)-int64_t(c.z)+1;
    return w>0&&h>0&&w<=64&&h<=64&&w*h<=4096;
}
__device__ int floorDiv4(int64_t v){return int(v/4-(v%4<0));}
__device__ bool spiralClimatePass(uint64_t lo,uint64_t hi,const CvHipCondition &c,unsigned char *d) {
    double value,unused;cvhip_noise::evaluate(lo,hi,c,c.x,0,c.z,0,0,0,false,d,value,unused);value*=10000.0;
    if(!isfinite(value)||fabs(value-c.lower)<1e-6||fabs(value-c.upper)<1e-6)return true;
    return c.invert?(value<=c.lower||value>=c.upper):(value>=c.lower&&value<=c.upper);
}
// A direct point-climate child of a single-center condition can only be used
// as a reject-only test after the first CPU center has been established.
// Return 2 for an absent/unsupported/over-budget group so the caller keeps the
// seed for ordinary CPU reference evaluation.
__device__ int centerPointChildrenResult(const CvHipCondition &parent,const CvHipCondition *program,
    unsigned ncfg,uint64_t lo,uint64_t hi,int centerX,int centerZ,unsigned char *d) {
    unsigned children=0;
    for(unsigned k=0;k<ncfg;k++)
        if(program[k].groupId==parent.groupId&&program[k].reserved==CVHIP_CENTER_POINT)
            ++children;
    if(children==0||children>8)return 2;
    for(unsigned k=0;k<ncfg;k++)
        if(program[k].groupId==parent.groupId&&program[k].reserved==CVHIP_CENTER_POINT) {
            CvHipCondition point=program[k];
            const int64_t bx=int64_t(centerX)+point.x,bz=int64_t(centerZ)+point.z;
            if(bx<INT_MIN||bx>INT_MAX||bz<INT_MIN||bz>INT_MAX)return 2;
            const int64_t qx=bx/4-(bx%4<0),qz=bz/4-(bz%4<0);
            if(qx<INT_MIN||qx>INT_MAX||qz<INT_MIN||qz>INT_MAX)return 2;
            point.x=int(qx);point.z=int(qz);
            if(!spiralClimatePass(lo,hi,point,d))return 0;
        }
    return 1;
}
__device__ bool spiralHasClimateMatch(const CvHipCondition &marker,const CvHipCondition *program,unsigned ncfg,
    uint64_t lo,uint64_t hi,unsigned char *d) {
    int64_t step=marker.scale;
    const int64_t width=int64_t(marker.x2)-int64_t(marker.x)+1;
    const int64_t height=int64_t(marker.z2)-int64_t(marker.z)+1;
    if(step<=0||width<=0||height<=0||width>256||height>256||width*height>256)
        return true; // Unknown/over-budget means CPU replay, never rejection.
    unsigned children=0;
    for(unsigned k=0;k<ncfg;k++)
        if(program[k].groupId==marker.groupId&&program[k].reserved==CVHIP_SPIRAL_POINT)
            ++children;
    if(children==0||uint64_t(width*height)*children>1024)
        return true;
    for(int rz=marker.z;rz<=marker.z2;rz++)for(int rx=marker.x;rx<=marker.x2;rx++) {
        int64_t px=int64_t(rx)*step,pz=int64_t(rz)*step;
        if(px<marker.areaX1||px>marker.areaX2||pz<marker.areaZ1||pz>marker.areaZ2)continue;
        if(marker.radiusSq) {
            int64_t dx=px-marker.refx,dz=pz-marker.refz;
            if(dx*dx+dz*dz>=int64_t(marker.radiusSq))continue;
        }
        bool all=true,found=false;
        for(unsigned k=0;k<ncfg;k++)if(program[k].groupId==marker.groupId&&program[k].reserved==CVHIP_SPIRAL_POINT) {
            found=true;CvHipCondition point=program[k];
            point.x=floorDiv4(px+point.x);point.z=floorDiv4(pz+point.z);
            if(!spiralClimatePass(lo,hi,point,d)){all=false;break;}
        }
        if(found&&all)return true;
    }
    return false;
}

__device__ int sampleNextInt(uint64_t &s,int bound) {
    unsigned bits,value;
    do {
        s=(s*0x5deece66dULL+11)&((1ULL<<48)-1);bits=unsigned(s>>17);
        if(!(bound&(bound-1)))return int((uint64_t(bound)*bits)>>31);
        value=bits%unsigned(bound);
    }while(int32_t(bits-value+unsigned(bound-1))<0);
    return int(value);
}
__device__ void sampleWilson(double n,double p,double z,double &lo,double &hi) {
    double s=z*z/n,t=1/(1+s),w=t*(p+0.5*s);
    double d=t*z*sqrt((p*(1-p)+0.25*s)/n)+FLT_EPSILON;
    lo=w-d;hi=w+d;
}
// The pinned monteCarloBiomes algorithm, bounded to 128 GPU samples. Preserve its
// Java RNG, optional shuffle, sample order and default return after exhaustion.
// Numerical boundary cases are left for the CPU rather than rejected.
__device__ int sampleCondition(const CvHipCondition &c,uint64_t seed,const BiomeProgram *p,unsigned char *d) {
    int w=c.x2-c.x+1,h=c.z2-c.z+1,shuffle[64];
    if(w<=0||h<=0)return 2;
    uint64_t n=uint64_t(w)*h;if(c.matchAny&&n>64)return 2;
    if(c.matchAny)for(int i=0;i<int(n);i++)shuffle[i]=i;
    uint64_t rng=(seed^0x5deece66dULL)&((1ULL<<48)-1),a,b;
    cvhip_noise::seed(seed,a,b);uint64_t lo=cvhip_noise::next(a,b),hi=cvhip_noise::next(a,b);
    double successes=0,m=0;
    unsigned limit=n<128?unsigned(n):128;
    for(unsigned i=0;i<limit;i++) {
        int x,z;
        if(c.matchAny) {
            int k=sampleNextInt(rng,int(n-i)),cell=shuffle[k];
            shuffle[k]=shuffle[n-i-1];x=c.x+cell%w;z=c.z+cell/w;
        } else {
            x=c.x+sampleNextInt(rng,w);sampleNextInt(rng,1);z=c.z+sampleNextInt(rng,h);
        }
        if(c.radiusSq) {
            int64_t dx=int64_t(x)*4-c.refx,dz=int64_t(z)*4-c.refz;
            if(dx*dx+dz*dz>=int64_t(c.radiusSq))continue;
        }
        bool ok;
        if(c.reserved==CVHIP_NOISE_SAMPLE) {
            double value,unused;cvhip_noise::evaluate(lo,hi,c,x,0,z,0,0,0,false,d,value,unused);value*=10000;
            if(!isfinite(value)||fabs(value-c.lower)<1e-6||fabs(value-c.upper)<1e-6)return 2;
            ok=value>c.lower&&value<c.upper;if(c.invert)ok=!ok;
        } else {
            CvHipCondition point=c;point.x=x;point.z=z;
            int biome=modernBiome(point,seed,p,d);
            if(biome<0||biome>=192||(biome>=64&&biome<128))return 2;
            uint64_t mask=uint64_t(1)<<(biome>=128?biome-128:biome);
            ok=bool(mask&(biome<128?c.find:c.findM));
        }
        successes+=ok;
        m+=1;
        double per=1/m,lower,upper;sampleWilson(m,successes*per,c.zscore,lower,upper);
        double yes=lower-per-c.coverage,no=upper+per-c.coverage,narrow=upper-lower-c.wilsonWidth;
        if(fabs(yes)<1e-9||fabs(no)<1e-9||fabs(narrow)<1e-9)return 2;
        if(yes>0)return 1;
        if(no<0)return 0;
        if(narrow<0){double delta=successes*per-c.coverage;if(fabs(delta)<1e-9)return 2;return delta>0;}
    }
    return n<=limit?1:2;
}

template<int Dimension> __global__ void biomeFilter(uint64_t start,unsigned count,const uint64_t *seeds,
    const CvHipCondition *program,unsigned ncfg,unsigned char *mask,const uint32_t *offsets,const BiomeProgram *biomes,
    unsigned char *centerScratch,size_t centerStride,unsigned centerCells) {
    __shared__ unsigned char permutations[257*64];
    unsigned job=blockIdx.x*blockDim.x+threadIdx.x;if(job>=count)return;
    unsigned i=offsets?(offsets[job]&0x7fffffffU):job;
    if(!mask[i])return;
    const uint64_t seed=seeds?seeds[i]:start+i;
    if(program[0].preserveCpuZero&&seed==0){mask[i]=2;return;}
    for(unsigned j=0;j<ncfg;j++) {
        const auto &c=program[j];
        if(c.dimension!=Dimension)continue;
        if(c.reserved==CVHIP_NOISE_SAMPLE||c.reserved==CVHIP_BIOME_SAMPLE) {
            int result=sampleCondition(c,seed,biomes,permutations+threadIdx.x);
            if(!result){mask[i]=0;return;}if(result==2)mask[i]=2;
            continue;
        }
        if(c.reserved==CVHIP_BIOME_CENTER) {
            int result=centerMayContain(c,seed,biomes,permutations+threadIdx.x);
            if(c.required>0&&!result){mask[i]=0;return;}
            if(result==2){mask[i]=2;continue;}
            if(result==1) {
                // The large-window route above can only prove that every
                // CPU sparse start violates a necessary climate bound. Once
                // any start is possible, the unchanged CPU locator performs
                // the authoritative area/flood-fill test and center ordering.
                if(!centerWindowHasExactDeviceSupport(c)) {mask[i]=2;continue;}
                unsigned char *scratch=nullptr;
                if(centerScratch&&centerStride&&centerCells&&int64_t(c.x2)-c.x+1>0&&int64_t(c.z2)-c.z+1>0&&
                   uint64_t(int64_t(c.x2)-c.x+1)*uint64_t(int64_t(c.z2)-c.z+1)>64)
                    scratch=centerScratch+size_t(job)*centerStride;
                int centerX=0,centerZ=0;
                const int exact=centerExactResult(c,seed,biomes,permutations+threadIdx.x,scratch,centerCells,
                    c.groupId?&centerX:nullptr,c.groupId?&centerZ:nullptr);
                if(c.required>0) {
                    if(exact==0){mask[i]=0;return;}
                    if(exact==1&&c.groupId) {
                        uint64_t childA,childB;cvhip_noise::seed(seed,childA,childB);
                        const uint64_t childLo=cvhip_noise::next(childA,childB),childHi=cvhip_noise::next(childA,childB);
                        const int childResult=centerPointChildrenResult(c,program,ncfg,childLo,childHi,
                            centerX,centerZ,permutations+threadIdx.x);
                        if(childResult==0){mask[i]=0;return;}
                    }
                    mask[i]=2;
                } else {
                    if(exact==1){mask[i]=0;return;}
                    mask[i]=2;
                }
            }
            continue;
        }
        if(c.reserved==CVHIP_SPIRAL_CLIMATE) {
            uint64_t a,b;cvhip_noise::seed(seed,a,b);
            uint64_t lo=cvhip_noise::next(a,b),hi=cvhip_noise::next(a,b);
            if(!spiralHasClimateMatch(c,program,ncfg,lo,hi,permutations+threadIdx.x)){mask[i]=0;return;}
            mask[i]=2;continue;
        }
        if(c.reserved!=CVHIP_BIOME)continue;
        uint64_t found=0,foundM=0;bool uncertain=false;
        for(int z=c.z;z<=c.z2;z++)for(int x=c.x;x<=c.x2;x++) {
        Range range={c.scale,x,z,1,1,c.y,1};
        int cache[32]={},biome=none;
        if constexpr(Dimension==DIM_OVERWORLD) {
            CvHipCondition point=c;point.x=x;point.z=z;
            biome=modernBiome(point,seed,biomes,permutations+threadIdx.x);
        } else if constexpr(Dimension==DIM_NETHER) {
            NetherNoise noise;setNetherSeed(&noise,seed);
            if(genNetherScaled(&noise,cache,range,c.mc,getVoronoiSHA(seed))==0)biome=cache[0];
        } else {
            EndNoise noise;setEndSeed(&noise,c.mc,seed);
            CvHipCondition point=c;point.x=x;point.z=z;biome=coarseEndBiome(&noise,point);
        }
        if(biome<0||biome>=192||(biome>=64&&biome<128)) {uncertain=true;continue;}
        uint64_t bit=uint64_t(1)<<(biome>=128?biome-128:biome);
        if(biome<128)found|=bit;else foundM|=bit;
        if((found&c.exclude)||(foundM&c.excludeM)){mask[i]=0;return;}
        }
        bool reject=((found&c.find)!=c.find)||((foundM&c.findM)!=c.findM)||
            ((c.pick||c.pickM)&&!((found&c.pick)||(foundM&c.pickM)));
        if(reject&&!uncertain) {mask[i]=0;return;}
        if(uncertain)mask[i]=2;
        // CPU retains the condition's original returned position/branch logic.
    }
}
}
void cvhip_destroy_biomes(void *handle);
void *cvhip_create_biomes(const CvHipCondition *program,unsigned count) {
    auto *host=new(std::nothrow) HostBiomeProgram;if(!host)return nullptr;
    int mc=0,large=0;
    for(unsigned i=0;i<count;i++)if(program[i].reserved==CVHIP_BIOME||program[i].reserved==CVHIP_NOISE_SAMPLE||program[i].reserved==CVHIP_BIOME_SAMPLE||program[i].reserved==CVHIP_BIOME_CENTER||program[i].reserved==CVHIP_SPIRAL_CLIMATE||program[i].reserved==CVHIP_SPIRAL_POINT) {
        host->dimensions|=1U<<(program[i].dimension+1);
        if(program[i].dimension==DIM_OVERWORLD&&program[i].reserved!=CVHIP_NOISE_SAMPLE){mc=program[i].mc;large=program[i].large;}
        if(program[i].reserved==CVHIP_BIOME_CENTER) {
            const int64_t w=int64_t(program[i].x2)-program[i].x+1;
            const int64_t h=int64_t(program[i].z2)-program[i].z+1;
            const uint64_t cells=w>0&&h>0?uint64_t(w)*uint64_t(h):0;
            // Scratch is needed only by the bounded device flood-fill path.
            // Wider windows use the sparse climate-only necessary check and
            // are always replayed by the CPU when any start remains possible.
            if(w>0&&h>0&&w<=64&&h<=64&&cells>64&&cells<=4096)
                host->centerCells=std::max(host->centerCells,unsigned(cells));
        }
    }
    if(host->centerCells) {
        const size_t qcap=size_t(host->centerCells)*4+1;
        const size_t classBytes=(size_t(host->centerCells)+3)&~size_t(3);
        host->centerStride=size_t(host->centerCells)*sizeof(int)+classBytes+
            qcap*3*sizeof(int);
    }
    if(!mc)return host;
    if(hipMalloc(&host->device,sizeof(BiomeProgram))!=hipSuccess){delete host;return nullptr;}
    BiomeNoise bn{};initBiomeNoise(&bn,mc);setBiomeSeed(&bn,0,large);
    BiomeProgram p{};
    auto splineIndex=[&](const Spline *src)->int {
        if(src->len==1)return 42+(reinterpret_cast<const FixSpline*>(src)-bn.ss.fstack);
        return src-bn.ss.stack;
    };
    p.root=splineIndex(bn.sp);
    for(int i=0;i<bn.ss.flen;i++){p.splines[42+i].len=1;p.splines[42+i].value=bn.ss.fstack[i].val;}
    for(int i=0;i<bn.ss.len;i++) {
        const auto &src=bn.ss.stack[i];auto &dst=p.splines[i];dst.len=src.len;dst.typ=src.typ;
        for(int j=0;j<src.len;j++){dst.loc[j]=src.loc[j];dst.der[j]=src.der[j];dst.child[j]=splineIndex(src.val[j]);}
    }
    BiomeTree tree;size_t stepBytes=0,paramBytes=0,nodeBytes=0;
#define SELECT_TREE(name) tree={name##_steps,&name##_param[0][0],name##_nodes,name##_order,sizeof(name##_nodes)/sizeof(uint64_t)};stepBytes=sizeof(name##_steps);paramBytes=sizeof(name##_param);nodeBytes=sizeof(name##_nodes)
    if(mc>=MC_26_3){SELECT_TREE(btree263);}
    else if(mc>=MC_26_2){SELECT_TREE(btree262);}
    else if(mc>=MC_1_21_5){SELECT_TREE(btree215);}
    else if(mc>=MC_1_21_4){SELECT_TREE(btree21wd);}
    else if(mc>=MC_1_20_6){SELECT_TREE(btree20);}
    else if(mc>=MC_1_19_4){SELECT_TREE(btree19);}
    else if(mc>=MC_1_19_2){SELECT_TREE(btree192);}
    else{SELECT_TREE(btree18);}
#undef SELECT_TREE
    auto upload=[](void **dst,const void *src,size_t bytes) {
        return hipMalloc(dst,bytes)==hipSuccess&&hipMemcpy(*dst,src,bytes,hipMemcpyHostToDevice)==hipSuccess;
    };
    if(!upload(&host->steps,tree.steps,stepBytes)||!upload(&host->param,tree.param,paramBytes)||!upload(&host->nodes,tree.nodes,nodeBytes)) {
        cvhip_destroy_biomes(host);return nullptr;
    }
    p.tree=tree;p.tree.steps=static_cast<const uint32_t*>(host->steps);
    p.tree.param=static_cast<const int32_t*>(host->param);p.tree.nodes=static_cast<const uint64_t*>(host->nodes);
    const uint64_t salts[6][2]={
        {large?0x944b0073edf549dbULL:0x5c7e6b29735f0d7fULL,large?0x4ff44347e9d22b96ULL:0xf7d86f1bbc734988ULL},
        {large?0x71b8ab943dbd5301ULL:0x81bb4d22e8dc168eULL,large?0xbb63ddcf39ff7a2bULL:0xf1c8b4bea16303cdULL},
        {large?0x9a3f51a113fce8dcULL:0x83886c9d0ae3a662ULL,large?0xee2dbd157e5dcdadULL:0xafa638a61b42e8adULL},
        {large?0x8c984b1f8702a951ULL:0xd02491e6058f6fd8ULL,large?0xead7b1f92bae535fULL:0x4792512c94c17a80ULL},
        {0x080518cf6af25384ULL,0x3f3dfb40a54febd5ULL},{0xefc8ef4d36102b34ULL,0x1beeeb324a0f24eaULL}};
    const uint64_t keys[17][2]={
        {0xc613bf766619f992ULL,0x954753f86691b86aULL},{0x7eee475a921c6cf5ULL,0xf2bd39426f8da413ULL},
        {0xfc0027cef9683114ULL,0xb758d3954dcbfdd3ULL},{0xd1fc8a05be565ecaULL,0xdc2a3915cbdda25bULL},
        {0xb198de63a8012672ULL,0x7b84cad43ef7b5a8ULL},{0x0fd787bfbc403ec3ULL,0x74a4a31ca21b48b8ULL},
        {0x36d326eed40efeb2ULL,0x5be9ce18223c636aULL},{0x082fe255f8be6631ULL,0x4e96119e22dedc81ULL},
        {0x0ef68ec68504005eULL,0x48b6bf93a2789640ULL},{0xf11268128982754fULL,0x257a1d670430b0aaULL},
        {0xe51c98ce7d1de664ULL,0x5f9478a733040c45ULL},{0x6d7b49e7e429850aULL,0x2e3063c622a24777ULL},
        {0xbd90d5377ba1b762ULL,0xc07317d419a7548dULL},{0x53d39c6752dac858ULL,0xbcd1c5a80ab65b3eULL},
        {0xb4a24d7a84e7677bULL,0x023ff9668e89b5c4ULL},{0xdffa22b534c5f608ULL,0xb9b67517d3665ca9ULL},
        {0xd50708086cef4d7cULL,0x6e1651ecc7f43309ULL}};
    for(int para=0;para<NP_MAX;para++) {
        auto &c=p.climate[para];const auto &noise=bn.climate[para];c.lo=salts[para][0];c.hi=salts[para][1];
        c.na=noise.octA.octcnt;c.nb=noise.octB.octcnt;c.amplitude=noise.amplitude;int n=0;
        for(const auto *side:{&noise.octA,&noise.octB})for(int j=0;j<side->octcnt;j++) {
            const auto &o=side->octaves[j];int k=int(std::lround(std::log2(o.lacunarity)))+16;
            if(k<0||k>16){cvhip_destroy_biomes(host);return nullptr;}
            c.octaves[n++]={keys[k][0],keys[k][1],o.lacunarity,o.amplitude};
        }
    }
    if(hipMemcpy(host->device,&p,sizeof(p),hipMemcpyHostToDevice)!=hipSuccess){cvhip_destroy_biomes(host);return nullptr;}
    return host;
}
void cvhip_destroy_biomes(void *handle) {auto *p=static_cast<HostBiomeProgram*>(handle);if(p){if(p->device)(void)hipFree(p->device);if(p->steps)(void)hipFree(p->steps);if(p->param)(void)hipFree(p->param);if(p->nodes)(void)hipFree(p->nodes);delete p;}}
unsigned cvhip_biome_center_cells(void *handle) {auto *p=static_cast<HostBiomeProgram*>(handle);return p?p->centerCells:0;}
size_t cvhip_biome_center_stride(void *handle) {auto *p=static_cast<HostBiomeProgram*>(handle);return p?p->centerStride:0;}
hipError_t cvhip_launch_biome_filter(void *handle,hipStream_t stream,uint64_t start,unsigned count,
    const uint64_t *seeds,const CvHipCondition *program,unsigned ncfg,unsigned char *mask,const uint32_t *offsets,unsigned char *scratch,size_t stride,unsigned cells) {
    auto *p=static_cast<HostBiomeProgram*>(handle);if(!p)return hipErrorInvalidValue;
    if(p->dimensions&(1U<<(DIM_OVERWORLD+1)))hipLaunchKernelGGL((biomeFilter<DIM_OVERWORLD>),dim3((count+63)/64),dim3(64),0,stream,start,count,seeds,program,ncfg,mask,offsets,p->device,scratch,stride,cells);
    if(p->dimensions&(1U<<(DIM_NETHER+1)))hipLaunchKernelGGL((biomeFilter<DIM_NETHER>),dim3((count+63)/64),dim3(64),0,stream,start,count,seeds,program,ncfg,mask,offsets,p->device,scratch,stride,cells);
    if(p->dimensions&(1U<<(DIM_END+1)))hipLaunchKernelGGL((biomeFilter<DIM_END>),dim3((count+63)/64),dim3(64),0,stream,start,count,seeds,program,ncfg,mask,offsets,p->device,scratch,stride,cells);
    return hipGetLastError();
}
