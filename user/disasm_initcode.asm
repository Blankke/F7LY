
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	00000517          	auipc	a0,0x0
   8:	3fc50513          	addi	a0,a0,1020 # 0x400
   c:	3ca000ef          	jal	0x3d6
  10:	459d                	li	a1,7
  12:	00000517          	auipc	a0,0x0
  16:	3fe50513          	addi	a0,a0,1022 # 0x410
  1a:	3bc000ef          	jal	0x3d6
  1e:	55fd                	li	a1,-1
  20:	00000517          	auipc	a0,0x0
  24:	3f050513          	addi	a0,a0,1008 # 0x410
  28:	3ae000ef          	jal	0x3d6
  2c:	156000ef          	jal	0x182
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
  f6:	fbbff0ef          	jal	0xb0
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
 19a:	ec5ff0ef          	jal	0x5e
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
 1b2:	0605ca63          	bltz	a1,0x226
 1b6:	4e01                	li	t3,0
 1b8:	830a                	mv	t1,sp
 1ba:	868a                	mv	a3,sp
 1bc:	4781                	li	a5,0
 1be:	47800813          	li	a6,1144
 1c2:	88be                	mv	a7,a5
 1c4:	0017851b          	addiw	a0,a5,1
 1c8:	87aa                	mv	a5,a0
 1ca:	02c5f73b          	remuw	a4,a1,a2
 1ce:	1702                	slli	a4,a4,0x20
 1d0:	9301                	srli	a4,a4,0x20
 1d2:	9742                	add	a4,a4,a6
 1d4:	00074703          	lbu	a4,0(a4)
 1d8:	00e68023          	sb	a4,0(a3)
 1dc:	872e                	mv	a4,a1
 1de:	02c5d5bb          	divuw	a1,a1,a2
 1e2:	0685                	addi	a3,a3,1
 1e4:	fcc77fe3          	bgeu	a4,a2,0x1c2
 1e8:	000e0c63          	beqz	t3,0x200
 1ec:	01050793          	addi	a5,a0,16
 1f0:	00278533          	add	a0,a5,sp
 1f4:	02d00793          	li	a5,45
 1f8:	fef50823          	sb	a5,-16(a0)
 1fc:	0028879b          	addiw	a5,a7,2
 200:	fff7891b          	addiw	s2,a5,-1
 204:	00678433          	add	s0,a5,t1
 208:	fff44583          	lbu	a1,-1(s0)
 20c:	8526                	mv	a0,s1
 20e:	f7fff0ef          	jal	0x18c
 212:	397d                	addiw	s2,s2,-1
 214:	147d                	addi	s0,s0,-1
 216:	fe0959e3          	bgez	s2,0x208
 21a:	70a2                	ld	ra,40(sp)
 21c:	7402                	ld	s0,32(sp)
 21e:	64e2                	ld	s1,24(sp)
 220:	6942                	ld	s2,16(sp)
 222:	6145                	addi	sp,sp,48
 224:	8082                	ret
 226:	40b005bb          	negw	a1,a1
 22a:	4e05                	li	t3,1
 22c:	b771                	j	0x1b8
 22e:	715d                	addi	sp,sp,-80
 230:	e486                	sd	ra,72(sp)
 232:	fc26                	sd	s1,56(sp)
 234:	0005c483          	lbu	s1,0(a1)
 238:	16048c63          	beqz	s1,0x3b0
 23c:	e0a2                	sd	s0,64(sp)
 23e:	f84a                	sd	s2,48(sp)
 240:	f44e                	sd	s3,40(sp)
 242:	f052                	sd	s4,32(sp)
 244:	ec56                	sd	s5,24(sp)
 246:	e85a                	sd	s6,16(sp)
 248:	8a2a                	mv	s4,a0
 24a:	8b32                	mv	s6,a2
 24c:	00158413          	addi	s0,a1,1
 250:	4901                	li	s2,0
 252:	02500993          	li	s3,37
 256:	4ad5                	li	s5,21
 258:	a829                	j	0x272
 25a:	85a6                	mv	a1,s1
 25c:	8552                	mv	a0,s4
 25e:	f2fff0ef          	jal	0x18c
 262:	a019                	j	0x268
 264:	01390d63          	beq	s2,s3,0x27e
 268:	0405                	addi	s0,s0,1
 26a:	fff44483          	lbu	s1,-1(s0)
 26e:	12048b63          	beqz	s1,0x3a4
 272:	fe0919e3          	bnez	s2,0x264
 276:	ff3492e3          	bne	s1,s3,0x25a
 27a:	894e                	mv	s2,s3
 27c:	b7f5                	j	0x268
 27e:	0f348e63          	beq	s1,s3,0x37a
 282:	f9d4879b          	addiw	a5,s1,-99
 286:	0ff7f793          	zext.b	a5,a5
 28a:	0efaef63          	bltu	s5,a5,0x388
 28e:	f9d4879b          	addiw	a5,s1,-99
 292:	0ff7f713          	zext.b	a4,a5
 296:	0eeae963          	bltu	s5,a4,0x388
 29a:	00271793          	slli	a5,a4,0x2
 29e:	42000713          	li	a4,1056
 2a2:	97ba                	add	a5,a5,a4
 2a4:	439c                	lw	a5,0(a5)
 2a6:	97ba                	add	a5,a5,a4
 2a8:	8782                	jr	a5
 2aa:	008b0493          	addi	s1,s6,8
 2ae:	4685                	li	a3,1
 2b0:	4629                	li	a2,10
 2b2:	000b2583          	lw	a1,0(s6)
 2b6:	8552                	mv	a0,s4
 2b8:	eedff0ef          	jal	0x1a4
 2bc:	8b26                	mv	s6,s1
 2be:	4901                	li	s2,0
 2c0:	b765                	j	0x268
 2c2:	008b0493          	addi	s1,s6,8
 2c6:	4681                	li	a3,0
 2c8:	4629                	li	a2,10
 2ca:	000b2583          	lw	a1,0(s6)
 2ce:	8552                	mv	a0,s4
 2d0:	ed5ff0ef          	jal	0x1a4
 2d4:	8b26                	mv	s6,s1
 2d6:	4901                	li	s2,0
 2d8:	bf41                	j	0x268
 2da:	008b0493          	addi	s1,s6,8
 2de:	4681                	li	a3,0
 2e0:	4641                	li	a2,16
 2e2:	000b2583          	lw	a1,0(s6)
 2e6:	8552                	mv	a0,s4
 2e8:	ebdff0ef          	jal	0x1a4
 2ec:	8b26                	mv	s6,s1
 2ee:	4901                	li	s2,0
 2f0:	bfa5                	j	0x268
 2f2:	e45e                	sd	s7,8(sp)
 2f4:	008b0b93          	addi	s7,s6,8
 2f8:	000b3903          	ld	s2,0(s6)
 2fc:	03000593          	li	a1,48
 300:	8552                	mv	a0,s4
 302:	e8bff0ef          	jal	0x18c
 306:	07800593          	li	a1,120
 30a:	8552                	mv	a0,s4
 30c:	e81ff0ef          	jal	0x18c
 310:	44c1                	li	s1,16
 312:	47800b13          	li	s6,1144
 316:	03c95793          	srli	a5,s2,0x3c
 31a:	97da                	add	a5,a5,s6
 31c:	0007c583          	lbu	a1,0(a5)
 320:	8552                	mv	a0,s4
 322:	e6bff0ef          	jal	0x18c
 326:	0912                	slli	s2,s2,0x4
 328:	14fd                	addi	s1,s1,-1
 32a:	f4f5                	bnez	s1,0x316
 32c:	8b5e                	mv	s6,s7
 32e:	4901                	li	s2,0
 330:	6ba2                	ld	s7,8(sp)
 332:	bf1d                	j	0x268
 334:	008b0913          	addi	s2,s6,8
 338:	000b3483          	ld	s1,0(s6)
 33c:	cc91                	beqz	s1,0x358
 33e:	0004c583          	lbu	a1,0(s1)
 342:	cdb1                	beqz	a1,0x39e
 344:	8552                	mv	a0,s4
 346:	e47ff0ef          	jal	0x18c
 34a:	0485                	addi	s1,s1,1
 34c:	0004c583          	lbu	a1,0(s1)
 350:	f9f5                	bnez	a1,0x344
 352:	8b4a                	mv	s6,s2
 354:	4901                	li	s2,0
 356:	bf09                	j	0x268
 358:	00000497          	auipc	s1,0x0
 35c:	0c048493          	addi	s1,s1,192 # 0x418
 360:	02800593          	li	a1,40
 364:	b7c5                	j	0x344
 366:	008b0493          	addi	s1,s6,8
 36a:	000b4583          	lbu	a1,0(s6)
 36e:	8552                	mv	a0,s4
 370:	e1dff0ef          	jal	0x18c
 374:	8b26                	mv	s6,s1
 376:	4901                	li	s2,0
 378:	bdc5                	j	0x268
 37a:	02500593          	li	a1,37
 37e:	8552                	mv	a0,s4
 380:	e0dff0ef          	jal	0x18c
 384:	4901                	li	s2,0
 386:	b5cd                	j	0x268
 388:	02500593          	li	a1,37
 38c:	8552                	mv	a0,s4
 38e:	dffff0ef          	jal	0x18c
 392:	85a6                	mv	a1,s1
 394:	8552                	mv	a0,s4
 396:	df7ff0ef          	jal	0x18c
 39a:	4901                	li	s2,0
 39c:	b5f1                	j	0x268
 39e:	8b4a                	mv	s6,s2
 3a0:	4901                	li	s2,0
 3a2:	b5d9                	j	0x268
 3a4:	6406                	ld	s0,64(sp)
 3a6:	7942                	ld	s2,48(sp)
 3a8:	79a2                	ld	s3,40(sp)
 3aa:	7a02                	ld	s4,32(sp)
 3ac:	6ae2                	ld	s5,24(sp)
 3ae:	6b42                	ld	s6,16(sp)
 3b0:	60a6                	ld	ra,72(sp)
 3b2:	74e2                	ld	s1,56(sp)
 3b4:	6161                	addi	sp,sp,80
 3b6:	8082                	ret
 3b8:	715d                	addi	sp,sp,-80
 3ba:	ec06                	sd	ra,24(sp)
 3bc:	f032                	sd	a2,32(sp)
 3be:	f436                	sd	a3,40(sp)
 3c0:	f83a                	sd	a4,48(sp)
 3c2:	fc3e                	sd	a5,56(sp)
 3c4:	e0c2                	sd	a6,64(sp)
 3c6:	e4c6                	sd	a7,72(sp)
 3c8:	1010                	addi	a2,sp,32
 3ca:	e432                	sd	a2,8(sp)
 3cc:	e63ff0ef          	jal	0x22e
 3d0:	60e2                	ld	ra,24(sp)
 3d2:	6161                	addi	sp,sp,80
 3d4:	8082                	ret
 3d6:	711d                	addi	sp,sp,-96
 3d8:	ec06                	sd	ra,24(sp)
 3da:	f42e                	sd	a1,40(sp)
 3dc:	f832                	sd	a2,48(sp)
 3de:	fc36                	sd	a3,56(sp)
 3e0:	e0ba                	sd	a4,64(sp)
 3e2:	e4be                	sd	a5,72(sp)
 3e4:	e8c2                	sd	a6,80(sp)
 3e6:	ecc6                	sd	a7,88(sp)
 3e8:	1030                	addi	a2,sp,40
 3ea:	e432                	sd	a2,8(sp)
 3ec:	85aa                	mv	a1,a0
 3ee:	4505                	li	a0,1
 3f0:	e3fff0ef          	jal	0x22e
 3f4:	60e2                	ld	ra,24(sp)
 3f6:	6125                	addi	sp,sp,96
 3f8:	8082                	ret
 3fa:	0000                	unimp
 3fc:	0000                	unimp
 3fe:	0000                	unimp
 400:	6548                	ld	a0,136(a0)
 402:	6c6c                	ld	a1,216(s0)
 404:	57202c6f          	jal	s8,0x2976
 408:	646c726f          	jal	tp,0xc7a4e
 40c:	0a21                	addi	s4,s4,8
 40e:	0000                	unimp
 410:	2061                	.insn	2, 0x2061
 412:	203d                	.insn	2, 0x203d
 414:	6425                	lui	s0,0x9
 416:	000a                	c.slli	zero,0x2
 418:	6e28                	ld	a0,88(a2)
 41a:	6c75                	lui	s8,0x1d
 41c:	296c                	fld	fa1,208(a0)
 41e:	0000                	unimp
 420:	ff46                	sd	a7,440(sp)
 422:	ffff                	.insn	2, 0xffff
 424:	fe8a                	sd	sp,376(sp)
 426:	ffff                	.insn	2, 0xffff
 428:	ff68                	sd	a0,248(a4)
 42a:	ffff                	.insn	2, 0xffff
 42c:	ff68                	sd	a0,248(a4)
 42e:	ffff                	.insn	2, 0xffff
 430:	ff68                	sd	a0,248(a4)
 432:	ffff                	.insn	2, 0xffff
 434:	ff68                	sd	a0,248(a4)
 436:	ffff                	.insn	2, 0xffff
 438:	ff68                	sd	a0,248(a4)
 43a:	ffff                	.insn	2, 0xffff
 43c:	ff68                	sd	a0,248(a4)
 43e:	ffff                	.insn	2, 0xffff
 440:	ff68                	sd	a0,248(a4)
 442:	ffff                	.insn	2, 0xffff
 444:	fea2                	sd	s0,376(sp)
 446:	ffff                	.insn	2, 0xffff
 448:	ff68                	sd	a0,248(a4)
 44a:	ffff                	.insn	2, 0xffff
 44c:	ff68                	sd	a0,248(a4)
 44e:	ffff                	.insn	2, 0xffff
 450:	ff68                	sd	a0,248(a4)
 452:	ffff                	.insn	2, 0xffff
 454:	fed2                	sd	s4,376(sp)
 456:	ffff                	.insn	2, 0xffff
 458:	ff68                	sd	a0,248(a4)
 45a:	ffff                	.insn	2, 0xffff
 45c:	ff68                	sd	a0,248(a4)
 45e:	ffff                	.insn	2, 0xffff
 460:	ff14                	sd	a3,56(a4)
 462:	ffff                	.insn	2, 0xffff
 464:	ff68                	sd	a0,248(a4)
 466:	ffff                	.insn	2, 0xffff
 468:	ff68                	sd	a0,248(a4)
 46a:	ffff                	.insn	2, 0xffff
 46c:	ff68                	sd	a0,248(a4)
 46e:	ffff                	.insn	2, 0xffff
 470:	ff68                	sd	a0,248(a4)
 472:	ffff                	.insn	2, 0xffff
 474:	feba                	sd	a4,376(sp)
 476:	ffff                	.insn	2, 0xffff
 478:	3130                	fld	fa2,96(a0)
 47a:	3332                	fld	ft6,296(sp)
 47c:	3534                	fld	fa3,104(a0)
 47e:	3736                	fld	fa4,360(sp)
 480:	3938                	fld	fa4,112(a0)
 482:	4241                	li	tp,16
 484:	46454443          	.insn	4, 0x46454443
	...
