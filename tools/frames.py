
import hashlib, os, sys
p = r"C:\Users\DuWeisi\digimon-color-passport\button.avi"
size = 96*96*3
data = open(p,'rb').read()
n = len(data)//size
print("bytes", len(data), "frames", n, "rem", len(data)%size)
prev=None
changes=[]
for i in range(n):
    f=data[i*size:(i+1)*size]
    h=hashlib.sha256(f).hexdigest()[:12]
    if h!=prev:
        changes.append((i,h,sum(f)/len(f)))
        prev=h
print("changes", len(changes))
for c in changes[:80]:
    print(c)
