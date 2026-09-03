// pitch_keyhold_probe (3 Sep 2026, round-19 item 3): TEST THE STALE-KEY
// HYPOTHESIS for Sean's press-play defect ("off key for a tiny bit when I
// press play; not in the bounce"). The key engine's continuous mode needs
// 2 s of NEW audio per pass and holds its incumbent through silence and
// transport stops; the pitch device applies whatever the feed holds. So the
// first ~2 s after play run on the PREVIOUS key. Measured here, JUCE-free,
// with the production KeyEngine and the production pitch chain:
//   E1 CLEAN     KeyEngine continuous over sourceNEW from reset: every
//                reading change, audio-clock time, key, conf, tuning.
//   E2 STALE     prime the engine with OTHER material (dry.wav, the old
//                take: G major / E minor class per pitch_key_forensic),
//                1 s of stopped silence, then sourceNEW with NO reset -
//                when does the incumbent fall, and to what?
//   E3 SELF      prime with sourceNEW's own tail (stop at the end, play
//                from the top) - same measurement.
//   E4 DAMAGE    render sourceNEW's first 3 s through the production pitch
//                chain (hard, ign OFF, tau 6, seam 60) under the STALE key
//                until E2's measured switch time, then cross-fade to
//                D minor exactly as refreshAutoKey does; compare against a
//                clean D-minor render: off-grid to D minor and the notes
//                that moved a semitone.
//   pitch_keyhold_probe <sourceNEW.wav> <primer.wav>
//   Build: g++ -std=c++17 -O2 -ISource tools/pitch_keyhold_probe/main.cpp Source/EedKeyEngine.cpp -o keyhold
#include "EedKeyEngine.h"
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
static const char* PC[12]={"C","C#","D","Eb","E","F","F#","G","G#","A","Bb","B"};
struct Change { double t; int root; bool minor; float conf; float hz; };
// feed audio in 256-sample blocks, update() every block, log reading changes; t0 = clock offset for logging
static void feed (KeyEngine& e, const std::vector<float>& x, double fs, double& clock, std::vector<Change>& log, double logFrom)
{
    KeyReading last=e.getReading();
    for(size_t p=0;p<x.size();p+=256)
    { const int n=(int)std::min<size_t>(256,x.size()-p);
      e.pushBlock(x.data()+p,nullptr,n); e.update(); clock+=n/fs;
      const KeyReading r=e.getReading();
      if(r.valid&&(!last.valid||r.root!=last.root||r.minor!=last.minor||std::fabs(r.confidence-last.confidence)>0.02f))
      { if(clock>=logFrom) log.push_back({clock-logFrom,r.root,r.minor,r.confidence,r.tuningHz}); last=r; } }
}
static void printLog (const char* title, const std::vector<Change>& log, const KeyReading& final)
{
    std::printf("%s\n",title);
    for(const Change& c:log) std::printf("    t=%6.2fs  %-2s %-5s conf %.2f  tuning %.2f Hz%s\n",c.t,PC[c.root],c.minor?"minor":"major",c.conf,c.hz,c.conf>=0.5f?"":"  (below gate -> chromatic)");
    if(log.empty()) std::printf("    (no reading change)\n");
    std::printf("    final: %s %s conf %.2f\n",final.valid?PC[final.root]:"-",final.valid?(final.minor?"minor":"major"):"",final.confidence);
}
// ---- production pitch chain with a scale that can switch at a given time ----
struct Sched { double tSwitch; int rootA; bool minorA; int rootB; bool minorB; bool chromA; };
static void applyKey (PitchCorrect& c, int root, bool minor, bool chrom)
{
    static const int major[7]={0,2,4,5,7,9,11}, mn[7]={0,2,3,5,7,8,10};
    // setDegree takes INTERVALS relative to the key root (the convention every
    // render tool uses: kMinor intervals + setKeyRoot(2) for D minor).
    for(int s=0;s<12;++s){ bool in=chrom; if(!chrom) for(int d=0;d<7;++d) if((minor?mn[d]:major[d])==s) in=true; c.setDegree(s,in,0); }
    c.setKeyRoot(chrom?0:root);
}
static std::vector<float> render (const std::vector<float>& in, double fs, int vt, const Sched& S)
{
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs=det.hopMs();
    PitchCorrect corr; corr.prepare(fs,det.inputHopLength(vt)); corr.initDegrees();
    applyKey(corr,S.rootA,S.minorA,S.chromA);
    corr.setRetuneMs(6.0f); corr.setFlex(0); corr.setHumanize(0); corr.setIgnoreVibrato(false); corr.setNaturalVibrato(0); corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz; for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine sh; sh.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    sh.setFormantMode(PsolaEngine::kFormantPreserve); sh.setPitchLagSamples(det.pitchLagFor(vt)); sh.setDriftBleed(true); sh.setSeamRampMs(60.0f);
    const int lat=sh.latencySamples();
    std::vector<float> raw(in.size(),0.0f); PitchEngine::HopEvent ev[64];
    float target=0,sliceF0=0; float shift=PsolaEngine::kNoShift; bool sliceVoiced=false; bool switched=false;
    for(size_t p=0;p+256<=in.size();p+=256)
    {
        if(!switched&&S.tSwitch>=0&&(double)p/fs>=S.tSwitch){ corr.beginScaleCrossfade(); applyKey(corr,S.rootB,S.minorB,false); switched=true; }
        det.process(in.data()+p,nullptr,256);
        const uint64_t blockStart=det.inputPosition()-256; const int n=det.drainHops(ev,64); int cursor=0;
        for(int h=0;h<=n;++h)
        { int sliceEnd=256; if(h<n){ const int64_t rel=(int64_t)ev[h].inputPos-(int64_t)blockStart; sliceEnd=(int)std::clamp(rel,(int64_t)cursor,(int64_t)256); }
          if(sliceEnd>cursor){ sh.process(in.data()+p+(size_t)cursor,raw.data()+p+(size_t)cursor,sliceEnd-cursor,sliceF0,sliceVoiced,target,shift); cursor=sliceEnd; }
          if(h<n){ float rO=-1,rN=-1; const bool seed=ev[h].voiced&&ev[h].f0Hz>0&&gate.lastGood()<=0;
                   if(gate.isBigJump(ev[h].f0Hz,ev[h].voiced)||seed){ const double ref=seed?2.0*ev[h].f0Hz:gate.lastGood(); rO=sh.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ref)); rN=sh.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ev[h].f0Hz)); }
                   const float g=gate.filter(ev[h].f0Hz,ev[h].voiced,hopMs,rO,rN); sliceF0=g; sliceVoiced=ev[h].voiced;
                   const float t=corr.process(g,ev[h].voiced,hopMs); if(t>0){ target=t; shift=corr.shiftPreferred()?corr.lastShiftCents():PsolaEngine::kNoShift; } } }
    }
    std::vector<float> out(in.size(),0.0f); for(size_t i=(size_t)lat;i<raw.size();++i) out[i-(size_t)lat]=raw[i]; return out;
}
static std::vector<double> track (const std::vector<float>& x, double fs, int vt, int& hop)
{ PitchEngine e; e.prepare(fs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal); hop=e.inputHopLength(vt); std::vector<double> t;
  for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop){ e.process(x.data()+p,nullptr,hop); const PitchReading r=e.getReading(); t.push_back(r.voiced&&r.f0Hz>0?1200.0*std::log2(r.f0Hz/440.0):NAN); } return t; }
static double offDmin (double c){ static const int mn[7]={0,2,3,5,7,8,10}; const long s0=std::lround(c/100.0); double b=1e9; for(long k=s0-2;k<=s0+2;++k){ const int pc=(int)(((k+9)%12+12)%12); for(int d=0;d<7;++d) if((mn[d]+2)%12==pc) b=std::min(b,std::fabs(c-100.0*k)); } return b; }
static double pct(std::vector<double> v,double q){ if(v.empty())return 0; std::sort(v.begin(),v.end()); return v[std::min(v.size()-1,(size_t)(q*v.size()))]; }
int main(int argc,char**argv)
{
    if(argc<3){ std::printf("usage: %s <sourceNEW.wav> <primer.wav>\n",argv[0]); return 1; }
    std::vector<float> src,prim; double fs=0,fs2=0;
    if(!readWavMono(argv[1],src,fs)){ std::printf("bad source\n"); return 1; }
    if(!std::strncmp(argv[2],"synth:",6))
    {   // 12 s of triads in the named major key, 4 chords x 3 s, 8 harmonics each, 1/h amplitude - a confident, unambiguous primer
        int root=0; const char* nm=argv[2]+6; for(int k=0;k<12;++k) if(!std::strncmp(nm,PC[k],std::strlen(PC[k]))&&(int)std::strlen(PC[k])==(int)(std::strchr(nm,'m')?std::strchr(nm,'m')-nm:std::strlen(nm))) root=k;
        static const int prog[4][3]={{0,4,7},{5,9,0},{7,11,2},{0,4,7}};
        prim.assign((size_t)(12.0*fs),0.0f);
        for(int ch=0;ch<4;++ch) for(int v=0;v<3;++v){ const double f=130.81*std::pow(2.0,((root+prog[ch][v])%12)/12.0)*(v==0?1:(v==1?1:1.5));
          for(size_t i=(size_t)(ch*3.0*fs);i<(size_t)((ch+1)*3.0*fs)&&i<prim.size();++i){ double sgn=0; for(int h=1;h<=8;++h) sgn+=std::sin(2*M_PI*f*h*(double)i/fs)/h; prim[i]+=(float)(0.08*sgn); } }
        fs2=fs;
    }
    else if(!readWavMono(argv[2],prim,fs2)){ std::printf("bad primer\n"); return 1; }
    std::vector<float> silence((size_t)fs,0.0f);
    // E1
    { KeyEngine e; e.prepare(fs,256); e.setContinuous(true); double clk=0; std::vector<Change> log; feed(e,src,fs,clk,log,0.0);
      printLog("E1 CLEAN: KeyEngine continuous on sourceNEW from reset (t = audio seconds)",log,e.getReading()); }
    // E2
    double switchT=-1; int staleRoot=0; bool staleMinor=false; float staleConf=0;
    { KeyEngine e; e.prepare(fs,256); e.setContinuous(true); double clk=0; std::vector<Change> pre; feed(e,prim,fs,clk,pre,0.0);
      const KeyReading primed=e.getReading(); staleRoot=primed.root; staleMinor=primed.minor; staleConf=primed.confidence;
      std::printf("\nE2 STALE: primed on %s -> incumbent %s %s conf %.2f; then 1.0s stopped silence; then sourceNEW from the top, NO reset\n",argv[2],PC[primed.root],primed.minor?"minor":"major",primed.confidence);
      feed(e,silence,fs,clk,pre,1e9); const double t0=clk; std::vector<Change> log; feed(e,src,fs,clk,log,t0);
      printLog("  readings after play (t = seconds since play):",log,e.getReading());
      for(const Change& c:log) if(c.conf>=0.5f&&(c.root!=staleRoot||c.minor!=staleMinor)){ switchT=c.t; break; }
      if(switchT<0){ for(const Change& c:log) if(c.conf<0.5f){ switchT=c.t; break; } }
      std::printf("  INCUMBENT SURVIVED %s after play\n",switchT<0?"THE WHOLE TAKE":(std::to_string(switchT).substr(0,5)+" s").c_str()); }
    // E3
    { KeyEngine e; e.prepare(fs,256); e.setContinuous(true); double clk=0; std::vector<Change> pre; feed(e,src,fs,clk,pre,0.0);
      const KeyReading primed=e.getReading();
      std::printf("\nE3 SELF: primed on sourceNEW itself (stop at the end) -> incumbent %s %s conf %.2f; 1.0s silence; play from the top\n",PC[primed.root],primed.minor?"minor":"major",primed.confidence);
      feed(e,silence,fs,clk,pre,1e9); const double t0=clk; std::vector<Change> log; feed(e,src,fs,clk,log,t0);
      printLog("  readings after play:",log,e.getReading()); }
    // E4
    {
        const int vt=1;
        const bool staleUsable=staleConf>=0.5f;
        const double sw=switchT<0?1e9:switchT;
        Sched stale{sw,staleRoot,staleMinor,2,true,!staleUsable};   // stale key (or chromatic if below gate) until the measured switch, then D minor
        Sched clean{-1,2,true,2,true,false};
        auto a=render(src,fs,vt,stale), b=render(src,fs,vt,clean);
        int hop=0; auto ta=track(a,fs,vt,hop), tb=track(b,fs,vt,hop), ts=track(src,fs,vt,hop); const double hs=hop/fs;
        std::printf("\nE4 DAMAGE: sourceNEW through the production chain (hard, ign OFF, tau 6, seam 60) under %s%s until %.2fs then cross-fade to D minor, vs clean D minor\n",
            staleUsable?PC[staleRoot]:"CHROMATIC (stale conf below gate)",staleUsable?(staleMinor?" minor":" major"):"",sw>1e8?99.0:sw);
        for(int win=0;win<3;++win)
        { const double w0=win==0?0:(win==1?2.5:5.0), w1=win==0?2.5:(win==1?5.0:13.5);
          std::vector<double> oa,ob,os; long semi=0,n=0;
          for(size_t h=0;h<ta.size()&&h<tb.size()&&h<ts.size();++h){ const double t=h*hs; if(t<w0||t>=w1) continue; if(std::isnan(ta[h])||std::isnan(tb[h])||std::isnan(ts[h])) continue;
            oa.push_back(offDmin(ta[h])); ob.push_back(offDmin(tb[h])); os.push_back(offDmin(ts[h])); ++n; if(std::fabs(ta[h]-tb[h])>60) ++semi; }
          std::printf("  %4.1f-%4.1fs: off-grid-to-Dminor median  stale %5.2fc  clean %5.2fc  source %5.2fc | hops where stale and clean differ by >60c: %ld of %ld (%.1f%%)\n",
              w0,w1,pct(oa,0.5),pct(ob,0.5),pct(os,0.5),semi,n,n?100.0*semi/n:0); }
        // which notes moved: list runs where |stale-clean|>60c
        std::printf("  semitone-moved runs (t, source Hz, stale vs clean cents from A440):");
        bool inRun=false; double rs=0; int cnt=0;
        for(size_t h=0;h<ta.size()&&h<tb.size();++h){ const bool m=!std::isnan(ta[h])&&!std::isnan(tb[h])&&std::fabs(ta[h]-tb[h])>60;
          if(m&&!inRun){ inRun=true; rs=h*hs; } if(!m&&inRun){ inRun=false; if(h*hs-rs>=0.04&&cnt<12){ std::printf(" [%.2f-%.2fs %s->%s]",rs,h*hs,PC[(int)(((std::lround(ta[(size_t)((rs+0.02)/hs)]/100.0)+9)%12+12)%12)],PC[(int)(((std::lround(tb[(size_t)((rs+0.02)/hs)]/100.0)+9)%12+12)%12)]); ++cnt; } } }
        std::printf("\n");
    }
    // E5: the applied-scale mismatch cost over the WHOLE take: chromatic vs D minor (what a below-gate auto key gives on a solo vocal)
    {
        const int vt=1; Sched chrom{-1,0,false,0,false,true}, clean{-1,2,true,2,true,false};
        auto a=render(src,fs,vt,chrom), b=render(src,fs,vt,clean); int hop=0; auto ta=track(a,fs,vt,hop), tb=track(b,fs,vt,hop); const double hs=hop/fs;
        long semi=0,n=0; double semiS=0; for(size_t h=0;h<ta.size()&&h<tb.size();++h){ if(std::isnan(ta[h])||std::isnan(tb[h])) continue; ++n; if(std::fabs(ta[h]-tb[h])>60){ ++semi; semiS+=hs; } }
        std::printf("\nE5 WHOLE TAKE, CHROMATIC (below-gate auto key on a solo vocal) vs D MINOR: %ld of %ld voiced hops (%.1f%%, %.2f s) land a semitone apart\n",semi,n,n?100.0*semi/n:0,semiS);
    }
    return 0;
}
