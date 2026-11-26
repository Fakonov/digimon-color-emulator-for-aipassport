import io
p1=open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\part1.md",encoding='utf-8').read()
p2=open(r"C:\Users\DuWeisi\digimon-color-passport\tmp\r6b\part2.md",encoding='utf-8').read()
out=r"C:\Users\DuWeisi\digimon-color-passport\docs\notes\R6B_guest.md"
open(out,'w',encoding='utf-8',newline='\n').write(p1+p2)
print("wrote", len(p1+p2), "chars")
