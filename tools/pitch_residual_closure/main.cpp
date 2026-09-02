// pitch_residual_closure (2 Sep 2026, round-8 ruling): the differential
// form, one tracker, no offline truth ruler.
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
    // tap lookup: last entry with inPos <= t
    auto tapAt=[&](double t,double& f0,double& tg)
    { f0=0;tg=0;
      size_t lo=0,hi=on.tap.size();
      while(lo<hi){ size_t m=(lo+hi)/2; if((double)on.tap[m].inPos<=t)lo=m+1; else hi=m; }
      if(lo==0) return;
      f0=on.tap[lo-1].f0Here; tg=on.tap[lo-1].target; };
    auto residuals=[&](long h0,long h1,std::vector<double>& pred,std::vector<double>& meas,
                       std::vector<std::vector<double>>* trP,std::vector<std::vector<double>>* trM)
    { for(long h=h0;h<h1;++h)
      { if(h>=(long)tSrc.size()||h>=(long)tOut.size()) break;
        if(tSrc[(size_t)h]<=0||tOut[(size_t)h]<=0) continue;
        double f0,tg; tapAt((double)h*hop+0.5*hop,f0,tg);
        if(f0<=0||tg<=0) continue;
        const double rp=1200.0*std::log2(tSrc[(size_t)h]/f0);
        const double rm=1200.0*std::log2(tOut[(size_t)h]/tg);
        if(std::fabs(rp)>200||std::fabs(rm)>200) continue;
        pred.push_back(std::fabs(rp)); meas.push_back(std::fabs(rm));
        const size_t bin=(size_t)((h-h0)*hopS/0.005);
        if(trP&&bin<trP->size()) (*trP)[bin].push_back(std::fabs(rp));
        if(trM&&bin<trM->size()) (*trM)[bin].push_back(std::fabs(rm)); } };
    // windows exactly as the other probes
    std::vector<long> onsets;
    { int uv=1000;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){++uv;continue;}
        if(uv*hopS>=0.06) onsets.push_back((long)h);
        uv=0; } }
    const long win=(long)std::lround(0.150/hopS);
    auto q=[](std::vector<double> v,double f)->double
    { if(v.empty())return -1; std::sort(v.begin(),v.end());
      return v[(size_t)std::min((double)v.size()-1.0,f*(double)v.size())]; };

    // ---- 1: THE SUSTAIN GATE ------------------------------------------
    std::vector<double> sp,smv;
    { int run=0;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){run=0;continue;} ++run;
        if(run==(int)std::lround(0.30/hopS))
          residuals((long)h,(long)h+win,sp,smv,nullptr,nullptr); } }
    std::printf("SUSTAIN GATE: predicted med %.2fc p75 %.2fc | measured med %.2fc p75 %.2fc (n=%zu)\n",
        q(sp,0.5),q(sp,0.75),q(smv,0.5),q(smv,0.75),sp.size());

    // ---- 2: onsets ----------------------------------------------------
    std::vector<double> op,om;
    std::vector<std::vector<double>> trP(30),trM(30);
    for(long o:onsets) residuals(o,o+win,op,om,&trP,&trM);
    std::printf("ONSETS (%zu): predicted med %.2fc p75 %.2fc p90 %.2fc | measured med %.2fc p75 %.2fc p90 %.2fc (n=%zu)\n",
        onsets.size(),q(op,0.5),q(op,0.75),q(op,0.9),q(om,0.5),q(om,0.75),q(om,0.9),op.size());
    auto traj=[&](const char* nm,std::vector<std::vector<double>>& tr)
    { std::printf("  %s traj:",nm);
      for(auto& b:tr){ if(b.empty()){std::printf("   .");continue;}
        std::sort(b.begin(),b.end()); std::printf(" %3.0f",b[b.size()/2]); }
      std::printf("\n"); };
    traj("pred",trP); traj("meas",trM);
    return 0;
}
