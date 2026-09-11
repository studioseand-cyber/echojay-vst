// pitch_key_forensic (3 Sep 2026, round-18 ruling C step 1): recover the
// APPLIED key and tuning reference of a corrected bounce from its audio,
// because nothing on disk records what EchoJay Pitch's auto-key had frozen
// at bounce time (the key path never reads the transport; the feed is
// wall-clock; the displayed setting is not necessarily the applied one).
//
// Method: fine f0 track (production PitchEngine, 8192-sample blocks);
// SUSTAINED hops = runs >= 60ms where consecutive hops move < 4c. For those:
//   GRID OFFSET  median signed cents from the nearest A=440 semitone - a
//                hard-tuned bounce sits at the applied reference (439.1 Hz
//                reads -3.5c: the circular-reference defect's signature).
//   TIGHTNESS    median |offset| after removing the grid offset - hard-
//                tuned ~1-2c, natural/dry 10-20c.
//   OCCUPANCY    seconds of sustained time per pitch class at the recovered
//                grid; share OUTSIDE D natural minor; the best-fitting keys
//                of 24 by on-scale share. A bounce corrected to D minor has
//                ~zero out-of-scale mass; chromatic keeps the source's; a
//                wrong key shows a different best fit than the source's own.
// Limits, stated: enharmonic-equivalent scales (D minor / F major) are
// indistinguishable and harmless; a wrong key that shares most degrees
// shows as a small out-of-scale residue, not zero.
//
//   pitch_key_forensic <vt> <file.wav>...
//   Build: g++ -std=c++17 -O2 -ISource tools/pitch_key_forensic/main.cpp -o kf
#include "EedPitchEngine.h"
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
static long alignLag (const std::vector<float>& ref, const std::vector<float>& x)
{
    const int dec=16; std::vector<double> a,b;
    for(size_t i=0;i+dec<=ref.size();i+=dec){ double s=0; for(int k=0;k<dec;++k)s+=std::fabs(ref[i+k]); a.push_back(s); }
    for(size_t i=0;i+dec<=x.size();i+=dec){ double s=0; for(int k=0;k<dec;++k)s+=std::fabs(x[i+k]); b.push_back(s); }
    const long span=4000/dec; long best=0; double bestV=-1;
    for(long L=-span;L<=span;++L){ double s=0; long n=0; for(long i=0;i<(long)a.size();++i){ const long j=i+L; if(j<0||j>=(long)b.size()) continue; s+=a[(size_t)i]*b[(size_t)j]; ++n; } if(n>0&&s>bestV){ bestV=s; best=L; } }
    long coarse=best*dec, fine=coarse; bestV=-1;
    for(long L=coarse-64;L<=coarse+64;++L){ double s=0; for(size_t i=0;i<ref.size();i+=4){ const long j=(long)i+L; if(j<0||j>=(long)x.size()) continue; s+=(double)ref[i]*(double)x[(size_t)j]; } if(s>bestV){ bestV=s; fine=L; } }
    return fine;
}
static const char* PC[12]={"C","C#","D","Eb","E","F","F#","G","G#","A","Bb","B"};
static double pct(std::vector<double> v,double q){ if(v.empty())return 0; std::sort(v.begin(),v.end()); return v[std::min(v.size()-1,(size_t)(q*v.size()))]; }
int main(int argc,char**argv)
{
    if(argc<3){ std::printf("usage: %s <vt> <file.wav>...\n",argv[0]); return 1; }
    const int vt=atoi(argv[1]);
    // optional source for improve-rate and onset windows (KF_SOURCE=<wav>)
    std::vector<float> srcX; double srcFs=0; std::vector<double> srcC; std::vector<double> words; int srcHop=0;
    if(getenv("KF_SOURCE")&&readWavMono(getenv("KF_SOURCE"),srcX,srcFs))
    { PitchEngine e; e.prepare(srcFs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal); srcHop=e.inputHopLength(vt);
      for(size_t p=0;p+(size_t)srcHop<=srcX.size();p+=(size_t)srcHop){ e.process(srcX.data()+p,nullptr,srcHop); const PitchReading r=e.getReading(); srcC.push_back(r.voiced&&r.f0Hz>0?1200.0*std::log2(r.f0Hz/440.0):NAN); }
      int uv=1000; const double hs=srcHop/srcFs;
      for(size_t h=0;h<srcC.size();++h){ if(std::isnan(srcC[h])){++uv;continue;} if(uv*hs>=0.06) words.push_back((double)h*hs); uv=0; } }
    static const int major[7]={0,2,4,5,7,9,11}, minor[7]={0,2,3,5,7,8,10};
    for(int a=2;a<argc;++a)
    {
        std::vector<float> x; double fs=0;
        if(!readWavMono(argv[a],x,fs)){ std::printf("%s unreadable\n",argv[a]); continue; }
        PitchEngine e; e.prepare(fs,8192); e.setVoiceType(vt); e.setTracking(PitchEngine::kNormal);
        const int hop=e.inputHopLength(vt); const double hopS=hop/fs;
        std::vector<double> c;   // cents from A440 per hop, 0 = unvoiced (use NAN)
        for(size_t p=0;p+(size_t)hop<=x.size();p+=(size_t)hop)
        { e.process(x.data()+p,nullptr,hop); const PitchReading r=e.getReading();
          c.push_back(r.voiced&&r.f0Hz>0?1200.0*std::log2(r.f0Hz/440.0):NAN); }
        // sustained runs
        std::vector<char> sus(c.size(),0);
        { size_t s=0;
          for(size_t h=1;h<=c.size();++h)
          { const bool cont=h<c.size()&&!std::isnan(c[h])&&!std::isnan(c[h-1])&&std::fabs(c[h]-c[h-1])<4.0;
            if(!cont){ if(h-s>=(size_t)std::lround(0.060/hopS)&&!std::isnan(c[s])) for(size_t k=s;k<h;++k) sus[k]=1; s=h; } } }
        std::vector<double> off; double voicedS=0, susS=0;
        for(size_t h=0;h<c.size();++h){ if(std::isnan(c[h])) continue; voicedS+=hopS; if(!sus[h]) continue; susS+=hopS;
            const double r=std::fmod(std::fmod(c[h],100.0)+150.0,100.0)-50.0; off.push_back(r); }
        const double grid=pct(off,0.5);
        std::vector<double> tight; for(double o:off) { double d=o-grid; if(d>50)d-=100; if(d<-50)d+=100; tight.push_back(std::fabs(d)); }
        double occ[12]={0};
        for(size_t h=0;h<c.size();++h){ if(std::isnan(c[h])||!sus[h]) continue;
            const long semi=std::lround((c[h]-grid)/100.0); const int pc=(int)(((semi+9)%12+12)%12); occ[pc]+=hopS; }
        double best[24]; for(int k=0;k<24;++k){ double s=0; for(int d=0;d<7;++d) s+=occ[((k<12?major[d]:minor[d])+k%12)%12]; best[k]=s/std::max(1e-9,susS); }
        int order[24]; for(int i=0;i<24;++i) order[i]=i; std::sort(order,order+24,[&](int p,int q){ return best[p]>best[q]; });
        double dmin=0; for(int d=0;d<7;++d) dmin+=occ[(minor[d]+2)%12];
        std::string nm=argv[a]; const size_t sl=nm.rfind('/'); if(sl!=std::string::npos) nm=nm.substr(sl+1);
        std::printf("\n%s  voiced %.1fs  sustained %.1fs\n",nm.c_str(),voicedS,susS);
        std::printf("  GRID OFFSET (median signed cents from A440 semitones): %+.2fc  => reference %.2f Hz\n",grid,440.0*std::pow(2.0,grid/1200.0));
        std::printf("  TIGHTNESS |offset| after grid removal: median %.2fc  p75 %.2fc  (hard-tuned ~1-2c, dry 10-20c)\n",pct(tight,0.5),pct(tight,0.75));
        std::printf("  OCCUPANCY (s):"); for(int p=0;p<12;++p) std::printf(" %s %.2f",PC[p],occ[p]); std::printf("\n");
        std::printf("  IN D NATURAL MINOR: %.1f%%   OUTSIDE: %.1f%%  (",100.0*dmin/std::max(1e-9,susS),100.0*(1.0-dmin/std::max(1e-9,susS)));
        for(int p=0;p<12;++p){ bool in=false; for(int d=0;d<7;++d) if((minor[d]+2)%12==p) in=true; if(!in&&occ[p]>0.005) std::printf(" %s %.2fs",PC[p],occ[p]); } std::printf(" )\n");
        // ---- OFF-GRID TO D NATURAL MINOR at 440 and at the recovered reference ----
        auto offgrid=[&](double refShiftC, std::vector<double>& onset, std::vector<double>& allv, double& under5, double& improve)
        {
            onset.clear(); allv.clear(); long n5=0, nImp=0, nCmp=0;
            const long lag=srcX.empty()?0:alignLag(srcX,x);
            for(size_t h=0;h<c.size();++h)
            { if(std::isnan(c[h])) continue;
              const double cc=c[h]-refShiftC;   // cents from the reference grid's A
              // nearest D-natural-minor degree: pitch class of the semitone, then snap to scale
              const double semi=cc/100.0; const long s0=std::lround(semi);
              double bestD=1e9; for(long k=s0-2;k<=s0+2;++k){ const int pc=(int)(((k+9)%12+12)%12); bool in=false; for(int d=0;d<7;++d) if((minor[d]+2)%12==pc) in=true; if(in) bestD=std::min(bestD,std::fabs(cc-100.0*k)); }
              allv.push_back(bestD); if(bestD<5) ++n5;
              const double t=(double)h*hopS; bool ws=false; for(double w:words) if(t>=w&&t<w+0.150){ ws=true; break; }
              if(ws) onset.push_back(bestD);
              if(!srcC.empty()){ const long j=(long)std::llround(((double)h*hop-(double)lag)/srcHop); if(j>=0&&j<(long)srcC.size()&&!std::isnan(srcC[(size_t)j]))
                { const double sc=srcC[(size_t)j]-refShiftC; const long t0=std::lround(sc/100.0); double bs=1e9; for(long k=t0-2;k<=t0+2;++k){ const int pc=(int)(((k+9)%12+12)%12); bool in=false; for(int d=0;d<7;++d) if((minor[d]+2)%12==pc) in=true; if(in) bs=std::min(bs,std::fabs(sc-100.0*k)); }
                  ++nCmp; if(bestD<bs) ++nImp; } } }
            under5=allv.empty()?0:100.0*n5/(double)allv.size(); improve=nCmp?100.0*nImp/(double)nCmp:-1;
        };
        for(int pass=0;pass<2;++pass)
        { std::vector<double> on,al; double u5,imp; const double sh=pass?grid:0.0; offgrid(sh,on,al,u5,imp);
          std::printf("  OFF-GRID vs D minor @ %s (%.2f Hz): onset(150ms) med %5.2f p75 %5.2f p90 %5.2f | all-voiced med %5.2f <5c %4.1f%%",
              pass?"RECOVERED ref":"440          ",440.0*std::pow(2.0,sh/1200.0),pct(on,0.5),pct(on,0.75),pct(on,0.9),pct(al,0.5),u5);
          if(imp>=0) std::printf(" | improve-rate vs source %4.1f%%",imp); std::printf("  (n onset %zu, all %zu)\n",on.size(),al.size()); }
        // NOTE-CENTRE off-grid (vibrato-neutral): per voiced run >= 200 ms, the run's MEAN cents -> its off-grid to D minor; duration-weighted median over runs
        { std::vector<std::pair<double,double>> runs; size_t s0=0;
          for(size_t h=1;h<=c.size();++h)
          { const bool cont=h<c.size()&&!std::isnan(c[h])&&!std::isnan(c[h-1])&&std::fabs(c[h]-c[h-1])<12.0;
            if(!cont){ if(!std::isnan(c[s0])&&(h-s0)*hopS>=0.200){ double m=0; for(size_t k=s0;k<h;++k) m+=c[k]; m/=(double)(h-s0);
                const long t0=std::lround(m/100.0); double bd=1e9; for(long k=t0-2;k<=t0+2;++k){ const int pc=(int)(((k+9)%12+12)%12); for(int d=0;d<7;++d) if((minor[d]+2)%12==pc) bd=std::min(bd,std::fabs(m-100.0*k)); }
                runs.push_back({bd,(double)(h-s0)*hopS}); } s0=h; } }
          std::sort(runs.begin(),runs.end()); double tot=0; for(auto& r:runs) tot+=r.second; double acc=0, med=0; for(auto& r:runs){ acc+=r.second; if(acc>=tot/2){ med=r.first; break; } }
          std::printf("  NOTE-CENTRE off-grid vs D minor (runs >= 200ms, mean pitch per run, duration-weighted median): %.2fc over %zu runs / %.1fs\n",med,runs.size(),tot); }
        std::printf("  BEST-FIT KEYS:"); for(int i=0;i<4;++i){ const int k=order[i]; std::printf("  %s %s %.1f%%",PC[k%12],k<12?"maj":"min",100.0*best[k]); } std::printf("\n");
    }
    return 0;
}
