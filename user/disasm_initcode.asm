
user/initcode-la:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
       0:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
       4:	29c02061 	st.d        	$ra, $sp, 8(0x8)
       8:	1a000004 	pcalau12i   	$a0, 0
       c:	02dec084 	addi.d      	$a0, $a0, 1968(0x7b0)
      10:	5405b400 	bl          	1460(0x5b4)	# 0x5c4
      14:	54022400 	bl          	548(0x224)	# 0x238
      18:	00150004 	move        	$a0, $zero
      1c:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
      20:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
      24:	4c000020 	jirl        	$zero, $ra, 0
      28:	0280e00b 	addi.w      	$a7, $zero, 56(0x38)
      2c:	02860007 	addi.w      	$a3, $zero, 384(0x180)
      30:	002b0000 	syscall     	0x0
      34:	00408084 	slli.w      	$a0, $a0, 0x0
      38:	4c000020 	jirl        	$zero, $ra, 0
      3c:	0280e40b 	addi.w      	$a7, $zero, 57(0x39)
      40:	002b0000 	syscall     	0x0
      44:	00408084 	slli.w      	$a0, $a0, 0x0
      48:	4c000020 	jirl        	$zero, $ra, 0
      4c:	0280fc0b 	addi.w      	$a7, $zero, 63(0x3f)
      50:	002b0000 	syscall     	0x0
      54:	4c000020 	jirl        	$zero, $ra, 0
      58:	0281000b 	addi.w      	$a7, $zero, 64(0x40)
      5c:	002b0000 	syscall     	0x0
      60:	4c000020 	jirl        	$zero, $ra, 0
      64:	0282b00b 	addi.w      	$a7, $zero, 172(0xac)
      68:	002b0000 	syscall     	0x0
      6c:	00408084 	slli.w      	$a0, $a0, 0x0
      70:	4c000020 	jirl        	$zero, $ra, 0
      74:	0282b40b 	addi.w      	$a7, $zero, 173(0xad)
      78:	002b0000 	syscall     	0x0
      7c:	00408084 	slli.w      	$a0, $a0, 0x0
      80:	4c000020 	jirl        	$zero, $ra, 0
      84:	0281f00b 	addi.w      	$a7, $zero, 124(0x7c)
      88:	002b0000 	syscall     	0x0
      8c:	00408084 	slli.w      	$a0, $a0, 0x0
      90:	4c000020 	jirl        	$zero, $ra, 0
      94:	0283700b 	addi.w      	$a7, $zero, 220(0xdc)
      98:	02804404 	addi.w      	$a0, $zero, 17(0x11)
      9c:	00150005 	move        	$a1, $zero
      a0:	00150006 	move        	$a2, $zero
      a4:	00150007 	move        	$a3, $zero
      a8:	00150008 	move        	$a4, $zero
      ac:	002b0000 	syscall     	0x0
      b0:	00408084 	slli.w      	$a0, $a0, 0x0
      b4:	4c000020 	jirl        	$zero, $ra, 0
      b8:	001500c5 	move        	$a1, $a2
      bc:	00150106 	move        	$a2, $a4
      c0:	400008a0 	beqz        	$a1, 8(0x8)	# 0xc8
      c4:	00109ca5 	add.d       	$a1, $a1, $a3
      c8:	0283700b 	addi.w      	$a7, $zero, 220(0xdc)
      cc:	00150007 	move        	$a3, $zero
      d0:	00150008 	move        	$a4, $zero
      d4:	00150009 	move        	$a5, $zero
      d8:	002b0000 	syscall     	0x0
      dc:	00408084 	slli.w      	$a0, $a0, 0x0
      e0:	4c000020 	jirl        	$zero, $ra, 0
      e4:	0281740b 	addi.w      	$a7, $zero, 93(0x5d)
      e8:	002b0000 	syscall     	0x0
      ec:	4c000020 	jirl        	$zero, $ra, 0
      f0:	0284100b 	addi.w      	$a7, $zero, 260(0x104)
      f4:	00150007 	move        	$a3, $zero
      f8:	002b0000 	syscall     	0x0
      fc:	00408084 	slli.w      	$a0, $a0, 0x0
     100:	4c000020 	jirl        	$zero, $ra, 0
     104:	0283740b 	addi.w      	$a7, $zero, 221(0xdd)
     108:	002b0000 	syscall     	0x0
     10c:	00408084 	slli.w      	$a0, $a0, 0x0
     110:	4c000020 	jirl        	$zero, $ra, 0
     114:	0283740b 	addi.w      	$a7, $zero, 221(0xdd)
     118:	002b0000 	syscall     	0x0
     11c:	00408084 	slli.w      	$a0, $a0, 0x0
     120:	4c000020 	jirl        	$zero, $ra, 0
     124:	0282640b 	addi.w      	$a7, $zero, 153(0x99)
     128:	002b0000 	syscall     	0x0
     12c:	4c000020 	jirl        	$zero, $ra, 0
     130:	0015008c 	move        	$t0, $a0
     134:	0280340b 	addi.w      	$a7, $zero, 13(0xd)
     138:	00df0084 	bstrpick.d  	$a0, $a0, 0x1f, 0x0
     13c:	002b0000 	syscall     	0x0
     140:	00131184 	maskeqz     	$a0, $t0, $a0
     144:	4c000020 	jirl        	$zero, $ra, 0
     148:	02835c0b 	addi.w      	$a7, $zero, 215(0xd7)
     14c:	002b0000 	syscall     	0x0
     150:	00408084 	slli.w      	$a0, $a0, 0x0
     154:	4c000020 	jirl        	$zero, $ra, 0
     158:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
     15c:	29c02061 	st.d        	$ra, $sp, 8(0x8)
     160:	00150006 	move        	$a2, $zero
     164:	00150085 	move        	$a1, $a0
     168:	02bffc04 	addi.w      	$a0, $zero, -1(0xfff)
     16c:	57ff87ff 	bl          	-124(0xfffff84)	# 0xf0
     170:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
     174:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
     178:	4c000020 	jirl        	$zero, $ra, 0
     17c:	0280940b 	addi.w      	$a7, $zero, 37(0x25)
     180:	00df0108 	bstrpick.d  	$a4, $a4, 0x1f, 0x0
     184:	002b0000 	syscall     	0x0
     188:	00408084 	slli.w      	$a0, $a0, 0x0
     18c:	4c000020 	jirl        	$zero, $ra, 0
     190:	02808c0b 	addi.w      	$a7, $zero, 35(0x23)
     194:	00df00c6 	bstrpick.d  	$a2, $a2, 0x1f, 0x0
     198:	002b0000 	syscall     	0x0
     19c:	00408084 	slli.w      	$a0, $a0, 0x0
     1a0:	4c000020 	jirl        	$zero, $ra, 0
     1a4:	0282800b 	addi.w      	$a7, $zero, 160(0xa0)
     1a8:	002b0000 	syscall     	0x0
     1ac:	00408084 	slli.w      	$a0, $a0, 0x0
     1b0:	4c000020 	jirl        	$zero, $ra, 0
     1b4:	0283580b 	addi.w      	$a7, $zero, 214(0xd6)
     1b8:	002b0000 	syscall     	0x0
     1bc:	00408084 	slli.w      	$a0, $a0, 0x0
     1c0:	4c000020 	jirl        	$zero, $ra, 0
     1c4:	0280440b 	addi.w      	$a7, $zero, 17(0x11)
     1c8:	002b0000 	syscall     	0x0
     1cc:	4c000020 	jirl        	$zero, $ra, 0
     1d0:	0280c40b 	addi.w      	$a7, $zero, 49(0x31)
     1d4:	002b0000 	syscall     	0x0
     1d8:	00408084 	slli.w      	$a0, $a0, 0x0
     1dc:	4c000020 	jirl        	$zero, $ra, 0
     1e0:	0280f40b 	addi.w      	$a7, $zero, 61(0x3d)
     1e4:	002b0000 	syscall     	0x0
     1e8:	00408084 	slli.w      	$a0, $a0, 0x0
     1ec:	4c000020 	jirl        	$zero, $ra, 0
     1f0:	0280ec0b 	addi.w      	$a7, $zero, 59(0x3b)
     1f4:	00150005 	move        	$a1, $zero
     1f8:	002b0000 	syscall     	0x0
     1fc:	00408084 	slli.w      	$a0, $a0, 0x0
     200:	4c000020 	jirl        	$zero, $ra, 0
     204:	02805c0b 	addi.w      	$a7, $zero, 23(0x17)
     208:	002b0000 	syscall     	0x0
     20c:	00408084 	slli.w      	$a0, $a0, 0x0
     210:	4c000020 	jirl        	$zero, $ra, 0
     214:	0280a00b 	addi.w      	$a7, $zero, 40(0x28)
     218:	002b0000 	syscall     	0x0
     21c:	00408084 	slli.w      	$a0, $a0, 0x0
     220:	4c000020 	jirl        	$zero, $ra, 0
     224:	02809c0b 	addi.w      	$a7, $zero, 39(0x27)
     228:	00150005 	move        	$a1, $zero
     22c:	002b0000 	syscall     	0x0
     230:	00408084 	slli.w      	$a0, $a0, 0x0
     234:	4c000020 	jirl        	$zero, $ra, 0
     238:	02804c0b 	addi.w      	$a7, $zero, 19(0x13)
     23c:	002b0000 	syscall     	0x0
     240:	00408084 	slli.w      	$a0, $a0, 0x0
     244:	4c000020 	jirl        	$zero, $ra, 0
     248:	02ff8063 	addi.d      	$sp, $sp, -32(0xfe0)
     24c:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     250:	29003c65 	st.b        	$a1, $sp, 15(0xf)
     254:	02800406 	addi.w      	$a2, $zero, 1(0x1)
     258:	02c03c65 	addi.d      	$a1, $sp, 15(0xf)
     25c:	57fdffff 	bl          	-516(0xffffdfc)	# 0x58
     260:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     264:	02c08063 	addi.d      	$sp, $sp, 32(0x20)
     268:	4c000020 	jirl        	$zero, $ra, 0
     26c:	02ff4063 	addi.d      	$sp, $sp, -48(0xfd0)
     270:	29c0a061 	st.d        	$ra, $sp, 40(0x28)
     274:	29c08077 	st.d        	$s0, $sp, 32(0x20)
     278:	29c06078 	st.d        	$s1, $sp, 24(0x18)
     27c:	29c04079 	st.d        	$s2, $sp, 16(0x10)
     280:	00150097 	move        	$s0, $a0
     284:	400008e0 	beqz        	$a3, 8(0x8)	# 0x28c
     288:	6000b4a0 	bltz        	$a1, 180(0xb4)	# 0x33c
     28c:	004080a5 	slli.w      	$a1, $a1, 0x0
     290:	00150011 	move        	$t5, $zero
     294:	00150079 	move        	$s2, $sp
     298:	0015032e 	move        	$t2, $s2
     29c:	0015000c 	move        	$t0, $zero
     2a0:	004080c6 	slli.w      	$a2, $a2, 0x0
     2a4:	1a00002f 	pcalau12i   	$t3, 1(0x1)
     2a8:	02e421ef 	addi.d      	$t3, $t3, -1784(0x908)
     2ac:	00150190 	move        	$t4, $t0
     2b0:	0280058c 	addi.w      	$t0, $t0, 1(0x1)
     2b4:	002198ad 	mod.wu      	$t1, $a1, $a2
     2b8:	00df01ad 	bstrpick.d  	$t1, $t1, 0x1f, 0x0
     2bc:	382035ed 	ldx.bu      	$t1, $t3, $t1
     2c0:	290001cd 	st.b        	$t1, $t2, 0
     2c4:	004080ad 	slli.w      	$t1, $a1, 0x0
     2c8:	002118a5 	div.wu      	$a1, $a1, $a2
     2cc:	02c005ce 	addi.d      	$t2, $t2, 1(0x1)
     2d0:	6fffdda6 	bgeu        	$t1, $a2, -36(0x3ffdc)	# 0x2ac
     2d4:	40001a20 	beqz        	$t5, 24(0x18)	# 0x2ec
     2d8:	02c0418c 	addi.d      	$t0, $t0, 16(0x10)
     2dc:	00108d8c 	add.d       	$t0, $t0, $sp
     2e0:	0280b40d 	addi.w      	$t1, $zero, 45(0x2d)
     2e4:	293fc18d 	st.b        	$t1, $t0, -16(0xff0)
     2e8:	02800a0c 	addi.w      	$t0, $t4, 2(0x2)
     2ec:	6400380c 	blez        	$t0, 56(0x38)	# 0x324
     2f0:	02c0418d 	addi.d      	$t1, $t0, 16(0x10)
     2f4:	00108db8 	add.d       	$s1, $t1, $sp
     2f8:	02ffbf18 	addi.d      	$s1, $s1, -17(0xfef)
     2fc:	02fffb39 	addi.d      	$s2, $s2, -2(0xffe)
     300:	0010b339 	add.d       	$s2, $s2, $t0
     304:	02bffd8c 	addi.w      	$t0, $t0, -1(0xfff)
     308:	00df018c 	bstrpick.d  	$t0, $t0, 0x1f, 0x0
     30c:	0011b339 	sub.d       	$s2, $s2, $t0
     310:	28000305 	ld.b        	$a1, $s1, 0
     314:	001502e4 	move        	$a0, $s0
     318:	57ff33ff 	bl          	-208(0xfffff30)	# 0x248
     31c:	02ffff18 	addi.d      	$s1, $s1, -1(0xfff)
     320:	5ffff319 	bne         	$s1, $s2, -16(0x3fff0)	# 0x310
     324:	28c0a061 	ld.d        	$ra, $sp, 40(0x28)
     328:	28c08077 	ld.d        	$s0, $sp, 32(0x20)
     32c:	28c06078 	ld.d        	$s1, $sp, 24(0x18)
     330:	28c04079 	ld.d        	$s2, $sp, 16(0x10)
     334:	02c0c063 	addi.d      	$sp, $sp, 48(0x30)
     338:	4c000020 	jirl        	$zero, $ra, 0
     33c:	00111405 	sub.w       	$a1, $zero, $a1
     340:	004080a5 	slli.w      	$a1, $a1, 0x0
     344:	02800411 	addi.w      	$t5, $zero, 1(0x1)
     348:	53ff4fff 	b           	-180(0xfffff4c)	# 0x294
     34c:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     350:	29c12061 	st.d        	$ra, $sp, 72(0x48)
     354:	29c10077 	st.d        	$s0, $sp, 64(0x40)
     358:	29c0e078 	st.d        	$s1, $sp, 56(0x38)
     35c:	29c0c079 	st.d        	$s2, $sp, 48(0x30)
     360:	29c0a07a 	st.d        	$s3, $sp, 40(0x28)
     364:	29c0807b 	st.d        	$s4, $sp, 32(0x20)
     368:	29c0607c 	st.d        	$s5, $sp, 24(0x18)
     36c:	29c0407d 	st.d        	$s6, $sp, 16(0x10)
     370:	29c0207e 	st.d        	$s7, $sp, 8(0x8)
     374:	280000b7 	ld.b        	$s0, $a1, 0
     378:	4001eae0 	beqz        	$s0, 488(0x1e8)	# 0x560
     37c:	0015009a 	move        	$s3, $a0
     380:	001500dc 	move        	$s5, $a2
     384:	02c004b8 	addi.d      	$s1, $a1, 1(0x1)
     388:	00150019 	move        	$s2, $zero
     38c:	0280941b 	addi.w      	$s4, $zero, 37(0x25)
     390:	0280541d 	addi.w      	$s6, $zero, 21(0x15)
     394:	50002400 	b           	36(0x24)	# 0x3b8
     398:	001502e5 	move        	$a1, $s0
     39c:	00150344 	move        	$a0, $s3
     3a0:	57feabff 	bl          	-344(0xffffea8)	# 0x248
     3a4:	50000800 	b           	8(0x8)	# 0x3ac
     3a8:	58002b3b 	beq         	$s2, $s4, 40(0x28)	# 0x3d0
     3ac:	02c00718 	addi.d      	$s1, $s1, 1(0x1)
     3b0:	283fff17 	ld.b        	$s0, $s1, -1(0xfff)
     3b4:	4001aee0 	beqz        	$s0, 428(0x1ac)	# 0x560
     3b8:	0343feed 	andi        	$t1, $s0, 0xff
     3bc:	006782ec 	bstrpick.w  	$t0, $s0, 0x7, 0x0
     3c0:	47ffeb3f 	bnez        	$s2, -24(0x7fffe8)	# 0x3a8
     3c4:	5fffd59b 	bne         	$t0, $s4, -44(0x3ffd4)	# 0x398
     3c8:	00150199 	move        	$s2, $t0
     3cc:	53ffe3ff 	b           	-32(0xfffffe0)	# 0x3ac
     3d0:	5801519b 	beq         	$t0, $s4, 336(0x150)	# 0x520
     3d4:	02be75ac 	addi.w      	$t0, $t1, -99(0xf9d)
     3d8:	68015fac 	bltu        	$s6, $t0, 348(0x15c)	# 0x534
     3dc:	02be75ad 	addi.w      	$t1, $t1, -99(0xf9d)
     3e0:	680157ac 	bltu        	$s6, $t0, 340(0x154)	# 0x534
     3e4:	00df01ad 	bstrpick.d  	$t1, $t1, 0x1f, 0x0
     3e8:	002d01ad 	alsl.d      	$t1, $t1, $zero, 0x3
     3ec:	1a00002c 	pcalau12i   	$t0, 1(0x1)
     3f0:	02e1618c 	addi.d      	$t0, $t0, -1960(0x858)
     3f4:	380c358d 	ldx.d       	$t1, $t0, $t1
     3f8:	0010b58c 	add.d       	$t0, $t0, $t1
     3fc:	4c000180 	jirl        	$zero, $t0, 0
     400:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     404:	02800407 	addi.w      	$a3, $zero, 1(0x1)
     408:	02802806 	addi.w      	$a2, $zero, 10(0xa)
     40c:	24000385 	ldptr.w     	$a1, $s5, 0
     410:	00150344 	move        	$a0, $s3
     414:	57fe5bff 	bl          	-424(0xffffe58)	# 0x26c
     418:	001502fc 	move        	$s5, $s0
     41c:	00150019 	move        	$s2, $zero
     420:	53ff8fff 	b           	-116(0xfffff8c)	# 0x3ac
     424:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     428:	00150007 	move        	$a3, $zero
     42c:	02802806 	addi.w      	$a2, $zero, 10(0xa)
     430:	24000385 	ldptr.w     	$a1, $s5, 0
     434:	00150344 	move        	$a0, $s3
     438:	57fe37ff 	bl          	-460(0xffffe34)	# 0x26c
     43c:	001502fc 	move        	$s5, $s0
     440:	00150019 	move        	$s2, $zero
     444:	53ff6bff 	b           	-152(0xfffff68)	# 0x3ac
     448:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     44c:	00150007 	move        	$a3, $zero
     450:	02804006 	addi.w      	$a2, $zero, 16(0x10)
     454:	24000385 	ldptr.w     	$a1, $s5, 0
     458:	00150344 	move        	$a0, $s3
     45c:	57fe13ff 	bl          	-496(0xffffe10)	# 0x26c
     460:	001502fc 	move        	$s5, $s0
     464:	00150019 	move        	$s2, $zero
     468:	53ff47ff 	b           	-188(0xfffff44)	# 0x3ac
     46c:	02c0239e 	addi.d      	$s7, $s5, 8(0x8)
     470:	26000399 	ldptr.d     	$s2, $s5, 0
     474:	0280c005 	addi.w      	$a1, $zero, 48(0x30)
     478:	00150344 	move        	$a0, $s3
     47c:	57fdcfff 	bl          	-564(0xffffdcc)	# 0x248
     480:	0281e005 	addi.w      	$a1, $zero, 120(0x78)
     484:	00150344 	move        	$a0, $s3
     488:	57fdc3ff 	bl          	-576(0xffffdc0)	# 0x248
     48c:	02804017 	addi.w      	$s0, $zero, 16(0x10)
     490:	1a00003c 	pcalau12i   	$s5, 1(0x1)
     494:	02e4239c 	addi.d      	$s5, $s5, -1784(0x908)
     498:	0045f32c 	srli.d      	$t0, $s2, 0x3c
     49c:	38003385 	ldx.b       	$a1, $s5, $t0
     4a0:	00150344 	move        	$a0, $s3
     4a4:	57fda7ff 	bl          	-604(0xffffda4)	# 0x248
     4a8:	00411339 	slli.d      	$s2, $s2, 0x4
     4ac:	02fffef7 	addi.d      	$s0, $s0, -1(0xfff)
     4b0:	47ffeaff 	bnez        	$s0, -24(0x7fffe8)	# 0x498
     4b4:	001503dc 	move        	$s5, $s7
     4b8:	00150019 	move        	$s2, $zero
     4bc:	53fef3ff 	b           	-272(0xffffef0)	# 0x3ac
     4c0:	02c02399 	addi.d      	$s2, $s5, 8(0x8)
     4c4:	26000397 	ldptr.d     	$s0, $s5, 0
     4c8:	40002ee0 	beqz        	$s0, 44(0x2c)	# 0x4f4
     4cc:	280002e5 	ld.b        	$a1, $s0, 0
     4d0:	400084a0 	beqz        	$a1, 132(0x84)	# 0x554
     4d4:	00150344 	move        	$a0, $s3
     4d8:	57fd73ff 	bl          	-656(0xffffd70)	# 0x248
     4dc:	02c006f7 	addi.d      	$s0, $s0, 1(0x1)
     4e0:	280002e5 	ld.b        	$a1, $s0, 0
     4e4:	47fff0bf 	bnez        	$a1, -16(0x7ffff0)	# 0x4d4
     4e8:	0015033c 	move        	$s5, $s2
     4ec:	00150019 	move        	$s2, $zero
     4f0:	53febfff 	b           	-324(0xffffebc)	# 0x3ac
     4f4:	1a000017 	pcalau12i   	$s0, 0
     4f8:	02df02f7 	addi.d      	$s0, $s0, 1984(0x7c0)
     4fc:	0280a005 	addi.w      	$a1, $zero, 40(0x28)
     500:	53ffd7ff 	b           	-44(0xfffffd4)	# 0x4d4
     504:	02c02397 	addi.d      	$s0, $s5, 8(0x8)
     508:	28000385 	ld.b        	$a1, $s5, 0
     50c:	00150344 	move        	$a0, $s3
     510:	57fd3bff 	bl          	-712(0xffffd38)	# 0x248
     514:	001502fc 	move        	$s5, $s0
     518:	00150019 	move        	$s2, $zero
     51c:	53fe93ff 	b           	-368(0xffffe90)	# 0x3ac
     520:	001502e5 	move        	$a1, $s0
     524:	00150344 	move        	$a0, $s3
     528:	57fd23ff 	bl          	-736(0xffffd20)	# 0x248
     52c:	00150019 	move        	$s2, $zero
     530:	53fe7fff 	b           	-388(0xffffe7c)	# 0x3ac
     534:	02809405 	addi.w      	$a1, $zero, 37(0x25)
     538:	00150344 	move        	$a0, $s3
     53c:	57fd0fff 	bl          	-756(0xffffd0c)	# 0x248
     540:	001502e5 	move        	$a1, $s0
     544:	00150344 	move        	$a0, $s3
     548:	57fd03ff 	bl          	-768(0xffffd00)	# 0x248
     54c:	00150019 	move        	$s2, $zero
     550:	53fe5fff 	b           	-420(0xffffe5c)	# 0x3ac
     554:	0015033c 	move        	$s5, $s2
     558:	00150019 	move        	$s2, $zero
     55c:	53fe53ff 	b           	-432(0xffffe50)	# 0x3ac
     560:	28c12061 	ld.d        	$ra, $sp, 72(0x48)
     564:	28c10077 	ld.d        	$s0, $sp, 64(0x40)
     568:	28c0e078 	ld.d        	$s1, $sp, 56(0x38)
     56c:	28c0c079 	ld.d        	$s2, $sp, 48(0x30)
     570:	28c0a07a 	ld.d        	$s3, $sp, 40(0x28)
     574:	28c0807b 	ld.d        	$s4, $sp, 32(0x20)
     578:	28c0607c 	ld.d        	$s5, $sp, 24(0x18)
     57c:	28c0407d 	ld.d        	$s6, $sp, 16(0x10)
     580:	28c0207e 	ld.d        	$s7, $sp, 8(0x8)
     584:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     588:	4c000020 	jirl        	$zero, $ra, 0
     58c:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     590:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     594:	29c08066 	st.d        	$a2, $sp, 32(0x20)
     598:	29c0a067 	st.d        	$a3, $sp, 40(0x28)
     59c:	29c0c068 	st.d        	$a4, $sp, 48(0x30)
     5a0:	29c0e069 	st.d        	$a5, $sp, 56(0x38)
     5a4:	29c1006a 	st.d        	$a6, $sp, 64(0x40)
     5a8:	29c1206b 	st.d        	$a7, $sp, 72(0x48)
     5ac:	02c08066 	addi.d      	$a2, $sp, 32(0x20)
     5b0:	29c02066 	st.d        	$a2, $sp, 8(0x8)
     5b4:	57fd9bff 	bl          	-616(0xffffd98)	# 0x34c
     5b8:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     5bc:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     5c0:	4c000020 	jirl        	$zero, $ra, 0
     5c4:	02fe8063 	addi.d      	$sp, $sp, -96(0xfa0)
     5c8:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     5cc:	29c0a065 	st.d        	$a1, $sp, 40(0x28)
     5d0:	29c0c066 	st.d        	$a2, $sp, 48(0x30)
     5d4:	29c0e067 	st.d        	$a3, $sp, 56(0x38)
     5d8:	29c10068 	st.d        	$a4, $sp, 64(0x40)
     5dc:	29c12069 	st.d        	$a5, $sp, 72(0x48)
     5e0:	29c1406a 	st.d        	$a6, $sp, 80(0x50)
     5e4:	29c1606b 	st.d        	$a7, $sp, 88(0x58)
     5e8:	02c0a066 	addi.d      	$a2, $sp, 40(0x28)
     5ec:	29c02066 	st.d        	$a2, $sp, 8(0x8)
     5f0:	00150085 	move        	$a1, $a0
     5f4:	02800404 	addi.w      	$a0, $zero, 1(0x1)
     5f8:	57fd57ff 	bl          	-684(0xffffd54)	# 0x34c
     5fc:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     600:	02c18063 	addi.d      	$sp, $sp, 96(0x60)
     604:	4c000020 	jirl        	$zero, $ra, 0
     608:	02ff8063 	addi.d      	$sp, $sp, -32(0xfe0)
     60c:	29c06061 	st.d        	$ra, $sp, 24(0x18)
     610:	29c04077 	st.d        	$s0, $sp, 16(0x10)
     614:	00150097 	move        	$s0, $a0
     618:	57fa7fff 	bl          	-1412(0xffffa7c)	# 0x94
     61c:	60003880 	bltz        	$a0, 56(0x38)	# 0x654
     620:	44005480 	bnez        	$a0, 84(0x54)	# 0x674
     624:	00150006 	move        	$a2, $zero
     628:	00150005 	move        	$a1, $zero
     62c:	001502e4 	move        	$a0, $s0
     630:	57fae7ff 	bl          	-1308(0xffffae4)	# 0x114
     634:	60003080 	bltz        	$a0, 48(0x30)	# 0x664
     638:	00150004 	move        	$a0, $zero
     63c:	57faabff 	bl          	-1368(0xffffaa8)	# 0xe4
     640:	00150004 	move        	$a0, $zero
     644:	28c06061 	ld.d        	$ra, $sp, 24(0x18)
     648:	28c04077 	ld.d        	$s0, $sp, 16(0x10)
     64c:	02c08063 	addi.d      	$sp, $sp, 32(0x20)
     650:	4c000020 	jirl        	$zero, $ra, 0
     654:	1a000004 	pcalau12i   	$a0, 0
     658:	02df2084 	addi.d      	$a0, $a0, 1992(0x7c8)
     65c:	57ff6bff 	bl          	-152(0xfffff68)	# 0x5c4
     660:	53ffe3ff 	b           	-32(0xfffffe0)	# 0x640
     664:	1a000004 	pcalau12i   	$a0, 0
     668:	02df6084 	addi.d      	$a0, $a0, 2008(0x7d8)
     66c:	57ff5bff 	bl          	-168(0xfffff58)	# 0x5c4
     670:	53ffcbff 	b           	-56(0xfffffc8)	# 0x638
     674:	02be700c 	addi.w      	$t0, $zero, -100(0xf9c)
     678:	2980306c 	st.w        	$t0, $sp, 12(0xc)
     67c:	02c03064 	addi.d      	$a0, $sp, 12(0xc)
     680:	57fadbff 	bl          	-1320(0xffffad8)	# 0x158
     684:	67ffbc80 	bgez        	$a0, -68(0x3ffbc)	# 0x640
     688:	1a000004 	pcalau12i   	$a0, 0
     68c:	02dfa084 	addi.d      	$a0, $a0, 2024(0x7e8)
     690:	57ff37ff 	bl          	-204(0xfffff34)	# 0x5c4
     694:	53ffafff 	b           	-84(0xfffffac)	# 0x640
     698:	02ffc063 	addi.d      	$sp, $sp, -16(0xff0)
     69c:	29c02061 	st.d        	$ra, $sp, 8(0x8)
     6a0:	1a000004 	pcalau12i   	$a0, 0
     6a4:	02dfe084 	addi.d      	$a0, $a0, 2040(0x7f8)
     6a8:	57fb2bff 	bl          	-1240(0xffffb28)	# 0x1d0
     6ac:	1a000024 	pcalau12i   	$a0, 1(0x1)
     6b0:	02e04084 	addi.d      	$a0, $a0, -2032(0x810)
     6b4:	57ff57ff 	bl          	-172(0xfffff54)	# 0x608
     6b8:	00150004 	move        	$a0, $zero
     6bc:	28c02061 	ld.d        	$ra, $sp, 8(0x8)
     6c0:	02c04063 	addi.d      	$sp, $sp, 16(0x10)
     6c4:	4c000020 	jirl        	$zero, $ra, 0
     6c8:	02fec063 	addi.d      	$sp, $sp, -80(0xfb0)
     6cc:	29c12061 	st.d        	$ra, $sp, 72(0x48)
     6d0:	29c10077 	st.d        	$s0, $sp, 64(0x40)
     6d4:	57f9c3ff 	bl          	-1600(0xffff9c0)	# 0x94
     6d8:	60007480 	bltz        	$a0, 116(0x74)	# 0x74c
     6dc:	00150097 	move        	$s0, $a0
     6e0:	44009880 	bnez        	$a0, 152(0x98)	# 0x778
     6e4:	1a000024 	pcalau12i   	$a0, 1(0x1)
     6e8:	02e0a084 	addi.d      	$a0, $a0, -2008(0x828)
     6ec:	57fae7ff 	bl          	-1308(0xffffae4)	# 0x1d0
     6f0:	29c02060 	st.d        	$zero, $sp, 8(0x8)
     6f4:	29c04060 	st.d        	$zero, $sp, 16(0x10)
     6f8:	29c06060 	st.d        	$zero, $sp, 24(0x18)
     6fc:	29c08060 	st.d        	$zero, $sp, 32(0x20)
     700:	29c0a060 	st.d        	$zero, $sp, 40(0x28)
     704:	29c0c060 	st.d        	$zero, $sp, 48(0x30)
     708:	29c0e060 	st.d        	$zero, $sp, 56(0x38)
     70c:	1a00002c 	pcalau12i   	$t0, 1(0x1)
     710:	02e0e18c 	addi.d      	$t0, $t0, -1992(0x838)
     714:	2700006c 	stptr.d     	$t0, $sp, 0
     718:	00150006 	move        	$a2, $zero
     71c:	00150065 	move        	$a1, $sp
     720:	1a000024 	pcalau12i   	$a0, 1(0x1)
     724:	02e10084 	addi.d      	$a0, $a0, -1984(0x840)
     728:	57f9efff 	bl          	-1556(0xffff9ec)	# 0x114
     72c:	60003480 	bltz        	$a0, 52(0x34)	# 0x760
     730:	00150004 	move        	$a0, $zero
     734:	57f9b3ff 	bl          	-1616(0xffff9b0)	# 0xe4
     738:	001502e4 	move        	$a0, $s0
     73c:	28c12061 	ld.d        	$ra, $sp, 72(0x48)
     740:	28c10077 	ld.d        	$s0, $sp, 64(0x40)
     744:	02c14063 	addi.d      	$sp, $sp, 80(0x50)
     748:	4c000020 	jirl        	$zero, $ra, 0
     74c:	1a000024 	pcalau12i   	$a0, 1(0x1)
     750:	02e06084 	addi.d      	$a0, $a0, -2024(0x818)
     754:	57fe73ff 	bl          	-400(0xffffe70)	# 0x5c4
     758:	02bffc17 	addi.w      	$s0, $zero, -1(0xfff)
     75c:	53ffdfff 	b           	-36(0xfffffdc)	# 0x738
     760:	1a000024 	pcalau12i   	$a0, 1(0x1)
     764:	02e12084 	addi.d      	$a0, $a0, -1976(0x848)
     768:	57fe5fff 	bl          	-420(0xffffe5c)	# 0x5c4
     76c:	02800404 	addi.w      	$a0, $zero, 1(0x1)
     770:	57f977ff 	bl          	-1676(0xffff974)	# 0xe4
     774:	53ffbfff 	b           	-68(0xfffffbc)	# 0x730
     778:	02be700c 	addi.w      	$t0, $zero, -100(0xf9c)
     77c:	2500006c 	stptr.w     	$t0, $sp, 0
     780:	00150064 	move        	$a0, $sp
     784:	57f9d7ff 	bl          	-1580(0xffff9d4)	# 0x158
     788:	60000c80 	bltz        	$a0, 12(0xc)	# 0x794
     78c:	00150017 	move        	$s0, $zero
     790:	53ffabff 	b           	-88(0xfffffa8)	# 0x738
     794:	1a000004 	pcalau12i   	$a0, 0
     798:	02dfa084 	addi.d      	$a0, $a0, 2024(0x7e8)
     79c:	57fe2bff 	bl          	-472(0xffffe28)	# 0x5c4
     7a0:	53ffefff 	b           	-20(0xfffffec)	# 0x78c
	...
     7b0:	6c6c6548 	bgeu        	$a6, $a4, 27748(0x6c64)	# 0x7414
     7b4:	57202c6f 	bl          	29302828(0x1bf202c)	# 0x1bf27e0
     7b8:	646c726f 	bge         	$t7, $t3, 27760(0x6c70)	# 0x7428
     7bc:	00000a21 	0x00000a21
     7c0:	6c756e28 	bgeu        	$t5, $a4, 30060(0x756c)	# 0x7d2c
     7c4:	0000296c 	cto.d       	$t0, $a7
     7c8:	6b726f66 	bltu        	$s4, $a2, -36244(0x3726c)	# 0xffffffffffff7a34
     7cc:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16930
     7d0:	0064656c 	bstrins.w   	$t0, $a7, 0x4, 0x19
     7d4:	00000000 	0x00000000
     7d8:	63657865 	blt         	$sp, $a1, -39560(0x36578)	# 0xffffffffffff6d50
     7dc:	66206576 	bge         	$a7, $fp, -122780(0x22064)	# 0xfffffffffffe2840
     7e0:	656c6961 	bge         	$a7, $ra, 93288(0x16c68)	# 0x17448
     7e4:	00000064 	0x00000064
     7e8:	74696177 	0x74696177
     7ec:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16950
     7f0:	00000a6c 	0x00000a6c
     7f4:	00000000 	0x00000000
     7f8:	746e6d2f 	0x746e6d2f
     7fc:	73756d2f 	0x73756d2f
     800:	61622f6c 	blt         	$s4, $t0, 90668(0x1622c)	# 0x16a2c
     804:	2f636973 	0x2f636973
	...
     810:	70616d6d 	0x70616d6d
     814:	00000000 	0x00000000
     818:	6b726f66 	bltu        	$s4, $a2, -36244(0x3726c)	# 0xffffffffffff7a84
     81c:	69616620 	bltu        	$t5, $zero, 90468(0x16164)	# 0x16980
     820:	0a64656c 	0x0a64656c
     824:	00000000 	0x00000000
     828:	746e6d2f 	0x746e6d2f
     82c:	696c672f 	bltu        	$s2, $t3, 93284(0x16c64)	# 0x17490
     830:	002f6362 	0x002f6362
     834:	00000000 	0x00000000
     838:	00687361 	bstrins.w   	$ra, $s4, 0x8, 0x1c
     83c:	00000000 	0x00000000
     840:	79737562 	0x79737562
     844:	00786f62 	bstrins.w   	$tp, $s4, 0x18, 0x1b
     848:	63657865 	blt         	$sp, $a1, -39560(0x36578)	# 0xffffffffffff6dc0
     84c:	66206576 	bge         	$a7, $fp, -122780(0x22064)	# 0xfffffffffffe28b0
     850:	656c6961 	bge         	$a7, $ra, 93288(0x16c68)	# 0x174b8
     854:	00000a64 	0x00000a64
     858:	fffffcac 	0xfffffcac
     85c:	ffffffff 	0xffffffff
     860:	fffffba8 	0xfffffba8
     864:	ffffffff 	0xffffffff
     868:	fffffcdc 	0xfffffcdc
     86c:	ffffffff 	0xffffffff
     870:	fffffcdc 	0xfffffcdc
     874:	ffffffff 	0xffffffff
     878:	fffffcdc 	0xfffffcdc
     87c:	ffffffff 	0xffffffff
     880:	fffffcdc 	0xfffffcdc
     884:	ffffffff 	0xffffffff
     888:	fffffcdc 	0xfffffcdc
     88c:	ffffffff 	0xffffffff
     890:	fffffcdc 	0xfffffcdc
     894:	ffffffff 	0xffffffff
     898:	fffffcdc 	0xfffffcdc
     89c:	ffffffff 	0xffffffff
     8a0:	fffffbcc 	0xfffffbcc
     8a4:	ffffffff 	0xffffffff
     8a8:	fffffcdc 	0xfffffcdc
     8ac:	ffffffff 	0xffffffff
     8b0:	fffffcdc 	0xfffffcdc
     8b4:	ffffffff 	0xffffffff
     8b8:	fffffcdc 	0xfffffcdc
     8bc:	ffffffff 	0xffffffff
     8c0:	fffffc14 	0xfffffc14
     8c4:	ffffffff 	0xffffffff
     8c8:	fffffcdc 	0xfffffcdc
     8cc:	ffffffff 	0xffffffff
     8d0:	fffffcdc 	0xfffffcdc
     8d4:	ffffffff 	0xffffffff
     8d8:	fffffc68 	0xfffffc68
     8dc:	ffffffff 	0xffffffff
     8e0:	fffffcdc 	0xfffffcdc
     8e4:	ffffffff 	0xffffffff
     8e8:	fffffcdc 	0xfffffcdc
     8ec:	ffffffff 	0xffffffff
     8f0:	fffffcdc 	0xfffffcdc
     8f4:	ffffffff 	0xffffffff
     8f8:	fffffcdc 	0xfffffcdc
     8fc:	ffffffff 	0xffffffff
     900:	fffffbf0 	0xfffffbf0
     904:	ffffffff 	0xffffffff
     908:	33323130 	0x33323130
     90c:	37363534 	0x37363534
     910:	42413938 	beqz        	$a5, -1949384(0x624138)	# 0xffffffffffe24a48
     914:	46454443 	bnez        	$tp, 935236(0xe4544)	# 0xe4e58
	...
    1000:	00000010 	0x00000010
    1004:	00000000 	0x00000000
    1008:	00527a01 	0x00527a01
    100c:	01017c01 	fadd.d      	$fa1, $fa0, $fs7
    1010:	00030d1b 	0x00030d1b
    1014:	00000018 	0x00000018
    1018:	00000018 	0x00000018
    101c:	ffffefe4 	0xffffefe4
    1020:	00000028 	0x00000028
    1024:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1028:	58028144 	beq         	$a6, $a0, 640(0x280)	# 0x12a8
    102c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1030:	00000010 	0x00000010
    1034:	00000034 	0x00000034
    1038:	ffffeff0 	0xffffeff0
    103c:	00000014 	0x00000014
    1040:	00000000 	0x00000000
    1044:	00000010 	0x00000010
    1048:	00000048 	0x00000048
    104c:	ffffeff0 	0xffffeff0
    1050:	00000010 	0x00000010
    1054:	00000000 	0x00000000
    1058:	00000010 	0x00000010
    105c:	0000005c 	0x0000005c
    1060:	ffffefec 	0xffffefec
    1064:	0000000c 	0x0000000c
    1068:	00000000 	0x00000000
    106c:	00000010 	0x00000010
    1070:	00000070 	0x00000070
    1074:	ffffefe4 	0xffffefe4
    1078:	0000000c 	0x0000000c
    107c:	00000000 	0x00000000
    1080:	00000010 	0x00000010
    1084:	00000084 	0x00000084
    1088:	ffffefdc 	0xffffefdc
    108c:	00000010 	0x00000010
    1090:	00000000 	0x00000000
    1094:	00000010 	0x00000010
    1098:	00000098 	0x00000098
    109c:	ffffefd8 	0xffffefd8
    10a0:	00000010 	0x00000010
    10a4:	00000000 	0x00000000
    10a8:	00000010 	0x00000010
    10ac:	000000ac 	0x000000ac
    10b0:	ffffefd4 	0xffffefd4
    10b4:	00000010 	0x00000010
    10b8:	00000000 	0x00000000
    10bc:	00000010 	0x00000010
    10c0:	000000c0 	0x000000c0
    10c4:	ffffefd0 	0xffffefd0
    10c8:	00000024 	0x00000024
    10cc:	00000000 	0x00000000
    10d0:	00000010 	0x00000010
    10d4:	000000d4 	0x000000d4
    10d8:	ffffefe0 	0xffffefe0
    10dc:	0000002c 	0x0000002c
    10e0:	00000000 	0x00000000
    10e4:	00000010 	0x00000010
    10e8:	000000e8 	0x000000e8
    10ec:	ffffeff8 	0xffffeff8
    10f0:	0000000c 	0x0000000c
    10f4:	00000000 	0x00000000
    10f8:	00000010 	0x00000010
    10fc:	000000fc 	0x000000fc
    1100:	ffffeff0 	0xffffeff0
    1104:	00000014 	0x00000014
    1108:	00000000 	0x00000000
    110c:	00000010 	0x00000010
    1110:	00000110 	0x00000110
    1114:	ffffeff0 	0xffffeff0
    1118:	00000010 	0x00000010
    111c:	00000000 	0x00000000
    1120:	00000010 	0x00000010
    1124:	00000124 	0x00000124
    1128:	ffffefec 	0xffffefec
    112c:	00000010 	0x00000010
    1130:	00000000 	0x00000000
    1134:	00000010 	0x00000010
    1138:	00000138 	0x00000138
    113c:	ffffefe8 	0xffffefe8
    1140:	0000000c 	0x0000000c
    1144:	00000000 	0x00000000
    1148:	00000010 	0x00000010
    114c:	0000014c 	0x0000014c
    1150:	ffffefe0 	0xffffefe0
    1154:	00000018 	0x00000018
    1158:	00000000 	0x00000000
    115c:	00000010 	0x00000010
    1160:	00000160 	0x00000160
    1164:	ffffefe4 	0xffffefe4
    1168:	00000010 	0x00000010
    116c:	00000000 	0x00000000
    1170:	00000018 	0x00000018
    1174:	00000174 	0x00000174
    1178:	ffffefe0 	0xffffefe0
    117c:	00000024 	0x00000024
    1180:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1184:	54028144 	bl          	84935296(0x5100280)	# 0x5101404
    1188:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    118c:	00000010 	0x00000010
    1190:	00000190 	0x00000190
    1194:	ffffefe8 	0xffffefe8
    1198:	00000014 	0x00000014
    119c:	00000000 	0x00000000
    11a0:	00000010 	0x00000010
    11a4:	000001a4 	0x000001a4
    11a8:	ffffefe8 	0xffffefe8
    11ac:	00000014 	0x00000014
    11b0:	00000000 	0x00000000
    11b4:	00000010 	0x00000010
    11b8:	000001b8 	0x000001b8
    11bc:	ffffefe8 	0xffffefe8
    11c0:	00000010 	0x00000010
    11c4:	00000000 	0x00000000
    11c8:	00000010 	0x00000010
    11cc:	000001cc 	0x000001cc
    11d0:	ffffefe4 	0xffffefe4
    11d4:	00000010 	0x00000010
    11d8:	00000000 	0x00000000
    11dc:	00000010 	0x00000010
    11e0:	000001e0 	0x000001e0
    11e4:	ffffefe0 	0xffffefe0
    11e8:	0000000c 	0x0000000c
    11ec:	00000000 	0x00000000
    11f0:	00000010 	0x00000010
    11f4:	000001f4 	0x000001f4
    11f8:	ffffefd8 	0xffffefd8
    11fc:	00000010 	0x00000010
    1200:	00000000 	0x00000000
    1204:	00000010 	0x00000010
    1208:	00000208 	0x00000208
    120c:	ffffefd4 	0xffffefd4
    1210:	00000010 	0x00000010
    1214:	00000000 	0x00000000
    1218:	00000010 	0x00000010
    121c:	0000021c 	0x0000021c
    1220:	ffffefd0 	0xffffefd0
    1224:	00000014 	0x00000014
    1228:	00000000 	0x00000000
    122c:	00000010 	0x00000010
    1230:	00000230 	0x00000230
    1234:	ffffefd0 	0xffffefd0
    1238:	00000010 	0x00000010
    123c:	00000000 	0x00000000
    1240:	00000010 	0x00000010
    1244:	00000244 	0x00000244
    1248:	ffffefcc 	0xffffefcc
    124c:	00000010 	0x00000010
    1250:	00000000 	0x00000000
    1254:	00000010 	0x00000010
    1258:	00000258 	0x00000258
    125c:	ffffefc8 	0xffffefc8
    1260:	00000014 	0x00000014
    1264:	00000000 	0x00000000
    1268:	00000014 	0x00000014
    126c:	0000026c 	0x0000026c
    1270:	ffffefc8 	0xffffefc8
    1274:	00000010 	0x00000010
	...
    1280:	00000018 	0x00000018
    1284:	00000284 	0x00000284
    1288:	ffffefc0 	0xffffefc0
    128c:	00000024 	0x00000024
    1290:	200e4400 	ll.w        	$zero, $zero, 3652(0xe44)
    1294:	54028144 	bl          	84935296(0x5100280)	# 0x5101514
    1298:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    129c:	00000028 	0x00000028
    12a0:	000002a0 	0x000002a0
    12a4:	ffffefc8 	0xffffefc8
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
    12d0:	fffff07c 	0xfffff07c
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
    1310:	fffff27c 	0xfffff27c
    1314:	00000038 	0x00000038
    1318:	500e4400 	b           	3652(0xe44)	# 0x215c
    131c:	680e8144 	bltu        	$a6, $a0, 3712(0xe80)	# 0x219c
    1320:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1324:	00000018 	0x00000018
    1328:	00000328 	0x00000328
    132c:	fffff298 	0xfffff298
    1330:	00000044 	0x00000044
    1334:	600e4400 	bltz        	$zero, 3652(0xe44)	# 0x2178
    1338:	74128144 	0x74128144
    133c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1340:	00000020 	0x00000020
    1344:	00000344 	0x00000344
    1348:	fffff2c0 	0xfffff2c0
    134c:	00000090 	0x00000090
    1350:	200e4400 	ll.w        	$zero, $zero, 3652(0xe44)
    1354:	97028148 	0x97028148
    1358:	c10a7404 	0xc10a7404
    135c:	0e44d744 	0x0e44d744
    1360:	000b4400 	0x000b4400
    1364:	00000018 	0x00000018
    1368:	00000368 	0x00000368
    136c:	fffff32c 	0xfffff32c
    1370:	00000030 	0x00000030
    1374:	100e4400 	addu16i.d   	$zero, $zero, 913(0x391)
    1378:	60028144 	blt         	$a6, $a0, 640(0x280)	# 0x15f8
    137c:	000e44c1 	bytepick.d  	$ra, $a2, $t5, 0x4
    1380:	00000020 	0x00000020
    1384:	00000384 	0x00000384
    1388:	fffff340 	0xfffff340
    138c:	000000dc 	0x000000dc
    1390:	500e4400 	b           	3652(0xe44)	# 0x21d4
    1394:	97028148 	0x97028148
    1398:	0a6c0204 	0x0a6c0204
    139c:	44d744c1 	bnez        	$a2, 317252(0x4d744)	# 0x4eae0
    13a0:	0b44000e 	0x0b44000e
	...
