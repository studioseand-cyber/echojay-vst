// pitch_onset_probe (2 Sep 2026, the onset-shakiness pass): windows the
// first 150ms after each word/note onset and reports, per tau and for the
// UNITY-RATIO discriminator, the output's onset wobble - the number that
// halves the hypothesis tree. Voice type alto_tenor BY DEFAULT: every
// prior probe silently pinned low_male while Sean runs alto_tenor.
//
// M1: full chain at Sean's settings, tau 6 vs 150 (the dial's capped
//     range; 400 is no longer reachable - PITCH_P0_VALIDATION.md 17.7).
// M2: engine driven with shift = 0.0 in shift mode - ratio EXACTLY 1.0,
//     corrector out of the loop, everything else (seams, grains, formant
//     preserve, gate) unchanged.
// M3: grain path (splice disabled) at unity - the epoch census, onset-
//     windowed, for the DEFECT_GRAIN_EPOCH_UNITY re-open.
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
struct Drive {
    std::vector<float> out;
    std::vector<int>   uvHop;    // per hop: !voiced (input side), with inputPos
    std::vector<long>  hopPos;
    int latency=0;
};
// mode 0: full chain at tau; mode 1: unity shift (corrector bypassed);
// mode 2: unity + grain path (splice disabled)
static Drive drive (const std::vector<float>& in, double fs, int vt, float tau, int mode)
{
    Drive d;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,false,0);
    for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0);
    corr.setKeyRoot(2); corr.setRetuneMs(tau);
    corr.setFlex(0); corr.setHumanize(0);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(0);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    if(mode==2) sh.debugDisableSplice(true);
    d.latency=sh.latencySamples();
    d.out.assign(in.size(),0.0f);
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
            { sh.process(in.data()+p+(size_t)cursor,d.out.data()+p+(size_t)cursor,
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
                d.uvHop.push_back(ev[h].voiced?0:1);
                d.hopPos.push_back((long)ev[h].inputPos);
                if(mode==0)
                { const float t=corr.process(g,ev[h].voiced,hopMs);
                  if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; } }
                else
                { if(g>0){ target=g; shift=0.0f; } }   // UNITY: ratio exactly 1
            }
        }
    }
    // latency-align
    std::vector<float> al(in.size(),0.0f);
    for(size_t i=(size_t)d.latency;i<d.out.size();++i) al[i-(size_t)d.latency]=d.out[i];
    d.out=al;
    return d;
}
// fine per-hop cents track (hop 128 - roughly per-cycle at low-male f0)
static std::vector<double> fineTrack (const std::vector<float>& x, double fs, int vt, int& hop)
{
    PitchEngine e; e.prepare(fs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal);
    hop=e.inputHopLength(vt);
    std::vector<double> t;
    for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop)
    { e.process(x.data()+p,nullptr,hop);
      const PitchReading r=e.getReading();
      t.push_back(r.voiced&&r.f0Hz>0?1200.0*std::log2(r.f0Hz/440.0):-1e9); }
    return t;
}
int main (int argc, char** argv)
{
    const char* path = argc>1?argv[1]:"/Users/SeanD/Music/Logic/test/Bounces/sourceNEW.wav";
    const int vt = argc>2?atoi(argv[2]):1;   // alto_tenor default, deliberately
    std::vector<float> in; double fs=0;
    if(!readWavMono(path,in,fs)){ std::printf("bad input\n"); return 1; }
    int hop=0;
    auto srcT=fineTrack(in,fs,vt,hop);
    const double hopS=hop/fs;
    // onsets: voiced start after >=60ms unvoiced in the SOURCE track
    std::vector<long> onsets;
    { int uv=1000;
      for(size_t h=0;h<srcT.size();++h)
      { if(srcT[h]<-1e8){++uv;continue;}
        if(uv*hopS>=0.06) onsets.push_back((long)h);
        uv=0; } }
    std::printf("material %s  voice %d (%s)  onsets %d  [file ONSET_SHAKINESS_RESEARCH.md absent from repo - spec taken from the ruling message]\n",
        path, vt, PitchEngine::voiceRange(vt).id, (int)onsets.size());
    // THE FLOOR ROW: the source measured by the same ruler - at unity the
    // in-band output is near-bit-exact input, so all onset elevation must
    // be read against the take's own onset instability + tracker noise.
    {
        auto winStats=[&](const std::vector<double>& T,long h0,long h1,double& wob,double& jit)
        {
            std::vector<double> seg;
            for(long h=h0;h<h1&&h<(long)T.size();++h) if(T[(size_t)h]>-1e8) seg.push_back(T[(size_t)h]);
            wob=0; jit=0;
            if(seg.size()>=5)
            { double acc=0;int n=0;
              for(size_t i=2;i+2<seg.size();++i)
              { double v[5]={seg[i-2],seg[i-1],seg[i],seg[i+1],seg[i+2]};
                std::sort(v,v+5); acc+=std::fabs(seg[i]-v[2]); ++n; }
              wob=n?acc/n:0;
              double ja=0;int jn=0;
              for(size_t i=1;i<seg.size();++i){ ja+=std::fabs(seg[i]-seg[i-1]); ++jn; }
              jit=jn?ja/jn:0; }
        };
        std::vector<double> onW,onJ,susW,susJ;
        const long win=(long)std::lround(0.150/hopS);
        for(long o:onsets){ double w,j; winStats(srcT,o,o+win,w,j); if(w>0){onW.push_back(w);onJ.push_back(j);} }
        { int run=0;
          for(size_t h=0;h<srcT.size();++h)
          { if(srcT[h]<-1e8){run=0;continue;} ++run;
            if(run==(int)std::lround(0.30/hopS))
            { double w,j; winStats(srcT,(long)h,(long)h+win,w,j); if(w>0){susW.push_back(w);susJ.push_back(j);} } } }
        auto med=[](std::vector<double>&v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
        std::printf("SOURCE     onset wobble med %5.2fc  jitter %5.2fc/hop  | sustain wobble %5.2fc jitter %5.2fc/hop\n",
            med(onW), med(onJ), med(susW), med(susJ));
    }
    // Optional argv[3]: a RENDERED file (e.g. antaresNEW) measured by the
    // same ruler with the SOURCE's onsets - sample-aligned matched pair,
    // within the calibration rule (a plugin-rendered bounce).
    if (argc>3)
    {
        std::vector<float> rx; double rfs=0;
        if (readWavMono(argv[3],rx,rfs))
        {
            int rh=0; auto rT=fineTrack(rx,rfs,vt,rh);
            auto winStats=[&](const std::vector<double>& T,long h0,long h1,double& wob,double& jit)
            {
                std::vector<double> seg;
                for(long h=h0;h<h1&&h<(long)T.size();++h) if(T[(size_t)h]>-1e8) seg.push_back(T[(size_t)h]);
                wob=0; jit=0;
                if(seg.size()>=5)
                { double acc=0;int n=0;
                  for(size_t i=2;i+2<seg.size();++i)
                  { double v[5]={seg[i-2],seg[i-1],seg[i],seg[i+1],seg[i+2]};
                    std::sort(v,v+5); acc+=std::fabs(seg[i]-v[2]); ++n; }
                  wob=n?acc/n:0;
                  double ja=0;int jn=0;
                  for(size_t i=1;i<seg.size();++i){ ja+=std::fabs(seg[i]-seg[i-1]); ++jn; }
                  jit=jn?ja/jn:0; }
            };
            std::vector<double> onW,onJ,susW,susJ;
            const long win=(long)std::lround(0.150/hopS);
            for(long o:onsets){ double w,j; winStats(rT,o,o+win,w,j); if(w>0){onW.push_back(w);onJ.push_back(j);} }
            { int run=0;
              for(size_t h=0;h<srcT.size();++h)
              { if(srcT[h]<-1e8){run=0;continue;} ++run;
                if(run==(int)std::lround(0.30/hopS))
                { double w,j; winStats(rT,(long)h,(long)h+win,w,j); if(w>0){susW.push_back(w);susJ.push_back(j);} } } }
            auto med=[](std::vector<double>&v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
            const double oj=med(onJ), sj=med(susJ);
            std::printf("RENDER %s: onset wobble %5.2fc jitter %5.2fc/hop | sustain wobble %5.2fc jitter %5.2fc/hop | CONTRAST %.2fx\n",
                argv[3], med(onW), oj, med(susW), sj, sj>0?oj/sj:0);
        }
    }
    struct Row { const char* name; float tau; int mode; };
    const Row rows[] = {
        { "M1 tau6   ", 6.0f, 0 },
        { "M1 tau150 ", 150.0f, 0 },   // 400 unreachable: capped (17.7)
        { "M2 UNITY  ", 6.0f, 1 },
        { "M3 grain1 ", 6.0f, 2 },
    };
    for (const auto& r : rows)
    {
        auto d = drive(in,fs,vt,r.tau,r.mode);
        int oh=0; auto outT=fineTrack(d.out,fs,vt,oh);
        // per-window metrics
        auto winStats=[&](long h0,long h1,double& wob,double& jit,int& uvc)
        {
            std::vector<double> seg;
            for(long h=h0;h<h1&&h<(long)outT.size();++h) if(outT[(size_t)h]>-1e8) seg.push_back(outT[(size_t)h]);
            wob=0; jit=0; uvc=0;
            if(seg.size()>=5)
            {
                // detrended wobble: |x - median5(x)|
                double acc=0; int n=0;
                for(size_t i=2;i+2<seg.size();++i)
                { double v[5]={seg[i-2],seg[i-1],seg[i],seg[i+1],seg[i+2]};
                  std::sort(v,v+5); acc+=std::fabs(seg[i]-v[2]); ++n; }
                wob=n?acc/n:0;
                double ja=0; int jn=0;
                for(size_t i=1;i<seg.size();++i){ ja+=std::fabs(seg[i]-seg[i-1]); ++jn; }
                jit=jn?ja/jn:0;   // cents/hop step = period perturbation proxy
            }
            // low-confidence/unvoiced input hops in the window (source frame)
            for(size_t k=0;k<d.hopPos.size();++k)
            { const long hh=(long)(d.hopPos[k]/hop);
              if(hh>=h0&&hh<h1&&d.uvHop[k]) ++uvc; }
        };
        std::vector<double> onW,onJ,susW,susJ; int onUv=0;
        const long win=(long)std::lround(0.150/hopS);
        for(long o:onsets)
        { double w,j;int u; winStats(o,o+win,w,j,u);
          if(w>0){onW.push_back(w);onJ.push_back(j);} onUv+=u; }
        // sustain windows: 300ms+ into voiced runs
        { int run=0;
          for(size_t h=0;h<srcT.size();++h)
          { if(srcT[h]<-1e8){run=0;continue;}
            ++run;
            if(run==(int)std::lround(0.30/hopS))
            { double w,j;int u; winStats((long)h,(long)h+win,w,j,u);
              if(w>0){susW.push_back(w);susJ.push_back(j);} } } }
        auto med=[](std::vector<double>&v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
        std::printf("%s onset wobble med %5.2fc  jitter %5.2fc/hop  | sustain wobble %5.2fc jitter %5.2fc/hop | uv-in-onset-hops %d\n",
            r.name, med(onW), med(onJ), med(susW), med(susJ), onUv);
    }
    return 0;
}
