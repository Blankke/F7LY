
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	482000ef          	jal	ra,0x486
   8:	18e000ef          	jal	ra,0x196
   c:	4501                	li	a0,0
   e:	60a2                	ld	ra,8(sp)
  10:	0141                	addi	sp,sp,16
  12:	8082                	ret
  14:	03800893          	li	a7,56
  18:	18000693          	li	a3,384
  1c:	00000073          	ecall
  20:	2501                	sext.w	a0,a0
  22:	8082                	ret
  24:	03900893          	li	a7,57
  28:	00000073          	ecall
  2c:	2501                	sext.w	a0,a0
  2e:	8082                	ret
  30:	03f00893          	li	a7,63
  34:	00000073          	ecall
  38:	8082                	ret
  3a:	04000893          	li	a7,64
  3e:	00000073          	ecall
  42:	8082                	ret
  44:	0ac00893          	li	a7,172
  48:	00000073          	ecall
  4c:	2501                	sext.w	a0,a0
  4e:	8082                	ret
  50:	0ad00893          	li	a7,173
  54:	00000073          	ecall
  58:	2501                	sext.w	a0,a0
  5a:	8082                	ret
  5c:	07c00893          	li	a7,124
  60:	00000073          	ecall
  64:	2501                	sext.w	a0,a0
  66:	8082                	ret
  68:	0dc00893          	li	a7,220
  6c:	4545                	li	a0,17
  6e:	4581                	li	a1,0
  70:	4601                	li	a2,0
  72:	4681                	li	a3,0
  74:	4701                	li	a4,0
  76:	00000073          	ecall
  7a:	2501                	sext.w	a0,a0
  7c:	8082                	ret
  7e:	85b2                	mv	a1,a2
  80:	863a                	mv	a2,a4
  82:	c191                	beqz	a1,0x86
  84:	95b6                	add	a1,a1,a3
  86:	0dc00893          	li	a7,220
  8a:	4681                	li	a3,0
  8c:	4701                	li	a4,0
  8e:	4781                	li	a5,0
  90:	00000073          	ecall
  94:	2501                	sext.w	a0,a0
  96:	8082                	ret
  98:	05d00893          	li	a7,93
  9c:	00000073          	ecall
  a0:	8082                	ret
  a2:	10400893          	li	a7,260
  a6:	4681                	li	a3,0
  a8:	00000073          	ecall
  ac:	2501                	sext.w	a0,a0
  ae:	8082                	ret
  b0:	0dd00893          	li	a7,221
  b4:	00000073          	ecall
  b8:	2501                	sext.w	a0,a0
  ba:	8082                	ret
  bc:	0dd00893          	li	a7,221
  c0:	00000073          	ecall
  c4:	2501                	sext.w	a0,a0
  c6:	8082                	ret
  c8:	09900893          	li	a7,153
  cc:	00000073          	ecall
  d0:	8082                	ret
  d2:	872a                	mv	a4,a0
  d4:	48b5                	li	a7,13
  d6:	1502                	slli	a0,a0,0x20
  d8:	9101                	srli	a0,a0,0x20
  da:	00000073          	ecall
  de:	00a037b3          	snez	a5,a0
  e2:	40f007b3          	neg	a5,a5
  e6:	00f77533          	and	a0,a4,a5
  ea:	8082                	ret
  ec:	0d700893          	li	a7,215
  f0:	00000073          	ecall
  f4:	2501                	sext.w	a0,a0
  f6:	8082                	ret
  f8:	1141                	addi	sp,sp,-16
  fa:	e406                	sd	ra,8(sp)
  fc:	85aa                	mv	a1,a0
  fe:	4601                	li	a2,0
 100:	557d                	li	a0,-1
 102:	fa1ff0ef          	jal	ra,0xa2
 106:	60a2                	ld	ra,8(sp)
 108:	0141                	addi	sp,sp,16
 10a:	8082                	ret
 10c:	02500893          	li	a7,37
 110:	1702                	slli	a4,a4,0x20
 112:	9301                	srli	a4,a4,0x20
 114:	00000073          	ecall
 118:	2501                	sext.w	a0,a0
 11a:	8082                	ret
 11c:	02300893          	li	a7,35
 120:	1602                	slli	a2,a2,0x20
 122:	9201                	srli	a2,a2,0x20
 124:	00000073          	ecall
 128:	2501                	sext.w	a0,a0
 12a:	8082                	ret
 12c:	0a000893          	li	a7,160
 130:	00000073          	ecall
 134:	2501                	sext.w	a0,a0
 136:	8082                	ret
 138:	0d600893          	li	a7,214
 13c:	00000073          	ecall
 140:	2501                	sext.w	a0,a0
 142:	8082                	ret
 144:	48c5                	li	a7,17
 146:	00000073          	ecall
 14a:	8082                	ret
 14c:	03100893          	li	a7,49
 150:	00000073          	ecall
 154:	2501                	sext.w	a0,a0
 156:	8082                	ret
 158:	03d00893          	li	a7,61
 15c:	00000073          	ecall
 160:	2501                	sext.w	a0,a0
 162:	8082                	ret
 164:	03b00893          	li	a7,59
 168:	4581                	li	a1,0
 16a:	00000073          	ecall
 16e:	2501                	sext.w	a0,a0
 170:	8082                	ret
 172:	48dd                	li	a7,23
 174:	00000073          	ecall
 178:	2501                	sext.w	a0,a0
 17a:	8082                	ret
 17c:	02800893          	li	a7,40
 180:	00000073          	ecall
 184:	2501                	sext.w	a0,a0
 186:	8082                	ret
 188:	02700893          	li	a7,39
 18c:	4581                	li	a1,0
 18e:	00000073          	ecall
 192:	2501                	sext.w	a0,a0
 194:	8082                	ret
 196:	48cd                	li	a7,19
 198:	00000073          	ecall
 19c:	2501                	sext.w	a0,a0
 19e:	8082                	ret
 1a0:	1101                	addi	sp,sp,-32
 1a2:	ec06                	sd	ra,24(sp)
 1a4:	00b107a3          	sb	a1,15(sp)
 1a8:	4605                	li	a2,1
 1aa:	00f10593          	addi	a1,sp,15
 1ae:	e8dff0ef          	jal	ra,0x3a
 1b2:	60e2                	ld	ra,24(sp)
 1b4:	6105                	addi	sp,sp,32
 1b6:	8082                	ret
 1b8:	7179                	addi	sp,sp,-48
 1ba:	f406                	sd	ra,40(sp)
 1bc:	ec26                	sd	s1,24(sp)
 1be:	e84a                	sd	s2,16(sp)
 1c0:	84aa                	mv	s1,a0
 1c2:	c299                	beqz	a3,0x1c8
 1c4:	0805c263          	bltz	a1,0x248
 1c8:	2581                	sext.w	a1,a1
 1ca:	4881                	li	a7,0
 1cc:	890a                	mv	s2,sp
 1ce:	86ca                	mv	a3,s2
 1d0:	4701                	li	a4,0
 1d2:	2601                	sext.w	a2,a2
 1d4:	64000513          	li	a0,1600
 1d8:	883a                	mv	a6,a4
 1da:	2705                	addiw	a4,a4,1
 1dc:	02c5f7bb          	remuw	a5,a1,a2
 1e0:	1782                	slli	a5,a5,0x20
 1e2:	9381                	srli	a5,a5,0x20
 1e4:	97aa                	add	a5,a5,a0
 1e6:	0007c783          	lbu	a5,0(a5)
 1ea:	00f68023          	sb	a5,0(a3)
 1ee:	0005879b          	sext.w	a5,a1
 1f2:	02c5d5bb          	divuw	a1,a1,a2
 1f6:	0685                	addi	a3,a3,1
 1f8:	fec7f0e3          	bgeu	a5,a2,0x1d8
 1fc:	00088c63          	beqz	a7,0x214
 200:	01070793          	addi	a5,a4,16
 204:	00278733          	add	a4,a5,sp
 208:	02d00793          	li	a5,45
 20c:	fef70823          	sb	a5,-16(a4)
 210:	0028071b          	addiw	a4,a6,2
 214:	02e05563          	blez	a4,0x23e
 218:	f022                	sd	s0,32(sp)
 21a:	840a                	mv	s0,sp
 21c:	943a                	add	s0,s0,a4
 21e:	197d                	addi	s2,s2,-1
 220:	993a                	add	s2,s2,a4
 222:	377d                	addiw	a4,a4,-1
 224:	1702                	slli	a4,a4,0x20
 226:	9301                	srli	a4,a4,0x20
 228:	40e90933          	sub	s2,s2,a4
 22c:	fff44583          	lbu	a1,-1(s0)
 230:	8526                	mv	a0,s1
 232:	f6fff0ef          	jal	ra,0x1a0
 236:	147d                	addi	s0,s0,-1
 238:	ff241ae3          	bne	s0,s2,0x22c
 23c:	7402                	ld	s0,32(sp)
 23e:	70a2                	ld	ra,40(sp)
 240:	64e2                	ld	s1,24(sp)
 242:	6942                	ld	s2,16(sp)
 244:	6145                	addi	sp,sp,48
 246:	8082                	ret
 248:	40b005bb          	negw	a1,a1
 24c:	4885                	li	a7,1
 24e:	bfbd                	j	0x1cc
 250:	715d                	addi	sp,sp,-80
 252:	e486                	sd	ra,72(sp)
 254:	fc26                	sd	s1,56(sp)
 256:	0005c483          	lbu	s1,0(a1)
 25a:	16048c63          	beqz	s1,0x3d2
 25e:	e0a2                	sd	s0,64(sp)
 260:	f84a                	sd	s2,48(sp)
 262:	f44e                	sd	s3,40(sp)
 264:	f052                	sd	s4,32(sp)
 266:	ec56                	sd	s5,24(sp)
 268:	e85a                	sd	s6,16(sp)
 26a:	8a2a                	mv	s4,a0
 26c:	8b32                	mv	s6,a2
 26e:	00158413          	addi	s0,a1,1
 272:	4901                	li	s2,0
 274:	02500993          	li	s3,37
 278:	4ad5                	li	s5,21
 27a:	a829                	j	0x294
 27c:	85a6                	mv	a1,s1
 27e:	8552                	mv	a0,s4
 280:	f21ff0ef          	jal	ra,0x1a0
 284:	a019                	j	0x28a
 286:	01390d63          	beq	s2,s3,0x2a0
 28a:	0405                	addi	s0,s0,1
 28c:	fff44483          	lbu	s1,-1(s0)
 290:	12048b63          	beqz	s1,0x3c6
 294:	fe0919e3          	bnez	s2,0x286
 298:	ff3492e3          	bne	s1,s3,0x27c
 29c:	894e                	mv	s2,s3
 29e:	b7f5                	j	0x28a
 2a0:	0f348e63          	beq	s1,s3,0x39c
 2a4:	f9d4879b          	addiw	a5,s1,-99
 2a8:	0ff7f793          	andi	a5,a5,255
 2ac:	0efaef63          	bltu	s5,a5,0x3aa
 2b0:	f9d4879b          	addiw	a5,s1,-99
 2b4:	0ff7f713          	andi	a4,a5,255
 2b8:	0eeae963          	bltu	s5,a4,0x3aa
 2bc:	00271793          	slli	a5,a4,0x2
 2c0:	5e800713          	li	a4,1512
 2c4:	97ba                	add	a5,a5,a4
 2c6:	439c                	lw	a5,0(a5)
 2c8:	97ba                	add	a5,a5,a4
 2ca:	8782                	jr	a5
 2cc:	008b0493          	addi	s1,s6,8
 2d0:	4685                	li	a3,1
 2d2:	4629                	li	a2,10
 2d4:	000b2583          	lw	a1,0(s6)
 2d8:	8552                	mv	a0,s4
 2da:	edfff0ef          	jal	ra,0x1b8
 2de:	8b26                	mv	s6,s1
 2e0:	4901                	li	s2,0
 2e2:	b765                	j	0x28a
 2e4:	008b0493          	addi	s1,s6,8
 2e8:	4681                	li	a3,0
 2ea:	4629                	li	a2,10
 2ec:	000b2583          	lw	a1,0(s6)
 2f0:	8552                	mv	a0,s4
 2f2:	ec7ff0ef          	jal	ra,0x1b8
 2f6:	8b26                	mv	s6,s1
 2f8:	4901                	li	s2,0
 2fa:	bf41                	j	0x28a
 2fc:	008b0493          	addi	s1,s6,8
 300:	4681                	li	a3,0
 302:	4641                	li	a2,16
 304:	000b2583          	lw	a1,0(s6)
 308:	8552                	mv	a0,s4
 30a:	eafff0ef          	jal	ra,0x1b8
 30e:	8b26                	mv	s6,s1
 310:	4901                	li	s2,0
 312:	bfa5                	j	0x28a
 314:	e45e                	sd	s7,8(sp)
 316:	008b0b93          	addi	s7,s6,8
 31a:	000b3903          	ld	s2,0(s6)
 31e:	03000593          	li	a1,48
 322:	8552                	mv	a0,s4
 324:	e7dff0ef          	jal	ra,0x1a0
 328:	07800593          	li	a1,120
 32c:	8552                	mv	a0,s4
 32e:	e73ff0ef          	jal	ra,0x1a0
 332:	44c1                	li	s1,16
 334:	64000b13          	li	s6,1600
 338:	03c95793          	srli	a5,s2,0x3c
 33c:	97da                	add	a5,a5,s6
 33e:	0007c583          	lbu	a1,0(a5)
 342:	8552                	mv	a0,s4
 344:	e5dff0ef          	jal	ra,0x1a0
 348:	0912                	slli	s2,s2,0x4
 34a:	14fd                	addi	s1,s1,-1
 34c:	f4f5                	bnez	s1,0x338
 34e:	8b5e                	mv	s6,s7
 350:	4901                	li	s2,0
 352:	6ba2                	ld	s7,8(sp)
 354:	bf1d                	j	0x28a
 356:	008b0913          	addi	s2,s6,8
 35a:	000b3483          	ld	s1,0(s6)
 35e:	cc91                	beqz	s1,0x37a
 360:	0004c583          	lbu	a1,0(s1)
 364:	cdb1                	beqz	a1,0x3c0
 366:	8552                	mv	a0,s4
 368:	e39ff0ef          	jal	ra,0x1a0
 36c:	0485                	addi	s1,s1,1
 36e:	0004c583          	lbu	a1,0(s1)
 372:	f9f5                	bnez	a1,0x366
 374:	8b4a                	mv	s6,s2
 376:	4901                	li	s2,0
 378:	bf09                	j	0x28a
 37a:	00000497          	auipc	s1,0x0
 37e:	1d648493          	addi	s1,s1,470 # 0x550
 382:	02800593          	li	a1,40
 386:	b7c5                	j	0x366
 388:	008b0493          	addi	s1,s6,8
 38c:	000b4583          	lbu	a1,0(s6)
 390:	8552                	mv	a0,s4
 392:	e0fff0ef          	jal	ra,0x1a0
 396:	8b26                	mv	s6,s1
 398:	4901                	li	s2,0
 39a:	bdc5                	j	0x28a
 39c:	02500593          	li	a1,37
 3a0:	8552                	mv	a0,s4
 3a2:	dffff0ef          	jal	ra,0x1a0
 3a6:	4901                	li	s2,0
 3a8:	b5cd                	j	0x28a
 3aa:	02500593          	li	a1,37
 3ae:	8552                	mv	a0,s4
 3b0:	df1ff0ef          	jal	ra,0x1a0
 3b4:	85a6                	mv	a1,s1
 3b6:	8552                	mv	a0,s4
 3b8:	de9ff0ef          	jal	ra,0x1a0
 3bc:	4901                	li	s2,0
 3be:	b5f1                	j	0x28a
 3c0:	8b4a                	mv	s6,s2
 3c2:	4901                	li	s2,0
 3c4:	b5d9                	j	0x28a
 3c6:	6406                	ld	s0,64(sp)
 3c8:	7942                	ld	s2,48(sp)
 3ca:	79a2                	ld	s3,40(sp)
 3cc:	7a02                	ld	s4,32(sp)
 3ce:	6ae2                	ld	s5,24(sp)
 3d0:	6b42                	ld	s6,16(sp)
 3d2:	60a6                	ld	ra,72(sp)
 3d4:	74e2                	ld	s1,56(sp)
 3d6:	6161                	addi	sp,sp,80
 3d8:	8082                	ret
 3da:	715d                	addi	sp,sp,-80
 3dc:	ec06                	sd	ra,24(sp)
 3de:	f032                	sd	a2,32(sp)
 3e0:	f436                	sd	a3,40(sp)
 3e2:	f83a                	sd	a4,48(sp)
 3e4:	fc3e                	sd	a5,56(sp)
 3e6:	e0c2                	sd	a6,64(sp)
 3e8:	e4c6                	sd	a7,72(sp)
 3ea:	1010                	addi	a2,sp,32
 3ec:	e432                	sd	a2,8(sp)
 3ee:	e63ff0ef          	jal	ra,0x250
 3f2:	60e2                	ld	ra,24(sp)
 3f4:	6161                	addi	sp,sp,80
 3f6:	8082                	ret
 3f8:	711d                	addi	sp,sp,-96
 3fa:	ec06                	sd	ra,24(sp)
 3fc:	f42e                	sd	a1,40(sp)
 3fe:	f832                	sd	a2,48(sp)
 400:	fc36                	sd	a3,56(sp)
 402:	e0ba                	sd	a4,64(sp)
 404:	e4be                	sd	a5,72(sp)
 406:	e8c2                	sd	a6,80(sp)
 408:	ecc6                	sd	a7,88(sp)
 40a:	1030                	addi	a2,sp,40
 40c:	e432                	sd	a2,8(sp)
 40e:	85aa                	mv	a1,a0
 410:	4505                	li	a0,1
 412:	e3fff0ef          	jal	ra,0x250
 416:	60e2                	ld	ra,24(sp)
 418:	6125                	addi	sp,sp,96
 41a:	8082                	ret
 41c:	1101                	addi	sp,sp,-32
 41e:	ec06                	sd	ra,24(sp)
 420:	e822                	sd	s0,16(sp)
 422:	842a                	mv	s0,a0
 424:	c45ff0ef          	jal	ra,0x68
 428:	02054263          	bltz	a0,0x44c
 42c:	ed15                	bnez	a0,0x468
 42e:	4601                	li	a2,0
 430:	4581                	li	a1,0
 432:	8522                	mv	a0,s0
 434:	c89ff0ef          	jal	ra,0xbc
 438:	02054163          	bltz	a0,0x45a
 43c:	4501                	li	a0,0
 43e:	c5bff0ef          	jal	ra,0x98
 442:	4501                	li	a0,0
 444:	60e2                	ld	ra,24(sp)
 446:	6442                	ld	s0,16(sp)
 448:	6105                	addi	sp,sp,32
 44a:	8082                	ret
 44c:	00000517          	auipc	a0,0x0
 450:	10c50513          	addi	a0,a0,268 # 0x558
 454:	fa5ff0ef          	jal	ra,0x3f8
 458:	b7ed                	j	0x442
 45a:	00000517          	auipc	a0,0x0
 45e:	10e50513          	addi	a0,a0,270 # 0x568
 462:	f97ff0ef          	jal	ra,0x3f8
 466:	bfd9                	j	0x43c
 468:	f9c00793          	li	a5,-100
 46c:	c63e                	sw	a5,12(sp)
 46e:	0068                	addi	a0,sp,12
 470:	c89ff0ef          	jal	ra,0xf8
 474:	fc0557e3          	bgez	a0,0x442
 478:	00000517          	auipc	a0,0x0
 47c:	10050513          	addi	a0,a0,256 # 0x578
 480:	f79ff0ef          	jal	ra,0x3f8
 484:	bf7d                	j	0x442
 486:	1141                	addi	sp,sp,-16
 488:	e406                	sd	ra,8(sp)
 48a:	00000517          	auipc	a0,0x0
 48e:	0fe50513          	addi	a0,a0,254 # 0x588
 492:	cbbff0ef          	jal	ra,0x14c
 496:	00000517          	auipc	a0,0x0
 49a:	10a50513          	addi	a0,a0,266 # 0x5a0
 49e:	f7fff0ef          	jal	ra,0x41c
 4a2:	4501                	li	a0,0
 4a4:	60a2                	ld	ra,8(sp)
 4a6:	0141                	addi	sp,sp,16
 4a8:	8082                	ret
 4aa:	715d                	addi	sp,sp,-80
 4ac:	e486                	sd	ra,72(sp)
 4ae:	e0a2                	sd	s0,64(sp)
 4b0:	bb9ff0ef          	jal	ra,0x68
 4b4:	04054863          	bltz	a0,0x504
 4b8:	842a                	mv	s0,a0
 4ba:	e53d                	bnez	a0,0x528
 4bc:	00000517          	auipc	a0,0x0
 4c0:	0fc50513          	addi	a0,a0,252 # 0x5b8
 4c4:	c89ff0ef          	jal	ra,0x14c
 4c8:	e402                	sd	zero,8(sp)
 4ca:	e802                	sd	zero,16(sp)
 4cc:	ec02                	sd	zero,24(sp)
 4ce:	f002                	sd	zero,32(sp)
 4d0:	f402                	sd	zero,40(sp)
 4d2:	f802                	sd	zero,48(sp)
 4d4:	fc02                	sd	zero,56(sp)
 4d6:	00000797          	auipc	a5,0x0
 4da:	0f278793          	addi	a5,a5,242 # 0x5c8
 4de:	e03e                	sd	a5,0(sp)
 4e0:	4601                	li	a2,0
 4e2:	858a                	mv	a1,sp
 4e4:	00000517          	auipc	a0,0x0
 4e8:	0ec50513          	addi	a0,a0,236 # 0x5d0
 4ec:	bd1ff0ef          	jal	ra,0xbc
 4f0:	02054263          	bltz	a0,0x514
 4f4:	4501                	li	a0,0
 4f6:	ba3ff0ef          	jal	ra,0x98
 4fa:	8522                	mv	a0,s0
 4fc:	60a6                	ld	ra,72(sp)
 4fe:	6406                	ld	s0,64(sp)
 500:	6161                	addi	sp,sp,80
 502:	8082                	ret
 504:	00000517          	auipc	a0,0x0
 508:	0a450513          	addi	a0,a0,164 # 0x5a8
 50c:	eedff0ef          	jal	ra,0x3f8
 510:	547d                	li	s0,-1
 512:	b7e5                	j	0x4fa
 514:	00000517          	auipc	a0,0x0
 518:	0c450513          	addi	a0,a0,196 # 0x5d8
 51c:	eddff0ef          	jal	ra,0x3f8
 520:	4505                	li	a0,1
 522:	b77ff0ef          	jal	ra,0x98
 526:	b7f9                	j	0x4f4
 528:	f9c00793          	li	a5,-100
 52c:	c03e                	sw	a5,0(sp)
 52e:	850a                	mv	a0,sp
 530:	bc9ff0ef          	jal	ra,0xf8
 534:	00054463          	bltz	a0,0x53c
 538:	4401                	li	s0,0
 53a:	b7c1                	j	0x4fa
 53c:	00000517          	auipc	a0,0x0
 540:	03c50513          	addi	a0,a0,60 # 0x578
 544:	eb5ff0ef          	jal	ra,0x3f8
 548:	bfc5                	j	0x538
 54a:	0000                	unimp
 54c:	0000                	unimp
 54e:	0000                	unimp
 550:	6e28                	ld	a0,88(a2)
 552:	6c75                	lui	s8,0x1d
 554:	296c                	fld	fa1,208(a0)
 556:	0000                	unimp
 558:	6f66                	ld	t5,88(sp)
 55a:	6b72                	ld	s6,280(sp)
 55c:	6620                	ld	s0,72(a2)
 55e:	6961                	lui	s2,0x18
 560:	656c                	ld	a1,200(a0)
 562:	0064                	addi	s1,sp,12
 564:	0000                	unimp
 566:	0000                	unimp
 568:	7865                	lui	a6,0xffff9
 56a:	6365                	lui	t1,0x19
 56c:	6576                	ld	a0,344(sp)
 56e:	6620                	ld	s0,72(a2)
 570:	6961                	lui	s2,0x18
 572:	656c                	ld	a1,200(a0)
 574:	0064                	addi	s1,sp,12
 576:	0000                	unimp
 578:	74696177          	0x74696177
 57c:	6620                	ld	s0,72(a2)
 57e:	6961                	lui	s2,0x18
 580:	0a6c                	addi	a1,sp,284
 582:	0000                	unimp
 584:	0000                	unimp
 586:	0000                	unimp
 588:	746e6d2f          	0x746e6d2f
 58c:	73756d2f          	0x73756d2f
 590:	2f6c                	fld	fa1,216(a4)
 592:	6162                	ld	sp,24(sp)
 594:	2f636973          	csrrsi	s2,0x2f6,6
	...
 5a0:	6d6d                	lui	s10,0x1b
 5a2:	7061                	c.lui	zero,0xffff8
 5a4:	0000                	unimp
 5a6:	0000                	unimp
 5a8:	6f66                	ld	t5,88(sp)
 5aa:	6b72                	ld	s6,280(sp)
 5ac:	6620                	ld	s0,72(a2)
 5ae:	6961                	lui	s2,0x18
 5b0:	656c                	ld	a1,200(a0)
 5b2:	0a64                	addi	s1,sp,284
 5b4:	0000                	unimp
 5b6:	0000                	unimp
 5b8:	746e6d2f          	0x746e6d2f
 5bc:	696c672f          	0x696c672f
 5c0:	6362                	ld	t1,24(sp)
 5c2:	0000002f          	vamoaddei8.v	zero,(zero),v0,v0,v0.t
 5c6:	0000                	unimp
 5c8:	7361                	lui	t1,0xffff8
 5ca:	0068                	addi	a0,sp,12
 5cc:	0000                	unimp
 5ce:	0000                	unimp
 5d0:	7562                	ld	a0,56(sp)
 5d2:	6f627973          	csrrci	s2,0x6f6,4
 5d6:	0078                	addi	a4,sp,12
 5d8:	7865                	lui	a6,0xffff9
 5da:	6365                	lui	t1,0x19
 5dc:	6576                	ld	a0,344(sp)
 5de:	6620                	ld	s0,72(a2)
 5e0:	6961                	lui	s2,0x18
 5e2:	656c                	ld	a1,200(a0)
 5e4:	0a64                	addi	s1,sp,284
 5e6:	0000                	unimp
 5e8:	fda0                	sd	s0,120(a1)
 5ea:	ffff                	0xffff
 5ec:	fce4                	sd	s1,248(s1)
 5ee:	ffff                	0xffff
 5f0:	fdc2                	sd	a6,248(sp)
 5f2:	ffff                	0xffff
 5f4:	fdc2                	sd	a6,248(sp)
 5f6:	ffff                	0xffff
 5f8:	fdc2                	sd	a6,248(sp)
 5fa:	ffff                	0xffff
 5fc:	fdc2                	sd	a6,248(sp)
 5fe:	ffff                	0xffff
 600:	fdc2                	sd	a6,248(sp)
 602:	ffff                	0xffff
 604:	fdc2                	sd	a6,248(sp)
 606:	ffff                	0xffff
 608:	fdc2                	sd	a6,248(sp)
 60a:	ffff                	0xffff
 60c:	fcfc                	sd	a5,248(s1)
 60e:	ffff                	0xffff
 610:	fdc2                	sd	a6,248(sp)
 612:	ffff                	0xffff
 614:	fdc2                	sd	a6,248(sp)
 616:	ffff                	0xffff
 618:	fdc2                	sd	a6,248(sp)
 61a:	ffff                	0xffff
 61c:	fd2c                	sd	a1,120(a0)
 61e:	ffff                	0xffff
 620:	fdc2                	sd	a6,248(sp)
 622:	ffff                	0xffff
 624:	fdc2                	sd	a6,248(sp)
 626:	ffff                	0xffff
 628:	fd6e                	sd	s11,184(sp)
 62a:	ffff                	0xffff
 62c:	fdc2                	sd	a6,248(sp)
 62e:	ffff                	0xffff
 630:	fdc2                	sd	a6,248(sp)
 632:	ffff                	0xffff
 634:	fdc2                	sd	a6,248(sp)
 636:	ffff                	0xffff
 638:	fdc2                	sd	a6,248(sp)
 63a:	ffff                	0xffff
 63c:	fd14                	sd	a3,56(a0)
 63e:	ffff                	0xffff
 640:	3130                	fld	fa2,96(a0)
 642:	3332                	fld	ft6,296(sp)
 644:	3534                	fld	fa3,104(a0)
 646:	3736                	fld	fa4,360(sp)
 648:	3938                	fld	fa4,112(a0)
 64a:	4241                	li	tp,16
 64c:	46454443          	fmadd.q	fs0,fa0,ft4,fs0,rmm
	...
 658:	0010                	0x10
 65a:	0000                	unimp
 65c:	0000                	unimp
 65e:	0000                	unimp
 660:	00527a03          	0x527a03
 664:	7c01                	lui	s8,0xfffe0
 666:	0101                	addi	sp,sp,0
 668:	00020d1b          	sext.w	s10,tp
 66c:	0018                	0x18
 66e:	0000                	unimp
 670:	0018                	0x18
 672:	0000                	unimp
 674:	f98c                	sd	a1,48(a1)
 676:	ffff                	0xffff
 678:	0014                	0x14
 67a:	0000                	unimp
 67c:	4200                	lw	s0,0(a2)
 67e:	100e                	c.slli	zero,0x23
 680:	8142                	mv	sp,a6
 682:	4c02                	lw	s8,0(sp)
 684:	42c1                	li	t0,16
 686:	000e                	c.slli	zero,0x3
 688:	0010                	0x10
 68a:	0000                	unimp
 68c:	0034                	addi	a3,sp,8
 68e:	0000                	unimp
 690:	f984                	sd	s1,48(a1)
 692:	ffff                	0xffff
 694:	0010                	0x10
 696:	0000                	unimp
 698:	0000                	unimp
 69a:	0000                	unimp
 69c:	0010                	0x10
 69e:	0000                	unimp
 6a0:	0048                	addi	a0,sp,4
 6a2:	0000                	unimp
 6a4:	f980                	sd	s0,48(a1)
 6a6:	ffff                	0xffff
 6a8:	000c                	0xc
 6aa:	0000                	unimp
 6ac:	0000                	unimp
 6ae:	0000                	unimp
 6b0:	0010                	0x10
 6b2:	0000                	unimp
 6b4:	005c                	addi	a5,sp,4
 6b6:	0000                	unimp
 6b8:	f978                	sd	a4,240(a0)
 6ba:	ffff                	0xffff
 6bc:	000a                	c.slli	zero,0x2
 6be:	0000                	unimp
 6c0:	0000                	unimp
 6c2:	0000                	unimp
 6c4:	0010                	0x10
 6c6:	0000                	unimp
 6c8:	0070                	addi	a2,sp,12
 6ca:	0000                	unimp
 6cc:	f96e                	sd	s11,176(sp)
 6ce:	ffff                	0xffff
 6d0:	000a                	c.slli	zero,0x2
 6d2:	0000                	unimp
 6d4:	0000                	unimp
 6d6:	0000                	unimp
 6d8:	0010                	0x10
 6da:	0000                	unimp
 6dc:	0084                	addi	s1,sp,64
 6de:	0000                	unimp
 6e0:	f964                	sd	s1,240(a0)
 6e2:	ffff                	0xffff
 6e4:	000c                	0xc
 6e6:	0000                	unimp
 6e8:	0000                	unimp
 6ea:	0000                	unimp
 6ec:	0010                	0x10
 6ee:	0000                	unimp
 6f0:	0098                	addi	a4,sp,64
 6f2:	0000                	unimp
 6f4:	f95c                	sd	a5,176(a0)
 6f6:	ffff                	0xffff
 6f8:	000c                	0xc
 6fa:	0000                	unimp
 6fc:	0000                	unimp
 6fe:	0000                	unimp
 700:	0010                	0x10
 702:	0000                	unimp
 704:	00ac                	addi	a1,sp,72
 706:	0000                	unimp
 708:	f954                	sd	a3,176(a0)
 70a:	ffff                	0xffff
 70c:	000c                	0xc
 70e:	0000                	unimp
 710:	0000                	unimp
 712:	0000                	unimp
 714:	0010                	0x10
 716:	0000                	unimp
 718:	00c0                	addi	s0,sp,68
 71a:	0000                	unimp
 71c:	f94c                	sd	a1,176(a0)
 71e:	ffff                	0xffff
 720:	0016                	c.slli	zero,0x5
 722:	0000                	unimp
 724:	0000                	unimp
 726:	0000                	unimp
 728:	0010                	0x10
 72a:	0000                	unimp
 72c:	00d4                	addi	a3,sp,68
 72e:	0000                	unimp
 730:	f94e                	sd	s3,176(sp)
 732:	ffff                	0xffff
 734:	001a                	c.slli	zero,0x6
 736:	0000                	unimp
 738:	0000                	unimp
 73a:	0000                	unimp
 73c:	0010                	0x10
 73e:	0000                	unimp
 740:	00e8                	addi	a0,sp,76
 742:	0000                	unimp
 744:	f954                	sd	a3,176(a0)
 746:	ffff                	0xffff
 748:	000a                	c.slli	zero,0x2
 74a:	0000                	unimp
 74c:	0000                	unimp
 74e:	0000                	unimp
 750:	0010                	0x10
 752:	0000                	unimp
 754:	00fc                	addi	a5,sp,76
 756:	0000                	unimp
 758:	f94a                	sd	s2,176(sp)
 75a:	ffff                	0xffff
 75c:	000e                	c.slli	zero,0x3
 75e:	0000                	unimp
 760:	0000                	unimp
 762:	0000                	unimp
 764:	0010                	0x10
 766:	0000                	unimp
 768:	0110                	addi	a2,sp,128
 76a:	0000                	unimp
 76c:	f944                	sd	s1,176(a0)
 76e:	ffff                	0xffff
 770:	000c                	0xc
 772:	0000                	unimp
 774:	0000                	unimp
 776:	0000                	unimp
 778:	0010                	0x10
 77a:	0000                	unimp
 77c:	0124                	addi	s1,sp,136
 77e:	0000                	unimp
 780:	f93c                	sd	a5,112(a0)
 782:	ffff                	0xffff
 784:	000c                	0xc
 786:	0000                	unimp
 788:	0000                	unimp
 78a:	0000                	unimp
 78c:	0010                	0x10
 78e:	0000                	unimp
 790:	0138                	addi	a4,sp,136
 792:	0000                	unimp
 794:	f934                	sd	a3,112(a0)
 796:	ffff                	0xffff
 798:	000a                	c.slli	zero,0x2
 79a:	0000                	unimp
 79c:	0000                	unimp
 79e:	0000                	unimp
 7a0:	0010                	0x10
 7a2:	0000                	unimp
 7a4:	014c                	addi	a1,sp,132
 7a6:	0000                	unimp
 7a8:	f92a                	sd	a0,176(sp)
 7aa:	ffff                	0xffff
 7ac:	001a                	c.slli	zero,0x6
 7ae:	0000                	unimp
 7b0:	0000                	unimp
 7b2:	0000                	unimp
 7b4:	0010                	0x10
 7b6:	0000                	unimp
 7b8:	0160                	addi	s0,sp,140
 7ba:	0000                	unimp
 7bc:	f930                	sd	a2,112(a0)
 7be:	ffff                	0xffff
 7c0:	000c                	0xc
 7c2:	0000                	unimp
 7c4:	0000                	unimp
 7c6:	0000                	unimp
 7c8:	0018                	0x18
 7ca:	0000                	unimp
 7cc:	0174                	addi	a3,sp,140
 7ce:	0000                	unimp
 7d0:	f928                	sd	a0,112(a0)
 7d2:	ffff                	0xffff
 7d4:	0014                	0x14
 7d6:	0000                	unimp
 7d8:	4200                	lw	s0,0(a2)
 7da:	100e                	c.slli	zero,0x23
 7dc:	8142                	mv	sp,a6
 7de:	4c02                	lw	s8,0(sp)
 7e0:	42c1                	li	t0,16
 7e2:	000e                	c.slli	zero,0x3
 7e4:	0010                	0x10
 7e6:	0000                	unimp
 7e8:	0190                	addi	a2,sp,192
 7ea:	0000                	unimp
 7ec:	f920                	sd	s0,112(a0)
 7ee:	ffff                	0xffff
 7f0:	0010                	0x10
 7f2:	0000                	unimp
 7f4:	0000                	unimp
 7f6:	0000                	unimp
 7f8:	0010                	0x10
 7fa:	0000                	unimp
 7fc:	01a4                	addi	s1,sp,200
 7fe:	0000                	unimp
 800:	f91c                	sd	a5,48(a0)
 802:	ffff                	0xffff
 804:	0010                	0x10
 806:	0000                	unimp
 808:	0000                	unimp
 80a:	0000                	unimp
 80c:	0010                	0x10
 80e:	0000                	unimp
 810:	01b8                	addi	a4,sp,200
 812:	0000                	unimp
 814:	f918                	sd	a4,48(a0)
 816:	ffff                	0xffff
 818:	000c                	0xc
 81a:	0000                	unimp
 81c:	0000                	unimp
 81e:	0000                	unimp
 820:	0010                	0x10
 822:	0000                	unimp
 824:	01cc                	addi	a1,sp,196
 826:	0000                	unimp
 828:	f910                	sd	a2,48(a0)
 82a:	ffff                	0xffff
 82c:	000c                	0xc
 82e:	0000                	unimp
 830:	0000                	unimp
 832:	0000                	unimp
 834:	0010                	0x10
 836:	0000                	unimp
 838:	01e0                	addi	s0,sp,204
 83a:	0000                	unimp
 83c:	f908                	sd	a0,48(a0)
 83e:	ffff                	0xffff
 840:	0008                	0x8
 842:	0000                	unimp
 844:	0000                	unimp
 846:	0000                	unimp
 848:	0010                	0x10
 84a:	0000                	unimp
 84c:	01f4                	addi	a3,sp,204
 84e:	0000                	unimp
 850:	f8fc                	sd	a5,240(s1)
 852:	ffff                	0xffff
 854:	000c                	0xc
 856:	0000                	unimp
 858:	0000                	unimp
 85a:	0000                	unimp
 85c:	0010                	0x10
 85e:	0000                	unimp
 860:	0208                	addi	a0,sp,256
 862:	0000                	unimp
 864:	f8f4                	sd	a3,240(s1)
 866:	ffff                	0xffff
 868:	000c                	0xc
 86a:	0000                	unimp
 86c:	0000                	unimp
 86e:	0000                	unimp
 870:	0010                	0x10
 872:	0000                	unimp
 874:	021c                	addi	a5,sp,256
 876:	0000                	unimp
 878:	f8ec                	sd	a1,240(s1)
 87a:	ffff                	0xffff
 87c:	000e                	c.slli	zero,0x3
 87e:	0000                	unimp
 880:	0000                	unimp
 882:	0000                	unimp
 884:	0010                	0x10
 886:	0000                	unimp
 888:	0230                	addi	a2,sp,264
 88a:	0000                	unimp
 88c:	f8e6                	sd	s9,112(sp)
 88e:	ffff                	0xffff
 890:	000a                	c.slli	zero,0x2
 892:	0000                	unimp
 894:	0000                	unimp
 896:	0000                	unimp
 898:	0010                	0x10
 89a:	0000                	unimp
 89c:	0244                	addi	s1,sp,260
 89e:	0000                	unimp
 8a0:	f8dc                	sd	a5,176(s1)
 8a2:	ffff                	0xffff
 8a4:	000c                	0xc
 8a6:	0000                	unimp
 8a8:	0000                	unimp
 8aa:	0000                	unimp
 8ac:	0010                	0x10
 8ae:	0000                	unimp
 8b0:	0258                	addi	a4,sp,260
 8b2:	0000                	unimp
 8b4:	f8d4                	sd	a3,176(s1)
 8b6:	ffff                	0xffff
 8b8:	000e                	c.slli	zero,0x3
 8ba:	0000                	unimp
 8bc:	0000                	unimp
 8be:	0000                	unimp
 8c0:	0014                	0x14
 8c2:	0000                	unimp
 8c4:	026c                	addi	a1,sp,268
 8c6:	0000                	unimp
 8c8:	f8ce                	sd	s3,112(sp)
 8ca:	ffff                	0xffff
 8cc:	000a                	c.slli	zero,0x2
	...
 8d6:	0000                	unimp
 8d8:	0018                	0x18
 8da:	0000                	unimp
 8dc:	0284                	addi	s1,sp,320
 8de:	0000                	unimp
 8e0:	f8c0                	sd	s0,176(s1)
 8e2:	ffff                	0xffff
 8e4:	0018                	0x18
 8e6:	0000                	unimp
 8e8:	4200                	lw	s0,0(a2)
 8ea:	200e                	fld	ft0,192(sp)
 8ec:	8142                	mv	sp,a6
 8ee:	5002                	0x5002
 8f0:	42c1                	li	t0,16
 8f2:	000e                	c.slli	zero,0x3
 8f4:	002c                	addi	a1,sp,8
 8f6:	0000                	unimp
 8f8:	02a0                	addi	s0,sp,328
 8fa:	0000                	unimp
 8fc:	f8bc                	sd	a5,112(s1)
 8fe:	ffff                	0xffff
 900:	0098                	addi	a4,sp,64
 902:	0000                	unimp
 904:	4200                	lw	s0,0(a2)
 906:	300e                	fld	ft0,224(sp)
 908:	8146                	mv	sp,a7
 90a:	8902                	jr	s2
 90c:	9206                	add	tp,tp,ra
 90e:	0208                	addi	a0,sp,256
 910:	885a                	mv	a6,s6
 912:	6404                	ld	s1,8(s0)
 914:	42c8                	lw	a0,4(a3)
 916:	c10a                	sw	sp,128(sp)
 918:	c942                	sw	a6,144(sp)
 91a:	d242                	sw	a6,36(sp)
 91c:	0e42                	slli	t3,t3,0x10
 91e:	4200                	lw	s0,0(a2)
 920:	0000000b          	0xb
 924:	0040                	addi	s0,sp,4
 926:	0000                	unimp
 928:	02d0                	addi	a2,sp,324
 92a:	0000                	unimp
 92c:	f924                	sd	s1,112(a0)
 92e:	ffff                	0xffff
 930:	018a                	slli	gp,gp,0x2
 932:	0000                	unimp
 934:	4200                	lw	s0,0(a2)
 936:	500e                	0x500e
 938:	8144                	0x8144
 93a:	8902                	jr	s2
 93c:	5406                	lw	s0,96(sp)
 93e:	0488                	addi	a0,sp,576
 940:	0892                	slli	a7,a7,0x4
 942:	0c940a93          	addi	s5,s0,201
 946:	0e95                	addi	t4,t4,5
 948:	1096                	slli	ra,ra,0x25
 94a:	ac02                	fsd	ft0,24(sp)
 94c:	3e021297          	auipc	t0,0x3e021
 950:	c87402d7          	0xc87402d7
 954:	d242                	sw	a6,36(sp)
 956:	d342                	sw	a6,164(sp)
 958:	d442                	sw	a6,40(sp)
 95a:	d542                	sw	a6,168(sp)
 95c:	d642                	sw	a6,44(sp)
 95e:	c142                	sw	a6,128(sp)
 960:	c942                	sw	a6,144(sp)
 962:	0e42                	slli	t3,t3,0x10
 964:	0000                	unimp
 966:	0000                	unimp
 968:	0018                	0x18
 96a:	0000                	unimp
 96c:	0314                	addi	a3,sp,384
 96e:	0000                	unimp
 970:	fa6a                	sd	s10,304(sp)
 972:	ffff                	0xffff
 974:	001e                	c.slli	zero,0x7
 976:	0000                	unimp
 978:	4200                	lw	s0,0(a2)
 97a:	500e                	0x500e
 97c:	8142                	mv	sp,a6
 97e:	560e                	lw	a2,224(sp)
 980:	42c1                	li	t0,16
 982:	000e                	c.slli	zero,0x3
 984:	0018                	0x18
 986:	0000                	unimp
 988:	0330                	addi	a2,sp,392
 98a:	0000                	unimp
 98c:	fa6c                	sd	a1,240(a2)
 98e:	ffff                	0xffff
 990:	0024                	addi	s1,sp,8
 992:	0000                	unimp
 994:	4200                	lw	s0,0(a2)
 996:	600e                	0x600e
 998:	8142                	mv	sp,a6
 99a:	5c12                	lw	s8,36(sp)
 99c:	42c1                	li	t0,16
 99e:	000e                	c.slli	zero,0x3
 9a0:	0020                	addi	s0,sp,8
 9a2:	0000                	unimp
 9a4:	034c                	addi	a1,sp,388
 9a6:	0000                	unimp
 9a8:	fa74                	sd	a3,240(a2)
 9aa:	ffff                	0xffff
 9ac:	006a                	c.slli	zero,0x1a
 9ae:	0000                	unimp
 9b0:	4200                	lw	s0,0(a2)
 9b2:	200e                	fld	ft0,192(sp)
 9b4:	8144                	0x8144
 9b6:	8802                	jr	a6
 9b8:	6404                	ld	s1,8(s0)
 9ba:	c10a                	sw	sp,128(sp)
 9bc:	c842                	sw	a6,16(sp)
 9be:	0e42                	slli	t3,t3,0x10
 9c0:	4200                	lw	s0,0(a2)
 9c2:	0018000b          	0x18000b
 9c6:	0000                	unimp
 9c8:	0370                	addi	a2,sp,396
 9ca:	0000                	unimp
 9cc:	faba                	sd	a4,368(sp)
 9ce:	ffff                	0xffff
 9d0:	0024                	addi	s1,sp,8
 9d2:	0000                	unimp
 9d4:	4200                	lw	s0,0(a2)
 9d6:	100e                	c.slli	zero,0x23
 9d8:	8142                	mv	sp,a6
 9da:	5c02                	lw	s8,32(sp)
 9dc:	42c1                	li	t0,16
 9de:	000e                	c.slli	zero,0x3
 9e0:	0020                	addi	s0,sp,8
 9e2:	0000                	unimp
 9e4:	038c                	addi	a1,sp,448
 9e6:	0000                	unimp
 9e8:	fac2                	sd	a6,368(sp)
 9ea:	ffff                	0xffff
 9ec:	00a0                	addi	s0,sp,72
 9ee:	0000                	unimp
 9f0:	4200                	lw	s0,0(a2)
 9f2:	500e                	0x500e
 9f4:	8144                	0x8144
 9f6:	8802                	jr	a6
 9f8:	0204                	addi	s1,sp,256
 9fa:	0a4e                	slli	s4,s4,0x13
 9fc:	42c1                	li	t0,16
 9fe:	42c8                	lw	a0,4(a3)
 a00:	000e                	c.slli	zero,0x3
 a02:	0b42                	slli	s6,s6,0x10
	...
