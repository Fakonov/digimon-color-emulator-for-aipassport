import sys, struct
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
print("sizes", len(a), len(b))
n=min(len(a),len(b))//2
wa=struct.unpack('<%dH'%n,a); wb=struct.unpack('<%dH'%n,b)
diff=[(i,wa[i],wb[i]) for i in range(n) if wa[i]!=wb[i]]
print("words differing:", len(diff), "of", n)
for i,x,y in diff[:40]:
    print("  %04X  %04X vs %04X"%(i,x,y))
