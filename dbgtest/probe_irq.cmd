# Absolute cycle of every IRQ6 entry (0x485D) around t = 900 ms.
# tracelog writes "PC TOTALCYCLES", so the phase of the 2 kHz timer can be read
# straight off the file.
go
gtime #900
trace C:\Users\DuWeisi\digimon-color-passport\dbgtest\mame_irq_abs.tr,0,noloop,{tracelog "%06X %d\n",pc,totalcycles}
gtime #6
trace off,0
quit
