
user/initcode-la:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
       0:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
       4:	29c02061 	st.d        	$ra, $sp, 8(0x8)
       8:	54068800 	bl          	1672(0x688)	# 0x690
       c:	54022400 	bl          	548(0x224)	# 0x230
      10:	00150004 	move        	$a0, $zero
      14:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
      18:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
      1c:	4c000020 	jirl        	$zero, $ra, 0
      20:	0280e00b 	addi.w      	$a7, $zero, 56(0x38)
      24:	02860007 	addi.w      	$a3, $zero, 384(0x180)
      28:	002b0000 	syscall     	0x0
      2c:	00408084 	slli.w      	$a0, $a0, 0x0
      30:	4c000020 	jirl        	$zero, $ra, 0
      34:	0280e40b 	addi.w      	$a7, $zero, 57(0x39)
      38:	002b0000 	syscall     	0x0
      3c:	00408084 	slli.w      	$a0, $a0, 0x0
      40:	4c000020 	jirl        	$zero, $ra, 0
      44:	0280fc0b 	addi.w      	$a7, $zero, 63(0x3f)
      48:	002b0000 	syscall     	0x0
      4c:	4c000020 	jirl        	$zero, $ra, 0
      50:	0281000b 	addi.w      	$a7, $zero, 64(0x40)
      54:	002b0000 	syscall     	0x0
      58:	4c000020 	jirl        	$zero, $ra, 0
      5c:	0282b00b 	addi.w      	$a7, $zero, 172(0xac)
      60:	002b0000 	syscall     	0x0
      64:	00408084 	slli.w      	$a0, $a0, 0x0
      68:	4c000020 	jirl        	$zero, $ra, 0
      6c:	0282b40b 	addi.w      	$a7, $zero, 173(0xad)
      70:	002b0000 	syscall     	0x0
      74:	00408084 	slli.w      	$a0, $a0, 0x0
      78:	4c000020 	jirl        	$zero, $ra, 0
      7c:	0281f00b 	addi.w      	$a7, $zero, 124(0x7c)
      80:	002b0000 	syscall     	0x0
      84:	00408084 	slli.w      	$a0, $a0, 0x0
      88:	4c000020 	jirl        	$zero, $ra, 0
      8c:	0283700b 	addi.w      	$a7, $zero, 220(0xdc)
      90:	02804404 	addi.w      	$a0, $zero, 17(0x11)
      94:	00150005 	move        	$a1, $zero
      98:	00150006 	move        	$a2, $zero
      9c:	00150007 	move        	$a3, $zero
      a0:	00150008 	move        	$a4, $zero
      a4:	002b0000 	syscall     	0x0
      a8:	00408084 	slli.w      	$a0, $a0, 0x0
      ac:	4c000020 	jirl        	$zero, $ra, 0
      b0:	001500c5 	move        	$a1, $a2
      b4:	00150106 	move        	$a2, $a4
      b8:	400008a0 	beqz        	$a1, 8(0x8)	# 0xc0
      bc:	00109ca5 	add.d       	$a1, $a1, $a3
      c0:	0283700b 	addi.w      	$a7, $zero, 220(0xdc)
      c4:	00150007 	move        	$a3, $zero
      c8:	00150008 	move        	$a4, $zero
      cc:	00150009 	move        	$a5, $zero
      d0:	002b0000 	syscall     	0x0
      d4:	00408084 	slli.w      	$a0, $a0, 0x0
      d8:	4c000020 	jirl        	$zero, $ra, 0
      dc:	0281740b 	addi.w      	$a7, $zero, 93(0x5d)
      e0:	002b0000 	syscall     	0x0
      e4:	4c000020 	jirl        	$zero, $ra, 0
      e8:	0284100b 	addi.w      	$a7, $zero, 260(0x104)
      ec:	00150007 	move        	$a3, $zero
      f0:	002b0000 	syscall     	0x0
      f4:	00408084 	slli.w      	$a0, $a0, 0x0
      f8:	4c000020 	jirl        	$zero, $ra, 0
      fc:	0283740b 	addi.w      	$a7, $zero, 221(0xdd)
     100:	002b0000 	syscall     	0x0
     104:	00408084 	slli.w      	$a0, $a0, 0x0
     108:	4c000020 	jirl        	$zero, $ra, 0
     10c:	0283740b 	addi.w      	$a7, $zero, 221(0xdd)
     110:	002b0000 	syscall     	0x0
     114:	00408084 	slli.w      	$a0, $a0, 0x0
     118:	4c000020 	jirl        	$zero, $ra, 0
     11c:	0282640b 	addi.w      	$a7, $zero, 153(0x99)
     120:	002b0000 	syscall     	0x0
     124:	4c000020 	jirl        	$zero, $ra, 0
     128:	0015008c 	move        	$t0, $a0
     12c:	0280340b 	addi.w      	$a7, $zero, 13(0xd)
     130:	00df0084 	bstrpick.d  	$a0, $a0, 0x1f, 0x0
     134:	002b0000 	syscall     	0x0
     138:	00131184 	maskeqz     	$a0, $t0, $a0
     13c:	4c000020 	jirl        	$zero, $ra, 0
     140:	02835c0b 	addi.w      	$a7, $zero, 215(0xd7)
     144:	002b0000 	syscall     	0x0
     148:	00408084 	slli.w      	$a0, $a0, 0x0
     14c:	4c000020 	jirl        	$zero, $ra, 0
     150:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
     154:	29c02061 	st.d        	$ra, $sp, 8(0x8)
     158:	00150006 	move        	$a2, $zero
     15c:	00150085 	move        	$a1, $a0
     160:	02bffc04 	addi.w      	$a0, $zero, -1(0xfff)
     164:	57ff87ff 	bl          	-124(0xfffff84)	# 0xe8
     168:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
     16c:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
     170:	4c000020 	jirl        	$zero, $ra, 0
     174:	0280940b 	addi.w      	$a7, $zero, 37(0x25)
     178:	00df0108 	bstrpick.d  	$a4, $a4, 0x1f, 0x0
     17c:	002b0000 	syscall     	0x0
     180:	00408084 	slli.w      	$a0, $a0, 0x0
     184:	4c000020 	jirl        	$zero, $ra, 0
     188:	02808c0b 	addi.w      	$a7, $zero, 35(0x23)
     18c:	00df00c6 	bstrpick.d  	$a2, $a2, 0x1f, 0x0
     190:	002b0000 	syscall     	0x0
     194:	00408084 	slli.w      	$a0, $a0, 0x0
     198:	4c000020 	jirl        	$zero, $ra, 0
     19c:	0282800b 	addi.w      	$a7, $zero, 160(0xa0)
     1a0:	002b0000 	syscall     	0x0
     1a4:	00408084 	slli.w      	$a0, $a0, 0x0
     1a8:	4c000020 	jirl        	$zero, $ra, 0
     1ac:	0283580b 	addi.w      	$a7, $zero, 214(0xd6)
     1b0:	002b0000 	syscall     	0x0
     1b4:	00408084 	slli.w      	$a0, $a0, 0x0
     1b8:	4c000020 	jirl        	$zero, $ra, 0
     1bc:	0280440b 	addi.w      	$a7, $zero, 17(0x11)
     1c0:	002b0000 	syscall     	0x0
     1c4:	4c000020 	jirl        	$zero, $ra, 0
     1c8:	0280c40b 	addi.w      	$a7, $zero, 49(0x31)
     1cc:	002b0000 	syscall     	0x0
     1d0:	00408084 	slli.w      	$a0, $a0, 0x0
     1d4:	4c000020 	jirl        	$zero, $ra, 0
     1d8:	0280f40b 	addi.w      	$a7, $zero, 61(0x3d)
     1dc:	002b0000 	syscall     	0x0
     1e0:	00408084 	slli.w      	$a0, $a0, 0x0
     1e4:	4c000020 	jirl        	$zero, $ra, 0
     1e8:	0280ec0b 	addi.w      	$a7, $zero, 59(0x3b)
     1ec:	00150005 	move        	$a1, $zero
     1f0:	002b0000 	syscall     	0x0
     1f4:	00408084 	slli.w      	$a0, $a0, 0x0
     1f8:	4c000020 	jirl        	$zero, $ra, 0
     1fc:	02805c0b 	addi.w      	$a7, $zero, 23(0x17)
     200:	002b0000 	syscall     	0x0
     204:	00408084 	slli.w      	$a0, $a0, 0x0
     208:	4c000020 	jirl        	$zero, $ra, 0
     20c:	0280a00b 	addi.w      	$a7, $zero, 40(0x28)
     210:	002b0000 	syscall     	0x0
     214:	00408084 	slli.w      	$a0, $a0, 0x0
     218:	4c000020 	jirl        	$zero, $ra, 0
     21c:	02809c0b 	addi.w      	$a7, $zero, 39(0x27)
     220:	00150005 	move        	$a1, $zero
     224:	002b0000 	syscall     	0x0
     228:	00408084 	slli.w      	$a0, $a0, 0x0
     22c:	4c000020 	jirl        	$zero, $ra, 0
     230:	02804c0b 	addi.w      	$a7, $zero, 19(0x13)
     234:	002b0000 	syscall     	0x0
     238:	00408084 	slli.w      	$a0, $a0, 0x0
     23c:	4c000020 	jirl        	$zero, $ra, 0
     240:	02ff8063 	addi.d      	$sp, $sp, -32(0xfe0)
     244:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     248:	29003c65 	st.b        	$a1, $sp, 15(0xf)
     24c:	02800406 	addi.w      	$a2, $zero, 1(0x1)
     250:	02c03c65 	addi.d      	$a1, $sp, 15(0xf)
     254:	57fdffff 	bl          	-516(0xffffdfc)	# 0x50
     258:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     25c:	02c08063 	addi.d      	$sp, $sp, 32(0x20)
     260:	4c000020 	jirl        	$zero, $ra, 0
     264:	02ff4063 	addi.d      	$sp, $sp, -48(0xfd0)
     268:	29c0a061 	st.d        	$ra, $sp, 40(0x28)
     26c:	29c08077 	st.d        	$s0, $sp, 32(0x20)
     270:	29c06078 	st.d        	$s1, $sp, 24(0x18)
     274:	29c04079 	st.d        	$s2, $sp, 16(0x10)
     278:	00150097 	move        	$s0, $a0
     27c:	400008e0 	beqz        	$a3, 8(0x8)	# 0x284
     280:	6000b4a0 	bltz        	$a1, 180(0xb4)	# 0x334
     284:	004080a5 	slli.w      	$a1, $a1, 0x0
     288:	00150011 	move        	$t5, $zero
     28c:	00150079 	move        	$s2, $sp
     290:	0015032e 	move        	$t2, $s2
     294:	0015000c 	move        	$t0, $zero
     298:	004080c6 	slli.w      	$a2, $a2, 0x0
     29c:	1a00002f 	pcalau12i   	$t3, 1(0x1)
     2a0:	02e3a1ef 	addi.d      	$t3, $t3, -1816(0x8e8)
     2a4:	00150190 	move        	$t4, $t0
     2a8:	0280058c 	addi.w      	$t0, $t0, 1(0x1)
     2ac:	002198ad 	mod.wu      	$t1, $a1, $a2
     2b0:	00df01ad 	bstrpick.d  	$t1, $t1, 0x1f, 0x0
     2b4:	382035ed 	ldx.bu      	$t1, $t3, $t1
     2b8:	290001cd 	st.b        	$t1, $t2, 0
     2bc:	004080ad 	slli.w      	$t1, $a1, 0x0
     2c0:	002118a5 	div.wu      	$a1, $a1, $a2
     2c4:	02c005ce 	addi.d      	$t2, $t2, 1(0x1)
     2c8:	6fffdda6 	bgeu        	$t1, $a2, -36(0x3ffdc)	# 0x2a4
     2cc:	40001a20 	beqz        	$t5, 24(0x18)	# 0x2e4
     2d0:	02c0418c 	addi.d      	$t0, $t0, 16(0x10)
     2d4:	00108d8c 	add.d       	$t0, $t0, $sp
     2d8:	0280b40d 	addi.w      	$t1, $zero, 45(0x2d)
     2dc:	293fc18d 	st.b        	$t1, $t0, -16(0xff0)
     2e0:	02800a0c 	addi.w      	$t0, $t4, 2(0x2)
     2e4:	6400380c 	blez        	$t0, 56(0x38)	# 0x31c
     2e8:	02c0418d 	addi.d      	$t1, $t0, 16(0x10)
     2ec:	00108db8 	add.d       	$s1, $t1, $sp
     2f0:	02ffbf18 	addi.d      	$s1, $s1, -17(0xfef)
     2f4:	02fffb39 	addi.d      	$s2, $s2, -2(0xffe)
     2f8:	0010b339 	add.d       	$s2, $s2, $t0
     2fc:	02bffd8c 	addi.w      	$t0, $t0, -1(0xfff)
     300:	00df018c 	bstrpick.d  	$t0, $t0, 0x1f, 0x0
     304:	0011b339 	sub.d       	$s2, $s2, $t0
     308:	28000305 	ld.b        	$a1, $s1, 0
     30c:	001502e4 	move        	$a0, $s0
     310:	57ff33ff 	bl          	-208(0xfffff30)	# 0x240
     314:	02ffff18 	addi.d      	$s1, $s1, -1(0xfff)
     318:	5ffff319 	bne         	$s1, $s2, -16(0x3fff0)	# 0x308
     31c:	28c0a061 	ld.d        	$ra, $sp, 40(0x28)
     320:	28c08077 	ld.d        	$s0, $sp, 32(0x20)
     324:	28c06078 	ld.d        	$s1, $sp, 24(0x18)
     328:	28c04079 	ld.d        	$s2, $sp, 16(0x10)
     32c:	02c0c063 	addi.d      	$sp, $sp, 48(0x30)
     330:	4c000020 	jirl        	$zero, $ra, 0
     334:	00111405 	sub.w       	$a1, $zero, $a1
     338:	004080a5 	slli.w      	$a1, $a1, 0x0
     33c:	02800411 	addi.w      	$t5, $zero, 1(0x1)
     340:	53ff4fff 	b           	-180(0xfffff4c)	# 0x28c
     344:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     348:	29c12061 	st.d        	$ra, $sp, 72(0x48)
     34c:	29c10077 	st.d        	$s0, $sp, 64(0x40)
     350:	29c0e078 	st.d        	$s1, $sp, 56(0x38)
     354:	29c0c079 	st.d        	$s2, $sp, 48(0x30)
     358:	29c0a07a 	st.d        	$s3, $sp, 40(0x28)
     35c:	29c0807b 	st.d        	$s4, $sp, 32(0x20)
     360:	29c0607c 	st.d        	$s5, $sp, 24(0x18)
     364:	29c0407d 	st.d        	$s6, $sp, 16(0x10)
     368:	29c0207e 	st.d        	$s7, $sp, 8(0x8)
     36c:	280000b7 	ld.b        	$s0, $a1, 0
     370:	4001eae0 	beqz        	$s0, 488(0x1e8)	# 0x558
     374:	0015009a 	move        	$s3, $a0
     378:	001500dc 	move        	$s5, $a2
     37c:	02c004b8 	addi.d      	$s1, $a1, 1(0x1)
     380:	00150019 	move        	$s2, $zero
     384:	0280941b 	addi.w      	$s4, $zero, 37(0x25)
     388:	0280541d 	addi.w      	$s6, $zero, 21(0x15)
     38c:	50002400 	b           	36(0x24)	# 0x3b0
     390:	001502e5 	move        	$a1, $s0
     394:	00150344 	move        	$a0, $s3
     398:	57feabff 	bl          	-344(0xffffea8)	# 0x240
     39c:	50000800 	b           	8(0x8)	# 0x3a4
     3a0:	58002b3b 	beq         	$s2, $s4, 40(0x28)	# 0x3c8
     3a4:	02c00718 	addi.d      	$s1, $s1, 1(0x1)
     3a8:	283fff17 	ld.b        	$s0, $s1, -1(0xfff)
     3ac:	4001aee0 	beqz        	$s0, 428(0x1ac)	# 0x558
     3b0:	0343feed 	andi        	$t1, $s0, 0xff
     3b4:	006782ec 	bstrpick.w  	$t0, $s0, 0x7, 0x0
     3b8:	47ffeb3f 	bnez        	$s2, -24(0x7fffe8)	# 0x3a0
     3bc:	5fffd59b 	bne         	$t0, $s4, -44(0x3ffd4)	# 0x390
     3c0:	00150199 	move        	$s2, $t0
     3c4:	53ffe3ff 	b           	-32(0xfffffe0)	# 0x3a4
     3c8:	5801519b 	beq         	$t0, $s4, 336(0x150)	# 0x518
     3cc:	02be75ac 	addi.w      	$t0, $t1, -99(0xf9d)
     3d0:	68015fac 	bltu        	$s6, $t0, 348(0x15c)	# 0x52c
     3d4:	02be75ad 	addi.w      	$t1, $t1, -99(0xf9d)
     3d8:	680157ac 	bltu        	$s6, $t0, 340(0x154)	# 0x52c
     3dc:	00df01ad 	bstrpick.d  	$t1, $t1, 0x1f, 0x0
     3e0:	002d01ad 	alsl.d      	$t1, $t1, $zero, 0x3
     3e4:	1a00002c 	pcalau12i   	$t0, 1(0x1)
     3e8:	02e0e18c 	addi.d      	$t0, $t0, -1992(0x838)
     3ec:	380c358d 	ldx.d       	$t1, $t0, $t1
     3f0:	0010b58c 	add.d       	$t0, $t0, $t1
     3f4:	4c000180 	jirl        	$zero, $t0, 0
     3f8:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     3fc:	02800407 	addi.w      	$a3, $zero, 1(0x1)
     400:	02802806 	addi.w      	$a2, $zero, 10(0xa)
     404:	24000385 	ldptr.w     	$a1, $s5, 0
     408:	00150344 	move        	$a0, $s3
     40c:	57fe5bff 	bl          	-424(0xffffe58)	# 0x264
     410:	001502fc 	move        	$s5, $s0
     414:	00150019 	move        	$s2, $zero
     418:	53ff8fff 	b           	-116(0xfffff8c)	# 0x3a4
     41c:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     420:	00150007 	move        	$a3, $zero
     424:	02802806 	addi.w      	$a2, $zero, 10(0xa)
     428:	24000385 	ldptr.w     	$a1, $s5, 0
     42c:	00150344 	move        	$a0, $s3
     430:	57fe37ff 	bl          	-460(0xffffe34)	# 0x264
     434:	001502fc 	move        	$s5, $s0
     438:	00150019 	move        	$s2, $zero
     43c:	53ff6bff 	b           	-152(0xfffff68)	# 0x3a4
     440:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     444:	00150007 	move        	$a3, $zero
     448:	02804006 	addi.w      	$a2, $zero, 16(0x10)
     44c:	24000385 	ldptr.w     	$a1, $s5, 0
     450:	00150344 	move        	$a0, $s3
     454:	57fe13ff 	bl          	-496(0xffffe10)	# 0x264
     458:	001502fc 	move        	$s5, $s0
     45c:	00150019 	move        	$s2, $zero
     460:	53ff47ff 	b           	-188(0xfffff44)	# 0x3a4
     464:	02c0239e 	addi.d      	$s7, $s5, 8(0x8)
     468:	26000399 	ldptr.d     	$s2, $s5, 0
     46c:	0280c005 	addi.w      	$a1, $zero, 48(0x30)
     470:	00150344 	move        	$a0, $s3
     474:	57fdcfff 	bl          	-564(0xffffdcc)	# 0x240
     478:	0281e005 	addi.w      	$a1, $zero, 120(0x78)
     47c:	00150344 	move        	$a0, $s3
     480:	57fdc3ff 	bl          	-576(0xffffdc0)	# 0x240
     484:	02804017 	addi.w      	$s0, $zero, 16(0x10)
     488:	1a00003c 	pcalau12i   	$s5, 1(0x1)
     48c:	02e3a39c 	addi.d      	$s5, $s5, -1816(0x8e8)
     490:	0045f32c 	srli.d      	$t0, $s2, 0x3c
     494:	38003385 	ldx.b       	$a1, $s5, $t0
     498:	00150344 	move        	$a0, $s3
     49c:	57fda7ff 	bl          	-604(0xffffda4)	# 0x240
     4a0:	00411339 	slli.d      	$s2, $s2, 0x4
     4a4:	02fffef7 	addi.d      	$s0, $s0, -1(0xfff)
     4a8:	47ffeaff 	bnez        	$s0, -24(0x7fffe8)	# 0x490
     4ac:	001503dc 	move        	$s5, $s7
     4b0:	00150019 	move        	$s2, $zero
     4b4:	53fef3ff 	b           	-272(0xffffef0)	# 0x3a4
     4b8:	02c02399 	addi.d      	$s2, $s5, 8(0x8)
     4bc:	26000397 	ldptr.d     	$s0, $s5, 0
     4c0:	40002ee0 	beqz        	$s0, 44(0x2c)	# 0x4ec
     4c4:	280002e5 	ld.b        	$a1, $s0, 0
     4c8:	400084a0 	beqz        	$a1, 132(0x84)	# 0x54c
     4cc:	00150344 	move        	$a0, $s3
     4d0:	57fd73ff 	bl          	-656(0xffffd70)	# 0x240
     4d4:	02c006f7 	addi.d      	$s0, $s0, 1(0x1)
     4d8:	280002e5 	ld.b        	$a1, $s0, 0
     4dc:	47fff0bf 	bnez        	$a1, -16(0x7ffff0)	# 0x4cc
     4e0:	0015033c 	move        	$s5, $s2
     4e4:	00150019 	move        	$s2, $zero
     4e8:	53febfff 	b           	-324(0xffffebc)	# 0x3a4
     4ec:	1a000017 	pcalau12i   	$s0, 0
     4f0:	02de82f7 	addi.d      	$s0, $s0, 1952(0x7a0)
     4f4:	0280a005 	addi.w      	$a1, $zero, 40(0x28)
     4f8:	53ffd7ff 	b           	-44(0xfffffd4)	# 0x4cc
     4fc:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     500:	28000385 	ld.b        	$a1, $s5, 0
     504:	00150344 	move        	$a0, $s3
     508:	57fd3bff 	bl          	-712(0xffffd38)	# 0x240
     50c:	001502fc 	move        	$s5, $s0
     510:	00150019 	move        	$s2, $zero
     514:	53fe93ff 	b           	-368(0xffffe90)	# 0x3a4
     518:	001502e5 	move        	$a1, $s0
     51c:	00150344 	move        	$a0, $s3
     520:	57fd23ff 	bl          	-736(0xffffd20)	# 0x240
     524:	00150019 	move        	$s2, $zero
     528:	53fe7fff 	b           	-388(0xffffe7c)	# 0x3a4
     52c:	02809405 	addi.w      	$a1, $zero, 37(0x25)
     530:	00150344 	move        	$a0, $s3
     534:	57fd0fff 	bl          	-756(0xffffd0c)	# 0x240
     538:	001502e5 	move        	$a1, $s0
     53c:	00150344 	move        	$a0, $s3
     540:	57fd03ff 	bl          	-768(0xffffd00)	# 0x240
     544:	00150019 	move        	$s2, $zero
     548:	53fe5fff 	b           	-420(0xffffe5c)	# 0x3a4
     54c:	0015033c 	move        	$s5, $s2
     550:	00150019 	move        	$s2, $zero
     554:	53fe53ff 	b           	-432(0xffffe50)	# 0x3a4
     558:	28c12061 	ld.d        	$ra, $sp, 72(0x48)
     55c:	28c10077 	ld.d        	$s0, $sp, 64(0x40)
     560:	28c0e078 	ld.d        	$s1, $sp, 56(0x38)
     564:	28c0c079 	ld.d        	$s2, $sp, 48(0x30)
     568:	28c0a07a 	ld.d        	$s3, $sp, 40(0x28)
     56c:	28c0807b 	ld.d        	$s4, $sp, 32(0x20)
     570:	28c0607c 	ld.d        	$s5, $sp, 24(0x18)
     574:	28c0407d 	ld.d        	$s6, $sp, 16(0x10)
     578:	28c0207e 	ld.d        	$s7, $sp, 8(0x8)
     57c:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     580:	4c000020 	jirl        	$zero, $ra, 0
     584:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     588:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     58c:	29c08066 	st.d        	$a2, $sp, 32(0x20)
     590:	29c0a067 	st.d        	$a3, $sp, 40(0x28)
     594:	29c0c068 	st.d        	$a4, $sp, 48(0x30)
     598:	29c0e069 	st.d        	$a5, $sp, 56(0x38)
     59c:	29c1006a 	st.d        	$a6, $sp, 64(0x40)
     5a0:	29c1206b 	st.d        	$a7, $sp, 72(0x48)
     5a4:	02c08066 	addi.d      	$a2, $sp, 32(0x20)
     5a8:	29c02066 	st.d        	$a2, $sp, 8(0x8)
     5ac:	57fd9bff 	bl          	-616(0xffffd98)	# 0x344
     5b0:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     5b4:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     5b8:	4c000020 	jirl        	$zero, $ra, 0
     5bc:	02fe8063 	addi.d      	$sp, $sp, -96(0xfa0)
     5c0:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     5c4:	29c0a065 	st.d        	$a1, $sp, 40(0x28)
     5c8:	29c0c066 	st.d        	$a2, $sp, 48(0x30)
     5cc:	29c0e067 	st.d        	$a3, $sp, 56(0x38)
     5d0:	29c10068 	st.d        	$a4, $sp, 64(0x40)
     5d4:	29c12069 	st.d        	$a5, $sp, 72(0x48)
     5d8:	29c1406a 	st.d        	$a6, $sp, 80(0x50)
     5dc:	29c1606b 	st.d        	$a7, $sp, 88(0x58)
     5e0:	02c0a066 	addi.d      	$a2, $sp, 40(0x28)
     5e4:	29c02066 	st.d        	$a2, $sp, 8(0x8)
     5e8:	00150085 	move        	$a1, $a0
     5ec:	02800404 	addi.w      	$a0, $zero, 1(0x1)
     5f0:	57fd57ff 	bl          	-684(0xffffd54)	# 0x344
     5f4:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     5f8:	02c18063 	addi.d      	$sp, $sp, 96(0x60)
     5fc:	4c000020 	jirl        	$zero, $ra, 0
     600:	02ff8063 	addi.d      	$sp, $sp, -32(0xfe0)
     604:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     608:	29c04077 	st.d        	$s0, $sp, 16(0x10)
     60c:	00150097 	move        	$s0, $a0
     610:	57fa7fff 	bl          	-1412(0xffffa7c)	# 0x8c
     614:	60003880 	bltz        	$a0, 56(0x38)	# 0x64c
     618:	44005480 	bnez        	$a0, 84(0x54)	# 0x66c
     61c:	00150006 	move        	$a2, $zero
     620:	00150005 	move        	$a1, $zero
     624:	001502e4 	move        	$a0, $s0
     628:	57fae7ff 	bl          	-1308(0xffffae4)	# 0x10c
     62c:	60003080 	bltz        	$a0, 48(0x30)	# 0x65c
     630:	00150004 	move        	$a0, $zero
     634:	57faabff 	bl          	-1368(0xffffaa8)	# 0xdc
     638:	00150004 	move        	$a0, $zero
     63c:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     640:	28c04077 	ld.d        	$s0, $sp, 16(0x10)
     644:	02c08063 	addi.d      	$sp, $sp, 32(0x20)
     648:	4c000020 	jirl        	$zero, $ra, 0
     64c:	1a000004 	pcalau12i   	$a0, 0
     650:	02dea084 	addi.d      	$a0, $a0, 1960(0x7a8)
     654:	57ff6bff 	bl          	-152(0xfffff68)	# 0x5bc
     658:	53ffe3ff 	b           	-32(0xfffffe0)	# 0x638
     65c:	1a000004 	pcalau12i   	$a0, 0
     660:	02dee084 	addi.d      	$a0, $a0, 1976(0x7b8)
     664:	57ff5bff 	bl          	-168(0xfffff58)	# 0x5bc
     668:	53ffcbff 	b           	-56(0xfffffc8)	# 0x630
     66c:	02be700c 	addi.w      	$t0, $zero, -100(0xf9c)
     670:	2980306c 	st.w        	$t0, $sp, 12(0xc)
     674:	02c03064 	addi.d      	$a0, $sp, 12(0xc)
     678:	57fadbff 	bl          	-1320(0xffffad8)	# 0x150
     67c:	67ffbc80 	bgez        	$a0, -68(0x3ffbc)	# 0x638
     680:	1a000004 	pcalau12i   	$a0, 0
     684:	02df2084 	addi.d      	$a0, $a0, 1992(0x7c8)
     688:	57ff37ff 	bl          	-204(0xfffff34)	# 0x5bc
     68c:	53ffafff 	b           	-84(0xfffffac)	# 0x638
     690:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
     694:	29c02061 	st.d        	$ra, $sp, 8(0x8)
     698:	1a000004 	pcalau12i   	$a0, 0
     69c:	02df6084 	addi.d      	$a0, $a0, 2008(0x7d8)
     6a0:	57fb2bff 	bl          	-1240(0xffffb28)	# 0x1c8
     6a4:	1a000004 	pcalau12i   	$a0, 0
     6a8:	02dfc084 	addi.d      	$a0, $a0, 2032(0x7f0)
     6ac:	57ff57ff 	bl          	-172(0xfffff54)	# 0x600
     6b0:	00150004 	move        	$a0, $zero
     6b4:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
     6b8:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
     6bc:	4c000020 	jirl        	$zero, $ra, 0
     6c0:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     6c4:	29c12061 	st.d        	$ra, $sp, 72(0x48)
     6c8:	29c10077 	st.d        	$s0, $sp, 64(0x40)
     6cc:	57f9c3ff 	bl          	-1600(0xffff9c0)	# 0x8c
     6d0:	60007480 	bltz        	$a0, 116(0x74)	# 0x744
     6d4:	00150097 	move        	$s0, $a0
     6d8:	44009880 	bnez        	$a0, 152(0x98)	# 0x770
     6dc:	1a000024 	pcalau12i   	$a0, 1(0x1)
     6e0:	02e02084 	addi.d      	$a0, $a0, -2040(0x808)
     6e4:	57fae7ff 	bl          	-1308(0xffffae4)	# 0x1c8
     6e8:	29c02060 	st.d        	$zero, $sp, 8(0x8)
     6ec:	29c04060 	st.d        	$zero, $sp, 16(0x10)
     6f0:	29c06060 	st.d        	$zero, $sp, 24(0x18)
     6f4:	29c08060 	st.d        	$zero, $sp, 32(0x20)
     6f8:	29c0a060 	st.d        	$zero, $sp, 40(0x28)
     6fc:	29c0c060 	st.d        	$zero, $sp, 48(0x30)
     700:	29c0e060 	st.d        	$zero, $sp, 56(0x38)
     704:	1a00002c 	pcalau12i   	$t0, 1(0x1)
     708:	02e0618c 	addi.d      	$t0, $t0, -2024(0x818)
     70c:	2700006c 	stptr.d     	$t0, $sp, 0
     710:	00150006 	move        	$a2, $zero
     714:	00150065 	move        	$a1, $sp
     718:	1a000024 	pcalau12i   	$a0, 1(0x1)
     71c:	02e08084 	addi.d      	$a0, $a0, -2016(0x820)
     720:	57f9efff 	bl          	-1556(0xffff9ec)	# 0x10c
     724:	60003480 	bltz        	$a0, 52(0x34)	# 0x758
     728:	00150004 	move        	$a0, $zero
     72c:	57f9b3ff 	bl          	-1616(0xffff9b0)	# 0xdc
     730:	001502e4 	move        	$a0, $s0
     734:	28c12061 	ld.d        	$ra, $sp, 72(0x48)
     738:	28c10077 	ld.d        	$s0, $sp, 64(0x40)
     73c:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     740:	4c000020 	jirl        	$zero, $ra, 0
     744:	1a000004 	pcalau12i   	$a0, 0
     748:	02dfe084 	addi.d      	$a0, $a0, 2040(0x7f8)
     74c:	57fe73ff 	bl          	-400(0xffffe70)	# 0x5bc
     750:	02bffc17 	addi.w      	$s0, $zero, -1(0xfff)
     754:	53ffdfff 	b           	-36(0xfffffdc)	# 0x730
     758:	1a000024 	pcalau12i   	$a0, 1(0x1)
     75c:	02e0a084 	addi.d      	$a0, $a0, -2008(0x828)
     760:	57fe5fff 	bl          	-420(0xffffe5c)	# 0x5bc
     764:	02800404 	addi.w      	$a0, $zero, 1(0x1)
     768:	57f977ff 	bl          	-1676(0xffff974)	# 0xdc
     76c:	53ffbfff 	b           	-68(0xfffffbc)	# 0x728
     770:	02be700c 	addi.w      	$t0, $zero, -100(0xf9c)
     774:	2500006c 	stptr.w     	$t0, $sp, 0
     778:	00150064 	move        	$a0, $sp
     77c:	57f9d7ff 	bl          	-1580(0xffff9d4)	# 0x150
     780:	60000c80 	bltz        	$a0, 12(0xc)	# 0x78c
     784:	00150017 	move        	$s0, $zero
     788:	53ffabff 	b           	-88(0xfffffa8)	# 0x730
     78c:	1a000004 	pcalau12i   	$a0, 0
     790:	02df2084 	addi.d      	$a0, $a0, 1992(0x7c8)
     794:	57fe2bff 	bl          	-472(0xffffe28)	# 0x5bc
     798:	53ffefff 	b           	-20(0xfffffec)	# 0x784
     79c:	00000000 	0x00000000
     7a0:	6c756e28 	bgeu        	$t5, $a4, 30060(0x756c)	# 0x7d0c
     7a4:	0000296c 	cto.d       	$t0, $a7
     7a8:	6b726f66 	bltu        	$s4, $a2, -36244(0x3726c)	# 0xffffffffffff7a14
     7ac:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16910
     7b0:	0064656c 	bstrins.w   	$t0, $a7, 0x4, 0x19
     7b4:	00000000 	0x00000000
     7b8:	63657865 	blt         	$sp, $a1, -39560(0x36578)	# 0xffffffffffff6d30
     7bc:	66206576 	bge         	$a7, $fp, -122780(0x22064)	# 0xfffffffffffe2820
     7c0:	656c6961 	bge         	$a7, $ra, 93288(0x16c68)	# 0x17428
     7c4:	00000064 	0x00000064
     7c8:	74696177 	0x74696177
     7cc:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16930
     7d0:	00000a6c 	0x00000a6c
     7d4:	00000000 	0x00000000
     7d8:	746e6d2f 	0x746e6d2f
     7dc:	73756d2f 	0x73756d2f
     7e0:	61622f6c 	blt         	$s4, $t0, 90668(0x1622c)	# 0x16a0c
     7e4:	2f636973 	0x2f636973
	...
     7f0:	70616d6d 	0x70616d6d
     7f4:	00000000 	0x00000000
     7f8:	6b726f66 	bltu        	$s4, $a2, -36244(0x3726c)	# 0xffffffffffff7a64
     7fc:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16960
     800:	0a64656c 	0x0a64656c
     804:	00000000 	0x00000000
     808:	746e6d2f 	0x746e6d2f
     80c:	696c672f 	bltu        	$s2, $t3, 93284(0x16c64)	# 0x17470
     810:	002f6362 	0x002f6362
     814:	00000000 	0x00000000
     818:	00687361 	bstrins.w   	$ra, $s4, 0x8, 0x1c
     81c:	00000000 	0x00000000
     820:	79737562 	0x79737562
     824:	00786f62 	bstrins.w   	$tp, $s4, 0x18, 0x1b
     828:	63657865 	blt         	$sp, $a1, -39560(0x36578)	# 0xffffffffffff6da0
     82c:	66206576 	bge         	$a7, $fp, -122780(0x22064)	# 0xfffffffffffe2890
     830:	656c6961 	bge         	$a7, $ra, 93288(0x16c68)	# 0x17498
     834:	00000a64 	0x00000a64
     838:	fffffcc4 	0xfffffcc4
     83c:	ffffffff 	0xffffffff
     840:	fffffbc0 	0xfffffbc0
     844:	ffffffff 	0xffffffff
     848:	fffffcf4 	0xfffffcf4
     84c:	ffffffff 	0xffffffff
     850:	fffffcf4 	0xfffffcf4
     854:	ffffffff 	0xffffffff
     858:	fffffcf4 	0xfffffcf4
     85c:	ffffffff 	0xffffffff
     860:	fffffcf4 	0xfffffcf4
     864:	ffffffff 	0xffffffff
     868:	fffffcf4 	0xfffffcf4
     86c:	ffffffff 	0xffffffff
     870:	fffffcf4 	0xfffffcf4
     874:	ffffffff 	0xffffffff
     878:	fffffcf4 	0xfffffcf4
     87c:	ffffffff 	0xffffffff
     880:	fffffbe4 	0xfffffbe4
     884:	ffffffff 	0xffffffff
     888:	fffffcf4 	0xfffffcf4
     88c:	ffffffff 	0xffffffff
     890:	fffffcf4 	0xfffffcf4
     894:	ffffffff 	0xffffffff
     898:	fffffcf4 	0xfffffcf4
     89c:	ffffffff 	0xffffffff
     8a0:	fffffc2c 	0xfffffc2c
     8a4:	ffffffff 	0xffffffff
     8a8:	fffffcf4 	0xfffffcf4
     8ac:	ffffffff 	0xffffffff
     8b0:	fffffcf4 	0xfffffcf4
     8b4:	ffffffff 	0xffffffff
     8b8:	fffffc80 	0xfffffc80
     8bc:	ffffffff 	0xffffffff
     8c0:	fffffcf4 	0xfffffcf4
     8c4:	ffffffff 	0xffffffff
     8c8:	fffffcf4 	0xfffffcf4
     8cc:	ffffffff 	0xffffffff
     8d0:	fffffcf4 	0xfffffcf4
     8d4:	ffffffff 	0xffffffff
     8d8:	fffffcf4 	0xfffffcf4
     8dc:	ffffffff 	0xffffffff
     8e0:	fffffc08 	0xfffffc08
     8e4:	ffffffff 	0xffffffff
     8e8:	33323130 	0x33323130
     8ec:	37363534 	0x37363534
     8f0:	42413938 	beqz        	$a5, -1949384(0x624138)	# 0xffffffffffe24a28
     8f4:	46454443 	bnez        	$tp, 935236(0xe4544)	# 0xe4e38
	...
    1000:	00000010 	0x00000010
    1004:	00000000 	0x00000000
    1008:	00527a01 	0x00527a01
    100c:	01017c01 	fadd.d      	$fa1, $fa0, $fs7
    1010:	00030d1b 	0x00030d1b
    1014:	00000018 	0x00000018
    1018:	00000018 	0x00000018
    101c:	ffffefe4 	0xffffefe4
    1020:	00000020 	0x00000020
    1024:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1028:	50028144 	b           	84935296(0x5100280)	# 0x51012a8
    102c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1030:	00000010 	0x00000010
    1034:	00000034 	0x00000034
    1038:	ffffefe8 	0xffffefe8
    103c:	00000014 	0x00000014
    1040:	00000000 	0x00000000
    1044:	00000010 	0x00000010
    1048:	00000048 	0x00000048
    104c:	ffffefe8 	0xffffefe8
    1050:	00000010 	0x00000010
    1054:	00000000 	0x00000000
    1058:	00000010 	0x00000010
    105c:	0000005c 	0x0000005c
    1060:	ffffefe4 	0xffffefe4
    1064:	0000000c 	0x0000000c
    1068:	00000000 	0x00000000
    106c:	00000010 	0x00000010
    1070:	00000070 	0x00000070
    1074:	ffffefdc 	0xffffefdc
    1078:	0000000c 	0x0000000c
    107c:	00000000 	0x00000000
    1080:	00000010 	0x00000010
    1084:	00000084 	0x00000084
    1088:	ffffefd4 	0xffffefd4
    108c:	00000010 	0x00000010
    1090:	00000000 	0x00000000
    1094:	00000010 	0x00000010
    1098:	00000098 	0x00000098
    109c:	ffffefd0 	0xffffefd0
    10a0:	00000010 	0x00000010
    10a4:	00000000 	0x00000000
    10a8:	00000010 	0x00000010
    10ac:	000000ac 	0x000000ac
    10b0:	ffffefcc 	0xffffefcc
    10b4:	00000010 	0x00000010
    10b8:	00000000 	0x00000000
    10bc:	00000010 	0x00000010
    10c0:	000000c0 	0x000000c0
    10c4:	ffffefc8 	0xffffefc8
    10c8:	00000024 	0x00000024
    10cc:	00000000 	0x00000000
    10d0:	00000010 	0x00000010
    10d4:	000000d4 	0x000000d4
    10d8:	ffffefd8 	0xffffefd8
    10dc:	0000002c 	0x0000002c
    10e0:	00000000 	0x00000000
    10e4:	00000010 	0x00000010
    10e8:	000000e8 	0x000000e8
    10ec:	ffffeff0 	0xffffeff0
    10f0:	0000000c 	0x0000000c
    10f4:	00000000 	0x00000000
    10f8:	00000010 	0x00000010
    10fc:	000000fc 	0x000000fc
    1100:	ffffefe8 	0xffffefe8
    1104:	00000014 	0x00000014
    1108:	00000000 	0x00000000
    110c:	00000010 	0x00000010
    1110:	00000110 	0x00000110
    1114:	ffffefe8 	0xffffefe8
    1118:	00000010 	0x00000010
    111c:	00000000 	0x00000000
    1120:	00000010 	0x00000010
    1124:	00000124 	0x00000124
    1128:	ffffefe4 	0xffffefe4
    112c:	00000010 	0x00000010
    1130:	00000000 	0x00000000
    1134:	00000010 	0x00000010
    1138:	00000138 	0x00000138
    113c:	ffffefe0 	0xffffefe0
    1140:	0000000c 	0x0000000c
    1144:	00000000 	0x00000000
    1148:	00000010 	0x00000010
    114c:	0000014c 	0x0000014c
    1150:	ffffefd8 	0xffffefd8
    1154:	00000018 	0x00000018
    1158:	00000000 	0x00000000
    115c:	00000010 	0x00000010
    1160:	00000160 	0x00000160
    1164:	ffffefdc 	0xffffefdc
    1168:	00000010 	0x00000010
    116c:	00000000 	0x00000000
    1170:	00000018 	0x00000018
    1174:	00000174 	0x00000174
    1178:	ffffefd8 	0xffffefd8
    117c:	00000024 	0x00000024
    1180:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1184:	54028144 	bl          	84935296(0x5100280)	# 0x5101404
    1188:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    118c:	00000010 	0x00000010
    1190:	00000190 	0x00000190
    1194:	ffffefe0 	0xffffefe0
    1198:	00000014 	0x00000014
    119c:	00000000 	0x00000000
    11a0:	00000010 	0x00000010
    11a4:	000001a4 	0x000001a4
    11a8:	ffffefe0 	0xffffefe0
    11ac:	00000014 	0x00000014
    11b0:	00000000 	0x00000000
    11b4:	00000010 	0x00000010
    11b8:	000001b8 	0x000001b8
    11bc:	ffffefe0 	0xffffefe0
    11c0:	00000010 	0x00000010
    11c4:	00000000 	0x00000000
    11c8:	00000010 	0x00000010
    11cc:	000001cc 	0x000001cc
    11d0:	ffffefdc 	0xffffefdc
    11d4:	00000010 	0x00000010
    11d8:	00000000 	0x00000000
    11dc:	00000010 	0x00000010
    11e0:	000001e0 	0x000001e0
    11e4:	ffffefd8 	0xffffefd8
    11e8:	0000000c 	0x0000000c
    11ec:	00000000 	0x00000000
    11f0:	00000010 	0x00000010
    11f4:	000001f4 	0x000001f4
    11f8:	ffffefd0 	0xffffefd0
    11fc:	00000010 	0x00000010
    1200:	00000000 	0x00000000
    1204:	00000010 	0x00000010
    1208:	00000208 	0x00000208
    120c:	ffffefcc 	0xffffefcc
    1210:	00000010 	0x00000010
    1214:	00000000 	0x00000000
    1218:	00000010 	0x00000010
    121c:	0000021c 	0x0000021c
    1220:	ffffefc8 	0xffffefc8
    1224:	00000014 	0x00000014
    1228:	00000000 	0x00000000
    122c:	00000010 	0x00000010
    1230:	00000230 	0x00000230
    1234:	ffffefc8 	0xffffefc8
    1238:	00000010 	0x00000010
    123c:	00000000 	0x00000000
    1240:	00000010 	0x00000010
    1244:	00000244 	0x00000244
    1248:	ffffefc4 	0xffffefc4
    124c:	00000010 	0x00000010
    1250:	00000000 	0x00000000
    1254:	00000010 	0x00000010
    1258:	00000258 	0x00000258
    125c:	ffffefc0 	0xffffefc0
    1260:	00000014 	0x00000014
    1264:	00000000 	0x00000000
    1268:	00000014 	0x00000014
    126c:	0000026c 	0x0000026c
    1270:	ffffefc0 	0xffffefc0
    1274:	00000010 	0x00000010
	...
    1280:	00000018 	0x00000018
    1284:	00000284 	0x00000284
    1288:	ffffefb8 	0xffffefb8
    128c:	00000024 	0x00000024
    1290:	200e4400 	ll.w        	$zero, $zero, 3652(0xe44)
    1294:	54028144 	bl          	84935296(0x5100280)	# 0x5101514
    1298:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    129c:	00000028 	0x00000028
    12a0:	000002a0 	0x000002a0
    12a4:	ffffefc0 	0xffffefc0
    12a8:	000000e0 	0x000000e0
    12ac:	300e4400 	0x300e4400
    12b0:	97028150 	0x97028150
    12b4:	99069804 	0x99069804
    12b8:	0aa80208 	0x0aa80208
    12bc:	44d744c1 	bnez        	$a2, 317252(0x4d744)	# 0x4ea00
    12c0:	44d944d8 	bnez        	$a2, -2041532(0x60d944)	# 0xffffffffffe0ec04
    12c4:	0b44000e 	0x0b44000e
    12c8:	0000003c 	0x0000003c
    12cc:	000002cc 	0x000002cc
    12d0:	fffff074 	0xfffff074
    12d4:	00000240 	0x00000240
    12d8:	500e4400 	b           	3652(0xe44)	# 0x211c
    12dc:	97028164 	0x97028164
    12e0:	99069804 	0x99069804
    12e4:	9b0a9a08 	0x9b0a9a08
    12e8:	9d0e9c0c 	0x9d0e9c0c
    12ec:	03129e10 	lu52i.d     	$t4, $t4, 1191(0x4a7)
    12f0:	44c101f0 	bnez        	$t3, -4144896(0x40c100)	# 0xffffffffffc0d3f0
    12f4:	44d844d7 	bnez        	$a2, -2303932(0x5cd844)	# 0xffffffffffdceb38
    12f8:	44da44d9 	bnez        	$a2, -1779132(0x64da44)	# 0xffffffffffe4ed3c
    12fc:	44dc44db 	bnez        	$a2, -1254332(0x6cdc44)	# 0xffffffffffecef40
    1300:	44de44dd 	bnez        	$a2, -729532(0x74de44)	# 0xfffffffffff4f144
    1304:	0000000e 	0x0000000e
    1308:	00000018 	0x00000018
    130c:	0000030c 	0x0000030c
    1310:	fffff274 	0xfffff274
    1314:	00000038 	0x00000038
    1318:	500e4400 	b           	3652(0xe44)	# 0x215c
    131c:	680e8144 	bltu        	$a6, $a0, 3712(0xe80)	# 0x219c
    1320:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1324:	00000018 	0x00000018
    1328:	00000328 	0x00000328
    132c:	fffff290 	0xfffff290
    1330:	00000044 	0x00000044
    1334:	600e4400 	bltz        	$zero, 3652(0xe44)	# 0x2178
    1338:	74128144 	0x74128144
    133c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1340:	00000020 	0x00000020
    1344:	00000344 	0x00000344
    1348:	fffff2b8 	0xfffff2b8
    134c:	00000090 	0x00000090
    1350:	200e4400 	ll.w        	$zero, $zero, 3652(0xe44)
    1354:	97028148 	0x97028148
    1358:	c10a7404 	0xc10a7404
    135c:	0e44d744 	0x0e44d744
    1360:	000b4400 	0x000b4400
    1364:	00000018 	0x00000018
    1368:	00000368 	0x00000368
    136c:	fffff324 	0xfffff324
    1370:	00000030 	0x00000030
    1374:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1378:	60028144 	blt         	$a6, $a0, 640(0x280)	# 0x15f8
    137c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1380:	00000020 	0x00000020
    1384:	00000384 	0x00000384
    1388:	fffff338 	0xfffff338
    138c:	000000dc 	0x000000dc
    1390:	500e4400 	b           	3652(0xe44)	# 0x21d4
    1394:	97028148 	0x97028148
    1398:	0a6c0204 	0x0a6c0204
    139c:	44d744c1 	bnez        	$a2, 317252(0x4d744)	# 0x4eae0
    13a0:	0b44000e 	0x0b44000e
	...
