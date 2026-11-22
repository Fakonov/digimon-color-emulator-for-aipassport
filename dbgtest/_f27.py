import io
b = io.open("dbgtest/ram15.bin","rb").read()
print("ram bytes:", len(b))
# show words at the PCs the device kept landing on
for pc in (0x02F4, 0x0321, 0x0323):
    off = pc*2
    w = b[off] | (b[off+1]<<8)
    print("%06X -> %04X" % (pc, w))
