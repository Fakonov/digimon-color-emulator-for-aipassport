import io,sys
p=r"C:\Users\DuWeisi\digimon-color-passport\tools\r6b\probe.cpp"
b=open(p,'rb').read()
print("CRLF", b.count(b'\r\n'), "LF", b.count(b'\n'))
