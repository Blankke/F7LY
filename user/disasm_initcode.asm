
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	460d                	li	a2,3
   6:	00000597          	auipc	a1,0x0
   a:	3ea58593          	addi	a1,a1,1002 # 0x3f0
   e:	4505                	li	a0,1
  10:	004000ef          	jal	ra,0x14
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
  68:	85b2                	mv	a1,a2
  6a:	863a                	mv	a2,a4
  6c:	c191                	beqz	a1,0x70
  6e:	95b6                	add	a1,a1,a3
  70:	0dc00893          	li	a7,220
  74:	4681                	li	a3,0
  76:	4701                	li	a4,0
  78:	4781                	li	a5,0
  7a:	00000073          	ecall
  7e:	2501                	sext.w	a0,a0
  80:	8082                	ret
  82:	05d00893          	li	a7,93
  86:	00000073          	ecall
  8a:	8082                	ret
  8c:	10400893          	li	a7,260
  90:	4681                	li	a3,0
  92:	00000073          	ecall
  96:	2501                	sext.w	a0,a0
  98:	8082                	ret
  9a:	0dd00893          	li	a7,221
  9e:	00000073          	ecall
  a2:	2501                	sext.w	a0,a0
  a4:	8082                	ret
  a6:	0dd00893          	li	a7,221
  aa:	00000073          	ecall
  ae:	2501                	sext.w	a0,a0
  b0:	8082                	ret
  b2:	09900893          	li	a7,153
  b6:	00000073          	ecall
  ba:	8082                	ret
  bc:	0d700893          	li	a7,215
  c0:	00000073          	ecall
  c4:	2501                	sext.w	a0,a0
  c6:	8082                	ret
  c8:	1141                	addi	sp,sp,-16
  ca:	e406                	sd	ra,8(sp)
  cc:	85aa                	mv	a1,a0
  ce:	4601                	li	a2,0
  d0:	557d                	li	a0,-1
  d2:	fbbff0ef          	jal	ra,0x8c
  d6:	60a2                	ld	ra,8(sp)
  d8:	0141                	addi	sp,sp,16
  da:	8082                	ret
  dc:	02500893          	li	a7,37
  e0:	1702                	slli	a4,a4,0x20
  e2:	9301                	srli	a4,a4,0x20
  e4:	00000073          	ecall
  e8:	2501                	sext.w	a0,a0
  ea:	8082                	ret
  ec:	02300893          	li	a7,35
  f0:	1602                	slli	a2,a2,0x20
  f2:	9201                	srli	a2,a2,0x20
  f4:	00000073          	ecall
  f8:	2501                	sext.w	a0,a0
  fa:	8082                	ret
  fc:	0a000893          	li	a7,160
 100:	00000073          	ecall
 104:	2501                	sext.w	a0,a0
 106:	8082                	ret
 108:	0d600893          	li	a7,214
 10c:	00000073          	ecall
 110:	2501                	sext.w	a0,a0
 112:	8082                	ret
 114:	03100893          	li	a7,49
 118:	00000073          	ecall
 11c:	2501                	sext.w	a0,a0
 11e:	8082                	ret
 120:	03d00893          	li	a7,61
 124:	00000073          	ecall
 128:	2501                	sext.w	a0,a0
 12a:	8082                	ret
 12c:	03b00893          	li	a7,59
 130:	4581                	li	a1,0
 132:	00000073          	ecall
 136:	2501                	sext.w	a0,a0
 138:	8082                	ret
 13a:	48dd                	li	a7,23
 13c:	00000073          	ecall
 140:	2501                	sext.w	a0,a0
 142:	8082                	ret
 144:	02800893          	li	a7,40
 148:	00000073          	ecall
 14c:	2501                	sext.w	a0,a0
 14e:	8082                	ret
 150:	02700893          	li	a7,39
 154:	4581                	li	a1,0
 156:	00000073          	ecall
 15a:	2501                	sext.w	a0,a0
 15c:	8082                	ret
 15e:	1101                	addi	sp,sp,-32
 160:	ec06                	sd	ra,24(sp)
 162:	00b107a3          	sb	a1,15(sp)
 166:	4605                	li	a2,1
 168:	00f10593          	addi	a1,sp,15
 16c:	ecfff0ef          	jal	ra,0x3a
 170:	60e2                	ld	ra,24(sp)
 172:	6105                	addi	sp,sp,32
 174:	8082                	ret
 176:	7179                	addi	sp,sp,-48
 178:	f406                	sd	ra,40(sp)
 17a:	f022                	sd	s0,32(sp)
 17c:	ec26                	sd	s1,24(sp)
 17e:	e84a                	sd	s2,16(sp)
 180:	84aa                	mv	s1,a0
 182:	c299                	beqz	a3,0x188
 184:	0805c163          	bltz	a1,0x206
 188:	2581                	sext.w	a1,a1
 18a:	4881                	li	a7,0
 18c:	890a                	mv	s2,sp
 18e:	86ca                	mv	a3,s2
 190:	4701                	li	a4,0
 192:	2601                	sext.w	a2,a2
 194:	45800513          	li	a0,1112
 198:	883a                	mv	a6,a4
 19a:	2705                	addiw	a4,a4,1
 19c:	02c5f7bb          	remuw	a5,a1,a2
 1a0:	1782                	slli	a5,a5,0x20
 1a2:	9381                	srli	a5,a5,0x20
 1a4:	97aa                	add	a5,a5,a0
 1a6:	0007c783          	lbu	a5,0(a5)
 1aa:	00f68023          	sb	a5,0(a3)
 1ae:	0005879b          	sext.w	a5,a1
 1b2:	02c5d5bb          	divuw	a1,a1,a2
 1b6:	0685                	addi	a3,a3,1
 1b8:	fec7f0e3          	bgeu	a5,a2,0x198
 1bc:	00088c63          	beqz	a7,0x1d4
 1c0:	01070793          	addi	a5,a4,16
 1c4:	00278733          	add	a4,a5,sp
 1c8:	02d00793          	li	a5,45
 1cc:	fef70823          	sb	a5,-16(a4)
 1d0:	0028071b          	addiw	a4,a6,2
 1d4:	02e05363          	blez	a4,0x1fa
 1d8:	840a                	mv	s0,sp
 1da:	943a                	add	s0,s0,a4
 1dc:	197d                	addi	s2,s2,-1
 1de:	993a                	add	s2,s2,a4
 1e0:	377d                	addiw	a4,a4,-1
 1e2:	1702                	slli	a4,a4,0x20
 1e4:	9301                	srli	a4,a4,0x20
 1e6:	40e90933          	sub	s2,s2,a4
 1ea:	fff44583          	lbu	a1,-1(s0)
 1ee:	8526                	mv	a0,s1
 1f0:	f6fff0ef          	jal	ra,0x15e
 1f4:	147d                	addi	s0,s0,-1
 1f6:	ff241ae3          	bne	s0,s2,0x1ea
 1fa:	70a2                	ld	ra,40(sp)
 1fc:	7402                	ld	s0,32(sp)
 1fe:	64e2                	ld	s1,24(sp)
 200:	6942                	ld	s2,16(sp)
 202:	6145                	addi	sp,sp,48
 204:	8082                	ret
 206:	40b005bb          	negw	a1,a1
 20a:	4885                	li	a7,1
 20c:	b741                	j	0x18c
 20e:	7159                	addi	sp,sp,-112
 210:	f486                	sd	ra,104(sp)
 212:	f0a2                	sd	s0,96(sp)
 214:	eca6                	sd	s1,88(sp)
 216:	e8ca                	sd	s2,80(sp)
 218:	e4ce                	sd	s3,72(sp)
 21a:	e0d2                	sd	s4,64(sp)
 21c:	fc56                	sd	s5,56(sp)
 21e:	f85a                	sd	s6,48(sp)
 220:	f45e                	sd	s7,40(sp)
 222:	f062                	sd	s8,32(sp)
 224:	ec66                	sd	s9,24(sp)
 226:	e86a                	sd	s10,16(sp)
 228:	e46e                	sd	s11,8(sp)
 22a:	0005c483          	lbu	s1,0(a1)
 22e:	14048e63          	beqz	s1,0x38a
 232:	8a2a                	mv	s4,a0
 234:	8db2                	mv	s11,a2
 236:	00158413          	addi	s0,a1,1
 23a:	4901                	li	s2,0
 23c:	02500993          	li	s3,37
 240:	4c01                	li	s8,0
 242:	4b55                	li	s6,21
 244:	40000b93          	li	s7,1024
 248:	02800c93          	li	s9,40
 24c:	45800a93          	li	s5,1112
 250:	a829                	j	0x26a
 252:	85a6                	mv	a1,s1
 254:	8552                	mv	a0,s4
 256:	f09ff0ef          	jal	ra,0x15e
 25a:	a019                	j	0x260
 25c:	01390d63          	beq	s2,s3,0x276
 260:	0405                	addi	s0,s0,1
 262:	fff44483          	lbu	s1,-1(s0)
 266:	12048263          	beqz	s1,0x38a
 26a:	fe0919e3          	bnez	s2,0x25c
 26e:	ff3492e3          	bne	s1,s3,0x252
 272:	894e                	mv	s2,s3
 274:	b7f5                	j	0x260
 276:	0f348763          	beq	s1,s3,0x364
 27a:	f9d4879b          	addiw	a5,s1,-99
 27e:	0ff7f793          	andi	a5,a5,255
 282:	0efb6763          	bltu	s6,a5,0x370
 286:	f9d4879b          	addiw	a5,s1,-99
 28a:	0ff7f713          	andi	a4,a5,255
 28e:	0eeb6163          	bltu	s6,a4,0x370
 292:	00271793          	slli	a5,a4,0x2
 296:	97de                	add	a5,a5,s7
 298:	439c                	lw	a5,0(a5)
 29a:	97de                	add	a5,a5,s7
 29c:	8782                	jr	a5
 29e:	008d8493          	addi	s1,s11,8
 2a2:	4685                	li	a3,1
 2a4:	4629                	li	a2,10
 2a6:	000da583          	lw	a1,0(s11)
 2aa:	8552                	mv	a0,s4
 2ac:	ecbff0ef          	jal	ra,0x176
 2b0:	8da6                	mv	s11,s1
 2b2:	8962                	mv	s2,s8
 2b4:	b775                	j	0x260
 2b6:	008d8493          	addi	s1,s11,8
 2ba:	86e2                	mv	a3,s8
 2bc:	4629                	li	a2,10
 2be:	000da583          	lw	a1,0(s11)
 2c2:	8552                	mv	a0,s4
 2c4:	eb3ff0ef          	jal	ra,0x176
 2c8:	8da6                	mv	s11,s1
 2ca:	8962                	mv	s2,s8
 2cc:	bf51                	j	0x260
 2ce:	008d8493          	addi	s1,s11,8
 2d2:	86e2                	mv	a3,s8
 2d4:	4641                	li	a2,16
 2d6:	000da583          	lw	a1,0(s11)
 2da:	8552                	mv	a0,s4
 2dc:	e9bff0ef          	jal	ra,0x176
 2e0:	8da6                	mv	s11,s1
 2e2:	8962                	mv	s2,s8
 2e4:	bfb5                	j	0x260
 2e6:	008d8d13          	addi	s10,s11,8
 2ea:	000db903          	ld	s2,0(s11)
 2ee:	03000593          	li	a1,48
 2f2:	8552                	mv	a0,s4
 2f4:	e6bff0ef          	jal	ra,0x15e
 2f8:	07800593          	li	a1,120
 2fc:	8552                	mv	a0,s4
 2fe:	e61ff0ef          	jal	ra,0x15e
 302:	44c1                	li	s1,16
 304:	03c95793          	srli	a5,s2,0x3c
 308:	97d6                	add	a5,a5,s5
 30a:	0007c583          	lbu	a1,0(a5)
 30e:	8552                	mv	a0,s4
 310:	e4fff0ef          	jal	ra,0x15e
 314:	0912                	slli	s2,s2,0x4
 316:	14fd                	addi	s1,s1,-1
 318:	f4f5                	bnez	s1,0x304
 31a:	8dea                	mv	s11,s10
 31c:	8962                	mv	s2,s8
 31e:	b789                	j	0x260
 320:	008d8913          	addi	s2,s11,8
 324:	000db483          	ld	s1,0(s11)
 328:	cc91                	beqz	s1,0x344
 32a:	0004c583          	lbu	a1,0(s1)
 32e:	c9b9                	beqz	a1,0x384
 330:	8552                	mv	a0,s4
 332:	e2dff0ef          	jal	ra,0x15e
 336:	0485                	addi	s1,s1,1
 338:	0004c583          	lbu	a1,0(s1)
 33c:	f9f5                	bnez	a1,0x330
 33e:	8dca                	mv	s11,s2
 340:	8962                	mv	s2,s8
 342:	bf39                	j	0x260
 344:	00000497          	auipc	s1,0x0
 348:	0b448493          	addi	s1,s1,180 # 0x3f8
 34c:	85e6                	mv	a1,s9
 34e:	b7cd                	j	0x330
 350:	008d8493          	addi	s1,s11,8
 354:	000dc583          	lbu	a1,0(s11)
 358:	8552                	mv	a0,s4
 35a:	e05ff0ef          	jal	ra,0x15e
 35e:	8da6                	mv	s11,s1
 360:	8962                	mv	s2,s8
 362:	bdfd                	j	0x260
 364:	85ce                	mv	a1,s3
 366:	8552                	mv	a0,s4
 368:	df7ff0ef          	jal	ra,0x15e
 36c:	8962                	mv	s2,s8
 36e:	bdcd                	j	0x260
 370:	85ce                	mv	a1,s3
 372:	8552                	mv	a0,s4
 374:	debff0ef          	jal	ra,0x15e
 378:	85a6                	mv	a1,s1
 37a:	8552                	mv	a0,s4
 37c:	de3ff0ef          	jal	ra,0x15e
 380:	8962                	mv	s2,s8
 382:	bdf9                	j	0x260
 384:	8dca                	mv	s11,s2
 386:	8962                	mv	s2,s8
 388:	bde1                	j	0x260
 38a:	70a6                	ld	ra,104(sp)
 38c:	7406                	ld	s0,96(sp)
 38e:	64e6                	ld	s1,88(sp)
 390:	6946                	ld	s2,80(sp)
 392:	69a6                	ld	s3,72(sp)
 394:	6a06                	ld	s4,64(sp)
 396:	7ae2                	ld	s5,56(sp)
 398:	7b42                	ld	s6,48(sp)
 39a:	7ba2                	ld	s7,40(sp)
 39c:	7c02                	ld	s8,32(sp)
 39e:	6ce2                	ld	s9,24(sp)
 3a0:	6d42                	ld	s10,16(sp)
 3a2:	6da2                	ld	s11,8(sp)
 3a4:	6165                	addi	sp,sp,112
 3a6:	8082                	ret
 3a8:	715d                	addi	sp,sp,-80
 3aa:	ec06                	sd	ra,24(sp)
 3ac:	f032                	sd	a2,32(sp)
 3ae:	f436                	sd	a3,40(sp)
 3b0:	f83a                	sd	a4,48(sp)
 3b2:	fc3e                	sd	a5,56(sp)
 3b4:	e0c2                	sd	a6,64(sp)
 3b6:	e4c6                	sd	a7,72(sp)
 3b8:	1010                	addi	a2,sp,32
 3ba:	e432                	sd	a2,8(sp)
 3bc:	e53ff0ef          	jal	ra,0x20e
 3c0:	60e2                	ld	ra,24(sp)
 3c2:	6161                	addi	sp,sp,80
 3c4:	8082                	ret
 3c6:	711d                	addi	sp,sp,-96
 3c8:	ec06                	sd	ra,24(sp)
 3ca:	f42e                	sd	a1,40(sp)
 3cc:	f832                	sd	a2,48(sp)
 3ce:	fc36                	sd	a3,56(sp)
 3d0:	e0ba                	sd	a4,64(sp)
 3d2:	e4be                	sd	a5,72(sp)
 3d4:	e8c2                	sd	a6,80(sp)
 3d6:	ecc6                	sd	a7,88(sp)
 3d8:	1030                	addi	a2,sp,40
 3da:	e432                	sd	a2,8(sp)
 3dc:	85aa                	mv	a1,a0
 3de:	4505                	li	a0,1
 3e0:	e2fff0ef          	jal	ra,0x20e
 3e4:	60e2                	ld	ra,24(sp)
 3e6:	6125                	addi	sp,sp,96
 3e8:	8082                	ret
 3ea:	0000                	unimp
 3ec:	0000                	unimp
 3ee:	0000                	unimp
 3f0:	6464                	ld	s1,200(s0)
 3f2:	0064                	addi	s1,sp,12
 3f4:	0000                	unimp
 3f6:	0000                	unimp
 3f8:	6e28                	ld	a0,88(a2)
 3fa:	6c75                	lui	s8,0x1d
 3fc:	296c                	fld	fa1,208(a0)
 3fe:	0000                	unimp
 400:	ff50                	sd	a2,184(a4)
 402:	ffff                	0xffff
 404:	fe9e                	sd	t2,376(sp)
 406:	ffff                	0xffff
 408:	ff70                	sd	a2,248(a4)
 40a:	ffff                	0xffff
 40c:	ff70                	sd	a2,248(a4)
 40e:	ffff                	0xffff
 410:	ff70                	sd	a2,248(a4)
 412:	ffff                	0xffff
 414:	ff70                	sd	a2,248(a4)
 416:	ffff                	0xffff
 418:	ff70                	sd	a2,248(a4)
 41a:	ffff                	0xffff
 41c:	ff70                	sd	a2,248(a4)
 41e:	ffff                	0xffff
 420:	ff70                	sd	a2,248(a4)
 422:	ffff                	0xffff
 424:	feb6                	sd	a3,376(sp)
 426:	ffff                	0xffff
 428:	ff70                	sd	a2,248(a4)
 42a:	ffff                	0xffff
 42c:	ff70                	sd	a2,248(a4)
 42e:	ffff                	0xffff
 430:	ff70                	sd	a2,248(a4)
 432:	ffff                	0xffff
 434:	fee6                	sd	s9,376(sp)
 436:	ffff                	0xffff
 438:	ff70                	sd	a2,248(a4)
 43a:	ffff                	0xffff
 43c:	ff70                	sd	a2,248(a4)
 43e:	ffff                	0xffff
 440:	ff20                	sd	s0,120(a4)
 442:	ffff                	0xffff
 444:	ff70                	sd	a2,248(a4)
 446:	ffff                	0xffff
 448:	ff70                	sd	a2,248(a4)
 44a:	ffff                	0xffff
 44c:	ff70                	sd	a2,248(a4)
 44e:	ffff                	0xffff
 450:	ff70                	sd	a2,248(a4)
 452:	ffff                	0xffff
 454:	fece                	sd	s3,376(sp)
 456:	ffff                	0xffff
 458:	3130                	fld	fa2,96(a0)
 45a:	3332                	fld	ft6,296(sp)
 45c:	3534                	fld	fa3,104(a0)
 45e:	3736                	fld	fa4,360(sp)
 460:	3938                	fld	fa4,112(a0)
 462:	4241                	li	tp,16
 464:	46454443          	fmadd.q	fs0,fa0,ft4,fs0,rmm
	...
