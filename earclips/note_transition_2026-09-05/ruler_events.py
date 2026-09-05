import numpy as np
e=np.load('/home/claude/pitch/dbit_err.npz')
v=np.load('/home/claude/pitch/dbit_vt2.npz')
t=e['t']; vs=e['vs']; hop=128; sr=48000
err={k:e['err_'+k] for k in ('src','ant','ecj')}
C={k:1200*np.log2(np.maximum(v['f0_'+k],1e-9)/440.0) for k in ('src','ant','ecj')}

# honest recount: drop octave-scale outliers (|err|>250c) as tracker/octave artefacts, report them separately
for k in ('ant','ecj'):
    a=err[k][vs]; oct_=np.abs(a)>250
    b=a[~oct_]
    print('%s: octave-scale frames %d  |  excluding them: >50c %d  >80c %d  >100c %d  p99 %.1fc'%(
        k.upper(),int(oct_.sum()),int((np.abs(b)>50).sum()),int((np.abs(b)>80).sum()),
        int((np.abs(b)>100).sum()),np.percentile(np.abs(b),99)))

# where are ecj's wrong-note events relative to source note transitions?
tgt=e['tgt']
trans=np.zeros(len(t),bool)
tt=tgt.copy(); tt[~vs]=np.nan
ch=np.where(np.abs(np.diff(np.nan_to_num(tt,nan=0)))>=50)[0]
for i in ch: trans[max(0,i-1):i+2]=True
print('\nsource note transitions found:',len(ch))
def events(k,thr=40.0,minms=25.0):
    bad=vs&(np.abs(err[k])>thr)&(np.abs(err[k])>np.abs(err['src']))&(np.abs(err[k])<250)
    idx=np.where(bad)[0]; out=[]
    if not len(idx): return out
    s=p=idx[0]
    for i in idx[1:]:
        if i-p>3:
            if (p-s)*hop/sr*1000>=minms: out.append((s,p))
            s=i
        p=i
    if (p-s)*hop/sr*1000>=minms: out.append((s,p))
    return out
for k in ('ant','ecj'):
    ev=events(k)
    near=0
    for a,b in ev:
        # is there a source note transition within 120 ms of the event?
        w=int(0.120*sr/hop)
        if trans[max(0,a-w):min(len(t),b+w)].any(): near+=1
    print('%s: %d sub-octave wrong-note events >=25ms, total %.0f ms; %d of %d within 120 ms of a source note change'%(
        k.upper(),len(ev),sum((b-a)*hop/sr*1000 for a,b in ev),near,len(ev)))
    for a,b in sorted(ev,key=lambda r:-(r[1]-r[0])):
        pk=np.nanargmax(np.abs(err[k][a:b+1]))+a
        w=int(0.120*sr/hop)
        n='TRANSITION' if trans[max(0,a-w):min(len(t),b+w)].any() else 'sustain'
        print('   t %.3f-%.3f (%3.0f ms) peak %+7.1fc  src note %+.0f -> out note %+.0f   [%s]'%(
            t[a],t[b],(b-a)*hop/sr*1000,err[k][pk],tgt[pk]/100,np.round(C[k][pk]/100),n))
