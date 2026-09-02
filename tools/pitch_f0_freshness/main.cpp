// pitch_f0_freshness (2 Sep 2026, round-7 ruling): predict the onset
// off-grid floor from the ESTIMATOR'S error alone. Output = target *
// f0_true(t)/f0Here(t), so the subtractor's residual is computable in
// advance of any engine change:
//
//   f0_true : best-effort NON-CAUSAL reference - per-cycle zero-crossing
//             periods on a zero-phase band-passed segment (effective
//             window ~1 cycle, no group delay by construction). Its own
//             error bound is MEASURED on a synthetic known-f0 take and
//             printed first; the ruler is not trusted by assertion.
//   f0Here  : what the engine's lag-compensated ring held - reconstructed
//             by running the exact detection chain of the render harness
//             (PitchEngine 256-block + F0JumpGate per hop) and applying
//             the ring's write rule (EedPsolaEngine.h:541-545): the
//             gated hop value, attributed lag = min(pitchLag, latency-1)
//             samples BACK, held to the next hop. Bridging is not
//             modelled (it rewrites zero-hops only, which are excluded).
//
// PREDICTION UNDER TEST (stated in the ruling before any number was
// seen): onset estimator error 3-5c (matching the measured onset
// off-grid floor), sustain error ~1-2c. If onsets come back ~1c the
// staleness mechanism is REFUTED and the pass stops again.
//
//   pitch_f0_freshness <take.wav> <voiceType>
//   pitch_f0_freshness --selftest
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
// ---- the reference ruler: per-cycle f0 points from a zero-phase band ----
struct F0Point { double tSamp, f0; };
static std::vector<F0Point> refCycles (const std::vector<float>& x, double fs,
                                       size_t s, size_t e, double centerHz)
{
    // pad 50ms each side so the filter transient stays outside the window
    const size_t pad=(size_t)(0.05*fs);
    const size_t a=s>pad?s-pad:0, b=std::min(x.size(),e+pad);
    std::vector<double> y(x.begin()+(long)a,x.begin()+(long)b);
    const double w0=2.0*M_PI*centerHz/fs, Q=5.0, al=std::sin(w0)/(2.0*Q);
    const double b0=al, b2=-al, a0=1.0+al, a1=-2.0*std::cos(w0), a2v=1.0-al;
    auto pass=[&](bool rev)
    { double x1=0,x2=0,y1=0,y2=0;
      if(!rev) for(double& v:y){ const double xn=v, yn=(b0*xn+b2*x2-a1*y1-a2v*y2)/a0; x2=x1;x1=xn;y2=y1;y1=yn;v=yn; }
      else for(size_t i=y.size();i-->0;){ const double xn=y[i], yn=(b0*xn+b2*x2-a1*y1-a2v*y2)/a0; x2=x1;x1=xn;y2=y1;y1=yn;y[i]=yn; } };
    pass(false); pass(true);   // zero phase: forward then backward
    std::vector<double> zc;
    for(size_t i=1;i<y.size();++i)
        if(y[i-1]<=0.0&&y[i]>0.0)
            zc.push_back((double)a+(double)(i-1)+(-y[i-1])/(y[i]-y[i-1]));
    std::vector<F0Point> out;
    const double nom=fs/centerHz;
    for(size_t i=1;i<zc.size();++i)
    { const double T=zc[i]-zc[i-1];
      if(T>0.5*nom&&T<2.0*nom)                       // octave guard
        out.push_back({0.5*(zc[i]+zc[i-1]), fs/T});
    }
    // 3-cycle median: cycle-to-cycle period variation is the SINGER'S OWN
    // jitter (real signal - the period-hist cross-check measured source
    // IQR ~10-12c per period), not estimator error. The freshness question
    // lives in the 30-80ms band; a 3-cycle median (~10-15ms support) sits
    // above cycle jitter and below the band. Without it the raw per-cycle
    // reference read 6.3c at sustains where the OUTPUT measurably tunes to
    // 1.9c - a ruler reading its own granularity, caught on first run.
    std::vector<F0Point> sm=out;
    for(size_t i=1;i+1<out.size();++i)
    { double v[3]={out[i-1].f0,out[i].f0,out[i+1].f0};
      std::sort(v,v+3); sm[i].f0=v[1]; }
    // keep only points inside the actual window
    std::vector<F0Point> in;
    for(auto& p:sm) if(p.tSamp>=(double)s&&p.tSamp<(double)e) in.push_back(p);
    return in;
}
// ---- the engine-side reconstruction: gated hop f0, lag-attributed ------
static void ringF0 (const std::vector<float>& in, double fs, int vt,
                    std::vector<double>& f0Here /* per sample */)
{
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    const int lag=std::min(det.pitchLagFor(vt),std::max(0,sh.latencySamples()-1));
    f0Here.assign(in.size(),0.0);
    // The engine MUST process the audio slice-by-slice exactly as the
    // render harness does: the F0JumpGate's octave-guard checks read
    // inputPeriodicity from the engine's own input ring. Reconstructing
    // the gate WITHOUT feeding that ring produced a different gated
    // sequence than any real render (caught on run 3: sustains read
    // 5-6c off BOTH independent truth rulers, which no tuned output
    // could have survived - the reconstruction was the outlier, not
    // the ring).
    std::vector<float> scratch(in.size(),0.0f);
    PitchEngine::HopEvent ev[64];
    float sliceF0=0; bool sliceVoiced=false;
    long prevPos=0; double prevF0=0;
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
            { sh.process(in.data()+p+(size_t)cursor,scratch.data()+p+(size_t)cursor,
                         sliceEnd-cursor,sliceF0,sliceVoiced,
                         sliceF0>0?sliceF0:0.0f,0.0f); cursor=sliceEnd; }
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
                // the ring holds prevF0 over [prevPos-lag, thisPos-lag)
                const long a=std::max(0L,prevPos-(long)lag);
                const long b=std::max(0L,(long)ev[h].inputPos-(long)lag);
                for(long i=a;i<b&&i<(long)f0Here.size();++i) f0Here[(size_t)i]=prevF0;
                prevPos=(long)ev[h].inputPos; prevF0=(g>0&&ev[h].voiced)?(double)g:0.0;
            }
        }
    }
    for(long i=std::max(0L,prevPos-1);i<(long)f0Here.size();++i) f0Here[(size_t)i]=prevF0;
}
// ---- coarse track for window picking (same as the other probes) --------
static std::vector<double> coarse (const std::vector<float>& x, double fs, int vt, int& hop)
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
static long fhShift_=0;   // ruler-alignment offset applied to the ring lookup
static void report (const std::vector<float>& x, double fs, int vt)
{
    int hop=0; auto tr=coarse(x,fs,vt,hop);
    const double hopS=hop/fs;
    std::vector<double> fh; ringF0(x,fs,vt,fh);
    // onsets + sustain windows exactly as pitch_onset_converge
    std::vector<long> onsets;
    { int uv=1000;
      for(size_t h=0;h<tr.size();++h)
      { if(tr[h]<=0){++uv;continue;}
        if(uv*hopS>=0.06) onsets.push_back((long)h);
        uv=0; } }
    const long win=(long)std::lround(0.150/hopS);
    auto winMedianHz=[&](long h0,long h1)->double
    { std::vector<double> f;
      for(long h=h0;h<h1&&h<(long)tr.size();++h) if(tr[h]>0) f.push_back(tr[h]);
      if(f.size()<3) return 0;
      std::sort(f.begin(),f.end()); return f[f.size()/2]; };
    // APPLES-TO-APPLES (second correction, caught on run 2): the measured
    // 3-5c output floor was read by the FINE TRACKER, whose own window
    // smooths the output over tens of ms. The raw residual is therefore
    // not the comparable quantity - the predicted residual must pass
    // through equivalent smoothing before its statistics mean anything.
    // r(t) on the fine 2.67ms grid, then a 24ms (9-point) moving median.
    auto collect=[&](long h0,long h1,std::vector<double>& pool,
                     std::vector<std::vector<double>>* traj)
    { const double c0=winMedianHz(h0,h1); if(c0<=0) return;
      const size_t s=(size_t)(h0*hop), e=std::min(x.size(),(size_t)(h1*hop));
      const auto cyc=refCycles(x,fs,s,e,c0);
      if(cyc.size()<4) return;
      std::vector<double> rg;                       // signed r per fine hop
      for(long h=h0;h<h1;++h)
      { const double t=(double)h*hop+0.5*hop;
        // interpolate the reference between cycle points
        size_t j=0; while(j+1<cyc.size()&&cyc[j+1].tSamp<t) ++j;
        double fr=0;
        if(j+1<cyc.size()&&cyc[j].tSamp<=t&&cyc[j+1].tSamp>=t
           &&cyc[j+1].tSamp-cyc[j].tSamp<3.0*fs/c0)  // gap guard: <3 cycles
        { const double a=(t-cyc[j].tSamp)/(cyc[j+1].tSamp-cyc[j].tSamp);
          fr=cyc[j].f0+a*(cyc[j+1].f0-cyc[j].f0); }
        const long ti=(long)t+fhShift_;
        const double fH=(ti>=0&&(size_t)ti<fh.size())?fh[(size_t)ti]:0.0;
        if(fr<=0||fH<=0){ rg.push_back(1e9); continue; }   // gap marker
        const double r=1200.0*std::log2(fr/fH);
        rg.push_back(std::fabs(r)>200.0?1e9:r); }   // octave disagreement out
      for(size_t i=0;i<rg.size();++i)
      { if(rg[i]>1e8) continue;
        std::vector<double> w;
        for(long k=-4;k<=4;++k)
        { const long ii=(long)i+k;
          if(ii>=0&&ii<(long)rg.size()&&rg[(size_t)ii]<1e8) w.push_back(rg[(size_t)ii]); }
        if(w.size()<5) continue;
        std::sort(w.begin(),w.end());
        const double sm=std::fabs(w[w.size()/2]);
        pool.push_back(sm);
        if(traj)
        { const size_t bin=(size_t)((double)i*hopS/0.005);
          if(bin<traj->size()) (*traj)[bin].push_back(sm); } } };
    // TIME-ALIGN THE RULERS FIRST (third correction, caught on run 4): the
    // residual as computed mixes staleness with cross-ruler timing error -
    // the ring is lag-compensated, the ZC reference is zero-phase, and any
    // net misalignment Delta manufactures |slope|*Delta cents under
    // vibrato (~400c/s at this take's 12c/5.5Hz: 15ms -> ~6c). The output
    // measurement never sees this (no cross-ruler time transfer), which is
    // how sustains could read 6c here while the output tunes to 1.9c.
    // Scan Delta on SUSTAINS (where truth is quasi-periodic), take the
    // minimiser, report both windows at that alignment. Delta itself is
    // reported: it is the net error of the engine's lag compensation as
    // seen by this ruler pair.
    std::vector<std::pair<long,long>> susWins;
    { int run=0;
      for(size_t h=0;h<tr.size();++h)
      { if(tr[h]<=0){run=0;continue;} ++run;
        if(run==(int)std::lround(0.30/hopS)) susWins.push_back({(long)h,(long)h+win}); } }
    double bestD=0, bestMed=1e9;
    for(double dMs=-40.0;dMs<=40.0;dMs+=1.0)
    {
        fhShift_=(long)std::llround(dMs*0.001*fs);
        std::vector<double> pool;
        for(auto& w:susWins) collect(w.first,w.second,pool,nullptr);
        if(pool.size()<30) continue;
        std::sort(pool.begin(),pool.end());
        const double m=pool[pool.size()/2];
        if(m<bestMed){ bestMed=m; bestD=dMs; }
    }
    fhShift_=(long)std::llround(bestD*0.001*fs);
    std::printf("  ruler alignment: best Delta %+.0fms (sustain med %.2fc there; %.2fc at Delta 0)\n",
        bestD,bestMed,[&]{ fhShift_=0; std::vector<double> p;
            for(auto& w:susWins) collect(w.first,w.second,p,nullptr);
            fhShift_=(long)std::llround(bestD*0.001*fs);
            if(p.empty())return -1.0; std::sort(p.begin(),p.end()); return p[p.size()/2]; }());
    std::vector<double> onPool, susPool;
    std::vector<std::vector<double>> traj(30);
    for(long o:onsets) collect(o,o+win,onPool,&traj);
    for(auto& w:susWins) collect(w.first,w.second,susPool,nullptr);
    auto q=[](std::vector<double>& v,double f)->double
    { if(v.empty())return -1; std::sort(v.begin(),v.end());
      return v[(size_t)std::min((double)v.size()-1.0,f*(double)v.size())]; };
    std::printf("  onsets %zu: estimator error med %.2fc  p75 %.2fc  p90 %.2fc  (n=%zu cycles)\n",
        onsets.size(),q(onPool,0.5),q(onPool,0.75),q(onPool,0.9),onPool.size());
    std::printf("  sustains:   estimator error med %.2fc  p75 %.2fc  p90 %.2fc  (n=%zu cycles)\n",
        q(susPool,0.5),q(susPool,0.75),q(susPool,0.9),susPool.size());
    std::printf("  median |err| trajectory across the onset window, 5ms bins:\n   ");
    for(auto& b:traj)
    { if(b.empty()){ std::printf("   ."); continue; }
      std::sort(b.begin(),b.end()); std::printf(" %3.0f",b[b.size()/2]); }
    std::printf("\n");
}
int main (int argc, char** argv)
{
    if(argc>1&&!std::strcmp(argv[1],"--selftest"))
    {
        // RULER ERROR BOUND: sawtooth, abrupt onset at 0.2s, 200Hz with a
        // -25c scoop recovering over 40ms and 10c 6Hz vibrato - a known
        // f0(t), measured by the same reference pipeline.
        const double fs=48000; const size_t N=(size_t)(1.0*fs);
        std::vector<float> x(N,0.0f); std::vector<double> truth(N,0.0);
        double ph=0;
        for(size_t i=(size_t)(0.2*fs);i<N;++i)
        { const double t=(double)i/fs-0.2;
          const double cents=-25.0*std::exp(-t/0.040)+10.0*std::sin(2.0*M_PI*6.0*t);
          const double f=200.0*std::pow(2.0,cents/1200.0);
          truth[i]=f; ph+=f/fs; ph-=std::floor(ph);
          x[i]=(float)(0.4*(2.0*ph-1.0)); }
        std::vector<double> errs;
        for(const F0Point& p:refCycles(x,fs,(size_t)(0.2*fs),(size_t)(0.35*fs),200.0))
        { const double tr=truth[(size_t)p.tSamp];
          if(tr>0) errs.push_back(std::fabs(1200.0*std::log2(p.f0/tr))); }
        std::sort(errs.begin(),errs.end());
        std::printf("RULER SELF-TEST (known 200Hz + 25c/40ms scoop + 10c vibrato, first 150ms):\n");
        std::printf("  median %.2fc  p90 %.2fc  max %.2fc  (n=%zu cycles)\n",
            errs.empty()?-1:errs[errs.size()/2],
            errs.empty()?-1:errs[(size_t)(0.9*(double)errs.size())],
            errs.empty()?-1:errs.back(),errs.size());
        return 0;
    }
    if(argc<3){ std::printf("usage: %s <take.wav> <vt> | --selftest\n",argv[0]); return 1; }
    std::vector<float> x; double fs=0;
    if(!readWavMono(argv[1],x,fs)){ std::printf("bad input\n"); return 1; }
    const int vt=atoi(argv[2]);
    std::printf("%s  voice %s\n",argv[1],PitchEngine::voiceRange(vt).id);
    report(x,fs,vt);
    return 0;
}
