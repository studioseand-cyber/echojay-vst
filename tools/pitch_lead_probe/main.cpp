// pitch_lead_probe (3 Sep 2026, ruling 2 of round 17): WHY does the f0 the
// shifter attaches to the audio lead the audio inside glides?
//
// Synthetic voiced signal with ANALYTIC f0(t): 20 harmonics, 1/h amplitude,
// phase-continuous, no gaps. For each glide rate R (cents/ms) the signal
// holds f_lo 400ms, glides +200c linearly in cents over 200/R ms, holds
// 400ms, glides back down, holds 400ms. Three observers, each compared with
// truth at the position it CLAIMS to describe:
//   HOP     the detector's HopEvent (f0Hz at inputPos)     - raw, uncompensated
//   RULER   PitchEngine at 8192-block, reading at hop h    - what pitch_activity
//           used as "src"; same estimator, so it carries the same lag
//   SHIFTER f0Here at the read pointer p (ring tap)        - pitchLag-compensated
// For each glide the lead is fitted: the time shift s (samples) minimising
// mean |cents(obs(p)) - cents(truth(p+s))| over the glide's central 60%.
// s>0 = the observer describes audio LATER than where it is attached = LEAD.
// Also reported: mean cents error mid-glide (what a corrector would act on).
// Constant ms across rates = an alignment offset ((a) or (c)); the design
// numbers (pitchLagFor, latency, the clamp at latency-1) are printed beside.
//
//   pitch_lead_probe <vt>
//   Build: g++ -std=c++17 -O2 -ISource tools/pitch_lead_probe/main.cpp -o lead
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <functional>
using namespace echojay;
static const double kFs=48000.0;
struct Seg { double t0,t1; double c0,c1; double vibDepth=0, vibHz=0; };   // cents relative to f_lo, linear in cents
struct Truth
{
    std::vector<Seg> segs; double fLo;
    double centsAt(double t) const
    { for(const Seg& s:segs) if(t>=s.t0&&t<s.t1) return s.c0+(s.c1-s.c0)*(t-s.t0)/(s.t1-s.t0)+(s.vibDepth>0?s.vibDepth*std::sin(2*M_PI*s.vibHz*(t-s.t0)):0.0);
      return segs.empty()?0:(t<segs.front().t0?segs.front().c0:segs.back().c1); }
    double hzAt(double t) const { return fLo*std::pow(2.0,centsAt(t)/1200.0); }
};
static double cents(double a,double b){ return 1200.0*std::log2(a/b); }
int main(int argc,char**argv)
{
    const int vt=argc>1?atoi(argv[1]):1;
    const double fLo=argc>2?atof(argv[2]):(vt==2?110.0:165.0);
    const double rates[]={0.25,0.5,1.0,2.0,4.0};
    Truth T; T.fLo=fLo; double t=0.3;
    struct Glide { double t0,t1,rate; bool up; };
    std::vector<Glide> glides;
    for(double R:rates)
    { const double dur=200.0/R*0.001;
      T.segs.push_back({t,t+0.4,0,0}); t+=0.4;
      T.segs.push_back({t,t+dur,0,200}); glides.push_back({t,t+dur,R,true}); t+=dur;
      T.segs.push_back({t,t+0.4,200,200}); t+=0.4;
      T.segs.push_back({t,t+dur,200,0}); glides.push_back({t,t+dur,R,false}); t+=dur;
      T.segs.push_back({t,t+0.4,0,0}); t+=0.4; }
    const double vibT0=t+0.2; T.segs.push_back({t,vibT0,0,0}); t=vibT0; T.segs.push_back({t,t+3.0,0,0,20.0,6.0}); const double vibT1=t+3.0; t=vibT1+0.2;
    const size_t N=(size_t)((t+0.3)*kFs);
    std::vector<float> x(N); double ph=0;
    for(size_t i=0;i<N;++i)
    { const double f=T.hzAt((double)i/kFs); ph+=2*M_PI*f/kFs; if(ph>2*M_PI) ph-=2*M_PI;
      double s=0; for(int h=1;h<=20;++h) s+=std::sin(h*ph)/h;
      x[i]=(float)(0.25*s); }
    // ---- observers ----
    // HOP + SHIFTER: the production chain at 256-sample blocks, grain path unity
    PitchEngine det; det.prepare(kFs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const int hop=det.inputHopLength(vt);
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int k=1;k<PitchEngine::kNumVoiceTypes;++k) worst=std::min(worst,PitchEngine::voiceRange(k).fMinHz);
    PsolaEngine sh; sh.prepare(kFs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve); sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    if(getenv("PA_COTIMED")) sh.setCoTimedTarget(true);
    if(getenv("PA_PERHOP")){ int W,tm,hp; det.lagModelFor(vt,W,tm,hp); sh.setPerHopLag(true,W,tm,hp); } sh.debugRingTap(true);
    std::printf("f_lo %.0f Hz (tau %.0f smp)  voice %s  fMin %.1f Hz  hop %d  windowLength %d  pitchLagFor %d smp (%.2f ms)  shifter latency %d  clamp min(lag,latency-1) = %d (%.2f ms)\n",
        fLo,kFs/fLo,PitchEngine::voiceRange(vt).id,PitchEngine::voiceRange(vt).fMinHz,hop,det.windowLength(),det.pitchLagFor(vt),
        1000.0*det.pitchLagFor(vt)/kFs,sh.latencySamples(),std::min(det.pitchLagFor(vt),std::max(0,sh.latencySamples()-1)),
        1000.0*std::min(det.pitchLagFor(vt),std::max(0,sh.latencySamples()-1))/kFs);
    struct Obs { uint64_t pos; double hz; };
    std::vector<Obs> hopObs, shObs, rulerObs, corrObs;
    // CORRECTOR-IN-LOOP at depth 0 (applied-shift mode): the target is the hop's
    // own pitch, so the emitted ratio should be exactly 1. Any deviation, in
    // cents / glide rate, is the skew between the TARGET's clock (the hop) and
    // the audio the shifter applies it to (the read pointer). Measured via the
    // effective-ratio tap: obs hz = truth(p) * effR(p), compared with truth(p).
    PitchCorrect corr; corr.prepare(kFs,hop); corr.initDegrees(); for(int k=0;k<12;++k) corr.setDegree(k,true,0);
    corr.setRetuneMs(6.0f); corr.setFlex(0); corr.setHumanize(0); corr.setIgnoreVibrato(false); corr.setNaturalVibrato(0);
    corr.debugDepthScale(0.0f,2); corr.reset(); F0JumpGate gate;
    PsolaEngine sh2; sh2.prepare(kFs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh2.setFormantMode(PsolaEngine::kFormantPreserve); sh2.setPitchLagSamples(det.pitchLagFor(vt)); sh2.setDriftBleed(true);
    if(getenv("PA_COTIMED")) sh2.setCoTimedTarget(true);
    if(getenv("PA_PERHOP")){ int W,tm,hp; det.lagModelFor(vt,W,tm,hp); sh2.setPerHopLag(true,W,tm,hp); } sh2.debugRingTap(true);
    { PitchEngine det2; det2.prepare(kFs,256); det2.setVoiceType(vt); det2.setTracking(PitchEngine::kNormal);
      std::vector<float> raw2(N,0.0f); PitchEngine::HopEvent ev2[64]; float target=0,sliceF0=0; float shift=PsolaEngine::kNoShift; bool sliceVoiced=false;
      for(size_t p=0;p+256<=N;p+=256)
      { det2.process(x.data()+p,nullptr,256); const uint64_t blockStart=det2.inputPosition()-256; const int n=det2.drainHops(ev2,64); int cursor=0;
        for(int h=0;h<=n;++h)
        { int sliceEnd=256; if(h<n){ const int64_t rel=(int64_t)ev2[h].inputPos-(int64_t)blockStart; sliceEnd=(int)std::clamp(rel,(int64_t)cursor,(int64_t)256); }
          if(sliceEnd>cursor){ sh2.process(x.data()+p+(size_t)cursor,raw2.data()+p+(size_t)cursor,sliceEnd-cursor,sliceF0,sliceVoiced,target,shift); cursor=sliceEnd; }
          if(h<n){ const float g=gate.filter(ev2[h].f0Hz,ev2[h].voiced,det2.hopMs(),-1,-1); sliceF0=g; sliceVoiced=ev2[h].voiced;
                   const float t=corr.process(g,ev2[h].voiced,det2.hopMs()); if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; } } } }
      for(const auto& tp:sh2.debugEffRData()) if(tp.effR>0) corrObs.push_back({tp.inPos,T.hzAt((double)tp.inPos/kFs)*(double)tp.effR}); }
    std::vector<float> raw(N,0.0f);
    PitchEngine::HopEvent ev[64]; float target=0,sliceF0=0; float shift=0.0f; bool sliceVoiced=false;
    for(size_t p=0;p+256<=N;p+=256)
    {
        det.process(x.data()+p,nullptr,256);
        const uint64_t blockStart=det.inputPosition()-256;
        const int n=det.drainHops(ev,64); int cursor=0;
        for(int h=0;h<=n;++h)
        { int sliceEnd=256;
          if(h<n){ const int64_t rel=(int64_t)ev[h].inputPos-(int64_t)blockStart; sliceEnd=(int)std::clamp(rel,(int64_t)cursor,(int64_t)256); }
          if(sliceEnd>cursor){ sh.process(x.data()+p+(size_t)cursor,raw.data()+p+(size_t)cursor,sliceEnd-cursor,sliceF0,sliceVoiced,target,shift); cursor=sliceEnd; }
          if(h<n){ if(ev[h].voiced&&ev[h].f0Hz>0){ hopObs.push_back({ev[h].inputPos,ev[h].f0Hz}); target=ev[h].f0Hz; sliceF0=ev[h].f0Hz; sliceVoiced=true; } } }
    }
    for(const auto& tp:sh.debugRingTapData()) shObs.push_back({tp.inPos,tp.f0Here});
    // tap entries are written on CHANGE only; expand to a per-hop series by holding
    { PitchEngine e; e.prepare(kFs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal);
      for(size_t p=0;p+(size_t)hop<=N;p+=(size_t)hop){ e.process(x.data()+p,nullptr,hop); const PitchReading r=e.getReading(); if(r.voiced&&r.f0Hz>0) rulerObs.push_back({(uint64_t)p,r.f0Hz}); } }
    auto held=[&](const std::vector<Obs>& o){ std::vector<Obs> out; size_t k=0; for(uint64_t p=0;p<N;p+=(uint64_t)hop){ while(k+1<o.size()&&o[k+1].pos<=p) ++k; if(!o.empty()&&o[k].pos<=p) out.push_back({p,o[k].hz}); } return out; };
    const auto shHeld=held(shObs);
    auto fit=[&](const std::vector<Obs>& o,const Glide& g,double& meanErrC)->double
    {
        const double a=g.t0+0.2*(g.t1-g.t0), b=g.t0+0.8*(g.t1-g.t0);
        std::vector<const Obs*> in; for(const Obs& q:o){ const double tt=(double)q.pos/kFs; if(tt>=a&&tt<b) in.push_back(&q); }
        if(in.size()<3){ meanErrC=0; return 0; }
        double se=0; for(auto q:in) se+=cents(q->hz,T.hzAt((double)q->pos/kFs)); meanErrC=se/(double)in.size();
        long best=0; double bestE=1e9;
        for(long s=-3000;s<=3000;s+=4)
        { double e=0; for(auto q:in) e+=std::fabs(cents(q->hz,T.hzAt(((double)q->pos+(double)s)/kFs))); e/=(double)in.size();
          if(e<bestE){ bestE=e; best=s; } }
        return 1000.0*(double)best/kFs;
    };
    const auto corrHeld=held(corrObs);
    std::printf("\n  rate     dir |   HOP event (raw)          |   RULER (8192-blk, at hop)  |   SHIFTER f0Here at read ptr |  EMITTED @ depth 0 (target clock vs audio)\n");
    std::printf("  c/ms         |  lead ms   mid-glide err   |  lead ms   mid-glide err    |  lead ms   mid-glide err     |  skew ms   mid-glide err\n");
    for(const Glide& g:glides)
    { double e1,e2,e3,e4; const double l1=fit(hopObs,g,e1), l2=fit(rulerObs,g,e2), l3=fit(shHeld,g,e3), l4=fit(corrHeld,g,e4);
      std::printf("  %5.2f  %s  | %+8.2f   %+7.1fc         | %+8.2f   %+7.1fc          | %+8.2f   %+7.1fc          | %+8.2f   %+7.1fc\n",g.rate,g.up?"up  ":"down",l1,e1,l2,e2,l3,e3,l4,e4); }
    { double se=0; int n=0; for(const Obs& q:corrHeld){ const double tt=(double)q.pos/kFs; if(tt>0.45&&tt<0.65){ se+=std::fabs(cents(q.hz,T.hzAt(tt))); ++n; } }
      std::printf("  steady-note |emitted err| at depth 0: %.2fc (n %d)\n",n?se/n:0,n); }
    { double se2=0,pk=0; int n=0; double sf=0,pkf=0; int nf=0;
      for(const Obs& q:corrHeld){ const double tt=(double)q.pos/kFs; if(tt>vibT0+0.5&&tt<vibT1-0.1){ const double e=cents(q.hz,T.hzAt(tt)); se2+=e*e; pk=std::max(pk,std::fabs(e)); ++n; } }
      for(const Obs& q:shHeld){ const double tt=(double)q.pos/kFs; if(tt>vibT0+0.5&&tt<vibT1-0.1){ const double e=cents(q.hz,T.hzAt(tt)); sf+=e*e; pkf=std::max(pkf,std::fabs(e)); ++nf; } }
      std::printf("  RIPPLE on a 40c p-p / 6 Hz vibrato note: EMITTED@depth0 error rms %.2fc peak %.2fc (n %d) | f0Here error rms %.2fc peak %.2fc\n",n?std::sqrt(se2/n):0,pk,n,nf?std::sqrt(sf/nf):0,pkf); }
    // steady-state sanity
    { double se=0; int n=0; for(const Obs& q:shHeld){ const double tt=(double)q.pos/kFs; if(tt>0.45&&tt<0.65){ se+=std::fabs(cents(q.hz,T.hzAt(tt))); ++n; } }
      std::printf("\n  steady-note |err| shifter f0Here: %.2fc (n %d)\n",n?se/n:0,n); }
    return 0;
}
