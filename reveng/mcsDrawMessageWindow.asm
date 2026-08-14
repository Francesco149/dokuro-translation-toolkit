
00148650 <+0x48650>:
  148650:	27bdfff0 	addiu	sp,sp,-16
  148654:	ffbf0000 	sd	ra,0(sp)
  148658:	0080282d 	daddu	a1,a0,zero
  14865c:	44806000 	mtc1	zero,$f12
  148660:	3c023f80 	lui	v0,0x3f80
  148664:	44826800 	mtc1	v0,$f13
  148668:	3c040036 	lui	a0,0x36
  14866c:	24842650 	addiu	a0,a0,9808
  148670:	2406ffff 	addiu	a2,zero,-1
  148674:	00c0382d 	daddu	a3,a2,zero
  148678:	0000402d 	daddu	t0,zero,zero
  14867c:	0c059100 	jal	0x164400
  148680:	00000000 	sll	zero,zero,0x0
  148684:	dfbf0000 	ld	ra,0(sp)
  148688:	27bd0010 	addiu	sp,sp,16
  14868c:	03e00008 	jr	ra
  148690:	00000000 	sll	zero,zero,0x0
