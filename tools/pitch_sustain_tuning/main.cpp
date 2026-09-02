// sustune (2 Sep 2026): tuning metrics on SUSTAINS ONLY (>300ms into
// source-voiced runs) - separating tuning from jitter. D minor, 440.
#include "EedPitchEngine.h"
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
static std::vector<double> track (const std::vector<float>& x, double fs, int vt, int& hop)
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
static double offGrid (double f0)
{
    static const int pcs[7]={2,4,5,7,9,10,0};
    const double midi=69.0+12.0*std::log2(f0/440.0);
    double best=1e9;
    for(int k=-1;k<=1;++k) for(int j=0;j<7;++j)
    { const double note=12.0*std::floor(midi/12.0+k)+pcs[j];
      best=std::min(best,std::fabs(midi-note)*100.0); }
    return best;
}
int main (int argc, char** argv)
{
    // argv: source antares echojay
    std::vector<float> s,a,e; double fs=0,f2=0,f3=0;
    if(argc<4||!readWavMono(argv[1],s,fs)||!readWavMono(argv[2],a,f2)||!readWavMono(argv[3],e,f3))
    { std::printf("bad inputs\n"); return 1; }
    int hop=0;
    auto ts=track(s,fs,1,hop), ta=track(a,fs,1,hop), te=track(e,fs,1,hop);
    const double hopS=hop/fs;
    // sustain hops: >300ms into source-voiced runs
    std::vector<size_t> sus;
    { int run=0;
      for(size_t h=0;h<ts.size();++h)
      { if(ts[h]<=0){run=0;continue;} ++run;
        if(run*hopS>0.30) sus.push_back(h); } }
    auto semi=[](double f){ return (long)std::lround(69.0+12.0*std::log2(f/440.0)); };
    struct M { std::vector<double> og; int inS=0,n=0,imp=0,impN=0,same=0,sameN=0; };
    auto meas=[&](std::vector<double>& T)->M
    { M m;
      static const int pcs[7]={2,4,5,7,9,10,0};
      for(size_t h:sus)
      { if(h>=T.size()||T[h]<=0) continue;
        ++m.n; m.og.push_back(offGrid(T[h]));
        const int pc=(int)((semi(T[h])%12+12)%12);
        for(int j=0;j<7;++j) if(pc==pcs[j]){++m.inS;break;}
        if(ts[h]>0){ ++m.impN; if(offGrid(T[h])<offGrid(ts[h]))++m.imp; }
        if(h<ta.size()&&ta[h]>0){ ++m.sameN; if(semi(T[h])==semi(ta[h]))++m.same; } }
      return m; };
    auto pr=[&](const char* nm, std::vector<double>& T)
    { M m=meas(T);
      std::sort(m.og.begin(),m.og.end());
      std::printf("%-8s sustains n=%d | off-grid med %5.1fc | in-scale %5.1f%% | improve %5.1f%% | same-semi(vs Antares) %5.1f%%\n",
        nm,m.n,m.og.empty()?0:m.og[m.og.size()/2],
        100.0*m.inS/std::max(1,m.n),100.0*m.imp/std::max(1,m.impN),
        100.0*m.same/std::max(1,m.sameN)); };
    pr("SOURCE",ts); pr("ANTARES",ta); pr("ECHOJAY",te);
    return 0;
}
