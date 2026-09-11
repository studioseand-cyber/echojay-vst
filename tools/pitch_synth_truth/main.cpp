// pitch_synth_truth (2 Sep 2026, round-10 ruling): the truth-bearing
// synthetic take, and the CALIBRATION ARM that decides account B (a
// tracker-calibration bias between raw and resampled/spliced audio)
// OFFLINE, with no engine in the path.
//
// REQUIREMENT 2 - realism derived from named failure modes
// (ONSET_SHAKINESS_RESEARCH.md sections 2-3), not convenience:
//   INCLUDED: amplitude attack (35ms rise from silence - YIN's named
//     rising-amplitude subharmonic bias, highest priority); an aperiodic
//     pre-voicing segment (80ms HP-filtered noise burst); an f0 scoop
//     into each note (-40c, tau 30ms -> initial slope ~1300c/s, decaying
//     through the real take's measured ~750c/s onset slope range, from
//     the pitch_onset_converge source trajectory 28c->10c over 24ms);
//     formant motion (two swept resonators, F1 600->780Hz, F2
//     1350->1080Hz over each note); aspiration noise (-24dB, breathy).
//   NOT INCLUDED (stated): creak/period-doubling (the OLD take's
//     dominant mode - no validated synthesis of it; the low register
//     variant carries slow jitter only); consonant formant transitions
//     beyond the resonator sweep; room/reverb.
//
// Two registers, honoring both-takes-in-spirit: alto (D-minor tones
// around 220-330Hz, vt=1) and low-male (110-165Hz, vt=2, mild 0.3%
// pitch jitter). Notes are detuned +/-10..25c so a corrector has work.
//
// REQUIREMENT 1 - the calibration arm (this file, mode "calib"):
//   T on raw vs truth (b_raw), on offline-RESAMPLED versions at known
//   constant ratios (+/-10, +/-25, +/-50c; cubic hermite, the engine's
//   own interpolator family), and on offline-SPLICED versions (the
//   engine's exact splice convention: drift += r-1 per sample, jump one
//   fractional period at |drift| > 0.75T, 4ms raised-cosine crossfade).
//   Deliverable: b_res - b_raw and b_spl - b_raw, per ratio, sustains,
//   per-instant signed. ~3c -> account B confirmed, STOP before any
//   render. ~0 -> account B excluded.
//
//   pitch_synth_truth calib
//   pitch_synth_truth write <out.wav> <alto|low>   (render arm's input)
//   pitch_synth_truth render
//
// RENDER ARM (account B excluded first): full chain at tau6, taps on.
//   REQUIREMENT 3 GATE, read before anything else: leg 3 (T(out) vs
//   tapped target) must reproduce the real take's 4.86c onset / 1.74c
//   sustain magnitudes; ~1c means the synthetic is too easy and nothing
//   transfers.
//   PRIMARY DELIVERABLE: tapped f0Here vs KNOWN truth - the estimator's
//   onset freshness error with no tracker in the path. Round-6 standing
//   prediction (onset 3-5c, sustain 1-2c) finally marked against ground
//   truth.
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
static constexpr double kFs = 48000.0;
struct Synth { std::vector<float> x; std::vector<double> truthHz; };
static uint32_t rng_ = 0x2545F491u;
static double frand() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
                        return (double) (rng_ & 0xFFFFFF) / 8388608.0 - 1.0; }
static Synth makeTake (bool low)
{
    rng_ = low ? 0x9E3779B9u : 0x2545F491u;
    // D natural minor tones, detuned so the corrector has work
    static const double altoMidi[12] = { 62,65,69,67,64,62,70,69,65,67,62,69 };
    static const double detune[12]   = { +15,-20,+25,-10,+18,-25,+12,-15,+22,-12,+20,-18 };
    Synth S;
    const double noteLen=0.55, gapLen=0.14, burstLen=0.08;
    const size_t total=(size_t)(12.0*(noteLen+gapLen)*kFs)+4800;
    S.x.assign(total,0.0f); S.truthHz.assign(total,0.0);
    double t0=0.1;
    for(int n=0;n<12;++n)
    {
        const double midi=altoMidi[n]-(low?12.0:0.0);
        const double base=440.0*std::pow(2.0,(midi-69.0+detune[n]/100.0)/12.0);
        // pre-voicing aperiodic burst (plosive/aspiration)
        { const size_t bs=(size_t)((t0-burstLen)*kFs), be=(size_t)(t0*kFs);
          double hp=0;
          for(size_t i=bs;i<be&&i<total;++i)
          { const double w=frand(); const double y=w-hp; hp+=0.02*(w-hp);
            const double env=std::sin(M_PI*(double)(i-bs)/(double)(be-bs));
            S.x[i]+=(float)(0.12*y*env); } }
        // the voiced note
        const size_t vs=(size_t)(t0*kFs), ve=(size_t)((t0+noteLen)*kFs);
        double ph=0, f1=600, f2=1350;
        double b1s[2]={0,0}, b2s[2]={0,0};
        double jit=0, wob=0, wobLp=0;
        // HARDENING (requirement-3 gate, round 10): the first cut's truth
        // trajectory was deterministic-smooth and the gate read onset leg3
        // at 2.22c vs the real take's 4.86c. The real take's onsets carry
        // STOCHASTIC instability (measured 5.22c/hop source onset jitter)
        // - so the synthetic gets an onset-concentrated random wobble
        // (~8-25Hz random walk band, ~7c initial scale, decaying tau
        // 80ms) plus per-note scoop variation and a slower 35ms attack.
        const double scoopMag=35.0+(double)((n*7)%25);
        const double scoopTau=0.025+0.001*(double)((n*5)%20);
        for(size_t i=vs;i<ve&&i<total;++i)
        {
            const double tt=((double)i/kFs)-t0;
            // f0: scoop + delayed vibrato + onset wobble + register jitter
            const double scoop=-scoopMag*std::exp(-tt/scoopTau);
            const double vibF=10.0*std::sin(2.0*M_PI*5.5*tt)
                              *std::clamp(tt/0.15,0.0,1.0);
            wob+=0.9*frand(); wob*=0.9993;                    // random walk
            wobLp+=0.0016*(wob-wobLp);                        // ~12Hz band
            const double onsetWob=wobLp*7.0*std::exp(-tt/0.080);
            if(low){ jit+=0.002*frand(); jit*=0.999; }
            const double cents=scoop+vibF+onsetWob+(low?300.0*jit:0.0);
            const double f=base*std::pow(2.0,cents/1200.0);
            S.truthHz[i]=f;
            ph+=f/kFs; ph-=std::floor(ph);
            // glottal-ish source: soft-clipped saw (harmonically rich)
            double src=2.0*ph-1.0; src=std::tanh(1.8*src);
            // amplitude attack: 20ms rise from silence + slight decay
            const double amp=(1.0-std::exp(-tt/0.035))*(1.0-0.25*tt/noteLen);
            src*=0.35*amp;
            // aspiration noise, -24dB relative, breathier in the attack
            src+=0.022*frand()*amp*(1.0+2.0*std::exp(-tt/0.040));
            // formant motion: two swept resonators, recomputed per sample
            f1=600.0+180.0*(tt/noteLen); f2=1350.0-270.0*(tt/noteLen);
            auto reso=[&](double* st,double fc,double q,double in)->double
            { const double w0=2.0*M_PI*fc/kFs, al=std::sin(w0)/(2.0*q);
              const double a0=1.0+al;
              const double y=(al*in-(-2.0*std::cos(w0))*st[0]-(1.0-al)*st[1])/a0;
              st[1]=st[0]; st[0]=y; return y; };
            const double body=0.6*reso(b1s,f1,8.0,src)+0.4*reso(b2s,f2,10.0,src);
            S.x[i]+=(float)std::clamp(0.5*src+0.9*body,-0.99,0.99);
        }
        t0+=noteLen+gapLen;
    }
    return S;
}
// engine-family cubic hermite (readInterp's 4-point form)
static float cubicAt (const std::vector<float>& x, double pos)
{
    const long i0=(long)pos; const float fr=(float)(pos-(double)i0);
    if(i0<1||i0+2>=(long)x.size()) return 0.0f;
    const float xm=x[(size_t)i0-1],x0=x[(size_t)i0],x1=x[(size_t)i0+1],x2=x[(size_t)i0+2];
    return x0+0.5f*fr*(x1-xm+fr*(2.0f*xm-5.0f*x0+4.0f*x1-x2+fr*(3.0f*(x0-x1)+x2-xm)));
}
static std::vector<float> resample (const std::vector<float>& x, double r)
{
    std::vector<float> y((size_t)((double)x.size()/r)-4,0.0f);
    for(size_t i=0;i<y.size();++i) y[i]=cubicAt(x,(double)i*r);
    return y;
}
// the engine's splice convention, offline: constant ratio, drift/jump/fade
static std::vector<float> spliceShift (const std::vector<float>& x,
                                       const std::vector<double>& truth, double r)
{
    std::vector<float> y(x.size(),0.0f);
    double drift=0, oldDrift=0; int fadeLen=0, fadePos=0;
    for(size_t p=0;p<x.size();++p)
    {
        drift+=r-1.0;
        const double f=truth[p]>0?truth[p]:150.0;
        const double Tf=kFs/f; const int T=(int)std::lround(Tf);
        if(fadeLen==0&&std::fabs(drift)>0.75*(double)T)
        { oldDrift=drift; drift+=drift>0?-Tf:Tf;
          fadeLen=std::max(16,std::min(T/2,(int)(0.004*kFs))); fadePos=0; }
        float v=cubicAt(x,(double)p+drift);
        if(fadeLen>0)
        { oldDrift+=r-1.0;
          const float w=0.5f*(1.0f-std::cos(M_PI*(float)fadePos/(float)fadeLen));
          v=w*v+(1.0f-w)*cubicAt(x,(double)p+oldDrift);
          if(++fadePos>=fadeLen) fadeLen=0; }
        y[p]=v;
    }
    return y;
}
static std::vector<double> fineTrack (const std::vector<float>& x, int vt, int& hop)
{
    PitchEngine e; e.prepare(kFs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal);
    hop=e.inputHopLength(vt);
    std::vector<double> t;
    for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop)
    { e.process(x.data()+p,nullptr,hop);
      const PitchReading r=e.getReading();
      t.push_back(r.voiced&&r.f0Hz>0?(double)r.f0Hz:0.0); }
    return t;
}
// signed per-instant tracker bias vs truth over sustains (>250ms into notes)
static void bias (const std::vector<float>& x, const std::vector<double>& truthOf,
                  double timeScale /* input-time of output sample i = i*timeScale */,
                  double pitchScale, int vt, const std::vector<double>& truthRaw,
                  double& med, double& p75a, size_t& n)
{
    int hop=0; auto T=fineTrack(x,vt,hop);
    (void)truthOf;
    std::vector<double> d;
    for(size_t h=0;h<T.size();++h)
    { if(T[h]<=0) continue;
      const double t=((double)h*hop+0.5*hop)*timeScale;
      if((size_t)t>=truthRaw.size()) break;
      const double tr=truthRaw[(size_t)t]*pitchScale;
      if(tr<=0) continue;
      // sustain: truth voiced for the previous 250ms of INPUT time
      const size_t back=(size_t)(0.25*kFs);
      if((size_t)t<back||truthRaw[(size_t)t-back]<=0) continue;
      const double c=1200.0*std::log2(T[h]/tr);
      if(std::fabs(c)>200) continue;
      d.push_back(c); }
    n=d.size(); med=0; p75a=0;
    if(d.empty()) return;
    std::sort(d.begin(),d.end());
    med=d[d.size()/2];
    std::vector<double> a; for(double v:d) a.push_back(std::fabs(v));
    std::sort(a.begin(),a.end());
    p75a=a[(size_t)(0.75*(double)a.size())];
}

static std::vector<float> gOut;
static std::vector<PsolaEngine::DbgRingTap> gTap;
static void renderChain (const std::vector<float>& in, int vt, float tau)
{
    const double fs=kFs;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s2=0;s2<12;++s2) corr.setDegree(s2,false,0);
    for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0);
    corr.setKeyRoot(2); corr.setRetuneMs(tau); corr.setFlex(0); corr.setHumanize(0);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(0); corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    sh.debugRingTap(true);
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
    gOut.assign(in.size(),0.0f);
    for(size_t i=(size_t)lat;i<raw.size();++i) gOut[i-(size_t)lat]=raw[i];
    gTap=sh.debugRingTapData();
}

static bool writeWav (const char* path, const std::vector<float>& x)
{
    FILE* f=std::fopen(path,"wb"); if(!f) return false;
    auto w32=[&](uint32_t v){ std::fwrite(&v,4,1,f); };
    auto w16=[&](uint16_t v){ std::fwrite(&v,2,1,f); };
    const uint32_t data=(uint32_t)x.size()*4;
    std::fwrite("RIFF",1,4,f); w32(36+data); std::fwrite("WAVE",1,4,f);
    std::fwrite("fmt ",1,4,f); w32(16); w16(3); w16(1); w32(48000); w32(48000*4); w16(4); w16(32);
    std::fwrite("data",1,4,f); w32(data);
    std::fwrite(x.data(),4,x.size(),f);
    std::fclose(f); return true;
}
int main (int argc, char** argv)
{

    if(argc>1&&!std::strcmp(argv[1],"render"))
    {
        for(int reg=0;reg<2;++reg)
        {
            const bool low=reg==1; const int vt=low?2:1;
            Synth S=makeTake(low);
            renderChain(S.x,vt,6.0f);
            std::printf("== %s register, full chain tau6, taps on (%zu tap entries)\n",
                low?"low_male":"alto",gTap.size());
            auto tapAt=[&](double t,double& f0,double& tg)
            { f0=0;tg=0; size_t lo=0,hi=gTap.size();
              while(lo<hi){ size_t m=(lo+hi)/2; if((double)gTap[m].inPos<=t)lo=m+1; else hi=m; }
              if(lo==0) return;
              if(t-(double)gTap[lo-1].inPos>2048.0) return;   // stale across gaps
              f0=gTap[lo-1].f0Here; tg=gTap[lo-1].target; };
            // truth-defined onsets: voiced start after >=60ms unvoiced
            std::vector<size_t> onsets;
            { size_t uv=100000;
              for(size_t i=0;i<S.truthHz.size();++i)
              { if(S.truthHz[i]<=0){++uv;continue;}
                if((double)uv/kFs>=0.06) onsets.push_back(i);
                uv=0; } }
            // REQUIREMENT 3 GATE: leg 3 through the tracker
            int hop=0; auto tOut=fineTrack(gOut,vt,hop);
            const double hopS=hop/kFs;
            std::vector<double> g3on,g3sus;
            for(size_t o:onsets)
              for(long h=(long)((double)o/hop);h<(long)((double)o/hop)+(long)(0.150/hopS);++h)
              { if(h<0||h>=(long)tOut.size()||tOut[(size_t)h]<=0) continue;
                double f0,tg; tapAt((double)h*hop+0.5*hop,f0,tg);
                if(tg<=0) continue;
                const double c=1200.0*std::log2(tOut[(size_t)h]/tg);
                if(std::fabs(c)<200) g3on.push_back(std::fabs(c)); }
            for(long h=0;h<(long)tOut.size();++h)
            { const double t=(double)h*hop+0.5*hop;
              const size_t back=(size_t)(0.30*kFs);
              if((size_t)t<back||(size_t)t>=S.truthHz.size()) continue;
              if(S.truthHz[(size_t)t]<=0||S.truthHz[(size_t)t-back]<=0) continue;
              if(tOut[(size_t)h]<=0) continue;
              double f0,tg; tapAt(t,f0,tg);
              if(tg<=0) continue;
              const double c=1200.0*std::log2(tOut[(size_t)h]/tg);
              if(std::fabs(c)<200) g3sus.push_back(std::fabs(c)); }
            auto Q=[](std::vector<double> v,double f)->double
            { if(v.empty())return -1; std::sort(v.begin(),v.end());
              return v[(size_t)std::min((double)v.size()-1.0,f*(double)v.size())]; };
            std::printf("  GATE leg3: onset med %.2fc (real take 4.86) | sustain med %.2fc (real 1.74)\n",
                Q(g3on,0.5),Q(g3sus,0.5));
            // PRIMARY: tapped f0Here vs KNOWN truth - no tracker anywhere
            std::vector<double> fOn,fSus;
            std::vector<std::vector<double>> traj(30);
            const double grid=hop;   // same 2.67ms reporting grid
            for(size_t o:onsets)
              for(int k=0;k<(int)(0.150*kFs/grid);++k)
              { const double t=(double)o+k*grid;
                if((size_t)t>=S.truthHz.size()||S.truthHz[(size_t)t]<=0) continue;
                double f0,tg; tapAt(t,f0,tg);
                if(f0<=0) continue;
                const double c=std::fabs(1200.0*std::log2(S.truthHz[(size_t)t]/f0));
                if(c>200) continue;
                fOn.push_back(c);
                const size_t bin=(size_t)(k*grid/kFs/0.005);
                if(bin<30) traj[bin].push_back(c); }
            for(size_t i=0;i<S.truthHz.size();i+=(size_t)grid)
            { const size_t back=(size_t)(0.30*kFs);
              if(i<back||S.truthHz[i]<=0||S.truthHz[i-back]<=0) continue;
              double f0,tg; tapAt((double)i,f0,tg);
              if(f0<=0) continue;
              const double c=std::fabs(1200.0*std::log2(S.truthHz[i]/f0));
              if(c<200) fSus.push_back(c); }
            std::printf("  PRIMARY f0Here vs TRUTH: onset med %.2fc p75 %.2fc p90 %.2fc (n=%zu) | sustain med %.2fc (n=%zu)\n",
                Q(fOn,0.5),Q(fOn,0.75),Q(fOn,0.9),fOn.size(),Q(fSus,0.5),fSus.size());
            std::printf("  freshness traj (5ms bins):");
            for(auto& b:traj){ if(b.empty()){std::printf("   .");continue;}
              std::sort(b.begin(),b.end()); std::printf(" %3.0f",b[b.size()/2]); }
            std::printf("\n");
        }
        return 0;
    }
    if(argc>2&&!std::strcmp(argv[1],"write"))
    {
        const bool low=argc>3&&!std::strcmp(argv[3],"low");
        Synth S=makeTake(low);
        if(!writeWav(argv[2],S.x)){ std::printf("write failed\n"); return 1; }
        // truth sidecar: raw doubles
        std::string tp=std::string(argv[2])+".truth";
        FILE* f=std::fopen(tp.c_str(),"wb");
        std::fwrite(S.truthHz.data(),sizeof(double),S.truthHz.size(),f);
        std::fclose(f);
        std::printf("wrote %s (+.truth), %zu samples, %s register\n",
            argv[2],S.x.size(),low?"low_male":"alto");
        return 0;
    }
    // ---- CALIBRATION ARM ----------------------------------------------
    static const double centsList[6]={-50,-25,-10,+10,+25,+50};
    for(int reg=0;reg<2;++reg)
    {
        const bool low=reg==1; const int vt=low?2:1;
        Synth S=makeTake(low);
        double bRaw,pRaw; size_t nRaw;
        bias(S.x,S.truthHz,1.0,1.0,vt,S.truthHz,bRaw,pRaw,nRaw);
        std::printf("== %s register (vt %d): b_raw signed med %+0.2fc (p75|.| %.2f, n=%zu)\n",
            low?"low_male":"alto",vt,bRaw,pRaw,nRaw);
        std::printf("   %-8s  %-28s  %-28s\n","ratio","RESAMPLED b-b_raw","SPLICED b-b_raw");
        for(double c:centsList)
        {
            const double r=std::pow(2.0,c/1200.0);
            auto res=resample(S.x,r);
            double bR,pR; size_t nR;
            bias(res,S.truthHz,r,r,vt,S.truthHz,bR,pR,nR);
            auto spl=spliceShift(S.x,S.truthHz,r);
            double bS,pS; size_t nS;
            bias(spl,S.truthHz,1.0,r,vt,S.truthHz,bS,pS,nS);
            std::printf("   %+5.0fc    med %+6.2fc (p75|.| %5.2f n=%4zu)  med %+6.2fc (p75|.| %5.2f n=%4zu)\n",
                c,bR-bRaw,pR,nR,bS-bRaw,pS,nS);
        }
    }
    return 0;
}
