// pitch_onset_converge (2 Sep 2026, section-4 ruling): for each
// source-picked onset, when does each plugin's output first touch the
// grid (|off-grid| < 10c), when does it STAY there for the rest of the
// 150ms window, what SHAPE is the approach (median |off-grid|
// trajectory per hop), and how often does it sit on the WRONG tone.
//
// Discriminates (a) wrong target / (b) late acquisition / (c) no hold.
//
//   pitch_onset_converge <source.wav> <voiceType> <dminor|chrom> <file|SELF:tau> ...
//
// SELF:<tau> renders the current engine in-process (full chain, IGN VIB
// on, flex/hum 0, scale = the grid argument) so takes with no bounce
// still get a current-engine cell. Grid "chrom" exists for the old take,
// whose key was never recorded - D-minor metrics would be fiction there.
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
static std::vector<float> renderSelf (const std::vector<float>& in, double fs,
                                      int vt, float tau, bool chrom)
{
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
    std::vector<float> out(in.size(),0.0f);
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
            { sh.process(in.data()+p+(size_t)cursor,out.data()+p+(size_t)cursor,
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
    const int lat=sh.latencySamples();
    std::vector<float> al(in.size(),0.0f);
    for(size_t i=(size_t)lat;i<out.size();++i) al[i-(size_t)lat]=out[i];
    return al;
}
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
static const int kMinorPcs[7]={2,4,5,7,9,10,0};   // D natural minor
struct Grid { bool chrom; };
// nearest grid tone as absolute semitone number, and distance in cents
static void nearest (double cents, const Grid& g, long& tone, double& dist)
{
    const double midi=69.0+cents/100.0;
    if(g.chrom){ tone=std::lround(midi); dist=std::fabs(midi-(double)tone)*100.0; return; }
    double best=1e9; long bt=0;
    for(int k=-1;k<=1;++k) for(int j=0;j<7;++j)
    { const long t=(long)(12.0*std::floor(midi/12.0+k))+kMinorPcs[j];
      const double d=std::fabs(midi-(double)t)*100.0;
      if(d<best){best=d;bt=t;} }
    tone=bt; dist=best;
}
int main (int argc, char** argv)
{
    if(argc<5){ std::printf("usage: %s <source.wav> <vt> <dminor|chrom> <file|SELF:tau>...\n",argv[0]); return 1; }
    std::vector<float> src; double fs=0;
    if(!readWavMono(argv[1],src,fs)){ std::printf("bad source\n"); return 1; }
    const int vt=atoi(argv[2]);
    const Grid grid{ std::strcmp(argv[3],"chrom")==0 };
    int hop=0; auto srcT=fineTrack(src,fs,vt,hop);
    const double hopS=hop/fs;
    const long win=(long)std::lround(0.150/hopS);
    std::vector<long> onsets;
    { int uv=1000;
      for(size_t h=0;h<srcT.size();++h)
      { if(srcT[h]<-1e8){++uv;continue;}
        if(uv*hopS>=0.06) onsets.push_back((long)h);
        uv=0; } }
    std::printf("source %s  voice %s  grid %s  onsets %zu  hop %.2fms  thresh 10c\n",
        argv[1],PitchEngine::voiceRange(vt).id,grid.chrom?"chromatic":"D-minor",onsets.size(),hopS*1000.0);
    // the singer's intended tone per onset: nearest grid tone of the
    // median source cents over the last 5 voiced hops of the window
    std::vector<long> intent(onsets.size(),LONG_MIN);
    for(size_t o=0;o<onsets.size();++o)
    { std::vector<double> tail;
      for(long h=onsets[o];h<onsets[o]+win&&h<(long)srcT.size();++h)
        if(srcT[(size_t)h]>-1e8) tail.push_back(srcT[(size_t)h]);
      if(tail.size()<5) continue;
      std::vector<double> last(tail.end()-5,tail.end());
      std::sort(last.begin(),last.end());
      long t; double d; nearest(last[2],grid,t,d); intent[o]=t; }
    for(int a=4;a<argc;++a)
    {
        std::vector<float> x; double fx=fs; std::string label=argv[a];
        if(!std::strncmp(argv[a],"SELF:",5))
        { const float tau=(float)atof(argv[a]+5);
          x=renderSelf(src,fs,vt,tau,grid.chrom);
          label="EchoJay current engine tau"+std::to_string((int)tau); }
        else if(!readWavMono(argv[a],x,fx)){ std::printf("%s unreadable\n",argv[a]); continue; }
        int h2=0; auto t=fineTrack(x,fx,vt,h2);
        std::vector<double> firstMs,holdMs,wrongMsAll; int nConv=0,nHold=0,nWrongOnsets=0,nSettleWrong=0,nSettle=0;
        std::vector<std::vector<double>> traj((size_t)win);
        for(size_t o=0;o<onsets.size();++o)
        {
            const long h0=onsets[o];
            long firstH=-1, holdH=-1; double wrongMs=0; bool sawWrong=false;
            // settled tone: median of last 5 voiced hops in the window
            std::vector<double> tail;
            for(long h=h0;h<h0+win&&h<(long)t.size();++h)
                if(t[(size_t)h]>-1e8) tail.push_back(t[(size_t)h]);
            if(tail.size()<5) continue;
            std::vector<double> last(tail.end()-5,tail.end());
            std::sort(last.begin(),last.end());
            long settled; double sd; nearest(last[2],grid,settled,sd);
            if(intent[o]!=LONG_MIN){ ++nSettle; if(settled!=intent[o]) ++nSettleWrong; }
            long lastBad=-1;
            for(long h=h0;h<h0+win&&h<(long)t.size();++h)
            {
                const double c=t[(size_t)h];
                if(c<-1e8) continue;
                long tone; double d; nearest(c,grid,tone,d);
                traj[(size_t)(h-h0)].push_back(d);
                if(d<10.0&&firstH<0) firstH=h;
                if(d>=10.0) lastBad=h;
                if(tone!=settled&&d<25.0){ wrongMs+=hopS*1000.0; sawWrong=true; }
            }
            if(firstH>=0){ ++nConv; firstMs.push_back((firstH-h0)*hopS*1000.0); }
            if(firstH>=0&&lastBad<(long)(h0+win-1))
            { holdH=std::max(firstH,lastBad+1); ++nHold; holdMs.push_back((holdH-h0)*hopS*1000.0); }
            if(sawWrong){ ++nWrongOnsets; wrongMsAll.push_back(wrongMs); }
        }
        auto med=[](std::vector<double>& v){ if(v.empty())return -1.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
        std::printf("\n%s\n",label.c_str());
        std::printf("  first-touch <10c: %d/%zu onsets, median %.1f ms\n",nConv,onsets.size(),med(firstMs));
        std::printf("  hold <10c to window end: %d/%zu onsets, median %.1f ms\n",nHold,onsets.size(),med(holdMs));
        std::printf("  wrong-tone (within 25c of a non-settled tone): %d onsets, median %.1f ms each\n",
            nWrongOnsets,med(wrongMsAll));
        std::printf("  settles on other-than-source-intent tone: %d/%d onsets\n",nSettleWrong,nSettle);
        std::printf("  median |off-grid| trajectory (c), hop 0..%ld:\n   ",win-1);
        for(long i=0;i<win;++i)
        { auto& v=traj[(size_t)i];
          if(v.empty()){ std::printf("  ."); continue; }
          std::sort(v.begin(),v.end());
          std::printf(" %2.0f",v[v.size()/2]); }
        std::printf("\n");
    }
    return 0;
}
