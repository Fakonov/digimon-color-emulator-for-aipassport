# Cycle accounting probe: log "PC TOTALCYCLES" for the first 20 ms so the host
# emulator's cycle model can be diffed against MAME's per instruction.
# NOTE: do not start a debug script with "go" - it blocks until a breakpoint.
trace C:\Users\DuWeisi\digimon-color-passport\dbgtest\mame_cyc.tr,0,noloop,{tracelog "%06X %d\n",pc,totalcycles}
gtime #20
trace off,0
quit
