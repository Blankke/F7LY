
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	00000517          	auipc	a0,0x0
   8:	41450513          	addi	a0,a0,1044 # 0x418
   c:	3e8000ef          	jal	ra,0x3f4
  10:	459d                	li	a1,7
  12:	00000517          	auipc	a0,0x0
  16:	41650513          	addi	a0,a0,1046 # 0x428
  1a:	3da000ef          	jal	ra,0x3f4
  1e:	55fd                	li	a1,-1
  20:	00000517          	auipc	a0,0x0
  24:	40850513          	addi	a0,a0,1032 # 0x428
  28:	3cc000ef          	jal	ra,0x3f4
  2c:	156000ef          	jal	ra,0x182
  30:	4501                	li	a0,0
  32:	60a2                	ld	ra,8(sp)
  34:	0141                	addi	sp,sp,16
  36:	8082                	ret
  38:	03800893          	li	a7,56
  3c:	18000693          	li	a3,384
  40:	00000073          	ecall
  44:	2501                	sext.w	a0,a0
  46:	8082                	ret
  48:	03900893          	li	a7,57
  4c:	00000073          	ecall
  50:	2501                	sext.w	a0,a0
  52:	8082                	ret
  54:	03f00893          	li	a7,63
  58:	00000073          	ecall
  5c:	8082                	ret
  5e:	04000893          	li	a7,64
  62:	00000073          	ecall
  66:	8082                	ret
  68:	0ac00893          	li	a7,172
  6c:	00000073          	ecall
  70:	2501                	sext.w	a0,a0
  72:	8082                	ret
  74:	0ad00893          	li	a7,173
  78:	00000073          	ecall
  7c:	2501                	sext.w	a0,a0
  7e:	8082                	ret
  80:	07c00893          	li	a7,124
  84:	00000073          	ecall
  88:	2501                	sext.w	a0,a0
  8a:	8082                	ret
  8c:	85b2                	mv	a1,a2
  8e:	863a                	mv	a2,a4
  90:	c191                	beqz	a1,0x94
  92:	95b6                	add	a1,a1,a3
  94:	0dc00893          	li	a7,220
  98:	4681                	li	a3,0
  9a:	4701                	li	a4,0
  9c:	4781                	li	a5,0
  9e:	00000073          	ecall
  a2:	2501                	sext.w	a0,a0
  a4:	8082                	ret
  a6:	05d00893          	li	a7,93
  aa:	00000073          	ecall
  ae:	8082                	ret
  b0:	10400893          	li	a7,260
  b4:	4681                	li	a3,0
  b6:	00000073          	ecall
  ba:	2501                	sext.w	a0,a0
  bc:	8082                	ret
  be:	0dd00893          	li	a7,221
  c2:	00000073          	ecall
  c6:	2501                	sext.w	a0,a0
  c8:	8082                	ret
  ca:	0dd00893          	li	a7,221
  ce:	00000073          	ecall
  d2:	2501                	sext.w	a0,a0
  d4:	8082                	ret
  d6:	09900893          	li	a7,153
  da:	00000073          	ecall
  de:	8082                	ret
  e0:	0d700893          	li	a7,215
  e4:	00000073          	ecall
  e8:	2501                	sext.w	a0,a0
  ea:	8082                	ret
  ec:	1141                	addi	sp,sp,-16
  ee:	e406                	sd	ra,8(sp)
  f0:	85aa                	mv	a1,a0
  f2:	4601                	li	a2,0
  f4:	557d                	li	a0,-1
  f6:	fbbff0ef          	jal	ra,0xb0
  fa:	60a2                	ld	ra,8(sp)
  fc:	0141                	addi	sp,sp,16
  fe:	8082                	ret
 100:	02500893          	li	a7,37
 104:	1702                	slli	a4,a4,0x20
 106:	9301                	srli	a4,a4,0x20
 108:	00000073          	ecall
 10c:	2501                	sext.w	a0,a0
 10e:	8082                	ret
 110:	02300893          	li	a7,35
 114:	1602                	slli	a2,a2,0x20
 116:	9201                	srli	a2,a2,0x20
 118:	00000073          	ecall
 11c:	2501                	sext.w	a0,a0
 11e:	8082                	ret
 120:	0a000893          	li	a7,160
 124:	00000073          	ecall
 128:	2501                	sext.w	a0,a0
 12a:	8082                	ret
 12c:	0d600893          	li	a7,214
 130:	00000073          	ecall
 134:	2501                	sext.w	a0,a0
 136:	8082                	ret
 138:	03100893          	li	a7,49
 13c:	00000073          	ecall
 140:	2501                	sext.w	a0,a0
 142:	8082                	ret
 144:	03d00893          	li	a7,61
 148:	00000073          	ecall
 14c:	2501                	sext.w	a0,a0
 14e:	8082                	ret
 150:	03b00893          	li	a7,59
 154:	4581                	li	a1,0
 156:	00000073          	ecall
 15a:	2501                	sext.w	a0,a0
 15c:	8082                	ret
 15e:	48dd                	li	a7,23
 160:	00000073          	ecall
 164:	2501                	sext.w	a0,a0
 166:	8082                	ret
 168:	02800893          	li	a7,40
 16c:	00000073          	ecall
 170:	2501                	sext.w	a0,a0
 172:	8082                	ret
 174:	02700893          	li	a7,39
 178:	4581                	li	a1,0
 17a:	00000073          	ecall
 17e:	2501                	sext.w	a0,a0
 180:	8082                	ret
 182:	48cd                	li	a7,19
 184:	00000073          	ecall
 188:	2501                	sext.w	a0,a0
 18a:	8082                	ret
 18c:	1101                	addi	sp,sp,-32
 18e:	ec06                	sd	ra,24(sp)
 190:	00b107a3          	sb	a1,15(sp)
 194:	4605                	li	a2,1
 196:	00f10593          	addi	a1,sp,15
 19a:	ec5ff0ef          	jal	ra,0x5e
 19e:	60e2                	ld	ra,24(sp)
 1a0:	6105                	addi	sp,sp,32
 1a2:	8082                	ret
 1a4:	7179                	addi	sp,sp,-48
 1a6:	f406                	sd	ra,40(sp)
 1a8:	f022                	sd	s0,32(sp)
 1aa:	ec26                	sd	s1,24(sp)
 1ac:	e84a                	sd	s2,16(sp)
 1ae:	84aa                	mv	s1,a0
 1b0:	c299                	beqz	a3,0x1b6
 1b2:	0805c163          	bltz	a1,0x234
 1b6:	2581                	sext.w	a1,a1
 1b8:	4881                	li	a7,0
 1ba:	890a                	mv	s2,sp
 1bc:	86ca                	mv	a3,s2
 1be:	4701                	li	a4,0
 1c0:	2601                	sext.w	a2,a2
 1c2:	49000513          	li	a0,1168
 1c6:	883a                	mv	a6,a4
 1c8:	2705                	addiw	a4,a4,1
 1ca:	02c5f7bb          	remuw	a5,a1,a2
 1ce:	1782                	slli	a5,a5,0x20
 1d0:	9381                	srli	a5,a5,0x20
 1d2:	97aa                	add	a5,a5,a0
 1d4:	0007c783          	lbu	a5,0(a5)
 1d8:	00f68023          	sb	a5,0(a3)
 1dc:	0005879b          	sext.w	a5,a1
 1e0:	02c5d5bb          	divuw	a1,a1,a2
 1e4:	0685                	addi	a3,a3,1
 1e6:	fec7f0e3          	bgeu	a5,a2,0x1c6
 1ea:	00088c63          	beqz	a7,0x202
 1ee:	01070793          	addi	a5,a4,16
 1f2:	00278733          	add	a4,a5,sp
 1f6:	02d00793          	li	a5,45
 1fa:	fef70823          	sb	a5,-16(a4)
 1fe:	0028071b          	addiw	a4,a6,2
 202:	02e05363          	blez	a4,0x228
 206:	840a                	mv	s0,sp
 208:	943a                	add	s0,s0,a4
 20a:	197d                	addi	s2,s2,-1
 20c:	993a                	add	s2,s2,a4
 20e:	377d                	addiw	a4,a4,-1
 210:	1702                	slli	a4,a4,0x20
 212:	9301                	srli	a4,a4,0x20
 214:	40e90933          	sub	s2,s2,a4
 218:	fff44583          	lbu	a1,-1(s0)
 21c:	8526                	mv	a0,s1
 21e:	f6fff0ef          	jal	ra,0x18c
 222:	147d                	addi	s0,s0,-1
 224:	ff241ae3          	bne	s0,s2,0x218
 228:	70a2                	ld	ra,40(sp)
 22a:	7402                	ld	s0,32(sp)
 22c:	64e2                	ld	s1,24(sp)
 22e:	6942                	ld	s2,16(sp)
 230:	6145                	addi	sp,sp,48
 232:	8082                	ret
 234:	40b005bb          	negw	a1,a1
 238:	4885                	li	a7,1
 23a:	b741                	j	0x1ba
 23c:	7159                	addi	sp,sp,-112
 23e:	f486                	sd	ra,104(sp)
 240:	f0a2                	sd	s0,96(sp)
 242:	eca6                	sd	s1,88(sp)
 244:	e8ca                	sd	s2,80(sp)
 246:	e4ce                	sd	s3,72(sp)
 248:	e0d2                	sd	s4,64(sp)
 24a:	fc56                	sd	s5,56(sp)
 24c:	f85a                	sd	s6,48(sp)
 24e:	f45e                	sd	s7,40(sp)
 250:	f062                	sd	s8,32(sp)
 252:	ec66                	sd	s9,24(sp)
 254:	e86a                	sd	s10,16(sp)
 256:	e46e                	sd	s11,8(sp)
 258:	0005c483          	lbu	s1,0(a1)
 25c:	14048e63          	beqz	s1,0x3b8
 260:	8a2a                	mv	s4,a0
 262:	8db2                	mv	s11,a2
 264:	00158413          	addi	s0,a1,1
 268:	4901                	li	s2,0
 26a:	02500993          	li	s3,37
 26e:	4c01                	li	s8,0
 270:	4b55                	li	s6,21
 272:	43800b93          	li	s7,1080
 276:	02800c93          	li	s9,40
 27a:	49000a93          	li	s5,1168
 27e:	a829                	j	0x298
 280:	85a6                	mv	a1,s1
 282:	8552                	mv	a0,s4
 284:	f09ff0ef          	jal	ra,0x18c
 288:	a019                	j	0x28e
 28a:	01390d63          	beq	s2,s3,0x2a4
 28e:	0405                	addi	s0,s0,1
 290:	fff44483          	lbu	s1,-1(s0)
 294:	12048263          	beqz	s1,0x3b8
 298:	fe0919e3          	bnez	s2,0x28a
 29c:	ff3492e3          	bne	s1,s3,0x280
 2a0:	894e                	mv	s2,s3
 2a2:	b7f5                	j	0x28e
 2a4:	0f348763          	beq	s1,s3,0x392
 2a8:	f9d4879b          	addiw	a5,s1,-99
 2ac:	0ff7f793          	andi	a5,a5,255
 2b0:	0efb6763          	bltu	s6,a5,0x39e
 2b4:	f9d4879b          	addiw	a5,s1,-99
 2b8:	0ff7f713          	andi	a4,a5,255
 2bc:	0eeb6163          	bltu	s6,a4,0x39e
 2c0:	00271793          	slli	a5,a4,0x2
 2c4:	97de                	add	a5,a5,s7
 2c6:	439c                	lw	a5,0(a5)
 2c8:	97de                	add	a5,a5,s7
 2ca:	8782                	jr	a5
 2cc:	008d8493          	addi	s1,s11,8
 2d0:	4685                	li	a3,1
 2d2:	4629                	li	a2,10
 2d4:	000da583          	lw	a1,0(s11)
 2d8:	8552                	mv	a0,s4
 2da:	ecbff0ef          	jal	ra,0x1a4
 2de:	8da6                	mv	s11,s1
 2e0:	8962                	mv	s2,s8
 2e2:	b775                	j	0x28e
 2e4:	008d8493          	addi	s1,s11,8
 2e8:	86e2                	mv	a3,s8
 2ea:	4629                	li	a2,10
 2ec:	000da583          	lw	a1,0(s11)
 2f0:	8552                	mv	a0,s4
 2f2:	eb3ff0ef          	jal	ra,0x1a4
 2f6:	8da6                	mv	s11,s1
 2f8:	8962                	mv	s2,s8
 2fa:	bf51                	j	0x28e
 2fc:	008d8493          	addi	s1,s11,8
 300:	86e2                	mv	a3,s8
 302:	4641                	li	a2,16
 304:	000da583          	lw	a1,0(s11)
 308:	8552                	mv	a0,s4
 30a:	e9bff0ef          	jal	ra,0x1a4
 30e:	8da6                	mv	s11,s1
 310:	8962                	mv	s2,s8
 312:	bfb5                	j	0x28e
 314:	008d8d13          	addi	s10,s11,8
 318:	000db903          	ld	s2,0(s11)
 31c:	03000593          	li	a1,48
 320:	8552                	mv	a0,s4
 322:	e6bff0ef          	jal	ra,0x18c
 326:	07800593          	li	a1,120
 32a:	8552                	mv	a0,s4
 32c:	e61ff0ef          	jal	ra,0x18c
 330:	44c1                	li	s1,16
 332:	03c95793          	srli	a5,s2,0x3c
 336:	97d6                	add	a5,a5,s5
 338:	0007c583          	lbu	a1,0(a5)
 33c:	8552                	mv	a0,s4
 33e:	e4fff0ef          	jal	ra,0x18c
 342:	0912                	slli	s2,s2,0x4
 344:	14fd                	addi	s1,s1,-1
 346:	f4f5                	bnez	s1,0x332
 348:	8dea                	mv	s11,s10
 34a:	8962                	mv	s2,s8
 34c:	b789                	j	0x28e
 34e:	008d8913          	addi	s2,s11,8
 352:	000db483          	ld	s1,0(s11)
 356:	cc91                	beqz	s1,0x372
 358:	0004c583          	lbu	a1,0(s1)
 35c:	c9b9                	beqz	a1,0x3b2
 35e:	8552                	mv	a0,s4
 360:	e2dff0ef          	jal	ra,0x18c
 364:	0485                	addi	s1,s1,1
 366:	0004c583          	lbu	a1,0(s1)
 36a:	f9f5                	bnez	a1,0x35e
 36c:	8dca                	mv	s11,s2
 36e:	8962                	mv	s2,s8
 370:	bf39                	j	0x28e
 372:	00000497          	auipc	s1,0x0
 376:	0be48493          	addi	s1,s1,190 # 0x430
 37a:	85e6                	mv	a1,s9
 37c:	b7cd                	j	0x35e
 37e:	008d8493          	addi	s1,s11,8
 382:	000dc583          	lbu	a1,0(s11)
 386:	8552                	mv	a0,s4
 388:	e05ff0ef          	jal	ra,0x18c
 38c:	8da6                	mv	s11,s1
 38e:	8962                	mv	s2,s8
 390:	bdfd                	j	0x28e
 392:	85ce                	mv	a1,s3
 394:	8552                	mv	a0,s4
 396:	df7ff0ef          	jal	ra,0x18c
 39a:	8962                	mv	s2,s8
 39c:	bdcd                	j	0x28e
 39e:	85ce                	mv	a1,s3
 3a0:	8552                	mv	a0,s4
 3a2:	debff0ef          	jal	ra,0x18c
 3a6:	85a6                	mv	a1,s1
 3a8:	8552                	mv	a0,s4
 3aa:	de3ff0ef          	jal	ra,0x18c
 3ae:	8962                	mv	s2,s8
 3b0:	bdf9                	j	0x28e
 3b2:	8dca                	mv	s11,s2
 3b4:	8962                	mv	s2,s8
 3b6:	bde1                	j	0x28e
 3b8:	70a6                	ld	ra,104(sp)
 3ba:	7406                	ld	s0,96(sp)
 3bc:	64e6                	ld	s1,88(sp)
 3be:	6946                	ld	s2,80(sp)
 3c0:	69a6                	ld	s3,72(sp)
 3c2:	6a06                	ld	s4,64(sp)
 3c4:	7ae2                	ld	s5,56(sp)
 3c6:	7b42                	ld	s6,48(sp)
 3c8:	7ba2                	ld	s7,40(sp)
 3ca:	7c02                	ld	s8,32(sp)
 3cc:	6ce2                	ld	s9,24(sp)
 3ce:	6d42                	ld	s10,16(sp)
 3d0:	6da2                	ld	s11,8(sp)
 3d2:	6165                	addi	sp,sp,112
 3d4:	8082                	ret
 3d6:	715d                	addi	sp,sp,-80
 3d8:	ec06                	sd	ra,24(sp)
 3da:	f032                	sd	a2,32(sp)
 3dc:	f436                	sd	a3,40(sp)
 3de:	f83a                	sd	a4,48(sp)
 3e0:	fc3e                	sd	a5,56(sp)
 3e2:	e0c2                	sd	a6,64(sp)
 3e4:	e4c6                	sd	a7,72(sp)
 3e6:	1010                	addi	a2,sp,32
 3e8:	e432                	sd	a2,8(sp)
 3ea:	e53ff0ef          	jal	ra,0x23c
 3ee:	60e2                	ld	ra,24(sp)
 3f0:	6161                	addi	sp,sp,80
 3f2:	8082                	ret
 3f4:	711d                	addi	sp,sp,-96
 3f6:	ec06                	sd	ra,24(sp)
 3f8:	f42e                	sd	a1,40(sp)
 3fa:	f832                	sd	a2,48(sp)
 3fc:	fc36                	sd	a3,56(sp)
 3fe:	e0ba                	sd	a4,64(sp)
 400:	e4be                	sd	a5,72(sp)
 402:	e8c2                	sd	a6,80(sp)
 404:	ecc6                	sd	a7,88(sp)
 406:	1030                	addi	a2,sp,40
 408:	e432                	sd	a2,8(sp)
 40a:	85aa                	mv	a1,a0
 40c:	4505                	li	a0,1
 40e:	e2fff0ef          	jal	ra,0x23c
 412:	60e2                	ld	ra,24(sp)
 414:	6125                	addi	sp,sp,96
 416:	8082                	ret
 418:	6548                	ld	a0,136(a0)
 41a:	6c6c                	ld	a1,216(s0)
 41c:	57202c6f          	jal	s8,0x298e
 420:	646c726f          	jal	tp,0xc7a66
 424:	0a21                	addi	s4,s4,8
 426:	0000                	unimp
 428:	2061                	0x2061
 42a:	203d                	0x203d
 42c:	6425                	lui	s0,0x9
 42e:	000a                	c.slli	zero,0x2
 430:	6e28                	ld	a0,88(a2)
 432:	6c75                	lui	s8,0x1d
 434:	296c                	fld	fa1,208(a0)
 436:	0000                	unimp
 438:	ff46                	sd	a7,440(sp)
 43a:	ffff                	0xffff
 43c:	fe94                	sd	a3,56(a3)
 43e:	ffff                	0xffff
 440:	ff66                	sd	s9,440(sp)
 442:	ffff                	0xffff
 444:	ff66                	sd	s9,440(sp)
 446:	ffff                	0xffff
 448:	ff66                	sd	s9,440(sp)
 44a:	ffff                	0xffff
 44c:	ff66                	sd	s9,440(sp)
 44e:	ffff                	0xffff
 450:	ff66                	sd	s9,440(sp)
 452:	ffff                	0xffff
 454:	ff66                	sd	s9,440(sp)
 456:	ffff                	0xffff
 458:	ff66                	sd	s9,440(sp)
 45a:	ffff                	0xffff
 45c:	feac                	sd	a1,120(a3)
 45e:	ffff                	0xffff
 460:	ff66                	sd	s9,440(sp)
 462:	ffff                	0xffff
 464:	ff66                	sd	s9,440(sp)
 466:	ffff                	0xffff
 468:	ff66                	sd	s9,440(sp)
 46a:	ffff                	0xffff
 46c:	fedc                	sd	a5,184(a3)
 46e:	ffff                	0xffff
 470:	ff66                	sd	s9,440(sp)
 472:	ffff                	0xffff
 474:	ff66                	sd	s9,440(sp)
 476:	ffff                	0xffff
 478:	ff16                	sd	t0,440(sp)
 47a:	ffff                	0xffff
 47c:	ff66                	sd	s9,440(sp)
 47e:	ffff                	0xffff
 480:	ff66                	sd	s9,440(sp)
 482:	ffff                	0xffff
 484:	ff66                	sd	s9,440(sp)
 486:	ffff                	0xffff
 488:	ff66                	sd	s9,440(sp)
 48a:	ffff                	0xffff
 48c:	fec4                	sd	s1,184(a3)
 48e:	ffff                	0xffff
 490:	3130                	fld	fa2,96(a0)
 492:	3332                	fld	ft6,296(sp)
 494:	3534                	fld	fa3,104(a0)
 496:	3736                	fld	fa4,360(sp)
 498:	3938                	fld	fa4,112(a0)
 49a:	4241                	li	tp,16
 49c:	46454443          	fmadd.q	fs0,fa0,ft4,fs0,rmm
	...
