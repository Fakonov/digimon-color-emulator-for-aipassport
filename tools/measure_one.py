
p=r'C:\Users\DuWeisi\digimon-color-passport\docs\FEASIBILITY.md'
b=open(p,'rb').read(); s=b.decode('utf-8')
print('bytes',len(b),'chars',len(s),'lines',s.count(chr(10))+1,'nonempty',sum(1 for l in s.split(chr(10)) if l.strip()))
