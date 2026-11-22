gtime 1000
trace C:\Users\DuWeisi\digimon-color-passport\dbgtest\pcA.tr,0,noloop,{tracelog "%06X\n",pc}
gtime #20
trace off,0
trace C:\Users\DuWeisi\digimon-color-passport\dbgtest\pcB.tr,0,noloop,{tracelog "%06X\n",pc}
gtime #100
trace off,0
quit
