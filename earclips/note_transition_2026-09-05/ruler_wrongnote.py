import numpy as np
v=np.load('/home/claude/pitch/dbit_vt2.npz')
sr=48000;hop=128
t=v['t']; vs=v['vs']
f0={k:v['f0_'+k] for k in ('src','ant','ecj')}
C={k:1200*np.log2(np.maximum(f0[k],1e-9)/440.0) for k in f0}

# which notes does each output actually land on? (loud, stable frames)
stable=vs.copy()
d1=np.abs(np.diff(C['src'],prepend=C['src'][0]))
stable&= (d1<3)
print('note-landing histogram, stable voiced frames (semitone index rel A4):')
for k in ('src','ant','ecj'):
    n=np.round(C[k][stable]/100).astype(int)
    u,c=np.unique(n,return_counts=True)
    o=np.argsort(-c)[:10]
    print(' ',k,' '.join('%d:%d'%(u[i],c[i]) for i in o))

# WRONG-NOTE INSTRUMENT: error relative to the note the SOURCE is actually on
tgt=100*np.round(C['src']/100)          # nearest semitone to the source
err={}
for k in ('src','ant','ecj'):
    e=np.full(len(t),np.nan); e[vs]=C[k][vs]-tgt[vs]; err[k]=e
print('\n|error vs the note the SOURCE is on| (cents), voiced frames:')
for k in ('src','ant','ecj'):
    a=np.abs(err[k][vs])
    print('  %s  med %5.2f  p90 %5.1f  p99 %5.1f  | >50c %d  >80c %d'%(
        k,np.median(a),np.percentile(a,90),np.percentile(a,99),int((a>50).sum()),int((a>80).sum())))

# event-level: contiguous runs where output is >40c from the source's own note AND worse than source
def events(k,thr=40.0,minms=25.0):
    bad=vs & (np.abs(err[k])>thr) & (np.abs(err[k])>np.abs(err['src']))
    idx=np.where(bad)[0]; out=[]
    if not len(idx): return out
    s=idx[0]; p=idx[0]
    for i in idx[1:]:
        if i-p>3:
            if (p-s)*hop/sr*1000>=minms: out.append((s,p))
            s=i
        p=i
    if (p-s)*hop/sr*1000>=minms: out.append((s,p))
    return out
for k in ('ant','ecj'):
    ev=events(k)
    tot=sum((b-a)*hop/sr*1000 for a,b in ev)
    print('\n%s: %d wrong-note events >=25 ms, total %.0f ms'%(k.upper(),len(ev),tot))
    for a,b in sorted(ev,key=lambda r:-(r[1]-r[0]))[:8]:
        pk=np.nanargmax(np.abs(err[k][a:b+1]))+a
        print('   t %.3f-%.3f (%3.0f ms)  peak %+6.1fc   source at that instant %+5.1fc off its own note'%(
            t[a],t[b],(b-a)*hop/sr*1000,err[k][pk],err['src'][pk]))
np.savez('/home/claude/pitch/dbit_err.npz',t=t,vs=vs,err_src=err['src'],err_ant=err['ant'],err_ecj=err['ecj'],tgt=tgt)
