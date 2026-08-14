
001694b0 <+0x694b0>:
  1694b0:	27bdffd0 	addiu	sp,sp,-48
  1694b4:	ffbf0020 	sd	ra,32(sp)
  1694b8:	7fb10010 	sq	s1,16(sp)
  1694bc:	7fb00000 	sq	s0,0(sp)
  1694c0:	0080882d 	daddu	s1,a0,zero
  1694c4:	00c0802d 	daddu	s0,a2,zero
  1694c8:	0c05a4f0 	jal	0x1693c0
  1694cc:	00000000 	sll	zero,zero,0x0
  1694d0:	14400004 	bne	v0,zero,0x1694e4
  1694d4:	00000000 	sll	zero,zero,0x0
  1694d8:	2402270f 	addiu	v0,zero,9999
  1694dc:	10000006 	beq	zero,zero,0x1694f8
  1694e0:	00000000 	sll	zero,zero,0x0
  1694e4:	0220202d 	daddu	a0,s1,zero
  1694e8:	0040282d 	daddu	a1,v0,zero
  1694ec:	0200302d 	daddu	a2,s0,zero
  1694f0:	0c05a544 	jal	0x169510
  1694f4:	00000000 	sll	zero,zero,0x0
  1694f8:	dfbf0020 	ld	ra,32(sp)
  1694fc:	7bb10010 	lq	s1,16(sp)
  169500:	7bb00000 	lq	s0,0(sp)
  169504:	27bd0030 	addiu	sp,sp,48
  169508:	03e00008 	jr	ra
  16950c:	00000000 	sll	zero,zero,0x0
