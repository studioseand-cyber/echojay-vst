// pitch_residual_closure (2 Sep 2026, rounds 8-9): the differential
// form, one tracker, no offline truth ruler. Round 9 re-expresses
// everything as PER-INSTANT SIGNED differences (median|a|-median|b| is
// not median|a-b| - the standing methodological correction) and reports
// a THREE-LEG DECOMPOSITION using the effective-ratio tap:
//   LEG 1  T(src)*effR vs T(out)     - does the shifter do what the
//          applied ratio says? must close ~0 or everything upstream is moot
//   LEG 2  effR vs target/f0Here     - what slew+bleed modify, and WHEN
//   LEG 3  T(out) vs target          - the residual, now decomposable
// PREDICTION (stated before the run): leg 2 ~3c on sustains, ~0 in the
// first 50ms after an onset - a tau~100ms feedback has no time inside
// an onset window.
//
//   f_out = f_in * target / f0Here
//   => output off-grid-vs-target = 1200*log2( T(source)/f0Here )
//
// Prediction side: T on the SOURCE + the tapped ring value (the real
// render's f0Here, via PsolaEngine::debugRingTap - no reconstruction).
// Measurement side: the SAME tracker T on the OUTPUT vs the tapped
// target. T's systematic bias appears on both sides and largely cancels.
//
// ORDER OF OPERATIONS, per the ruling:
//   0. BIT-IDENTITY: render with tap off and tap on, byte-compare.
//      Nothing is read from the tap until this passes.
//   1. SUSTAIN GATE: predicted must match measured at sustains within
//      tracker noise (and land near the known 1.9c). Fail -> report,
//      STOP, no onset numbers.
//   2. Onsets: med/p75/p90 + 150ms trajectory, both takes.
//   3. Standing prediction (stated before result): onset residual 3-5c,
//      sustain ~1-2c. Marked pass/fail.
//
//   pitch_residual_closure <take.wav> <voiceType> <dminor|chrom>
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
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
struct Render {
    std::vector<float> out;                       // latency-aligned
    std::vector<PsolaEngine::DbgRingTap> tap;     // inPos == aligned index
    std::vector<PsolaEngine::DbgEffR> effR;       // 64-sample grid
};
static Render renderChain (const std::vector<float>& in, double fs, int vt,
                           float tau, bool chrom, bool tapOn)
{
    Render R;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,chrom,0);
    if(!chrom){ for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0); corr.setKeyRoot(2); }
    else corr.setKeyRoot(0);
    corr.setRetuneMs(tau); corr.setFlex(0); corr.setHumanize(0);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(0);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    sh.debugRingTap(tapOn);
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
                const float t=corr.process(g,ev[h].voiced,hopMs);
                if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; }
            }
        }
    }
    R.out.assign(in.size(),0.0f);
    for(size_t i=(size_t)lat;i<raw.size();++i) R.out[i-(size_t)lat]=raw[i];
    R.tap=sh.debugRingTapData();   // inPos = p = aligned output index
    R.effR=sh.debugEffRData();
    return R;
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
int main (int argc, char** argv)
{
    if(argc<4){ std::printf("usage: %s <take.wav> <vt> <dminor|chrom>\n",argv[0]); return 1; }
    std::vector<float> in; double fs=0;
    if(!readWavMono(argv[1],in,fs)){ std::printf("bad input\n"); return 1; }
    const int vt=atoi(argv[2]);
    const bool chrom=!std::strcmp(argv[3],"chrom");
    std::printf("%s  voice %s  grid %s  tau6\n",argv[1],
        PitchEngine::voiceRange(vt).id,chrom?"chromatic":"D-minor");

    // ---- 0: bit-identity - the tap must not touch the audio ----------
    Render off=renderChain(in,fs,vt,6.0f,chrom,false);
    Render on =renderChain(in,fs,vt,6.0f,chrom,true);
    const bool identical = off.out.size()==on.out.size()
        && 0==std::memcmp(off.out.data(),on.out.data(),off.out.size()*sizeof(float));
    std::printf("BIT-IDENTITY tap-off vs tap-on: %s (%zu samples, %zu tap entries)\n",
        identical?"IDENTICAL":"** DIFFERS - TAP IS NOT NEUTRAL, STOPPING **",
        off.out.size(),on.tap.size());
    if(!identical) return 1;

    // ---- tracks: one tracker, both sides ------------------------------
    int hop=0;
    auto tSrc=fineTrack(in,fs,vt,hop);
    auto tOut=fineTrack(on.out,fs,vt,hop);
    const double hopS=hop/fs;
    const auto& tapR=on.effR;
    std::printf("effR entries: %zu\n",tapR.size());
    auto tapAt=[&](double t,double& f0,double& tg)
    { f0=0;tg=0;
      size_t lo=0,hi=on.tap.size();
      while(lo<hi){ size_t m=(lo+hi)/2; if((double)on.tap[m].inPos<=t)lo=m+1; else hi=m; }
      if(lo==0) return;
      f0=on.tap[lo-1].f0Here; tg=on.tap[lo-1].target; };
    auto effAt=[&](double t)->double
    { size_t lo=0,hi=tapR.size();
      while(lo<hi){ size_t m=(lo+hi)/2; if((double)tapR[m].inPos<=t)lo=m+1; else hi=m; }
      if(lo==0) return 0;
      // only trust an entry within 256 samples (the grid is 64): a stale
      // entry from before an unvoiced gap is not the current rate
      if(t-(double)tapR[lo-1].inPos>256.0) return 0;
      return (double)tapR[lo-1].effR; };
    struct Legs { std::vector<double> l1,l2,l3; };
    auto residuals=[&](long h0,long h1,Legs& L,
                       std::vector<std::vector<double>>* tr2,
                       std::vector<std::vector<double>>* tr3)
    { for(long h=h0;h<h1;++h)
      { if(h>=(long)tSrc.size()||h>=(long)tOut.size()) break;
        if(tSrc[(size_t)h]<=0||tOut[(size_t)h]<=0) continue;
        const double t=(double)h*hop+0.5*hop;
        double f0,tg; tapAt(t,f0,tg);
        const double eff=effAt(t);
        if(f0<=0||tg<=0||eff<=0) continue;
        const double l1=1200.0*std::log2(tSrc[(size_t)h]*eff/tOut[(size_t)h]);
        const double l2=1200.0*std::log2(eff*f0/tg);
        const double l3=1200.0*std::log2(tOut[(size_t)h]/tg);
        if(std::fabs(l3)>200||std::fabs(l1)>200) continue;
        L.l1.push_back(l1); L.l2.push_back(l2); L.l3.push_back(l3);
        const size_t bin=(size_t)((h-h0)*hopS/0.005);
        if(tr2&&bin<tr2->size()) (*tr2)[bin].push_back(l2);
        if(tr3&&bin<tr3->size()) (*tr3)[bin].push_back(l3); } };
    std::vector<long> onsets;
    { int uv=1000;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){++uv;continue;}
        if(uv*hopS>=0.06) onsets.push_back((long)h);
        uv=0; } }
    const long win=(long)std::lround(0.150/hopS);
    const long win50=(long)std::lround(0.050/hopS);
    auto Q=[](std::vector<double> v,double f)->double
    { if(v.empty())return 0; std::sort(v.begin(),v.end());
      return v[(size_t)std::min((double)v.size()-1.0,f*(double)v.size())]; };
    auto row=[&](const char* nm,std::vector<double>& v)
    { std::vector<double> a; for(double d:v) a.push_back(std::fabs(d));
      std::printf("    %-6s signed med %+6.2fc  [p25 %+6.2f p75 %+6.2f]  med|.| %5.2fc  (n=%zu)\n",
          nm,Q(v,0.5),Q(v,0.25),Q(v,0.75),Q(a,0.5),v.size()); };
    Legs S,O,O50;
    std::vector<std::vector<double>> tr2(30),tr3(30);
    { int run=0;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){run=0;continue;} ++run;
        if(run==(int)std::lround(0.30/hopS))
          residuals((long)h,(long)h+win,S,nullptr,nullptr); } }
    for(long o:onsets){ residuals(o,o+win,O,&tr2,&tr3);
                        residuals(o,o+win50,O50,nullptr,nullptr); }
    std::printf("  SUSTAINS:\n");  row("leg1",S.l1); row("leg2",S.l2); row("leg3",S.l3);
    std::printf("  ONSETS (150ms):\n"); row("leg1",O.l1); row("leg2",O.l2); row("leg3",O.l3);
    std::printf("  ONSETS (first 50ms):\n"); row("leg1",O50.l1); row("leg2",O50.l2); row("leg3",O50.l3);
    // FAILURE-BRANCH CORRELATIONS (round 9): leg 1 did not close on the
    // first run, so report what its per-instant difference correlates
    // with: (a) |pitch slope| - the displacement/delta-x-slope candidate;
    // (b) the estimator-vs-tracker difference TS-F0 - the calibration-
    // offset candidate (per-instant l1 = (TS-F0) + l2 - l3 exactly, so a
    // HIGH correlation here with l2,l3 quiet means the gap and the
    // "estimator error" are the same number seen twice, not two facts).
    {
        std::vector<double> l1v,slv,efv;
        auto grab=[&](long h0,long h1)
        { for(long h=h0;h<h1;++h)
          { if(h+1>=(long)tSrc.size()||h>=(long)tOut.size()) break;
            if(tSrc[(size_t)h]<=0||tSrc[(size_t)h+1]<=0||tOut[(size_t)h]<=0) continue;
            const double t=(double)h*hop+0.5*hop;
            double f0,tg; tapAt(t,f0,tg);
            const double eff=effAt(t);
            if(f0<=0||tg<=0||eff<=0) continue;
            const double l1=1200.0*std::log2(tSrc[(size_t)h]*eff/tOut[(size_t)h]);
            if(std::fabs(l1)>200) continue;
            const double slope=std::fabs(1200.0*std::log2(tSrc[(size_t)h+1]/tSrc[(size_t)h]))/hopS;
            const double ef=1200.0*std::log2(tSrc[(size_t)h]/f0);
            if(std::fabs(ef)>200) continue;
            l1v.push_back(l1); slv.push_back(slope); efv.push_back(ef); } };
        { int run=0;
          for(size_t h=0;h<tSrc.size();++h)
          { if(tSrc[h]<=0){run=0;continue;} ++run;
            if(run==(int)std::lround(0.30/hopS)) grab((long)h,(long)h+win); } }
        for(long o:onsets) grab(o,o+win);
        auto pearson=[&](std::vector<double>& a,std::vector<double>& b)->double
        { const size_t n=std::min(a.size(),b.size()); if(n<10) return 0;
          double ma=0,mb=0; for(size_t i=0;i<n;++i){ma+=a[i];mb+=b[i];} ma/=n;mb/=n;
          double sab=0,sa=0,sb=0;
          for(size_t i=0;i<n;++i){ sab+=(a[i]-ma)*(b[i]-mb); sa+=(a[i]-ma)*(a[i]-ma); sb+=(b[i]-mb)*(b[i]-mb); }
          return sa>0&&sb>0?sab/std::sqrt(sa*sb):0; };
        std::vector<double> l1a; for(double d:l1v) l1a.push_back(std::fabs(d));
        std::printf("  LEG1 correlations (n=%zu): |l1| vs |slope| r=%+.2f | l1 vs (TS-F0) r=%+.2f | |l1| vs |TS-F0| r=%+.2f\n",
            l1v.size(),pearson(l1a,slv),pearson(l1v,efv),
            [&]{ std::vector<double> e2; for(double d:efv) e2.push_back(std::fabs(d)); return pearson(l1a,e2); }());
    }
    auto traj=[&](const char* nm,std::vector<std::vector<double>>& tr)
    { std::printf("  %s traj (signed med, 5ms bins):",nm);
      for(auto& b:tr){ if(b.empty()){std::printf("    .");continue;}
        std::sort(b.begin(),b.end()); std::printf(" %+4.0f",b[b.size()/2]); }
      std::printf("\n"); };
    traj("leg2",tr2); traj("leg3",tr3);
    return 0;
}
