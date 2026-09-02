// pitch_glitch_events (2 Sep 2026, the waveform-discontinuity pivot):
// EVENT-LEVEL, not distributional - the standing caution's instrument.
// Long-term-prediction residual e[n] = x[n] - g*x[n-P] (P from the
// file's OWN local period, g from local normalised correlation), scored
// per 10ms block as peak|e| / blockRMS, EXCESS over the source at the
// aligned block index, loud+voiced blocks only. Every event above
// threshold is reported with its timestamp, excess, and distance to the
// nearest word start - aggregates cannot see 12 events in 860 blocks.
//
// Alignment: full cross-correlation lag search, REPORTED per file, and
// the compared blocks are verified voiced-in-source (the container's
// Antares column died of a 1714-sample offset against silent blocks).
//
// Self-renders (current engine, tau6) carry the ring tap; each event is
// cross-referenced against tap data: dry->wet seam within 20ms (tap
// entry gap), and the local f0Here/target step sizes.
//
//   pitch_glitch_events <source.wav> <vt> <dminor|chrom> <spec>...
//     spec: file:<path> | self:ignon | self:ignoff | self:grain
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
using namespace echojay;
static bool readWavMono (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f=std::fopen(path,"rb"); if(!f) return false;
    auto rd32=[&]{uint8_t b[4]; if(std::fread(b,1,4,f)!=4)return 0u; return (uint32_t)(b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24));};
    auto rd16=[&]{uint8_t b[2]; if(std::fread(b,1,2,f)!=2)return 0u; return (uint32_t)(b[0]|(b[1]<<8));};
    char tag[5]={};
    if(std::fread(tag,1,4,f)!=4||std::strncmp(tag,"RIFF",4)){std::fclose(f);return false;}
    rd32();
    if(std::fread(tag,1,4,f)!=4||std::strncmp(tag,"WAVE",4)){std::fclose(f);return false;}
    uint16_t fmt=0,ch=0,bits=0;uint32_t rate=0;
    while(std::fread(tag,1,4,f)==4)
    { uint32_t sz=rd32();
      if(!std::strncmp(tag,"fmt ",4)){fmt=(uint16_t)rd16();ch=(uint16_t)rd16();rate=rd32();rd32();rd16();bits=(uint16_t)rd16(); if(sz>16)std::fseek(f,(long)sz-16,SEEK_CUR);}
      else if(!std::strncmp(tag,"data",4))
      { uint32_t bp=bits/8u, fr=sz/(bp*ch); out.resize(fr);
        std::vector<uint8_t> b(bp*ch);
        for(uint32_t i=0;i<fr;++i){ if(std::fread(b.data(),1,b.size(),f)!=b.size())break;
          double acc=0;
          for(uint16_t c=0;c<ch;++c){ const uint8_t* q=b.data()+c*bp;
            if(fmt==3&&bits==32){float v;std::memcpy(&v,q,4);acc+=v;}
            else if(fmt==1&&bits==16){acc+=(double)(int16_t)(q[0]|(q[1]<<8))/32768.0;}
            else if(fmt==1&&bits==24){acc+=(double)((int32_t)((q[0]<<8)|(q[1]<<16)|((uint32_t)q[2]<<24))>>8)/8388608.0;} }
          out[i]=(float)(acc/ch); }
        fs=rate; std::fclose(f); return true; }
      else std::fseek(f,(long)(sz+(sz&1)),SEEK_CUR); }
    std::fclose(f); return false;
}
static std::vector<double> fineTrack (const std::vector<float>& x, double fs, int vt, int& hop)
{
    PitchEngine e; e.prepare(fs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal);
    hop=e.inputHopLength(vt);
    std::vector<double> t;
    for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop)
    { e.process(x.data()+p,nullptr,hop);
      const PitchReading r=e.getReading();
      t.push_back(r.voiced&&r.f0Hz>0?(double)r.f0Hz:0.0); }
    return t;
}
struct Render { std::vector<float> out; std::vector<PsolaEngine::DbgRingTap> tap; };
static Render renderSelf (const std::vector<float>& in, double fs, int vt,
                          bool chrom, bool ignVib, bool grain)
{
    Render R;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,chrom,0);
    if(!chrom){ for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0); corr.setKeyRoot(2); }
    else corr.setKeyRoot(0);
    corr.setRetuneMs(6.0f); corr.setFlex(0); corr.setHumanize(0);
    corr.setIgnoreVibrato(ignVib); corr.setNaturalVibrato(0);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    sh.debugRingTap(true);
    if(grain) sh.debugDisableSplice(true);
    const int lat=sh.latencySamples();
    std::vector<float> raw(in.size(),0.0f);
    PitchEngine::HopEvent ev[64];
    float target=0, sliceF0=0; float shift=PsolaEngine::kNoShift; bool sliceVoiced=false;
    for(size_t p=0;p+256<=in.size();p+=256)
    {
        det.process(in.data()+p,nullptr,256);
        const uint64_t blockStart=det.inputPosition()-256;
        const int n=det.drainHops(ev,64);
        int cursor=0;
        for(int h=0;h<=n;++h)
        {
            int sliceEnd=256;
            if(h<n){ const int64_t rel=(int64_t)ev[h].inputPos-(int64_t)blockStart;
                     sliceEnd=(int)std::clamp(rel,(int64_t)cursor,(int64_t)256); }
            if(sliceEnd>cursor)
            { sh.process(in.data()+p+(size_t)cursor,raw.data()+p+(size_t)cursor,
                         sliceEnd-cursor,sliceF0,sliceVoiced,target,shift); cursor=sliceEnd; }
            if(h<n)
            {
                float rO=-1,rN=-1;
                const bool seed=ev[h].voiced&&ev[h].f0Hz>0&&gate.lastGood()<=0;
                if(gate.isBigJump(ev[h].f0Hz,ev[h].voiced)||seed)
                { const double ref=seed?2.0*ev[h].f0Hz:gate.lastGood();
                  rO=sh.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ref));
                  rN=sh.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ev[h].f0Hz)); }
                const float g=gate.filter(ev[h].f0Hz,ev[h].voiced,hopMs,rO,rN);
                sliceF0=g; sliceVoiced=ev[h].voiced;
                if(grain)
                { if(g>0){ target=g; shift=0.0f; } }        // unity, grain path
                else
                { const float t=corr.process(g,ev[h].voiced,hopMs);
                  if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; } }
            }
        }
    }
    R.out.assign(in.size(),0.0f);
    for(size_t i=(size_t)lat;i<raw.size();++i) R.out[i-(size_t)lat]=raw[i];
    R.tap=sh.debugRingTapData();
    return R;
}
// alignment: coarse abs-envelope cross-correlation +-4000, refine +-64
static long alignLag (const std::vector<float>& ref, const std::vector<float>& x)
{
    const int dec=16;
    std::vector<double> a,b;
    for(size_t i=0;i+dec<=ref.size();i+=dec){ double s=0; for(int k=0;k<dec;++k)s+=std::fabs(ref[i+k]); a.push_back(s); }
    for(size_t i=0;i+dec<=x.size();i+=dec){ double s=0; for(int k=0;k<dec;++k)s+=std::fabs(x[i+k]); b.push_back(s); }
    const long span=4000/dec;
    long best=0; double bestV=-1;
    for(long L=-span;L<=span;++L)
    { double s=0; long n=0;
      for(long i=0;i<(long)a.size();++i)
      { const long j=i+L; if(j<0||j>=(long)b.size()) continue; s+=a[(size_t)i]*b[(size_t)j]; ++n; }
      if(n>0&&s>bestV){ bestV=s; best=L; } }
    long coarse=best*dec, fine=coarse; bestV=-1;
    for(long L=coarse-64;L<=coarse+64;++L)
    { double s=0;
      for(size_t i=0;i<ref.size();i+=4)
      { const long j=(long)i+L; if(j<0||j>=(long)x.size()) continue; s+=(double)ref[i]*(double)x[(size_t)j]; }
      if(s>bestV){ bestV=s; fine=L; } }
    return fine;   // x[i+lag] aligns with ref[i]
}
struct BlockScore { std::vector<double> score; std::vector<int> loud, voiced; double rmsGate; };
static BlockScore ltpScore (const std::vector<float>& x, double fs,
                            const std::vector<double>& track, int hop)
{
    const int B=(int)std::lround(0.010*fs);
    const size_t nb=x.size()/(size_t)B;
    BlockScore S; S.score.assign(nb,0); S.loud.assign(nb,0); S.voiced.assign(nb,0);
    std::vector<double> rmsAll;
    for(size_t b=0;b<nb;++b)
    { double s2=0;
      for(int i=0;i<B;++i){ const double v=x[b*B+(size_t)i]; s2+=v*v; }
      rmsAll.push_back(std::sqrt(s2/B)); }
    std::vector<double> tmp=rmsAll; std::sort(tmp.begin(),tmp.end());
    S.rmsGate=0.10*tmp[(size_t)(0.95*(double)tmp.size())];
    for(size_t b=0;b<nb;++b)
    {
        const size_t s=b*(size_t)B;
        const long h=(long)((double)(s+(size_t)B/2)/hop);
        const double f0=(h>=0&&h<(long)track.size())?track[(size_t)h]:0.0;
        if(f0<=0) continue;
        S.voiced[b]=1;
        if(rmsAll[b]<S.rmsGate) continue;
        S.loud[b]=1;
        const int P=(int)std::lround(fs/f0);
        if(s<(size_t)P+4) continue;
        double num=0,den=0;
        for(int i=0;i<B;++i)
        { const double c=x[s+(size_t)i], pr=x[s+(size_t)i-(size_t)P];
          num+=c*pr; den+=pr*pr; }
        const double g=den>1e-12?std::clamp(num/den,0.0,1.2):0.0;
        double pk=0;
        for(int i=0;i<B;++i)
        { const double e=std::fabs((double)x[s+(size_t)i]-g*(double)x[s+(size_t)i-(size_t)P]);
          pk=std::max(pk,e); }
        S.score[b]=rmsAll[b]>1e-9?pk/rmsAll[b]:0.0;
    }
    return S;
}
int main (int argc, char** argv)
{
    if(argc<5){ std::printf("usage: %s <source.wav> <vt> <dminor|chrom> <spec>...\n",argv[0]); return 1; }
    std::vector<float> src; double fs=0;
    if(!readWavMono(argv[1],src,fs)){ std::printf("bad source\n"); return 1; }
    const int vt=atoi(argv[2]);
    const bool chrom=!std::strcmp(argv[3],"chrom");
    int hop=0; auto tSrc=fineTrack(src,fs,vt,hop);
    const double hopS=hop/fs;
    // word starts: voiced onset after >=60ms unvoiced (the standing convention)
    std::vector<double> words;
    { int uv=1000;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){++uv;continue;}
        if(uv*hopS>=0.06) words.push_back((double)h*hopS);
        uv=0; } }
    auto srcScore=ltpScore(src,fs,tSrc,hop);
    std::printf("source %s  voice %s  word starts (%zu):",argv[1],
        PitchEngine::voiceRange(vt).id,words.size());
    for(double w:words) std::printf(" %.2f",w);
    std::printf("\n");
    for(int a=4;a<argc;++a)
    {
        std::string spec=argv[a];
        std::vector<float> x; Render R; bool haveTap=false;
        std::string label=spec;
        if(spec=="self:ignon"||spec=="self:ignoff"||spec=="self:grain")
        { R=renderSelf(src,fs,vt,chrom,spec=="self:ignon",spec=="self:grain");
          x=R.out; haveTap=true;
          label=spec=="self:grain"?"CURRENT grain-path unity"
               :spec=="self:ignon"?"CURRENT tau6 ignore-vib ON":"CURRENT tau6 ignore-vib OFF"; }
        else if(!spec.compare(0,5,"file:"))
        { double f2=0;
          if(!readWavMono(spec.c_str()+5,x,f2)){ std::printf("%s unreadable\n",spec.c_str()); continue; }
          label=spec.substr(5); const size_t sl=label.rfind('/');
          if(sl!=std::string::npos) label=label.substr(sl+1); }
        else { std::printf("bad spec %s\n",spec.c_str()); continue; }
        const long lag=haveTap?0:alignLag(src,x);
        int rh=0; auto tX=fineTrack(x,fs,vt,rh);
        auto S=ltpScore(x,fs,tX,rh);
        const int B=(int)std::lround(0.010*fs);
        // events: excess over source at the ALIGNED block, loud+voiced both sides
        struct Ev { double t, ex; };
        std::vector<Ev> evs;
        long unvoicedCompares=0, compared=0;
        for(size_t b=0;b<S.score.size();++b)
        {
            if(!S.loud[b]) continue;
            // this render block's source-time = (b*B + lag') where x[i+lag]~ref[i]
            const long sb=((long)(b*(size_t)B)-lag)/B;
            if(sb<0||sb>=(long)srcScore.score.size()) continue;
            ++compared;
            if(!srcScore.voiced[(size_t)sb]){ ++unvoicedCompares; continue; }
            if(!srcScore.loud[(size_t)sb]) continue;
            const double ex=S.score[b]-srcScore.score[(size_t)sb];
            if(ex>1.5) evs.push_back({ (double)sb*0.010, ex });
        }
        // merge adjacent blocks into events, keep the peak
        std::vector<Ev> merged;
        for(const Ev& e:evs)
        { if(!merged.empty()&&e.t-merged.back().t<0.030)
          { if(e.ex>merged.back().ex) merged.back()=e; }
          else merged.push_back(e); }
        int near=0;
        std::printf("\n%s  [lag %+ld smp; %ld/%ld loud blocks hit unvoiced source%s]\n",
            label.c_str(),lag,unvoicedCompares,compared,
            unvoicedCompares*10>compared?" ** ALIGNMENT SUSPECT **":"");
        std::printf("  events >1.5 excess: %zu\n",merged.size());
        for(const Ev& e:merged)
        {
            double dNear=1e9;
            for(double w:words) dNear=std::min(dNear,std::fabs(e.t-w));
            const bool close=[&]{ for(double w:words) if(e.t>=w-0.02&&e.t<=w+0.150) return true; return false; }();
            if(close) ++near;
            std::printf("    t=%6.2fs  excess %5.2f  nearest word start %+.0fms%s",
                e.t,e.ex,1000.0*dNear,close?"  [ONSET]":"");
            if(haveTap)
            {
                // tap cross-ref: dry->wet seam = gap in tap entries ending near t;
                // and the largest f0Here / target step within +-15ms
                const double ts=e.t*fs;
                double gapEnd=-1, gapLen=0, df0=0, dtg=0;
                for(size_t i=1;i<R.tap.size();++i)
                { const double t0=(double)R.tap[i-1].inPos, t1=(double)R.tap[i].inPos;
                  if(t1-t0>0.020*fs&&std::fabs(t1-ts)<0.030*fs){ gapEnd=t1/fs; gapLen=(t1-t0)/fs; }
                  if(std::fabs(t1-ts)<0.015*fs)
                  { df0=std::max(df0,std::fabs(1200.0*std::log2((double)R.tap[i].f0Here/R.tap[i-1].f0Here)));
                    dtg=std::max(dtg,std::fabs(1200.0*std::log2((double)R.tap[i].target/R.tap[i-1].target))); } }
                if(gapEnd>=0) std::printf("  | SEAM: wet resumes %.2fs after %.0fms dry",gapEnd,1000.0*gapLen);
                if(df0>0||dtg>0) std::printf("  | steps +-15ms: f0Here %.0fc target %.0fc",df0,dtg);
            }
            std::printf("\n");
        }
        std::printf("  within 150ms of a word start: %d/%zu (%.0f%%)\n",
            near,merged.size(),merged.empty()?0:100.0*near/(double)merged.size());
    }
    return 0;
}
