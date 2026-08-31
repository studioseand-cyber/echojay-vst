// depthstat (31 Aug 2026): is the vibrato-depth estimate stale at onsets?
// Logs per-hop |osc| = |inCents - slowCents| and confirm ticks, then scores
// candidate depth ENVELOPES (one-pole of |osc|, tau 50/150/300ms, each with
// and without reset-at-confirm) in two windows: first 100ms after a note
// boundary vs mid-sustain (>300ms in). The gate's whole premise is that
// onsets read shallow; a memory that carries the previous note's depth
// across the boundary is the three-times-seen stale-per-note-state bug.
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
    std::vector<float> in; double fs=0;
    if(argc<2||!readWavMono(argv[1],in,fs)){ std::printf("bad input\n"); return 1; }
    constexpr int vt = PitchEngine::kLowMale;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs = det.hopMs();
    PitchCorrect corr; corr.prepare(fs, det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,false,0);
    for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0);
    corr.setKeyRoot(2); corr.setRetuneMs(120);
    corr.setFlex(55); corr.setHumanize(60);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(100);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine ring; ring.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    ring.setPitchLagSamples(det.pitchLagFor(vt));
    std::vector<float> scratch(256);
    PitchEngine::HopEvent ev[64];
    std::vector<double> osc; std::vector<int> confT; std::vector<int> voiced;
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
            voiced.push_back(t>0&&g>0?1:0);
            osc.push_back(std::fabs(corr.lastInCents()-corr.debugSlowTrack()));
            confT.push_back(corr.noteChanges()!=lastNC?1:0);
            lastNC=corr.noteChanges();
        }
    }
    const double hopS=det.inputHopLength(vt)/fs;
    // window classification per hop: onset (<100ms after a confirm) / sustain (>300ms into a voiced-and-no-confirm run)
    std::vector<int> sinceConf(osc.size(),1<<30);
    { int s=1<<30;
      for(size_t h=0;h<osc.size();++h){ if(confT[h])s=0; else if(s<(1<<30))++s; sinceConf[h]=s; } }
    auto med=[](std::vector<double> v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
    // raw |osc|
    std::vector<double> rawOn, rawSus;
    for(size_t h=0;h<osc.size();++h)
    { if(!voiced[h])continue;
      const double tMs=sinceConf[h]*hopS*1000.0;
      if(tMs<100) rawOn.push_back(osc[h]);
      else if(tMs>300&&tMs<100000) rawSus.push_back(osc[h]); }
    std::printf("raw |osc|: onset(0-100ms) med %.1fc   sustain(>300ms) med %.1fc\n",
        med(rawOn),med(rawSus));
    for(double tc : {50.0,150.0,300.0})
      for(int rst=0;rst<2;++rst)
      {
        double env=0; const double a=std::exp(-hopS*1000.0/tc);
        std::vector<double> eOn,eSus;
        for(size_t h=0;h<osc.size();++h)
        { if(confT[h]&&rst)env=0;
          if(voiced[h]) env=osc[h]+(env-osc[h])*a;
          const double tMs=sinceConf[h]*hopS*1000.0;
          if(!voiced[h])continue;
          if(tMs<100)eOn.push_back(env);
          else if(tMs>300&&tMs<100000)eSus.push_back(env); }
        std::printf("env tau %3.0fms %s: onset med %.1fc   sustain med %.1fc\n",
            tc,rst?"RESET at confirm":"no reset        ",med(eOn),med(eSus));
      }
    return 0;
}
