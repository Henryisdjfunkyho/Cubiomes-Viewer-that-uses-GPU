#ifndef CVHIP_NOISE_DEVICE_H
#define CVHIP_NOISE_DEVICE_H
#include "cvhip.h"
#include "adaptive_probe_policy.h"
// LDS form of the same Xoroshiro/Perlin math used by the original filter.
namespace cvhip_noise {
__device__ inline uint64_t rotate(uint64_t a,int n){return(a<<n)|(a>>(64-n));}
__device__ inline uint64_t next(uint64_t &a,uint64_t &b){uint64_t r=rotate(a+b,17)+a,v=b^a;a=rotate(a,49)^v^(v<<21);b=rotate(v,28);return r;}
__device__ inline void seed(uint64_t s,uint64_t &a,uint64_t &b){
    a=s^0x6a09e667f3bcc909ULL;b=a+0x9e3779b97f4a7c15ULL;
    a=(a^(a>>30))*0xbf58476d1ce4e5b9ULL;b=(b^(b>>30))*0xbf58476d1ce4e5b9ULL;
    a=(a^(a>>27))*0x94d049bb133111ebULL;b=(b^(b>>27))*0x94d049bb133111ebULL;a^=a>>31;b^=b>>31;
}
__device__ inline double uniform(uint64_t &a,uint64_t &b){return(next(a,b)>>11)*0x1.0p-53;}
__device__ inline unsigned bounded(uint64_t &a,uint64_t &b,unsigned bound){
    uint64_t p=uint64_t(uint32_t(next(a,b)))*bound;uint32_t low=uint32_t(p);
    if(low<bound){unsigned t=(-bound)%bound;while(low<t){p=uint64_t(uint32_t(next(a,b)))*bound;low=uint32_t(p);}}
    return unsigned(p>>32);
}
__device__ inline double fade(double v){return v*v*v*(v*(v*6-15)+10);}
__device__ inline double mix(double t,double a,double b){return a+t*(b-a);}
__device__ inline double grad(unsigned i,double x,double y,double z){
    switch(i&15){case 0:return x+y;case 1:return-x+y;case 2:return x-y;case 3:return-x-y;
    case 4:return x+z;case 5:return-x+z;case 6:return x-z;case 7:return-x-z;
    case 8:return y+z;case 9:return-y+z;case 10:return y-z;case 11:return-y-z;
    case 12:return x+y;case 13:return-y+z;case 14:return-x+y;default:return-y-z;}
}
struct Offset {double x,y,z;};
__device__ inline Offset initialize(uint64_t a,uint64_t b,unsigned char *d){
    Offset o={uniform(a,b)*256,uniform(a,b)*256,uniform(a,b)*256};
    for(unsigned i=0;i<256;i++)d[i*64]=i;
    for(unsigned i=0;i<256;i++){unsigned j=i+bounded(a,b,256-i);unsigned char v=d[i*64];d[i*64]=d[j*64];d[j*64]=v;}
    d[256*64]=d[0];return o;
}
__device__ inline double sample(const Offset &o,double x,double y,double z,const unsigned char *d){
    x+=o.x;y+=o.y;z+=o.z;double ix=floor(x),iy=floor(y),iz=floor(z);x-=ix;y-=iy;z-=iz;
    double tx=fade(x),ty=fade(y),tz=fade(z);
    unsigned hx=unsigned(int(ix))&255,hy=unsigned(int(iy))&255,hz=unsigned(int(iz))&255;
    unsigned ax=(d[hx*64]+hy)&255,bx=(d[(hx+1)*64]+hy)&255;
    unsigned aa=d[ax*64],ab=d[(ax+1)*64],ba=d[bx*64],bb=d[(bx+1)*64];
    double a0=grad(d[((aa+hz)&255)*64],x,y,z),a1=grad(d[((ba+hz)&255)*64],x-1,y,z);
    double b0=grad(d[((ab+hz)&255)*64],x,y-1,z),b1=grad(d[((bb+hz)&255)*64],x-1,y-1,z);
    double c0=grad(d[((aa+hz+1)&255)*64],x,y,z-1),c1=grad(d[((ba+hz+1)&255)*64],x-1,y,z-1);
    double d0=grad(d[((ab+hz+1)&255)*64],x,y-1,z-1),d1=grad(d[((bb+hz+1)&255)*64],x-1,y-1,z-1);
    return mix(tz,mix(ty,mix(tx,a0,a1),mix(tx,b0,b1)),mix(ty,mix(tx,c0,c1),mix(tx,d0,d1)));
}
// The shift parameter needs two 3D samples. Reuse each octave permutation.
__device__ inline void evaluate(uint64_t lo,uint64_t hi,const CvHipCondition &c,
    double x,double y,double z,double x2,double y2,double z2,bool second,
    unsigned char *d,double &first,double &other){
    uint64_t a=lo^c.lo,b=hi^c.hi;double sums[2]={0,0},sums2[2]={0,0};unsigned index=0;
    for(int side=0;side<2;side++) {
        uint64_t sl=next(a,b),sh=next(a,b);int n=side?c.nb:c.na;double f=side?337.0/331.0:1;
        for(int j=0;j<n;j++,index++) {
            const auto &p=c.octaves[index];auto o=initialize(sl^p.lo,sh^p.hi,d);
            sums[side]+=p.amplitude*sample(o,(x*f)*p.frequency,(y*f)*p.frequency,(z*f)*p.frequency,d);
            if(second)sums2[side]+=p.amplitude*sample(o,(x2*f)*p.frequency,(y2*f)*p.frequency,(z2*f)*p.frequency,d);
        }
    }
    first=(sums[0]+sums[1])*c.amplitude;other=(sums2[0]+sums2[1])*c.amplitude;
}
/*
 * Safe early rejection for Cubiomes' adaptive getParaRange() path.
 *
 * In finders.c, the adaptive path clears `skip`, visits lattice coordinate
 * (0,0) first, and calls its extrema callback immediately after sampling it.
 * Only after that callback can later lattice cells be marked skipped. Thus
 * the initial lattice is visited in row-major order before skip acceleration
 * begins. Every point in its first-16 prefix is therefore part of the CPU's
 * adaptive pass unless an earlier callback has already rejected the seed.
 * These points can prove only the necessary directions below; every
 * survivor remains CPU work because later descent/skip behavior may alter
 * the extrema.
 */
__device__ inline bool rejectAdaptiveRangeAtCoarsePrefix(
    uint64_t lo,uint64_t hi,const CvHipCondition &c,unsigned char *d) {
    const bool exclusion=(c.matchAny&4)!=0;
    if(c.invert || (!exclusion && (c.matchAny&3)==0) || c.na<=0 || c.na>32 || c.nb<0 ||
       c.na+c.nb>32) return false;

    const int64_t width=int64_t(c.x2)-int64_t(c.x)+1;
    const int64_t height=int64_t(c.z2)-int64_t(c.z)+1;
    if(width<=0 || height<=0 || width>INT_MAX || height>INT_MAX ||
       width>INT64_MAX/height) return false;
    const int64_t cells=width*height;

    double lmax=0.0;
    double lmin=INFINITY;
    for(int i=0;i<c.na;i++) {
        const double lac=c.octaves[i].frequency;
        if(!isfinite(lac) || lac<=0.0) return false;
        if(lac<lmin) lmin=lac;
        if(lac>lmax) lmax=lac;
    }
    if(!isfinite(lmin) || !isfinite(lmax) || lmin<=0.0 || lmax<=0.0 ||
       double(cells)<1000.0*sqrt(lmax)) return false;

    int step=0;
    if(!cvhip_probe_policy::coarseStep(lmin,&step)) return false;
    const unsigned count=cvhip_probe_policy::coarsePrefixCount(width,height,step);
    if(count==0) return false;

    const bool testMin=(c.matchAny&1) && isfinite(c.lower);
    const bool testMax=(c.matchAny&2) && isfinite(c.upper);
    if(exclusion) {
        if(!isfinite(c.lower) && !isfinite(c.upper)) return false;
    } else if(!testMin && !testMax) return false;

    for(unsigned index=0;index<count;index++) {
        int64_t dx=0,dz=0;
        if(!cvhip_probe_policy::coarsePrefixOffset(index,width,height,step,&dx,&dz))
            return false;
        double value=0.0,unused=0.0;
        evaluate(lo,hi,c,double(int64_t(c.x)+dx),0.0,
                 double(int64_t(c.z)+dz),0.0,0.0,0.0,false,d,value,unused);
        value*=10000.0;
        if(!isfinite(value)) continue;
        // Preserve the verifier's conservative near-boundary policy.
        if((isfinite(c.lower) && fabs(value-c.lower)<1e-6) ||
           (isfinite(c.upper) && fabs(value-c.upper)<1e-6)) continue;
        if(cvhip_probe_policy::necessaryFailure(c.matchAny,value,c.lower,c.upper))
            return true;
    }
    return false;
}
// Compatibility name for existing device-side tests/callers.
__device__ inline bool rejectAdaptiveRangeAtFirstSample(
    uint64_t lo,uint64_t hi,const CvHipCondition &c,unsigned char *d) {
    return rejectAdaptiveRangeAtCoarsePrefix(lo,hi,c,d);
}
// Small rectangles use Cubiomes' exhaustive small-area branch. Reuse each
// octave's permutation across a bounded tile rather than rebuilding it per
// cell.
__device__ inline void evaluateGrid(uint64_t lo,uint64_t hi,const CvHipCondition &c,
    unsigned char *d,double &minimum,double &maximum) {
    /* Cubiomes' CPU exhaustive branch is selected by area, while a GPU
     * thread must keep its temporary storage bounded. Re-evaluate the
     * octaves in fixed 16-cell tiles and reduce each tile immediately. */
    const int64_t width=int64_t(c.x2)-int64_t(c.x)+1;
    const int64_t height=int64_t(c.z2)-int64_t(c.z)+1;
    if(width<=0||height<=0||width>INT_MAX||height>INT_MAX) {
        minimum=NAN;maximum=NAN;return;
    }
    const int64_t total=width*height;
    if(total<=0) {
        minimum=NAN;maximum=NAN;return;
    }
    minimum=INFINITY;maximum=-INFINITY;
    for(int64_t base=0;base<total;base+=16) {
        const int tile=int((total-base)<16?(total-base):16);
        double sums[2][16]={};
        uint64_t a=lo^c.lo,b=hi^c.hi;unsigned index=0;
        for(int side=0;side<2;side++) {
            uint64_t sl=next(a,b),sh=next(a,b);
            const double f=side?337.0/331.0:1.0;
            const int count=side?c.nb:c.na;
            for(int j=0;j<count;j++,index++) {
                const auto &p=c.octaves[index];
                auto o=initialize(sl^p.lo,sh^p.hi,d);
                for(int k=0;k<tile;k++) {
                    const int64_t cell=base+k;
                    const double cellX=double(int64_t(c.x)+cell%width);
                    const double cellZ=double(int64_t(c.z)+cell/width);
                    sums[side][k]+=p.amplitude*sample(o,
                        (cellX*f)*p.frequency,0,(cellZ*f)*p.frequency,d);
                }
            }
        }
        for(int k=0;k<tile;k++) {
            const double v=(sums[0][k]+sums[1][k])*c.amplitude*10000;
            /* Never let NaN handling in fmin/fmax turn an unsafe result into
             * a rejection. The caller treats NaN as an ambiguous CPU check. */
            if(!isfinite(v)) { minimum=NAN;maximum=NAN;return; }
            if(v<minimum)minimum=v;
            if(v>maximum)maximum=v;
        }
    }
}
}
#endif
