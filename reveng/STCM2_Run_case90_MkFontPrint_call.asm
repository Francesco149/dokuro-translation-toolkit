
00168ab0 <+0x68ab0>:
  168ab0:	44806000 	mtc1	zero,$f12
  168ab4:	3c023f80 	lui	v0,0x3f80
  168ab8:	44826800 	mtc1	v0,$f13
  168abc:	3c010176 	lui	at,0x176
  168ac0:	8c24d6a0 	lw	a0,-10592(at)
  168ac4:	3c010176 	lui	at,0x176
  168ac8:	8c25d6a4 	lw	a1,-10588(at)
  168acc:	2406ffff 	addiu	a2,zero,-1
  168ad0:	00c0382d 	daddu	a3,a2,zero
  168ad4:	0000402d 	daddu	t0,zero,zero
  168ad8:	0c059100 	jal	0x164400
  168adc:	00000000 	sll	zero,zero,0x0
