// perhist (2 Sep 2026): waveform-level period-length statistics over
// matched sustain windows. NO pitch tracker in the measurement loop -
// the engine is used ONLY to pick the windows (from the SOURCE track,
// same windows applied to all three files). Per window: band-pass the
// signal around the window's median f0 (2x biquad, Q=5), take rising
// zero-crossing intervals as period lengths, express each period as
// cents vs the window median period, pool the deviations.
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
struct Win { size_t s,e; double f0; };
int main (int argc, char** argv)
{
    std::vector<float> src; double fs=0;
    if(argc<2||!readWavMono(argv[1],src,fs)){std::printf("bad source\n");return 1;}
    // windows from the SOURCE track only
    PitchEngine eng; eng.prepare(fs,8192); eng.setVoiceType(1); eng.setTracking(PitchEngine::kNormal);
    const int hop=eng.inputHopLength(1); const double hopS=hop/fs;
    std::vector<double> t;
    for(size_t p=0;p+(size_t)hop<=src.size();p+=(size_t)hop)
    { eng.process(src.data()+p,nullptr,hop);
      const PitchReading r=eng.getReading();
      t.push_back(r.voiced&&r.f0Hz>0?(double)r.f0Hz:0.0); }
    // sustain windows: contiguous voiced hops from +300ms into a run to its end,
    // chopped into 250ms windows with a stable median f0
    std::vector<Win> wins;
    size_t h=0;
    while(h<t.size())
    { if(t[h]<=0){++h;continue;}
      size_t r0=h; while(h<t.size()&&t[h]>0)++h; size_t r1=h; // [r0,r1) voiced run
      size_t st=r0+(size_t)std::ceil(0.15/hopS);
      const size_t wlen=(size_t)std::llround(0.08/hopS);
      for(size_t w=st;w+wlen<=r1;w+=wlen)
      { std::vector<double> f(t.begin()+(long)w,t.begin()+(long)(w+wlen));
        std::sort(f.begin(),f.end());
        wins.push_back({(size_t)((double)w*hop),(size_t)((double)(w+wlen)*hop),f[f.size()/2]}); } }
    std::printf("windows: %zu (80ms sustains, +150ms in,, source-picked, applied to all files)\n",wins.size());
    for(int a=1;a<argc;++a)
    { std::vector<float> x; double fx=0;
      if(!readWavMono(argv[a],x,fx)){std::printf("%s: unreadable\n",argv[a]);continue;}
      std::vector<double> dev; // per-period deviation from window median, cents
      long nper=0;
      for(const Win& w:wins)
      { if(w.e>=x.size()) continue;
        // 2x biquad band-pass at w.f0, Q=5
        const double w0=2.0*M_PI*w.f0/fx, Q=5.0, al=std::sin(w0)/(2.0*Q);
        const double b0=al, b2=-al, a0=1.0+al, a1=-2.0*std::cos(w0), a2=1.0-al;
        std::vector<double> y(x.begin()+(long)w.s,x.begin()+(long)w.e);
        for(int pass=0;pass<2;++pass)
        { double x1=0,x2=0,y1=0,y2=0;
          for(double& v:y)
          { const double xn=v, yn=(b0*xn+b2*x2-a1*y1-a2*y2)/a0;
            x2=x1;x1=xn;y2=y1;y1=yn;v=yn; } }
        // rising zero crossings, sub-sample interpolated
        std::vector<double> zc;
        for(size_t i=1;i<y.size();++i)
          if(y[i-1]<=0.0&&y[i]>0.0)
            zc.push_back((double)(i-1)+(-y[i-1])/(y[i]-y[i-1]));
        // skip filter warm-up: drop first 3 crossings
        std::vector<double> per;
        for(size_t i=4;i<zc.size();++i)
        { const double p=zc[i]-zc[i-1];
          const double nom=fx/w.f0;
          if(p>0.5*nom&&p<2.0*nom) per.push_back(p); } // octave guard
        if(per.size()<6) continue;
        std::vector<double> ps=per; std::sort(ps.begin(),ps.end());
        const double med=ps[ps.size()/2];
        for(double p:per){ dev.push_back(1200.0*std::log2(p/med)); ++nper; }
      }
      std::sort(dev.begin(),dev.end());
      auto q=[&](double f){ return dev.empty()?0.0:dev[(size_t)std::min((double)dev.size()-1,f*(double)dev.size())]; };
      double s=0,s2=0; for(double d:dev){s+=d;s2+=d*d;}
      const double sd=dev.empty()?0:std::sqrt(std::max(0.0,s2/(double)dev.size()-(s/(double)dev.size())*(s/(double)dev.size())));
      // histogram: share of periods within +-5c / +-15c / beyond 30c of window median
      long in5=0,in15=0,out30=0;
      for(double d:dev){const double a2c=std::fabs(d); if(a2c<=5)++in5; if(a2c<=15)++in15; if(a2c>30)++out30;}
      std::printf("%-70s periods %6ld | IQR %6.1fc | std %6.1fc | within5c %4.1f%% | within15c %4.1f%% | beyond30c %4.1f%%\n",
        argv[a],nper,q(0.75)-q(0.25),sd,
        100.0*in5/std::max(1L,nper),100.0*in15/std::max(1L,nper),100.0*out30/std::max(1L,nper));
    }
    return 0;
}
