import numpy as np

def cmnd_matrix(x, sr, W=2048, hop=128, fmin=80.0, fmax=900.0):
    """YIN cumulative-mean-normalised difference for every frame, no per-frame decision."""
    tmin=max(2,int(sr/fmax)); tmax=min(W-1,int(sr/fmin))
    starts=np.arange(0, len(x)-W-tmax, hop)
    cs=np.concatenate(([0.0],np.cumsum(x.astype(np.float64)**2)))
    nfft=1<<int(np.ceil(np.log2((W+tmax)*2)))
    taus=np.arange(tmax+1)
    M=np.ones((len(starts),tmax+1)); rms=np.zeros(len(starts))
    for i,s in enumerate(starts):
        fr=x[s:s+W+tmax]
        p0=cs[s+W]-cs[s]; rms[i]=np.sqrt(p0/W)
        if rms[i]<1e-4: continue
        F=np.fft.rfft(fr,nfft)
        ac=np.fft.irfft(F*np.conj(F),nfft)[:tmax+1]
        pt=cs[s+W+taus]-cs[s+taus]
        d=p0+pt-2*ac
        run=np.cumsum(d[1:]); cm=np.ones(tmax+1); nz=run>0
        cm[1:][nz]=d[1:][nz]*taus[1:][nz]/run[nz]
        M[i]=cm
    return M,starts,rms,tmin,tmax

def viterbi(M,starts,rms,tmin,tmax,sr,lam=1.6,rms_gate=3e-3):
    """DP over tau with a cost on log-frequency jumps. Returns f0 per frame (0 = unvoiced)."""
    T=M.shape[0]; taus=np.arange(tmin,tmax)
    lt=np.log2(taus.astype(float))
    obs=M[:,tmin:tmax]
    voiced=(rms>rms_gate)&(obs.min(axis=1)<0.6)
    D=np.full((T,len(taus)),np.inf); B=np.zeros((T,len(taus)),dtype=np.int32)
    prev=None
    for i in range(T):
        if not voiced[i]:
            prev=None; continue
        if prev is None:
            D[i]=obs[i]
        else:
            # transition cost lam*|dlog2 tau|, computed with a banded search for speed
            c=D[prev][None,:]+lam*np.abs(lt[:,None]-lt[None,:])
            j=np.argmin(c,axis=1); B[i]=j
            D[i]=c[np.arange(len(taus)),j]+obs[i]
        prev=i
    f0=np.zeros(T)
    # backtrack over each voiced run
    i=T-1
    while i>=0:
        if not voiced[i]: i-=1; continue
        j=i
        while j-1>=0 and voiced[j-1]: j-=1
        k=int(np.argmin(D[i]))
        for m in range(i,j-1,-1):
            tau=taus[k]
            # parabolic refine
            tf=float(tau)
            if tmin+1<=tau<tmax-1:
                a_,b_,c_=M[m,tau-1],M[m,tau],M[m,tau+1]; den=a_-2*b_+c_
                if den!=0: tf=tau+0.5*(a_-c_)/den
            f0[m]=sr/tf
            k=B[m,k] if m>j else k
        i=j-1
    return f0,voiced
