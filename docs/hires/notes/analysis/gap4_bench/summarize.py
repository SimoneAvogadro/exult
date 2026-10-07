import re,sys,collections
G='/home/simonea/ultima7_exult/tmp/gap4/results/'
def parse(tag):
    rows=[]; scenes={}; other=collections.defaultdict(dict)
    for line in open(G+tag+'.txt'):
        p=line.split()
        if not p: continue
        if p[0]=='SCENE':
            d=dict(x.split('=',1) for x in p[3:] if '=' in x); scenes[p[2]]=d
        elif p[0]=='PAINT':
            d=dict(x.split('=',1) for x in p[3:] if '=' in x); d['scene']=p[2]; rows.append(d)
        elif p[0] in ('DIRTYALL','STRIP','SHIFTCOPY','NPCRECT','LERP','SHOW'):
            d=dict(x.split('=',1) for x in p[3:] if '=' in x); other[p[2]][p[0]]=d
    return scenes,rows,other
tags=sys.argv[1:]
order=['start','brit_st','brit_mkt','castle','throne','forest','dung','brit_xlu']
for tag in tags:
    scenes,rows,other=parse(tag)
    print('==',tag)
    print('%-9s %5s %5s %4s %6s %7s %7s %6s | %7s %7s %7s %7s | %6s %6s %6s %6s %6s'%('scene','chnk','objs','rle','pxK','xluK','fillK','lights','S1med','S1p90','flats','objs','strip','npc','lerp','dirty','show'))
    for sc in order:
        if sc not in scenes: continue
        s=scenes[sc]
        r1=[r for r in rows if r['scene']==sc and r['S']=='1'][0]
        o=other[sc]
        px=(int(s['px_flat'])+int(s['px_rle'])+int(s['px_xlu'])+int(s['px_fill']))/1000
        print('%-9s %5s %5s %4s %6.0f %7.1f %7.1f %6s | %7s %7s %7s %7s | %6s %6s %6s %6s %6s'%(sc,s['chunks'],s['objs'],s['rle_calls'],px,int(s['px_xlu'])/1000,int(s['px_fill'])/1000,s['lights'],r1['med'],r1['p90'],r1['flats'],r1['objs'],o['STRIP']['med'],o['NPCRECT']['med'],o['LERP']['med'],o['DIRTYALL']['med'],o['SHOW']['med']))
    Ss=sorted(set(int(r['S']) for r in rows if r['S']!='1'))
    if Ss:
        hdr='%-9s %7s'%('scene','S1')+''.join(' %8s %8s'%('S%d m1'%S,'S%d m2'%S) for S in Ss)+'  %7s %7s %7s'%('m1/S1','m2/S1','cacheMB')
        print(hdr)
        for sc in order:
            if sc not in scenes: continue
            r1=[r for r in rows if r['scene']==sc and r['S']=='1'][0]
            line='%-9s %7s'%(sc,r1['med'])
            for S in Ss:
                m1=[r for r in rows if r['scene']==sc and r['S']==str(S) and r['mode']=='1'][0]
                m2=[r for r in rows if r['scene']==sc and r['S']==str(S) and r['mode']=='2'][0]
                line+=' %8s %8s'%(m1['med'],m2['med'])
            Smax=max(Ss)
            m1=[r for r in rows if r['scene']==sc and r['S']==str(Smax) and r['mode']=='1'][0]
            m2=[r for r in rows if r['scene']==sc and r['S']==str(Smax) and r['mode']=='2'][0]
            line+='  %7.1f %7.1f %7s'%(float(m1['med'])/float(r1['med']),float(m2['med'])/float(r1['med']),m2['cacheMB'])
            print(line)
        # flats vs objs breakdown at Smax
        print('  breakdown at S=%d (mean ms): scene flats_m1 objs_m1 flatrle_m1 | flats_m2 objs_m2 flatrle_m2 cold_m2'%Smax)
        for sc in order:
            if sc not in scenes: continue
            m1=[r for r in rows if r['scene']==sc and r['S']==str(Smax) and r['mode']=='1'][0]
            m2=[r for r in rows if r['scene']==sc and r['S']==str(Smax) and r['mode']=='2'][0]
            print('  %-9s %7s %7s %7s | %7s %7s %7s %7s'%(sc,m1['flats'],m1['objs'],m1['flatrle'],m2['flats'],m2['objs'],m2['flatrle'],m2['cold']))
