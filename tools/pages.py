
b=open(r'C:\Users\DuWeisi\digimon-color-passport\roms\dmc_v1_base.bin','rb').read()
ps=4096
pages=[b[i:i+ps] for i in range(0,len(b),ps)]
nz=[p for p in pages if any(p)]
print('pages',len(pages),'nonzero',len(nz),'zero',len(pages)-len(nz))
print('nonzero bytes',len(nz)*ps, len(nz)*ps/1024/1024,'MiB')
print('partition',0x5f0000,0x5f0000/1024/1024,'MiB')
print('headroom sparse',0x5f0000-len(nz)*ps,(0x5f0000-len(nz)*ps)/1024/1024,'MiB')
