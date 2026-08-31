// pendstat: the FALSE-PENDING cost, measured directly (31 Aug 2026).
// Logs per hop {pending, slowCents, noteRef, noteChanges} under the SHIPPED
// envelope, then scores every conjunction threshold on the same
// trajectories: episodes/s, % reverted (never confirmed), and suspension
// seconds/s spent on episodes that never confirm - the cost itself.
#include "EedPitchEngine.h"
#include "EedPitchCorrect.h"
#include "EedPsolaEngine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
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
    const bool nat = argc>3 && !strcmp(argv[3],"nat");
    std::vector<float> in; double fs=0;
    if(argc<2||!readWavMono(argv[1],in,fs)){ std::printf("bad input\n"); return 1; }
    constexpr int vt = PitchEngine::kLowMale;
    PitchEngine det; det.prepare(fs,256); det.setVoiceType(vt); det.setTracking(PitchEngine::kNormal);
    const float hopMs = det.hopMs();
    PitchCorrect corr; corr.prepare(fs, det.inputHopLength(vt)); corr.initDegrees();
    static const int kMinor[7]={0,2,3,5,7,8,10};
    for(int s=0;s<12;++s) corr.setDegree(s,false,0);
    for(int k=0;k<7;++k) corr.setDegree(kMinor[k],true,0);
    corr.setKeyRoot(2); corr.setRetuneMs(tau);
    corr.setFlex(nat?55.0f:0.0f); corr.setHumanize(nat?60.0f:0.0f);
    corr.setIgnoreVibrato(true); corr.setNaturalVibrato(nat?100.0f:0.0f);
    corr.reset();
    F0JumpGate gate;
    float worst=PitchEngine::voiceRange(0).fMinHz;
    for(int t=1;t<PitchEngine::kNumVoiceTypes;++t) worst=std::min(worst,PitchEngine::voiceRange(t).fMinHz);
    PsolaEngine ring; ring.prepare(fs,256,PitchEngine::voiceRange(vt).fMinHz,worst);
    ring.setPitchLagSamples(det.pitchLagFor(vt));
    std::vector<float> scratch(256);
    PitchEngine::HopEvent ev[64];
    std::vector<int> pend, conf; std::vector<double> dep;   // per hop
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
            corr.process(g,ev[h].voiced,hopMs);
            pend.push_back(corr.debugPendingNow()?1:0);
            dep.push_back(std::fabs(corr.debugSlowTrack()-corr.debugNoteRef()));
            conf.push_back(corr.noteChanges()!=lastNC?1:0);
            lastNC=corr.noteChanges();
        }
    }
    const double hopS=det.inputHopLength(vt)/fs;
    const double durS=pend.size()*hopS;
    // Episodes: maximal pending runs; outcome = a confirm tick at/just after end.
    struct Ep { int a,b; bool confirmed; };
    std::vector<Ep> eps;
    for(size_t h=0;h<pend.size();)
    { if(!pend[h]){++h;continue;}
      size_t a=h; while(h<pend.size()&&pend[h])++h;
      bool c=false;
      for(size_t k=a;k<h+1&&k<pend.size();++k) if(conf[k]) c=true;
      if(h<pend.size()&&conf[h]) c=true;
      eps.push_back({(int)a,(int)h,c}); }
    int raised=(int)eps.size(), reverted=0;
    for(auto&e:eps) if(!e.confirmed)++reverted;
    std::printf("%s tau %.0f: pendings %.2f/s, reverted %d/%d (%.0f%%)\n",
        nat?"natural":"hard",tau,raised/durS,reverted,raised,100.0*reverted/std::max(1,raised));
    for(double TH : {0.0,10.0,20.0,30.0,40.0})
    {
        double susFalse=0, susTrue=0;
        for(auto&e:eps)
        { double s=0;
          for(int k=e.a;k<e.b;++k) if(dep[(size_t)k]>TH) s+=hopS;
          (e.confirmed?susTrue:susFalse)+=s; }
        std::printf("    TH=%2.0f: suspension %.3fs/s on FALSE pendings, %.3fs/s on real\n",
            TH,susFalse/durS,susTrue/durS);
    }
    return 0;
}
