import json,glob,os,sys
SP=os.path.dirname(os.path.abspath(__file__))
def draftroles(d):
    out={}
    for f in glob.glob(f'{SP}/{d}/phaseb/*/drafts/*.json'):
        cat=f.split('/')[-3]; j=json.load(open(f)); r={}
        for k in ['mix','decay','predelay','time','feedback','sync','attack','sustain','threshold','range']:
            v=j.get(k)
            if isinstance(v,dict): r[k]=v.get('control')
            elif k in j: r[k]=None
        nr=[n for n in j.get('notes',[]) if 'not_repeatable' in n]
        out[(cat,os.path.basename(f).split('.')[0], j.get('plugin',{}).get('name') if isinstance(j.get('plugin'),dict) else '')]=(r,nr)
    return out
for a,b in [(sys.argv[1],sys.argv[2]),(sys.argv[3],sys.argv[4])]:
    A,B=draftroles(a),draftroles(b); same=0
    for k in sorted(set(A)|set(B)):
        ra,rb=A.get(k,({},[])),B.get(k,({},[]))
        tag='SAME' if ra[0]==rb[0] else 'DIFF'; same+= tag=='SAME'
        print(f'{tag} {k[0]:9s} {k[2] or k[1]}')
        if tag=='DIFF': print(f'    before {ra[0]}\n    after  {rb[0]}')
        for n in rb[1]: print(f'    note: {n}')
    print(f'{a} vs {b}: {same} of {len(set(A)|set(B))} identical\n')
