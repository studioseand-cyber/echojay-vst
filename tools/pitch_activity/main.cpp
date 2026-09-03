// pitch_activity (3 Sep 2026, the slow-end-of-the-dial finding):
// ACTIVITY = |output pitch - source pitch| in cents, per detector hop, on
// hops voiced in BOTH files - the unit Sean hears and the container
// reported (ANT_MAX med 0.61c vs EJ_MAX 4.90c at max retune). Reports the
// distribution (med/p75/p90/>25c%), the first second in 250ms bins from
// file start (the transport-start signature), word-start (0-150ms after a
// voiced onset following >=60ms unvoiced) vs mid-note means, and the >25c
// events with timestamps.
//
// Self-renders reproduce the PRODUCTION chain exactly as tools/
// pitch_glitch_events does (gate -> corrector -> shifter, 256-sample
// blocks, hop-sliced, hold-last-target; corrector experiment flags at
// their shipped defaults - envExp 0 since f1d9f5f), with tau / seam ramp /
// ignore-vib / flex / humanize / natural-vib as arguments. They carry the
// effective-ratio tap, so activity can be ATTRIBUTED per hop:
//   commanded  = 1200*log2(effR)   (what the engine actually applied)
//   buckets, priority order:  SNAP  (within 100ms after a >50c target
//   jump: the note-boundary chase + confirmation snap), SEAM (within the
//   ramp window after a dry->wet re-entry >=15ms), DETECT (the gated f0 the
//   corrector aimed from differs from the fine source track by >10c: the
//   error passes into the ratio), GLIDE (everything else: the retune
//   envelope's own travel).  Plus the UNINTENDED residual |d - commanded|.
//
//   pitch_activity <source.wav> <vt> <dminor|chrom> <spec>...
//     spec: file:<path>
//           self:<tau>:<ramp>:<ign 0|1>[:<flex>:<humanize>:<natvib>]
//   Build: g++ -std=c++17 -O2 -ISource tools/pitch_activity/main.cpp -o pa
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
#include <numeric>
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
struct Cfg { float tau=150, ramp=0, flex=0, hum=0, nat=0; bool ign=true; bool grain=false; float gapSec=-1; int resetMask=0; size_t resetAt=0; };
struct Render { std::vector<float> out; std::vector<PsolaEngine::DbgRingTap> tap; std::vector<PsolaEngine::DbgEffR> effR; };
static Render renderSelf (const std::vector<float>& in, double fs, int vt, bool chrom, const Cfg& c)
{
    Render R;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,chrom,0);
    if(!chrom){ for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0); corr.setKeyRoot(2); }
    else corr.setKeyRoot(0);
    corr.setRetuneMs(c.tau); corr.setFlex(c.flex); corr.setHumanize(c.hum);
    corr.setIgnoreVibrato(c.ign); corr.setNaturalVibrato(c.nat);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve);
    sh.setPitchLagSamples(det.pitchLagFor(vt));
    sh.setDriftBleed(true);
    sh.debugRingTap(true);
    sh.setSeamRampMs(c.ramp);
    const int lat=sh.latencySamples();
    std::vector<float> raw(in.size(),0.0f);
    PitchEngine::HopEvent ev[64];
    float target=0, sliceF0=0; float shift=PsolaEngine::kNoShift; bool sliceVoiced=false;
    for(size_t p=0;p+256<=in.size();p+=256)
    {
        if(c.resetAt>0&&p<=c.resetAt&&p+256>c.resetAt&&c.resetMask)
        {
            // ABLATION at the restart boundary: which component's carried
            // state makes pass 2 differ from a clean render?
            if(c.resetMask&1) corr.reset();
            if(c.resetMask&2) gate.reset();
            if(c.resetMask&4) { det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal); }
            if(c.resetMask&8) { sh.reset(); }
            if(c.resetMask&16){ target=0; sliceF0=0; shift=PsolaEngine::kNoShift; sliceVoiced=false; }
        }
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
                if(c.grain){ if(g>0){ target=g; shift=0.0f; } }
                else { const float t=corr.process(g,ev[h].voiced,hopMs);
                       if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; } }
            }
        }
    }
    R.out.assign(in.size(),0.0f);
    for(size_t i=(size_t)lat;i<raw.size();++i) R.out[i-(size_t)lat]=raw[i];
    R.tap=sh.debugRingTapData();
    R.effR=sh.debugEffRData();
    return R;
}
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
static double pct (std::vector<double> v, double q)
{ if(v.empty()) return 0; std::sort(v.begin(),v.end()); return v[std::min(v.size()-1,(size_t)(q*(double)v.size()))]; }
static double cents (double a, double b) { return 1200.0*std::log2(a/b); }
int main (int argc, char** argv)
{
    if(argc<5){ std::printf("usage: %s <source.wav> <vt> <dminor|chrom> <spec>...\n",argv[0]); return 1; }
    std::vector<float> src; double fs=0;
    if(!readWavMono(argv[1],src,fs)){ std::printf("bad source\n"); return 1; }
    const int vt=atoi(argv[2]);
    const bool chrom=!std::strcmp(argv[3],"chrom");
    int hop=0; auto tSrc=fineTrack(src,fs,vt,hop);
    const double hopS=hop/fs;
    std::vector<double> words;
    { int uv=1000;
      for(size_t h=0;h<tSrc.size();++h)
      { if(tSrc[h]<=0){++uv;continue;}
        if(uv*hopS>=0.06) words.push_back((double)h*hopS);
        uv=0; } }
    size_t nv=0; for(double v:tSrc) if(v>0) ++nv;
    std::printf("source %s (%zu samples, mod 256 = %zu)  voice %s  fs %.0f  hop %d (%.2fms)  voiced hops %zu  word starts %zu\n",
        argv[1],src.size(),src.size()%256,PitchEngine::voiceRange(vt).id,fs,hop,1000*hopS,nv,words.size());
    for(int a=4;a<argc;++a)
    {
        std::string spec=argv[a]; std::string label=spec;
        std::vector<float> x; Render R; bool haveTap=false;
        if(spec=="unity")
        {
            Cfg c; c.grain=true; R=renderSelf(src,fs,vt,chrom,c); x=R.out; haveTap=true;
            label="UNITY (grain path, shift 0: the tracker-disagreement floor)";
        }
        else if(!spec.compare(0,6,"selfp:")||!spec.compare(0,6,"selfc:"))
        {
            const bool cut=!spec.compare(0,6,"selfc:");
            Cfg c; int ign=1; float arg=0;
            const int n=std::sscanf(spec.c_str()+6,"%f:%f:%d:%f:%f:%f:%f",&c.tau,&c.ramp,&ign,&c.flex,&c.hum,&c.nat,&arg);
            if(n<7){ std::printf("bad spec %s\n",spec.c_str()); continue; }
            c.ign=ign!=0;
            std::vector<float> cat; size_t off=0;
            if(cut){ off=(size_t)std::llround(arg*fs); cat.assign(src.begin(),src.begin()+(long)std::min(off,src.size())); cat.insert(cat.end(),src.begin(),src.end()); }
            else   { off=(size_t)arg; cat.assign(off,0.0f); cat.insert(cat.end(),src.begin(),src.end()); }
            Render R2=renderSelf(cat,fs,vt,chrom,c);
            R.out.assign(R2.out.begin()+(long)off,R2.out.end());
            for(const auto& t:R2.tap) if(t.inPos>=off) R.tap.push_back({t.inPos-off,t.f0Here,t.target});
            for(const auto& t:R2.effR) if(t.inPos>=off) R.effR.push_back({t.inPos-off,t.effR});
            x=R.out; haveTap=true;
            char b[220]; std::snprintf(b,sizeof b,"%s tau %.0f  seam %.0fms  ign-vib %s  flex %.0f hum %.0f natvib %.0f  (%s %s)",
                cut?"STOP-MID-NOTE-THEN-PLAY-FROM-TOP":"CLEAN-PADDED",c.tau,c.ramp,c.ign?"ON":"OFF",c.flex,c.hum,c.nat,
                cut?"previous playback stopped at":"input pre-padded by", cut?(std::to_string(arg)+"s, no silence between").c_str():(std::to_string((int)arg)+" zero samples").c_str());
            label=b;
        }
        else if(!spec.compare(0,5,"self:")||!spec.compare(0,6,"selfx:"))
        {
            const bool stale=!spec.compare(0,6,"selfx:");
            Cfg c; int ign=1;
            const int n=std::sscanf(spec.c_str()+(stale?6:5),"%f:%f:%d:%f:%f:%f:%f:%d",&c.tau,&c.ramp,&ign,&c.flex,&c.hum,&c.nat,&c.gapSec,&c.resetMask);
            if(n<3){ std::printf("bad spec %s\n",spec.c_str()); continue; }
            c.ign=ign!=0;
            if(stale)
            {
                // STALE-STATE RESTART: run the take once (a previous playback),
                // then gapSec of digital silence (transport stopped: Logic
                // keeps calling processBlock with zeros), then the take again
                // from the same engine instances with NO prepare/reset -
                // pass 2 is what a live transport start hears. Measured
                // against the source like any other render.
                const size_t gap=(size_t)std::llround(std::max(0.0f,c.gapSec)*fs);
                std::vector<float> cat(src); cat.insert(cat.end(),gap,0.0f); cat.insert(cat.end(),src.begin(),src.end());
                c.resetAt=src.size()+gap;
                Render R2=renderSelf(cat,fs,vt,chrom,c);
                const size_t off=src.size()+gap;
                R.out.assign(R2.out.begin()+(long)off,R2.out.end());
                for(const auto& t:R2.tap) if(t.inPos>=off) R.tap.push_back({t.inPos-off,t.f0Here,t.target});
                for(const auto& t:R2.effR) if(t.inPos>=off) R.effR.push_back({t.inPos-off,t.effR});
            }
            else R=renderSelf(src,fs,vt,chrom,c);
            x=R.out; haveTap=true;
            char b[200]; std::snprintf(b,sizeof b,"%s tau %.0f  seam %.0fms  ign-vib %s  flex %.0f hum %.0f natvib %.0f%s",
                stale?"STALE-RESTART":"SELF",c.tau,c.ramp,c.ign?"ON":"OFF",c.flex,c.hum,c.nat,stale?"":"");
            label=b; if(stale){ char g[96]; std::snprintf(g,sizeof g,"  (pass 2 after %.1fs stopped silence; reset mask %d)",c.gapSec,c.resetMask); label+=g; }
        }
        else if(!spec.compare(0,5,"file:"))
        { double f2=0;
          if(!readWavMono(spec.c_str()+5,x,f2)){ std::printf("%s unreadable\n",spec.c_str()); continue; }
          if(std::fabs(f2-fs)>1){ std::printf("%s: fs %.0f != source %.0f\n",spec.c_str(),f2,fs); continue; }
          label="FILE "+spec.substr(5); const size_t sl=label.rfind('/'); if(sl!=std::string::npos) label="FILE "+label.substr(sl+1); }
        else { std::printf("bad spec %s\n",spec.c_str()); continue; }
        const long lag=haveTap?0:alignLag(src,x);
        int rh=0; auto tX=fineTrack(x,fs,vt,rh);
        // per-hop activity
        struct Hop { size_t h; double t, d; };   // d signed cents out-src
        std::vector<Hop> hops; long octave=0, unvoicedOut=0;
        for(size_t h=0;h<tSrc.size();++h)
        {
            if(tSrc[h]<=0) continue;
            const long j=(long)std::llround(((double)h*hop+(double)lag)/hop);
            if(j<0||j>=(long)tX.size()) continue;
            if(tX[(size_t)j]<=0){ ++unvoicedOut; continue; }
            const double d=cents(tX[(size_t)j],tSrc[h]);
            if(std::fabs(d)>600){ ++octave; continue; }   // tracker octave disagreement, excluded (withdrawn class)
            hops.push_back({h,(double)h*hopS,d});
        }
        std::vector<double> ad; for(const Hop& q:hops) ad.push_back(std::fabs(q.d));
        long over25=0; for(double v:ad) if(v>25) ++over25;
        std::printf("\n%s  [lag %+ld smp; compared %zu hops; %ld src-voiced hops unvoiced in output; %ld octave-disagree excluded]\n",
            label.c_str(),lag,hops.size(),unvoicedOut,octave);
        std::printf("  ACTIVITY |out-src|  median %5.2fc  p75 %5.2fc  p90 %5.2fc  >25c %4.1f%%  mean %5.2fc\n",
            pct(ad,0.5),pct(ad,0.75),pct(ad,0.9),ad.empty()?0:100.0*over25/(double)ad.size(),
            ad.empty()?0:std::accumulate(ad.begin(),ad.end(),0.0)/(double)ad.size());
        // first second in 250ms bins from file start; signed mean too (above/below source)
        std::printf("  FROM FILE START (mean |d| / signed mean d / n):");
        for(int b=0;b<4;++b)
        { double s=0,ss=0; int n=0;
          for(const Hop& q:hops) if(q.t>=b*0.25&&q.t<(b+1)*0.25){ s+=std::fabs(q.d); ss+=q.d; ++n; }
          std::printf("  %d-%dms %5.2f/%+5.2f/%d",b*250,(b+1)*250,n?s/n:0,n?ss/n:0,n); }
        std::printf("\n");
        // first 130ms detail (raw signed track), for the transport-start signature
        std::printf("  FIRST 130ms signed d (cents, per hop):");
        int shown=0; for(const Hop& q:hops){ if(q.t<0.130&&shown<24){ std::printf(" %+.0f",q.d); ++shown; } }
        std::printf("\n");
        // word start vs mid-note
        { double sw=0,sm=0; int nw=0,nm=0;
          for(const Hop& q:hops)
          { bool ws=false; for(double w:words) if(q.t>=w&&q.t<w+0.150){ ws=true; break; }
            if(ws){ sw+=std::fabs(q.d); ++nw; } else { sm+=std::fabs(q.d); ++nm; } }
          std::printf("  WORD-START (0-150ms) mean %5.2fc (n %d)   MID-NOTE mean %5.2fc (n %d)\n",nw?sw/nw:0,nw,nm?sm/nm:0,nm); }
        // events >25c
        { struct Ev { double t, pk; }; std::vector<Ev> evs;
          for(const Hop& q:hops) if(std::fabs(q.d)>25)
          { if(!evs.empty()&&q.t-evs.back().t<0.030){ if(std::fabs(q.d)>std::fabs(evs.back().pk)) evs.back().pk=q.d; evs.back().t=q.t; }
            else evs.push_back({q.t,q.d}); }
          std::printf("  EVENTS >25c: %zu",evs.size());
          int k=0; for(const Ev& e:evs){ if(k++<30) std::printf("  %.2fs:%+.0f",e.t,e.pk); }
          if(evs.size()>30) std::printf("  ...");
          std::printf("\n"); }
        if(!haveTap) continue;
        // ---- attribution from the taps -------------------------------------
        // commanded cents per hop from the effR tap (nearest inPos)
        std::vector<double> cmd(tSrc.size(),0.0); std::vector<char> haveCmd(tSrc.size(),0);
        { size_t k=0;
          for(size_t h=0;h<tSrc.size();++h)
          { const double pos=(double)h*hop;
            while(k+1<R.effR.size()&&(double)R.effR[k+1].inPos<=pos) ++k;
            if(!R.effR.empty()&&std::fabs((double)R.effR[k].inPos-pos)<2.0*hop&&R.effR[k].effR>0)
            { cmd[h]=1200.0*std::log2((double)R.effR[k].effR); haveCmd[h]=1; } } }
        // snap windows: >50c target jump between consecutive ring-tap entries
        std::vector<double> snapT; std::vector<double> seamT;
        for(size_t i=1;i<R.tap.size();++i)
        { const double t1=(double)R.tap[i].inPos/fs;
          if(R.tap[i].target>0&&R.tap[i-1].target>0&&std::fabs(cents(R.tap[i].target,R.tap[i-1].target))>50) snapT.push_back(t1);
          if((double)(R.tap[i].inPos-R.tap[i-1].inPos)>=0.015*fs) seamT.push_back(t1); }
        // detector error per hop: f0Here vs fine source track
        std::vector<double> detErr(tSrc.size(),0.0);
        { size_t k=0;
          for(size_t h=0;h<tSrc.size();++h)
          { if(tSrc[h]<=0) continue; const double pos=(double)h*hop;
            while(k+1<R.tap.size()&&(double)R.tap[k+1].inPos<=pos) ++k;
            if(!R.tap.empty()&&std::fabs((double)R.tap[k].inPos-pos)<2.0*hop&&R.tap[k].f0Here>0)
              detErr[h]=cents(R.tap[k].f0Here,tSrc[h]); } }
        const double seamWin=0.060;   // fixed window so ramp 0 and ramp 60 are comparable
        enum { SNAP, SEAM, DETECT, GLIDE, NB };
        static const char* nm[NB]={"SNAP(100ms after >50c target jump)","SEAM(60ms after >=15ms dry re-entry)","DETECT(|f0Here-src|>10c)","GLIDE(remainder)"};
        double sum[NB]={0,0,0,0}, sumCmd[NB]={0,0,0,0}; int cnt[NB]={0,0,0,0}; double total=0; int n25[NB]={0,0,0,0};
        std::vector<double> resid;
        for(const Hop& q:hops)
        {
            int b=GLIDE;
            bool snap=false; for(double s:snapT) if(q.t>=s&&q.t<s+0.100){ snap=true; break; }
            bool seam=false; for(double s:seamT) if(q.t>=s&&q.t<s+seamWin){ seam=true; break; }
            if(snap) b=SNAP; else if(seam) b=SEAM; else if(std::fabs(detErr[q.h])>10) b=DETECT;
            const double a=std::fabs(q.d); sum[b]+=a; ++cnt[b]; total+=a; if(a>25) ++n25[b];
            if(haveCmd[q.h]){ sumCmd[b]+=std::fabs(cmd[q.h]); resid.push_back(std::fabs(q.d-cmd[q.h])); }
        }
        std::printf("  ATTRIBUTION (share of total |d|; hops; mean |d|; mean |commanded|; >25c events in bucket):\n");
        for(int b=0;b<NB;++b)
            std::printf("    %-40s %5.1f%%  n %5d (%4.1f%%)  mean %6.2fc  cmd %6.2fc  >25c %d\n",
                nm[b],total>0?100.0*sum[b]/total:0,cnt[b],hops.empty()?0:100.0*cnt[b]/(double)hops.size(),
                cnt[b]?sum[b]/cnt[b]:0,cnt[b]?sumCmd[b]/cnt[b]:0,n25[b]);
        std::printf("    UNINTENDED |d - commanded|: median %5.2fc  p90 %5.2fc  (n %zu)   snap windows %zu  seam re-entries %zu\n",
            pct(resid,0.5),pct(resid,0.9),resid.size(),snapT.size(),seamT.size());
        // per snap window: peak |d| and peak |cmd| (the confirmation snap's size vs tau)
        { std::vector<double> pd,pc;
          for(double sT:snapT)
          { double md=0,mc=0; for(const Hop& q:hops) if(q.t>=sT&&q.t<sT+0.100){ md=std::max(md,std::fabs(q.d)); if(haveCmd[q.h]) mc=std::max(mc,std::fabs(cmd[q.h])); }
            pd.push_back(md); pc.push_back(mc); }
          std::printf("    SNAP WINDOWS: peak |d| median %5.1fc p90 %5.1fc   peak |commanded| median %5.1fc p90 %5.1fc\n",pct(pd,0.5),pct(pd,0.9),pct(pc,0.5),pct(pc,0.9)); }
        if(getenv("PA_DUMP"))
        { double t0=0,t1=0; std::sscanf(getenv("PA_DUMP"),"%lf,%lf",&t0,&t1);
          std::printf("    DUMP %.2f-%.2fs  t | src Hz | out-src c | commanded c | f0Here-src c | target-src c\n",t0,t1);
          size_t k=0;
          for(const Hop& q:hops) if(q.t>=t0&&q.t<t1)
          { const double pos=(double)q.h*hop; while(k+1<R.tap.size()&&(double)R.tap[k+1].inPos<=pos) ++k;
            const double tg=(!R.tap.empty()&&R.tap[k].target>0)?cents(R.tap[k].target,tSrc[q.h]):0;
            std::printf("      %6.3f %7.1f %+7.1f %+7.1f %+7.1f %+7.1f\n",q.t,tSrc[q.h],q.d,haveCmd[q.h]?cmd[q.h]:0.0,detErr[q.h],tg); } }
        // first 250ms: commanded vs measured
        { double sc=0,sd=0; int n=0; for(const Hop& q:hops) if(q.t<0.25&&haveCmd[q.h]){ sc+=cmd[q.h]; sd+=q.d; ++n; }
          std::printf("    FIRST 250ms: signed mean commanded %+5.2fc vs measured %+5.2fc (n %d)\n",n?sc/n:0,n?sd/n:0,n); }
    }
    return 0;
}
