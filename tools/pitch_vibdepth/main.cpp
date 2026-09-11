// vibdepth: does vibrato SURVIVE? (30 Aug 2026 acceptance.) On sustained
// notes of the SOURCE, the output's 4-8Hz cents-modulation depth as a
// ratio of the source's, plus dominant modulation rate both sides.
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
static std::vector<float> track (const std::vector<float>& x, double fs, int& hop)
{
    PitchEngine e; e.prepare(fs,8192); e.setVoiceType(PitchEngine::kLowMale); e.setTracking(PitchEngine::kNormal);
    hop=e.inputHopLength(PitchEngine::kLowMale);
    std::vector<float> t;
    for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop)
    { e.process(x.data()+p,nullptr,hop);
      const PitchReading r=e.getReading();
      t.push_back(r.voiced&&r.f0Hz>0? (float)(1200.0*std::log2((double)r.f0Hz/440.0)) : -1e9f); }
    return t;
}
// band metric on a cents series: RMS depth (as peak-to-peak equivalent
// 2*sqrt(2)*rms) of the 4-8Hz band, and the dominant bin in 3-9Hz.
static void band (const std::vector<double>& x, double sr, double& ppc, double& rate)
{
    const int N=(int)x.size();
    double m=0; for(double v:x)m+=v; m/=std::max(1,N);
    double best=0; rate=0; double p48=0; int n48=0;
    for(double f=3.0; f<=9.0; f+=0.25)
    {
        double re=0,im=0;
        for(int i=0;i<N;++i){ const double w=2*M_PI*f*i/sr; re+=(x[(size_t)i]-m)*std::cos(w); im-=(x[(size_t)i]-m)*std::sin(w); }
        const double p=(re*re+im*im)/(N*(double)N)*4.0;   // amplitude^2 of that component
        if(f>=4.0&&f<=8.0){ p48+=p; ++n48; }
        if(p>best){ best=p; rate=f; }
    }
    ppc=2.0*std::sqrt(p48);   // ~peak-to-peak of band content
}
int main (int argc, char** argv)
{
    // argv: source render
    std::vector<float> s,r; double fs=0,f2=0;
    if(argc<3||!readWavMono(argv[1],s,fs)||!readWavMono(argv[2],r,f2)){ std::printf("bad inputs\n"); return 1; }
    int hop=0;
    auto ts=track(s,fs,hop), tr=track(r,fs,hop);
    const double sr=fs/hop;
    // sustained notes of the SOURCE: voiced >=600ms, range < 150c
    std::vector<double> ratios, srcR, outR, srcD;
    size_t h=0;
    while(h<ts.size())
    {
        while(h<ts.size()&&ts[h]<-1e8)++h;
        size_t h0=h; float lo=1e9f,hi=-1e9f;
        while(h<ts.size()&&ts[h]>-1e8){ lo=std::min(lo,ts[h]); hi=std::max(hi,ts[h]); ++h; }
        if((h-h0)*1.0/sr<0.6||hi-lo>150.0f) continue;
        std::vector<double> xs,xr;
        for(size_t k=h0;k<h&&k<tr.size();++k){ if(tr[k]<-1e8) goto next; xs.push_back(ts[k]); xr.push_back(tr[k]); }
        {
            double ps,rs,pr,rr;
            band(xs,sr,ps,rs); band(xr,sr,pr,rr);
            if(ps>=6.0)   // genuine vibrato present (>=6c p-p)
            { ratios.push_back(pr/ps); srcR.push_back(rs); outR.push_back(rr); srcD.push_back(ps); }
        }
        next:;
    }
    auto med=[](std::vector<double>&v){ if(v.empty())return 0.0; std::sort(v.begin(),v.end()); return v[v.size()/2]; };
    std::printf("%s: notes %d | depth ratio med %.2f | rate src %.2fHz out %.2fHz | src depth med %.1fc\n",
        argv[2],(int)ratios.size(),med(ratios),med(srcR),med(outR),med(srcD));
    return 0;
}
