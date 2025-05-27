
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	460d                	li	a2,3
   6:	00000597          	auipc	a1,0x0
   a:	16a58593          	addi	a1,a1,362 # 0x170
   e:	4505                	li	a0,1
  10:	006000ef          	jal	0x16
  14:	a001                	j	0x14
  16:	03800893          	li	a7,56
  1a:	18000693          	li	a3,384
  1e:	00000073          	ecall
  22:	2501                	sext.w	a0,a0
  24:	8082                	ret
  26:	03900893          	li	a7,57
  2a:	00000073          	ecall
  2e:	2501                	sext.w	a0,a0
  30:	8082                	ret
  32:	03f00893          	li	a7,63
  36:	00000073          	ecall
  3a:	8082                	ret
  3c:	04000893          	li	a7,64
  40:	00000073          	ecall
  44:	8082                	ret
  46:	0ac00893          	li	a7,172
  4a:	00000073          	ecall
  4e:	2501                	sext.w	a0,a0
  50:	8082                	ret
  52:	0ad00893          	li	a7,173
  56:	00000073          	ecall
  5a:	2501                	sext.w	a0,a0
  5c:	8082                	ret
  5e:	07c00893          	li	a7,124
  62:	00000073          	ecall
  66:	2501                	sext.w	a0,a0
  68:	8082                	ret
  6a:	85b2                	mv	a1,a2
  6c:	863a                	mv	a2,a4
  6e:	c191                	beqz	a1,0x72
  70:	95b6                	add	a1,a1,a3
  72:	0dc00893          	li	a7,220
  76:	4681                	li	a3,0
  78:	4701                	li	a4,0
  7a:	4781                	li	a5,0
  7c:	00000073          	ecall
  80:	2501                	sext.w	a0,a0
  82:	8082                	ret
  84:	05d00893          	li	a7,93
  88:	00000073          	ecall
  8c:	8082                	ret
  8e:	10400893          	li	a7,260
  92:	4681                	li	a3,0
  94:	00000073          	ecall
  98:	2501                	sext.w	a0,a0
  9a:	8082                	ret
  9c:	0dd00893          	li	a7,221
  a0:	00000073          	ecall
  a4:	2501                	sext.w	a0,a0
  a6:	8082                	ret
  a8:	0dd00893          	li	a7,221
  ac:	00000073          	ecall
  b0:	2501                	sext.w	a0,a0
  b2:	8082                	ret
  b4:	09900893          	li	a7,153
  b8:	00000073          	ecall
  bc:	8082                	ret
  be:	0d700893          	li	a7,215
  c2:	00000073          	ecall
  c6:	2501                	sext.w	a0,a0
  c8:	8082                	ret
  ca:	1141                	addi	sp,sp,-16
  cc:	e406                	sd	ra,8(sp)
  ce:	85aa                	mv	a1,a0
  d0:	4601                	li	a2,0
  d2:	557d                	li	a0,-1
  d4:	fbbff0ef          	jal	0x8e
  d8:	60a2                	ld	ra,8(sp)
  da:	0141                	addi	sp,sp,16
  dc:	8082                	ret
  de:	02500893          	li	a7,37
  e2:	1702                	slli	a4,a4,0x20
  e4:	9301                	srli	a4,a4,0x20
  e6:	00000073          	ecall
  ea:	2501                	sext.w	a0,a0
  ec:	8082                	ret
  ee:	02300893          	li	a7,35
  f2:	1602                	slli	a2,a2,0x20
  f4:	9201                	srli	a2,a2,0x20
  f6:	00000073          	ecall
  fa:	2501                	sext.w	a0,a0
  fc:	8082                	ret
  fe:	0a000893          	li	a7,160
 102:	00000073          	ecall
 106:	2501                	sext.w	a0,a0
 108:	8082                	ret
 10a:	0d600893          	li	a7,214
 10e:	00000073          	ecall
 112:	2501                	sext.w	a0,a0
 114:	8082                	ret
 116:	03100893          	li	a7,49
 11a:	00000073          	ecall
 11e:	2501                	sext.w	a0,a0
 120:	8082                	ret
 122:	03d00893          	li	a7,61
 126:	00000073          	ecall
 12a:	2501                	sext.w	a0,a0
 12c:	8082                	ret
 12e:	03b00893          	li	a7,59
 132:	4581                	li	a1,0
 134:	00000073          	ecall
 138:	2501                	sext.w	a0,a0
 13a:	8082                	ret
 13c:	48dd                	li	a7,23
 13e:	00000073          	ecall
 142:	2501                	sext.w	a0,a0
 144:	8082                	ret
 146:	02800893          	li	a7,40
 14a:	00000073          	ecall
 14e:	2501                	sext.w	a0,a0
 150:	8082                	ret
 152:	02700893          	li	a7,39
 156:	4581                	li	a1,0
 158:	00000073          	ecall
 15c:	2501                	sext.w	a0,a0
 15e:	8082                	ret
 160:	48cd                	li	a7,19
 162:	00000073          	ecall
 166:	2501                	sext.w	a0,a0
 168:	8082                	ret
 16a:	0000                	unimp
 16c:	0000                	unimp
 16e:	0000                	unimp
 170:	6464                	ld	s1,200(s0)
 172:	0064                	addi	s1,sp,12
