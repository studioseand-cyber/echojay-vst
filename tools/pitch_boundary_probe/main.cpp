// boundary: what does the retune envelope DO at a note boundary, per tau?
// (31 Aug 2026.) Drives detector -> gate -> corrector over sourceNEW at
// hard/ignoreVib-on with retune tau overridden, logs the applied target
// per hop, and reports for each confirmed note change: the SNAP
// DISCONTINUITY (one-hop jump in the applied target at confirmation) and
// the applied-shift trajectory through the new note's first 300 ms.
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
int main (int argc, char** argv)
{
    const float tau = argc>2 ? (float)atof(argv[2]) : 6.0f;
    std::vector<float> in; double fs=0;
    if(argc<2||!readWavMono(argv[1],in,fs)){ std::printf("bad input\n"); return 1; }

    constexpr int vt = PitchEngine::kLowMale;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs = det.hopMs();
    PitchCorrect corr; corr.prepare(fs, det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,false,0);
    for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0);
    corr.setKeyRoot(2);
    corr.setRetuneMs(tau); corr.setFlex(0); corr.setHumanize(0);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(0); corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine ring; ring.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    ring.setPitchLagSamples(det.pitchLagFor(vt));
    std::vector<float> scratch(256);
    PitchEngine::HopEvent ev[64];
    std::vector<double> tgtC, inC;      // per hop: applied target cents, in cents (C-frame diff irrelevant: use A-frame both)
    std::vector<int>    chg;            // hop indices where noteChanges() ticked
    uint32_t lastNC=0;
    for(size_t p=0;p+256<=in.size();p+=256)
    {
        det.process(in.data()+p,nullptr,256);
        ring.process(in.data()+p,scratch.data(),256,0,false,0);
        const int n=det.drainHops(ev,64);
        for(int h=0;h<n;++h)
        {
            float rO=-1,rN=-1;
            const bool seed=ev[h].voiced&&ev[h].f0Hz>0&&gate.lastGood()<=0;
            if(gate.isBigJump(ev[h].f0Hz,ev[h].voiced)||seed)
            { const double ref=seed?2.0*ev[h].f0Hz:gate.lastGood();
              rO=ring.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ref));
              rN=ring.inputPeriodicity(ev[h].inputPos,(int)std::lround(fs/ev[h].f0Hz)); }
            const float g=gate.filter(ev[h].f0Hz,ev[h].voiced,hopMs,rO,rN);
            const float t=corr.process(g,ev[h].voiced,hopMs);
            const bool voiced=ev[h].voiced&&g>0;
            tgtC.push_back(t>0? 1200.0*std::log2(t/440.0) : -1e9);
            inC.push_back(voiced? 1200.0*std::log2(g/440.0) : -1e9);
            if(corr.noteChanges()!=lastNC){ lastNC=corr.noteChanges(); chg.push_back((int)tgtC.size()-1); }
        }
    }
    const double hopS=det.inputHopLength(vt)/fs;
    auto med=[](std::vector<double>&v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
    std::vector<double> disc, s30, s150, s300, rem300;
    for(int k:chg)
    {
        if(k<1) continue;
        if(tgtC[(size_t)k]<-1e8||tgtC[(size_t)k-1]<-1e8) continue;
        disc.push_back(std::fabs(tgtC[(size_t)k]-tgtC[(size_t)k-1]));
        auto shiftAt=[&](double ms)->double{
            const int i=k+(int)std::lround(ms/1000.0/hopS);
            if(i>=(int)tgtC.size()||tgtC[(size_t)i]<-1e8||inC[(size_t)i]<-1e8) return -1;
            return std::fabs(tgtC[(size_t)i]-inC[(size_t)i]); };
        double a=shiftAt(30),b=shiftAt(150),c=shiftAt(300);
        if(a>=0)s30.push_back(a); if(b>=0)s150.push_back(b); if(c>=0)s300.push_back(c);
    }
    std::printf("tau %3.0f: boundaries %2d | SNAP disc med %5.1fc | applied |shift| med +30ms %4.1fc  +150ms %4.1fc  +300ms %4.1fc\n",
        tau,(int)chg.size(),med(disc),med(s30),med(s150),med(s300));
    return 0;
}
