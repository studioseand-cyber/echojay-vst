// vibvote: WHERE do vib-on's wrong-semitone frames live? (30 Aug 2026,
// DEFECT_VIBRATO_ON_TUNING_COST measurement round.) Wrong = echojay and
// antares both voiced, different nearest semitone. Location = time since
// the last RE-SEED EVENT: a voicing resume after a >=200ms source gap, or
// an Antares note change (the corrector re-seeds slowCents_ from one
// sample at both). If wrong frames concentrate in the first ~300ms after
// re-seeds for vib-on but not vib-off, the onset-seeding variant of the
// fragmentation-coat hypothesis is confirmed.
#include "EedPitchEngine.h"
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
struct Trk { std::vector<float> f0; int hop=0; };
static Trk track (const std::vector<float>& x, double fs)
{
    PitchEngine e; e.prepare(fs,8192); e.setVoiceType(PitchEngine::kLowMale); e.setTracking(PitchEngine::kNormal);
    Trk t; t.hop=e.inputHopLength(PitchEngine::kLowMale);
    for(size_t p=0;p+(size_t)t.hop<=x.size();p+=(size_t)t.hop)
    { e.process(x.data()+p,nullptr,t.hop);
      const PitchReading r=e.getReading();
      t.f0.push_back(r.voiced?r.f0Hz:0.0f); }
    return t;
}
int main (int argc, char** argv)
{
    // argv: source antares echojay
    std::vector<float> s,a,e; double fs=0,f2=0,f3=0;
    if(argc<4||!readWavMono(argv[1],s,fs)||!readWavMono(argv[2],a,f2)||!readWavMono(argv[3],e,f3))
    { std::printf("bad inputs\n"); return 1; }
    Trk ts=track(s,fs), ta=track(a,fs), te=track(e,fs);
    const double hopS=ts.hop/fs;
    auto semi=[&](float f){ return (int)std::lround(69.0+12.0*std::log2((double)f/440.0)); };
    // re-seed events: resume after >=200ms source gap; antares note change
    std::vector<int> reseed;
    { int gap=0; bool wasV=false; int lastSemi=-1000;
      for(size_t h=0;h<ts.f0.size();++h)
      { const bool v=ts.f0[h]>0;
        if(!v){ ++gap; wasV=false; if(h<ta.f0.size()&&ta.f0[h]<=0) lastSemi=-1000; continue; }
        if(!wasV && gap*hopS>=0.2) reseed.push_back((int)h);
        wasV=true; gap=0;
        if(h<ta.f0.size()&&ta.f0[h]>0)
        { const int sm=semi(ta.f0[h]);
          if(lastSemi!=-1000&&sm!=lastSemi) reseed.push_back((int)h);
          lastSemi=sm; } } }
    int wrongNear=0,nearN=0,wrongFar=0,farN=0;
    for(size_t h=0;h<te.f0.size()&&h<ta.f0.size();++h)
    { if(te.f0[h]<=0||ta.f0[h]<=0) continue;
      double d=1e9;
      for(int rs:reseed){ const double dt=((double)h-rs)*hopS; if(dt>=0&&dt<d) d=dt; }
      const bool near_=d<0.3;
      const bool wrong=semi(te.f0[h])!=semi(ta.f0[h]);
      if(near_){++nearN; if(wrong)++wrongNear;} else {++farN; if(wrong)++wrongFar;} }
    std::printf("%s: reseeds %d | within 300ms of reseed: wrong %d/%d (%.1f%%) | elsewhere: wrong %d/%d (%.1f%%)\n",
        argv[3],(int)reseed.size(),wrongNear,nearN,100.0*wrongNear/std::max(1,nearN),
        wrongFar,farN,100.0*wrongFar/std::max(1,farN));
    return 0;
}
