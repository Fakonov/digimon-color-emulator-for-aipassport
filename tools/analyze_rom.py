
import zlib, lzma, bz2, math, json, os, hashlib, collections, statistics, time
p = r'C:\Users\DuWeisi\digimon-color-passport\roms\dmc_v1_base.bin'
b = open(p,'rb').read()
n = len(b)
print('file', p)
print('size', n, hex(n), 'sha1', hashlib.sha1(b).hexdigest())
cnt = collections.Counter(b)
zero = cnt[0]; ff = cnt[255]
valid = n - zero - ff
print('zero', zero, f'{zero/n*100:.2f}%', 'ff', ff, f'{ff/n*100:.2f}%', 'neither', valid, f'{valid/n*100:.2f}%')
H = -sum((c/n)*math.log2(c/n) for c in cnt.values() if c)
print('overall entropy', round(H,4), 'bits/byte; distinct', len(cnt))
# top bytes
print('top bytes', [(hex(k),v,round(v/n*100,2)) for k,v in cnt.most_common(8)])
# per 4k pages
ps = 4096
pages = [b[i:i+ps] for i in range(0,n,ps)]
def ent(x):
    c = collections.Counter(x); m=len(x)
    return -sum((v/m)*math.log2(v/m) for v in c.values() if v)
ents = [ent(pg) for pg in pages]
ents_sorted = sorted(ents)
def pct(q): return ents_sorted[min(len(ents_sorted)-1, int(q*len(ents_sorted)))]
print('pages', len(pages))
print('entropy min/median/mean/max', round(min(ents),3), round(statistics.median(ents),3), round(statistics.mean(ents),3), round(max(ents),3))
print('entropy p10/p25/p75/p90/p99', round(pct(.1),3), round(pct(.25),3), round(pct(.75),3), round(pct(.9),3), round(pct(.99),3))
bins = [0]*11
for e in ents:
    bins[min(10,int(e*10/8*10))] += 1  # not used
# histogram by 0.5 bits
hist = collections.Counter(round(e*2)/2 for e in ents)
print('entropy hist (0.5 bins):', sorted(hist.items()))
for th in [8.0,7.9,7.5,7.0,6.5,6.0,5.0,4.0,3.0,2.0,1.0]:
    c = sum(1 for e in ents if e >= th)
    print(f'pages >= {th}: {c} ({c/len(ents)*100:.1f}%)')
# page-wise zero/ff
blank_pages = sum(1 for pg in pages if all(x==0 for x in pg))
ff_pages = sum(1 for pg in pages if all(x==255 for x in pg))
print('all-zero pages', blank_pages, 'all-ff pages', ff_pages)
# compression
for name, fn in [('zlib-9', lambda x: zlib.compress(x,9)), ('lzma-6', lambda x: lzma.compress(x, preset=6)), ('bz2-9', lambda x: bz2.compress(x,9))]:
    t=time.time(); c=fn(b); dt=time.time()-t
    print(f'{name}: {len(c)} bytes ratio {len(c)/n:.4f} saved {100-len(c)/n*100:.2f}% time {dt:.1f}s')
# page-level zlib overhead (for random access)
t=time.time(); pc=[len(zlib.compress(pg,9)) for pg in pages]; dt=time.time()-t
print('per-page zlib total', sum(pc), 'ratio', round(sum(pc)/n,4), 'time', round(dt,1))
print('per-page zlib min/median/max', min(pc), int(statistics.median(pc)), max(pc))
# 64k block compression
for bs in [4096, 16384, 65536, 262144]:
    cs=[]
    for i in range(0,n,bs):
        cs.append(len(zlib.compress(b[i:i+bs],9)))
    print(f'block {bs}: total {sum(cs)} ratio {sum(cs)/n:.4f}')
